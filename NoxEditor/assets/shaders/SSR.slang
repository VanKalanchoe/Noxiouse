#include "shaderIO.h"

// Screen-space reflections (RS4, Settings > Screen-Space Reflections; vs the RT reflections, Reflection.slang). UE5's / Godot's
// approach: the mirror reflection ray marched in screen space through this frame's depth buffer (perspective-correct depth along the
// segment, McGuire & Mara 2014), refined by bisection at the first crossing within a thickness; the radiance at the hit is last
// frame's lit scene (reprojected through the previous camera) at the blur mip the roughness' cone covers there. The confidence fades
// at the screen's edges, towards the camera, with roughness and near the ray's end; the deferred lighting blends the environment /
// reflection probes in where it is low. filterMain then blurs it by roughness on its own surface (Godot's SSR filter). Only
// perspective cameras.

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

// Towards the screen's edges SSR knows less and less (what is beyond is unknown): it fades out over the outer 20 % smoothly, so
// the environment / probes take over gradually, not at a line (UE's SSR edge fade).
float ssr_edge_fade(float2 uv)
{
    const float2 edge = min(uv, 1.0 - uv);
    return smoothstep(0.0, 0.2, min(edge.x, edge.y));
}

float4 traceReflection(MSOutput input, PushConstantSSR pushConstant);

[shader("fragment")]
float4 fragMain(MSOutput input, uniform PushConstantSSR pushConstant) : SV_Target0
{
    // Premultiplied: rgb = light x confidence (a), so the filter averages hits and misses correctly and the lighting blends once.
    const float4 reflection = traceReflection(input, pushConstant);
    return float4(reflection.rgb * reflection.a, reflection.a);
}

float4 traceReflection(MSOutput input, PushConstantSSR pushConstant)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    const int2 pixelCoord = int2(input.pos.xy);
    const float2 viewportSize = pushConstant.viewportSize;
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    const float depth = depthTex.Load(int3(pixelCoord, 0)).r;
    if (depth <= 0.0 || mul(ubo->proj, float4(0.0, 0.0, 2.0, 1.0)).w < 1.5) // the sky, or an orthographic camera
        return float4(0.0);

    Texture2D materialTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.materialTextureIndex, 0));
    const float materialRoughness = clamp(materialTex.Load(int3(pixelCoord, 0)).r, 0.0, 1.0);
    if (materialRoughness >= pushConstant.maxRoughness)
        return float4(0.0);

    const float2 uv = (float2(pixelCoord) + 0.5) / viewportSize;
    const float4 worldH = mul(pushConstant.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    const float3 worldPos = worldH.xyz / worldH.w;
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.normalTextureIndex, 0));
    const float3 rawNormal = normalTex.Load(int3(pixelCoord, 0)).rgb;
    const float lenN = length(rawNormal);
    if (lenN < 1e-6)
        return float4(0.0);
    const float3 pixelNormal = rawNormal / lenN;
    const float3 V = normalize(ubo->cameraWorldPos.xyz - worldPos);

    // Toksvig normal filtering (Toksvig 2004; UE's / Frostbite's roughness from normal variance, Godot 4's roughness limiter): the
    // reflection of a bumpy surface (a normal map's waves, cloth folds) is its average normal's, blurred by how much its normals
    // disagree. The 9 x 9 neighbours on this surface (every other pixel, view depth within 2 %): their mean normal is the ray's,
    // and its shortening (|mean| < 1) the variance added to the roughness: sigma^2 ~ 2 (1 - |mean|). The pixel's own mapped normal
    // made a mirror that swung with the slightest move of the camera on a wavy glossy awning; this one holds still and blurs as the
    // real reflection does. Flat glossy surfaces keep their mirror (their normals agree); curvature over 9 pixels adds little.
    const float nearPlaneZ = mul(ubo->proj, float4(0.0, 0.0, 1.0, 1.0)).z;
    const float viewZ = nearPlaneZ / depth;
    float3 normalSum = float3(0.0);
    float normalCount = 0.0;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            const int2 p = clamp(pixelCoord + int2(x, y) * 2, int2(0), int2(viewportSize) - 1);
            const float d = depthTex.Load(int3(p, 0)).r;
            if (d <= 0.0 || abs(nearPlaneZ / d - viewZ) > 0.02 * viewZ)
                continue;
            const float3 n = normalTex.Load(int3(p, 0)).rgb;
            const float len = length(n);
            if (len > 1e-6)
            {
                normalSum += n / len;
                normalCount += 1.0;
            }
        }
    }
    float3 N = pixelNormal;
    float roughness = materialRoughness;
    if (normalCount > 0.0 && dot(normalSum, normalSum) > 1e-8)
    {
        const float meanLength = saturate(length(normalSum) / normalCount);
        N = normalize(normalSum);
        roughness = sqrt(min(1.0, materialRoughness * materialRoughness + 2.0 * (1.0 - meanLength)));
    }
    if (roughness >= pushConstant.maxRoughness)
        return float4(0.0);

    // The ray: the mirror direction, the same every frame (Godot's SSR): roughness is the blur of what it finds (the cone's mip of the
    // lit scene, below), not random directions. A sampled lobe (stochastic SSR, UE4 / Frostbite / FidelityFX) needs a heavy denoiser
    // and still showed as grain; this has no noise to remove. The steps' offset is fixed per pixel: a still camera gives the same
    // reflection every frame.
    const float noise = frac(52.9829189 * frac(0.06711056 * float(pixelCoord.x) + 0.00583715 * float(pixelCoord.y)));
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
            const float tolerance = clamp(1.5 * stepZ, 0.02 * ubo->unitsPerMeter, pushConstant.thickness);
            if (behind > 0.0 && behind < tolerance)
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
    const float hitZ = max(mul(ubo->view, float4(hitWorld, 1.0)).z, nearPlane);
    const float focal = mul(ubo->proj, float4(0.0, 1.0, 0.0, 0.0)).y;
    // The roughness' cone where the ray lands: the blur that stands for the glossy lobe (Godot's cone of mips).
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

