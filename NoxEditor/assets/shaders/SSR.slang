#include "shaderIO.h"

// Screen-space reflections (RS4, Settings > Screen-Space Reflections; vs the RT reflections, Reflection.slang). UE5's / Godot's
// approach: the mirror reflection ray marched in screen space through this frame's depth buffer (perspective-correct depth along the
// segment, McGuire & Mara 2014), refined by bisection at the first crossing within a thickness; the radiance at the hit is last
// frame's lit scene (reprojected through the previous camera) at the blur mip the roughness' cone covers there. The confidence fades
// at the screen's edges, towards the camera, with roughness and near the ray's end; the deferred lighting blends the environment /
// reflection probes in where it is low. temporalMain then blends it with last frame's result where the reflection was (its mirror
// image's motion), clamped to this frame's neighbourhood: stable on a still camera, following the reflection when the camera moves
// (DLSS / TAA reproject the surface, not what it reflects). Only perspective cameras.

struct SSRPayload
{
    uint dummy;
};
groupshared SSRPayload gPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gPayload);
}

struct MSOutput
{
    float4 pos : SV_Position;
};

static const float2 positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(3, 1, 1)]
void meshMain(out indices uint3 triangles[1], out vertices MSOutput vertices[3], uint threadID : SV_GroupIndex)
{
    SetMeshOutputCounts(3, 1);
    if (threadID < 3)
        vertices[threadID].pos = float4(positions[threadID], 0.0, 1.0);
    if (threadID == 0)
        triangles[0] = uint3(0, 1, 2);
}

float2 ssr_clip_to_uv(float4 clip)
{
    return float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
}

float ssr_edge_fade(float2 uv)
{
    const float2 edge = min(uv, 1.0 - uv);
    return saturate(min(edge.x, edge.y) / 0.08);
}

struct SSROutput
{
    float4 reflection : SV_Target0; // rgb radiance, a confidence
    float hitDistance : SV_Target1; // world units, 0: no hit
};

float4 traceReflection(MSOutput input, PushConstantSSR pushConstant, out float hitDistanceOut);

[shader("fragment")]
SSROutput fragMain(MSOutput input, uniform PushConstantSSR pushConstant)
{
    SSROutput output;
    output.reflection = traceReflection(input, pushConstant, output.hitDistance);
    if (output.reflection.a <= 0.0)
        output.hitDistance = 0.0;
    return output;
}

float4 traceReflection(MSOutput input, PushConstantSSR pushConstant, out float hitDistanceOut)
{
    hitDistanceOut = 0.0;
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    const int2 pixelCoord = int2(input.pos.xy);
    const float2 viewportSize = pushConstant.viewportSize;
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    const float depth = depthTex.Load(int3(pixelCoord, 0)).r;
    if (depth <= 0.0 || mul(ubo->proj, float4(0.0, 0.0, 2.0, 1.0)).w < 1.5) // the sky, or an orthographic camera
        return float4(0.0);

    Texture2D materialTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.materialTextureIndex, 0));
    const float roughness = clamp(materialTex.Load(int3(pixelCoord, 0)).r, 0.0, 1.0);
    if (roughness >= pushConstant.maxRoughness)
        return float4(0.0);

    const float2 uv = (float2(pixelCoord) + 0.5) / viewportSize;
    const float4 worldH = mul(pushConstant.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    const float3 worldPos = worldH.xyz / worldH.w;
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.normalTextureIndex, 0));
    const float3 rawNormal = normalTex.Load(int3(pixelCoord, 0)).rgb;
    const float lenN = length(rawNormal);
    if (lenN < 1e-6)
        return float4(0.0);
    const float3 N = rawNormal / lenN;
    const float3 V = normalize(ubo->cameraWorldPos.xyz - worldPos);
    const float3 R = reflect(-V, N);

    // Towards the camera the ray soon goes behind what the screen shows: faded out (the view's +z is forward).
    const float3 viewR = mul(ubo->view, float4(R, 0.0)).xyz;
    const float facingFade = saturate((viewR.z + 0.25) * 4.0);
    if (facingFade <= 0.0)
        return float4(0.0);

    const float nearPlane = mul(ubo->proj, float4(0.0, 0.0, 1.0, 1.0)).z; // depth x view distance (infinite reverse Z)
    const float4x4 viewProj = mul(ubo->proj, ubo->view);
    const float3 start = worldPos + N * raster_surface_bias(ubo, worldPos, 0.01 * ubo->unitsPerMeter);
    const float startZ = mul(ubo->view, float4(start, 1.0)).z;
    float rayLength = pushConstant.maxDistance;
    if (viewR.z < 0.0) // ends in front of the near plane otherwise
        rayLength = min(rayLength, max(startZ - 2.0 * nearPlane, 0.0) / -viewR.z);
    if (rayLength <= 0.0)
        return float4(0.0);

    const float4 clip0 = mul(viewProj, float4(start, 1.0));
    const float4 clip1 = mul(viewProj, float4(start + R * rayLength, 1.0));
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
    const uint steps = clamp(uint(ceil(pixels)), 4u, clamp(pushConstant.maxSteps, 4u, 256u));

    const float2 noisePixel = float2(pixelCoord) + float(pushConstant.frameIndex % 64u) * float2(5.588238, 3.45367);
    const float noise = frac(52.9829189 * frac(0.06711056 * noisePixel.x + 0.00583715 * noisePixel.y));

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
            // The step's own depth span counts as thickness too (long steps at grazing angles would jump over thin surfaces).
            const float stepZ = abs(rayZ - 1.0 / lerp(invW0, invW1, previousT));
            if (behind > 0.0 && behind < max(pushConstant.thickness, 1.5 * stepZ))
            {
                hitT = t;
                break;
            }
        }
        previousT = t;
    }
    if (hitT < 0.0)
        return float4(0.0);

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
    const float2 hitUV = lerp(uv0, uv1, hi);
    const int2 hitPixel = int2(hitUV * viewportSize);
    const float hitDepth = depthTex.Load(int3(hitPixel, 0)).r;
    if (hitDepth <= 0.0)
        return float4(0.0);
    const float4 hitWorldH = mul(pushConstant.invViewProj, float4(hitUV.x * 2.0 - 1.0, 1.0 - hitUV.y * 2.0, hitDepth, 1.0));
    const float3 hitWorld = hitWorldH.xyz / hitWorldH.w;
    // The back of a surface (its front faces along the ray): not what the reflection sees.
    const float3 hitNormalRaw = normalTex.Load(int3(hitPixel, 0)).rgb;
    if (dot(hitNormalRaw, R) > 0.1 * length(hitNormalRaw))
        return float4(0.0);

    // Last frame's lit scene at the hit, through the previous camera (and its jitter: that image's pixels).
    const float4 previous = prev_frame_uv(ubo, hitWorld, viewportSize);
    if (previous.w <= 0.0)
        return float4(0.0);
    const float2 previousUV = previous.xy;
    if (any(previousUV <= 0.0) || any(previousUV >= 1.0))
        return float4(0.0);

    // The roughness' cone at the hit, in pixels: its blur mip (ColorPyramid.slang's 2x2 averages).
    const float hitDistance = length(hitWorld - start);
    hitDistanceOut = hitDistance;
    const float hitZ = max(mul(ubo->view, float4(hitWorld, 1.0)).z, nearPlane);
    const float focal = mul(ubo->proj, float4(0.0, 1.0, 0.0, 0.0)).y;
    const float coneDiameter = 2.0 * hitDistance * roughness * roughness;
    const float conePixels = coneDiameter * focal * viewportSize.y / (2.0 * hitZ);
    const float mip = clamp(log2(max(conePixels, 1.0)), 0.0, float(max(pushConstant.historyMipCount, 1u) - 1u));
    Texture2D historyTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.historyTextureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    const float3 radiance = historyTex.SampleLevel(linearClamp, previousUV, mip).rgb;

    const float roughnessFade = saturate((pushConstant.maxRoughness - roughness) / 0.15);
    const float distanceFade = 1.0 - smoothstep(0.8, 1.0, hitDistance / max(pushConstant.maxDistance, 1e-4));
    const float confidence = facingFade * roughnessFade * distanceFade * ssr_edge_fade(hitUV) * ssr_edge_fade(previousUV);
    return float4(max(radiance, float3(0.0)), confidence);
}

