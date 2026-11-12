// The screen-space reflection march (RS4), shared by the opaque surfaces' SSR (SSR.slang, from the G-buffer) and the transparent
// surfaces' (Material_TransparentLit_Mesh.slang, from their own shading, in raster): the mirror ray marched in screen space through a
// depth buffer (perspective-correct depth along the segment, McGuire & Mara 2014), refined by bisection at the first crossing within
// a thickness. Requires shaderIO.h before it.
#pragma once

float2 ssr_clip_to_uv(float4 clip)
{
    return float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
}

// Towards the screen's edges SSR knows less and less (what is beyond is unknown): it fades out over the outer 20 % smoothly, so
// the environment / probes take over gradually, not at a line (UE's SSR edge fade).
float ssr_edge_fade(float2 uv)
{
    const float2 edge = min(uv, 1.0 - uv);
    return smoothstep(0.0, 0.2, min(edge.x, edge.y));
}

// The ray from `start` along R (world space), at most `maxDistance` long, through `depthTex` (reverse Z, the view this frame):
// whether it hits what the depth buffer shows, and where (hitUV, its depth, how far along the ray in world units). facingFade: the
// ray towards the camera soon goes behind what the screen shows, so it fades out (the caller multiplies its confidence by it).
bool ssr_march(UniformBufferObject* ubo, Texture2D<float> depthTex, float3 start, float3 R, float maxDistance, float thickness,
               uint maxSteps, float2 viewportSize, float noise, out float2 hitUV, out float hitDepth, out float facingFade)
{
    hitUV = float2(0.0);
    hitDepth = 0.0;
    // Towards the camera the ray soon goes behind what the screen shows: faded out (the view's +z is forward).
    const float3 viewR = mul(ubo->view, float4(R, 0.0)).xyz;
    facingFade = saturate((viewR.z + 0.25) * 4.0);
    if (facingFade <= 0.0)
        return false;

    const float nearPlane = mul(ubo->proj, float4(0.0, 0.0, 1.0, 1.0)).z; // depth x view distance (infinite reverse Z)
    const float4x4 viewProj = mul(ubo->proj, ubo->view);
    const float startZ = mul(ubo->view, float4(start, 1.0)).z;
    float rayLength = maxDistance;
    if (viewR.z < 0.0) // ends in front of the near plane otherwise
        rayLength = min(rayLength, max(startZ - 2.0 * nearPlane, 0.0) / -viewR.z);
    if (rayLength <= 0.0)
        return false;

    const float4 clip0 = mul(viewProj, float4(start, 1.0));
    const float4 clip1 = mul(viewProj, float4(start + R * rayLength, 1.0));
    if (clip0.w <= 1e-5 || clip1.w <= 1e-5)
        return false;
    const float2 uv0 = ssr_clip_to_uv(clip0);
    const float2 uv1 = ssr_clip_to_uv(clip1);
    const float invW0 = 1.0 / clip0.w;
    const float invW1 = 1.0 / clip1.w;
    // Where the segment leaves the screen: no steps past it.
    float tEnd = 1.0;
    const float2 duv = uv1 - uv0;
    if (duv.x > 1e-6) tEnd = min(tEnd, (1.0 - uv0.x) / duv.x);
    if (duv.x < -1e-6) tEnd = min(tEnd, -uv0.x / duv.x);
    if (duv.y > 1e-6) tEnd = min(tEnd, (1.0 - uv0.y) / duv.y);
    if (duv.y < -1e-6) tEnd = min(tEnd, -uv0.y / duv.y);
    const float pixels = length(duv * viewportSize) * tEnd;
    const uint steps = clamp(uint(ceil(pixels)), 4u, clamp(maxSteps, 4u, 256u));

    float previousT = 0.0;
    float hitT = -1.0;
    for (uint i = 0; i < steps; ++i)
    {
        const float t = tEnd * (float(i) + noise) / float(steps);
        const float2 sampleUV = lerp(uv0, uv1, t);
        const float rayZ = 1.0 / lerp(invW0, invW1, t);
        const float sceneDepth = depthTex.Load(int3(int2(sampleUV * viewportSize), 0)).r;
        if (sceneDepth > 0.0)
        {
            const float behind = rayZ - nearPlane / sceneDepth;
            // A hit: the ray just behind what the pixel shows, within the step's own depth span (it may have crossed the surface
            // anywhere along the step: UE's ray cast tolerance), never more than the thickness. Unbounded, a long step at a grazing
            // angle counted anything the ray passed behind as a hit -- thin fixtures, cables, an awning's housing -- reflections
            // the RT reflections do not have, which moved as the camera changed what covered what.
            const float stepZ = abs(rayZ - 1.0 / lerp(invW0, invW1, previousT));
            const float tolerance = clamp(1.5 * stepZ, 0.02 * ubo->unitsPerMeter, thickness);
            if (behind > 0.0 && behind < tolerance)
            {
                hitT = t;
                break;
            }
        }
        previousT = t;
    }
    if (hitT < 0.0)
        return false;

    // Bisection between the last step in front and the first behind.
    float lo = previousT;
    float hi = hitT;
    for (uint i = 0; i < 5; ++i)
    {
        const float mid = 0.5 * (lo + hi);
        const float2 sampleUV = lerp(uv0, uv1, mid);
        const float sceneDepth = depthTex.Load(int3(int2(sampleUV * viewportSize), 0)).r;
        const float behind = 1.0 / lerp(invW0, invW1, mid) - (sceneDepth > 0.0 ? nearPlane / sceneDepth : 1e30);
        if (behind > 0.0)
            hi = mid;
        else
            lo = mid;
    }
    hitUV = lerp(uv0, uv1, hi);
    hitDepth = depthTex.Load(int3(int2(hitUV * viewportSize), 0)).r;
    return hitDepth > 0.0;
}
