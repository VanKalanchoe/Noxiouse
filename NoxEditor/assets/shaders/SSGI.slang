#include "shaderIO.h"

// Screen-space global illumination (RS5b, Settings > Screen-Space GI; UE5's SSGI): per pixel `rayCount` rays, cosine-distributed
// around the normal (turned per pixel and frame), marched through this frame's depth buffer as SSR.slang marches its mirror ray; a ray
// that hits takes last frame's lit scene there (reprojected; the SSR history). Monte Carlo: with cosine-distributed rays the mean
// radiance is the irradiance / pi, so traceMain writes the sum of the hits' radiance / rayCount (rgb) and the share of rays that hit
// (a) -- the deferred lighting adds the frame's other ambient for the rest. denoiseMain averages 3x3 neighbours of the same surface
// and accumulates over frames (reprojected, clamped to the neighbourhood). Perspective cameras only.

struct SSGIPayload
{
    uint dummy;
};
groupshared SSGIPayload gPayload;

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

float2 ssgi_clip_to_uv(float4 clip)
{
    return float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
}

float3 ssgi_world_position(float4x4 invViewProj, float2 uv, float depth)
{
    const float4 h = mul(invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return h.xyz / h.w;
}

[shader("fragment")]
float4 traceMain(MSOutput input, uniform PushConstantSSGI pushConstant) : SV_Target0
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    const int2 pixelCoord = int2(input.pos.xy);
    const float2 viewportSize = pushConstant.viewportSize;
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    const float depth = depthTex.Load(int3(pixelCoord, 0)).r;
    if (depth <= 0.0 || mul(ubo->proj, float4(0.0, 0.0, 2.0, 1.0)).w < 1.5)
        return float4(0.0);

    const float2 uv = (float2(pixelCoord) + 0.5) / viewportSize;
    const float3 worldPos = ssgi_world_position(pushConstant.invViewProj, uv, depth);
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.normalTextureIndex, 0));
    const float3 rawNormal = normalTex.Load(int3(pixelCoord, 0)).rgb;
    if (dot(rawNormal, rawNormal) < 1e-8)
        return float4(0.0);
    const float3 N = normalize(rawNormal);
    float3 tangent = normalize(cross(abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0), N));
    const float3 bitangent = cross(N, tangent);

    const float nearPlane = mul(ubo->proj, float4(0.0, 0.0, 1.0, 1.0)).z;
    const float4x4 viewProj = mul(ubo->proj, ubo->view);
    const float3 start = worldPos + N * raster_surface_bias(ubo, worldPos, 0.01 * ubo->unitsPerMeter);
    Texture2D historyTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.historyTextureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));

    // Per pixel and frame: interleaved gradient noise and an R2 sequence for the rays' directions and the steps' offset.
    const float2 noisePixel = float2(pixelCoord) + float(pushConstant.frameIndex % 64u) * float2(5.588238, 3.45367);
    const float ign = frac(52.9829189 * frac(0.06711056 * noisePixel.x + 0.00583715 * noisePixel.y));
    const uint rays = clamp(pushConstant.rayCount, 1u, 8u);
    const uint steps = clamp(pushConstant.steps, 4u, 64u);
    float3 radianceSum = float3(0.0);
    float hits = 0.0;
    for (uint ray = 0; ray < rays; ++ray)
    {
        const float2 xi = frac(float2(ign, frac(ign * 1.6180339887 + 0.5)) + float(ray) * float2(0.7548776662, 0.5698402910));
        // Cosine-distributed around the normal.
        const float r = sqrt(xi.x);
        const float phi = 6.283185307 * xi.y;
        const float3 direction = normalize(tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) + N * sqrt(max(1.0 - xi.x, 0.0)));

        float rayLength = pushConstant.maxDistance;
        const float3 viewDirection = mul(ubo->view, float4(direction, 0.0)).xyz;
        const float startZ = mul(ubo->view, float4(start, 1.0)).z;
        if (viewDirection.z < 0.0)
            rayLength = min(rayLength, max(startZ - 2.0 * nearPlane, 0.0) / -viewDirection.z);
        if (rayLength <= 0.0)
            continue;
        const float4 clip0 = mul(viewProj, float4(start, 1.0));
        const float4 clip1 = mul(viewProj, float4(start + direction * rayLength, 1.0));
        const float2 uv0 = ssgi_clip_to_uv(clip0);
        const float2 uv1 = ssgi_clip_to_uv(clip1);
        const float invW0 = 1.0 / clip0.w;
        const float invW1 = 1.0 / clip1.w;
        float previousT = 0.0;
        for (uint i = 0; i < steps; ++i)
        {
            // Steps denser near the pixel (the light close by matters most).
            const float s = (float(i) + frac(ign + float(ray) * 0.618)) / float(steps);
            const float t = s * s;
            const float2 sampleUV = lerp(uv0, uv1, t);
            if (any(sampleUV <= 0.0) || any(sampleUV >= 1.0))
                break;
            const float rayZ = 1.0 / lerp(invW0, invW1, t);
            const int2 samplePixel = int2(sampleUV * viewportSize);
            const float sceneDepth = depthTex.Load(int3(samplePixel, 0)).r;
            if (sceneDepth > 0.0)
            {
                const float behind = rayZ - nearPlane / sceneDepth;
                const float stepZ = abs(rayZ - 1.0 / lerp(invW0, invW1, previousT));
                if (behind > 0.0 && behind < max(pushConstant.thickness, 1.5 * stepZ))
                {
                    // The back of a surface lights nothing towards the ray.
                    const float3 hitNormal = normalTex.Load(int3(samplePixel, 0)).rgb;
                    if (dot(hitNormal, direction) < 0.0)
                    {
                        const float3 hitWorld = ssgi_world_position(pushConstant.invViewProj, sampleUV, sceneDepth);
                        const float4 previousClip = mul(ubo->prevProj, mul(ubo->prevView, float4(hitWorld, 1.0)));
                        const float2 previousUV = ssgi_clip_to_uv(previousClip);
                        if (previousClip.w > 0.0 && all(previousUV > 0.0) && all(previousUV < 1.0))
                        {
                            radianceSum += max(historyTex.SampleLevel(linearClamp, previousUV, min(1.0, float(pushConstant.historyMipCount) - 1.0)).rgb, float3(0.0));
                            hits += 1.0;
                        }
                    }
                    break;
                }
            }
            previousT = t;
        }
    }
    return float4(radianceSum / float(rays), hits / float(rays));
}