// --- temporalMain ----------------------------------------------------------------------------------------------------------------

float3 ssr_rgb_to_ycocg(float3 c)
{
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 ssr_ycocg_to_rgb(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

[shader("fragment")]
float4 temporalMain(MSOutput input, uniform PushConstantSSRTemporal pushConstant) : SV_Target0
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    const int2 pixelCoord = int2(input.pos.xy);
    const float2 viewportSize = pushConstant.viewportSize;
    Texture2D rawTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.rawTextureIndex, 0));
    const float4 raw = rawTex.Load(int3(pixelCoord, 0));
    if (pushConstant.historyTextureIndex == 0xFFFFFFFF)
        return raw;
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    const float depth = depthTex.Load(int3(pixelCoord, 0)).r;
    if (depth <= 0.0)
        return float4(0.0);

    // This frame's neighbourhood (3 x 3, YCoCg: luminance and chroma apart): what last frame's value may be.
    float3 lo = float3(1e30), hi = float3(-1e30);
    float aLo = 1.0, aHi = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const int2 p = clamp(pixelCoord + int2(x, y), int2(0), int2(viewportSize) - 1);
            const float4 s = rawTex.Load(int3(p, 0));
            const float3 c = ssr_rgb_to_ycocg(s.rgb);
            lo = min(lo, c);
            hi = max(hi, c);
            aLo = min(aLo, s.a);
            aHi = max(aHi, s.a);
        }
    }

    // Where the reflection was last frame: a mirror's is its mirror image, the hit distance further along the view ray (it moves as if
    // behind the surface); a rough one's blur moves with the surface.
    const float2 uv = (float2(pixelCoord) + 0.5) / viewportSize;
    const float4 worldH = mul(pushConstant.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    const float3 worldPos = worldH.xyz / worldH.w;
    const float3 camera = ubo->cameraWorldPos.xyz;
    const float surfaceDistance = max(length(worldPos - camera), 1e-4);
    Texture2D<float> hitTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.hitDistanceTextureIndex, 0));
    const float hitDistance = hitTex.Load(int3(pixelCoord, 0));
    Texture2D materialTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.materialTextureIndex, 0));
    const float roughness = saturate(materialTex.Load(int3(pixelCoord, 0)).r);
    const float mirror = hitDistance > 0.0 ? 1.0 - smoothstep(0.2, 0.45, roughness) : 0.0;
    const float3 mirrorImage = camera + (worldPos - camera) * ((surfaceDistance + hitDistance) / surfaceDistance);
    const float4 previous = prev_frame_uv(ubo, lerp(worldPos, mirrorImage, mirror), viewportSize);
    if (previous.w <= 0.0 || any(previous.xy <= 0.0) || any(previous.xy >= 1.0))
        return raw;

    Texture2D historyTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.historyTextureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    float4 history = max(historyTex.SampleLevel(linearClamp, previous.xy, 0.0), float4(0.0));
    history.rgb = ssr_ycocg_to_rgb(clamp(ssr_rgb_to_ycocg(history.rgb), lo, hi));
    history.a = clamp(history.a, aLo, aHi);
    return lerp(history, raw, saturate(pushConstant.currentWeight));
}