// --- filterMain ------------------------------------------------------------------------------------------------------------------
// Godot's SSR filter: the trace blurred by roughness on its own surface, this frame only. No temporal accumulation: the trace has no
// noise to average (one mirror ray, a fixed step offset), and reusing last frame's reflection, wherever it is reprojected to, makes it
// trail behind the camera.

[shader("fragment")]
float4 filterMain(MSOutput input, uniform PushConstantSSRFilter pushConstant) : SV_Target0
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    const int2 pixelCoord = int2(input.pos.xy);
    const float2 viewportSize = pushConstant.viewportSize;
    Texture2D rawTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.rawTextureIndex, 0));
    const float4 raw = rawTex.Load(int3(pixelCoord, 0));
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    const float depth = depthTex.Load(int3(pixelCoord, 0)).r;
    if (depth <= 0.0)
        return float4(0.0);
    Texture2D materialTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.materialTextureIndex, 0));
    const float roughness = saturate(materialTex.Load(int3(pixelCoord, 0)).r);
    if (roughness < 0.05)
        return raw; // a mirror: as traced
    const float nearPlane = mul(ubo->proj, float4(0.0, 0.0, 1.0, 1.0)).z;
    const float viewZ = nearPlane / depth;

    // 5 x 5 neighbours on this surface (view depth within 2 %), Gaussian, wider for a rougher surface (about a pixel at roughness
    // 0.05, the whole 5 x 5 from 0.5 on); premultiplied, so hits and misses average correctly.
    const float spread = lerp(0.6, 2.5, saturate(roughness / 0.5));
    float4 sum = float4(0.0);
    float weightSum = 0.0;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            const int2 p = clamp(pixelCoord + int2(x, y), int2(0), int2(viewportSize) - 1);
            const float d = depthTex.Load(int3(p, 0)).r;
            if (d <= 0.0)
                continue;
            const float w = saturate(1.0 - abs(nearPlane / d - viewZ) / (0.02 * viewZ)) * exp(-float(x * x + y * y) / (2.0 * spread * spread));
            sum += rawTex.Load(int3(p, 0)) * w;
            weightSum += w;
        }
    }
    return weightSum > 0.0 ? sum / weightSum : raw;
}