struct SSGIDenoiseOutput
{
    float4 accumulated : SV_Target0;
    float2 depthHistory : SV_Target1; // view depth, frames accumulated (next frame's validation)
};

[shader("fragment")]
SSGIDenoiseOutput denoiseMain(MSOutput input, uniform PushConstantSSGIDenoise pushConstant)
{
    SSGIDenoiseOutput output;
    output.accumulated = float4(0.0);
    output.depthHistory = float2(0.0);
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    const int2 pixelCoord = int2(input.pos.xy);
    const float2 viewportSize = pushConstant.viewportSize;
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    const float depth = depthTex.Load(int3(pixelCoord, 0)).r;
    if (depth <= 0.0)
        return output;
    Texture2D rawTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.rawTextureIndex, 0));
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.normalTextureIndex, 0));
    const float3 normal = normalize(normalTex.Load(int3(pixelCoord, 0)).rgb + float3(0.0, 1e-6, 0.0));
    const float nearPlane = mul(ubo->proj, float4(0.0, 0.0, 1.0, 1.0)).z;
    const float viewZ = nearPlane / depth;

    // 5x5 neighbours on the same surface (view depth within 2%, normals alike), Gaussian falloff.
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
            const float zWeight = saturate(1.0 - abs(nearPlane / d - viewZ) / (0.02 * viewZ));
            const float nWeight = pow(saturate(dot(normalize(normalTex.Load(int3(p, 0)).rgb + float3(0.0, 1e-6, 0.0)), normal)), 8.0);
            const float w = zWeight * nWeight * exp(-0.25 * float(x * x + y * y));
            sum += rawTex.Load(int3(p, 0)) * w;
            weightSum += w;
        }
    }
    const float4 current = weightSum > 0.0 ? sum / weightSum : rawTex.Load(int3(pixelCoord, 0));

    // Where this surface was last frame (the previous camera), kept only where last frame saw the same depth there (no history from
    // what was in front or behind: disocclusions start over).
    float frames = 1.0;
    float4 result = current;
    if (pushConstant.historyTextureIndex != 0xFFFFFFFF && pushConstant.historyDepthTextureIndex != 0xFFFFFFFF)
    {
        const float2 uv = (float2(pixelCoord) + 0.5) / viewportSize;
        const float4 worldH = mul(pushConstant.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
        const float3 worldPos = worldH.xyz / worldH.w;
        const float4 previousClip = mul(ubo->prevProj, mul(ubo->prevView, float4(worldPos, 1.0)));
        const float2 previousUV = float2(previousClip.x / previousClip.w * 0.5 + 0.5, 0.5 - previousClip.y / previousClip.w * 0.5);
        if (previousClip.w > 0.0 && all(previousUV > 0.0) && all(previousUV < 1.0))
        {
            Texture2D<float2> historyDepth = Texture2D<float2>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.historyDepthTextureIndex, 0));
            const float2 previous = historyDepth.Load(int3(int2(previousUV * viewportSize), 0));
            const float expectedZ = mul(ubo->prevView, float4(worldPos, 1.0)).z;
            if (previous.x > 0.0 && abs(previous.x - expectedZ) < 0.05 * expectedZ)
            {
                Texture2D historyTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.historyTextureIndex, 0));
                SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
                frames = min(previous.y + 1.0, max(pushConstant.maxHistory, 1.0));
                result = lerp(historyTex.SampleLevel(linearClamp, previousUV, 0.0), current, 1.0 / frames);
            }
        }
    }
    output.accumulated = result;
    output.depthHistory = float2(viewZ, frames);
    return output;
}
