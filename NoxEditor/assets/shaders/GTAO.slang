#include "shaderIO.h"

// Ground-truth ambient occlusion (RS3, Settings > Ambient Occlusion: GTAO): Jimenez, Wu, Pesce, Jarabo, "Practical Realtime
// Strategies for Accurate Indirect Occlusion" (2016), the formulation of Intel's XeGTAO -- what UE5 and Godot ship as their
// screen-space AO. Per pixel, sliceCount planes through the view direction (turned per pixel and frame); in each, the highest
// horizon on both sides is searched in the depth buffer within the AO radius (fading out over its last 40%), and the arc between
// the two horizons is integrated cosine-weighted around the normal projected into the slice. View space throughout (the camera's
// +z forward, +y up; the screen's y down). Writes the raw AO SSAOBlur.slang denoises, like SSAO.slang / RTAO.slang.

struct GTAOPayload
{
    uint dummy;
};
groupshared GTAOPayload gPayload;

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

static const float PI = 3.14159265358979;
static const float HALF_PI = 1.57079632679;

// A pixel's view-space position from the depth buffer (0: the sky).
float3 gtao_view_position(UniformBufferObject* ubo, float4x4 invViewProj, float2 uv, float depth)
{
    const float4 worldH = mul(invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return mul(ubo->view, float4(worldH.xyz / worldH.w, 1.0)).xyz;
}

// acos with the argument kept in range (the horizons' cosines come from normalized vectors, a hair past 1 in float).
float gtao_acos(float x)
{
    return acos(clamp(x, -1.0, 1.0));
}

[shader("fragment")]
float fragMain(MSOutput input, uniform PushConstantGTAO pushConstant) : SV_Target0
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    const int2 pixelCoord = int2(input.pos.xy);
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    const float depth = depthTex.Load(int3(pixelCoord, 0)).r;
    if (depth <= 0.0) // reverse-Z background
        return 1.0;

    const float2 viewportSize = pushConstant.viewportSize;
    const float2 uv = (float2(pixelCoord) + 0.5) / viewportSize;
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float4 worldH = mul(pushConstant.invViewProj, float4(ndc, depth, 1.0));
    const float3 worldPos = worldH.xyz / worldH.w;
    const float3 P = mul(ubo->view, float4(worldPos, 1.0)).xyz;

    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.normalTextureIndex, 0));
    const float3 rawNormal = normalTex.Load(int3(pixelCoord, 0)).rgb;
    const float lenN = length(rawNormal);
    const float3 N = normalize(mul(ubo->view, float4(lenN > 1e-6 ? rawNormal / lenN : float3(0.0, 1.0, 0.0), 0.0)).xyz);
    // Towards the eye: from the point in a perspective view, along the view axis in an orthographic one.
    const bool perspective = mul(ubo->proj, float4(0.0, 0.0, 2.0, 1.0)).w > 1.5;
    const float3 V = perspective ? normalize(-P) : float3(0.0, 0.0, -1.0);

    // The radius in world units and in pixels (one pixel's width at the surface).
    const float radius = ambient_occlusion_radius(pushConstant.invViewProj, ndc, depth, worldPos, viewportSize, pushConstant.radius,
                                                  pushConstant.shortRangePixels);
    const float4 neighborH = mul(pushConstant.invViewProj, float4(ndc + float2(2.0 / viewportSize.x, 0.0), depth, 1.0));
    const float pixelWidth = max(length(neighborH.xyz / neighborH.w - worldPos), 1e-6);
    const float radiusPixels = min(radius / pixelWidth, 0.25 * viewportSize.y);
    if (radiusPixels < 1.0)
        return 1.0;
    const float falloffStart = 0.6 * radius;

    // Interleaved gradient noise per pixel and frame: the slices' rotation and the steps' offset.
    const float2 noisePixel = float2(pixelCoord) + float(pushConstant.frameIndex % 64u) * float2(5.588238, 3.45367);
    const float noiseSlice = frac(52.9829189 * frac(0.06711056 * noisePixel.x + 0.00583715 * noisePixel.y));
    const float noiseStep = frac(noiseSlice * 1.6180339887 + 0.3183098862);

    const uint sliceCount = clamp(pushConstant.sliceCount, 1u, 8u);
    const uint steps = clamp(pushConstant.stepsPerSide, 1u, 16u);
    float visibility = 0.0;
    for (uint slice = 0; slice < sliceCount; ++slice)
    {
        const float phi = (float(slice) + noiseSlice) / float(sliceCount) * PI;
        const float2 omega = float2(cos(phi), sin(phi)); // screen direction (y down)
        // The slice plane: the view direction and the screen direction's view-space counterpart (y flipped).
        const float3 direction = float3(omega.x, -omega.y, 0.0);
        const float3 tangent = normalize(direction - dot(direction, V) * V);
        const float3 axis = cross(tangent, V);
        const float3 projectedNormal = N - axis * dot(N, axis);
        const float projectedLength = length(projectedNormal);
        if (projectedLength < 1e-4)
            continue;
        const float cosN = saturate(dot(projectedNormal, V) / projectedLength);
        const float n = (dot(projectedNormal, tangent) >= 0.0 ? 1.0 : -1.0) * gtao_acos(cosN); // the normal's angle from V towards +tangent

        // The highest horizon on each side (+tangent along +omega), starting at the surface's own tangent plane.
        float horizonCos0 = cos(n + HALF_PI);
        float horizonCos1 = cos(n - HALF_PI);
        for (uint step = 0; step < steps; ++step)
        {
            // Samples spread towards the radius (squared: more of them near the pixel), at least a pixel apart.
            const float s = (float(step) + noiseStep) / float(steps);
            const float offsetPixels = max(s * s * radiusPixels, float(step) + 1.0);
            const float2 offset = omega * offsetPixels / viewportSize;
            [unroll]
            for (uint side = 0; side < 2; ++side)
            {
                const float2 sampleUV = side == 0 ? uv + offset : uv - offset;
                if (any(sampleUV <= 0.0) || any(sampleUV >= 1.0))
                    continue;
                const float sampleDepth = depthTex.Load(int3(int2(sampleUV * viewportSize), 0)).r;
                if (sampleDepth <= 0.0)
                    continue;
                const float3 delta = gtao_view_position(ubo, pushConstant.invViewProj, sampleUV, sampleDepth) - P;
                const float distance = length(delta);
                if (distance < 1e-5)
                    continue;
                // Fades from the full horizon to none over the radius' last 40% (no occlusion from far behind).
                const float weight = saturate((radius - distance) / max(radius - falloffStart, 1e-5));
                const float sampleCos = dot(delta / distance, V);
                if (side == 0)
                    horizonCos0 = max(horizonCos0, lerp(cos(n + HALF_PI), sampleCos, weight));
                else
                    horizonCos1 = max(horizonCos1, lerp(cos(n - HALF_PI), sampleCos, weight));
            }
        }

        // The visible arc between the horizons (clamped to the hemisphere around the normal), cosine-weighted.
        const float h0 = n + max(-gtao_acos(horizonCos1) - n, -HALF_PI);
        const float h1 = n + min(gtao_acos(horizonCos0) - n, HALF_PI);
        const float sinN = sin(n);
        const float arc0 = (cosN + 2.0 * h0 * sinN - cos(2.0 * h0 - n)) * 0.25;
        const float arc1 = (cosN + 2.0 * h1 * sinN - cos(2.0 * h1 - n)) * 0.25;
        visibility += projectedLength * (arc0 + arc1);
    }
    const float ao = saturate(visibility / float(sliceCount));
    return pow(ao, pushConstant.power);
}
