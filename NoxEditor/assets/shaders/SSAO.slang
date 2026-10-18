#include "shaderIO.h"

// Raster-only ambient occlusion: a hemisphere-kernel SSAO (the same technique Godot's "SSAO" and UE5's legacy SSAO
// use), reprojected in world space rather than the usual view space, so it reuses this codebase's existing
// depth-to-world-position convention (reverse-Z, unproject through invViewProj) instead of introducing a second one.
// One fullscreen pass computes raw, noisy AO here; SSAOBlur.slang denoises it before DeferredLighting.slang applies it.

struct SSAOPayload
{
    uint dummy;
};
groupshared SSAOPayload gPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gPayload);
}

struct MSOutput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float2 positions[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

static const float2 uvs[3] = {
    float2(0.0, 0.0),
    float2(2.0, 0.0),
    float2(0.0, 2.0)
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(3, 1, 1)]
void meshMain(
    out indices uint3 triangles[1],
    out vertices MSOutput vertices[3],
    uint threadID : SV_GroupIndex,
    uniform PushConstantSSAO pushConstant
)
{
    SetMeshOutputCounts(3, 1);

    if (threadID < 3)
    {
        vertices[threadID].pos = float4(positions[threadID], 0.0, 1.0);
        vertices[threadID].uv = uvs[threadID];
    }

    if (threadID == 0)
    {
        triangles[0] = uint3(0, 1, 2);
    }
}

struct SSAOOutput
{
    float ao : SV_Target0;
};

// Cheap per-pixel hash instead of a tiled noise texture: rotates the kernel by a different angle every pixel so the
// (few) samples per pixel don't bias in the same direction across the whole screen -- the blur pass then removes
// the resulting high-frequency dither.
float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

[shader("fragment")]
SSAOOutput fragMain(MSOutput input, uniform PushConstantSSAO pushConstant)
{
    SSAOOutput output;
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    int2 pixelCoord = int2(input.pos.xy);

    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    float depth = depthTex.Load(int3(pixelCoord, 0)).r;
    if (depth <= 0.0) // reverse-Z background
    {
        output.ao = 1.0;
        return output;
    }

    float2 uv = (float2(pixelCoord) + 0.5) / pushConstant.viewportSize;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 worldPosH = mul(pushConstant.invViewProj, float4(ndc, depth, 1.0));
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.normalTextureIndex, 0));
    float3 rawNormal = normalTex.Load(int3(pixelCoord, 0)).rgb;
    float lenN = length(rawNormal);
    float3 N = lenN > 1e-6 ? (rawNormal / lenN) : float3(0.0, 1.0, 0.0);

    // Random rotation of the kernel around N (see hash12 above), built the same way GBufferMaterial.slang builds a
    // tangent frame from an arbitrary "up" reference (there is no authored tangent to reuse here -- any rotation
    // around N is equally valid for a sampling pattern that has no notion of surface UV).
    float angle = hash12(float2(pixelCoord)) * 6.28318530718;
    float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 tangent0 = normalize(cross(up, N));
    float3 bitangent0 = cross(N, tangent0);
    float3 tangent = tangent0 * cos(angle) + bitangent0 * sin(angle);
    float3 bitangent = -tangent0 * sin(angle) + bitangent0 * cos(angle);
    float3x3 TBN = float3x3(tangent, bitangent, N);

    float4* kernel = (float4*)pushConstant.kernelReference;
    float3 cameraWorldPos = ubo->cameraWorldPos.xyz;
    float occlusion = 0.0;

    for (uint i = 0; i < pushConstant.kernelSize; i++)
    {
        float3 sampleDir = mul(kernel[i].xyz, TBN); // tangent-space hemisphere sample -> world space
        float3 samplePosWorld = worldPos + sampleDir * pushConstant.radius;

        float4 sampleView = mul(pushConstant.view, float4(samplePosWorld, 1.0));
        float4 sampleClip = mul(pushConstant.proj, sampleView);
        if (sampleClip.w <= 0.0)
            continue;
        float2 sampleNDC = sampleClip.xy / sampleClip.w;
        float2 sampleUV = float2(sampleNDC.x * 0.5 + 0.5, 0.5 - sampleNDC.y * 0.5);
        if (any(sampleUV < float2(0.0, 0.0)) || any(sampleUV > float2(1.0, 1.0)))
            continue;

        int2 sampleCoord = int2(sampleUV * pushConstant.viewportSize);
        float sceneDepth = depthTex.Load(int3(sampleCoord, 0)).r;
        if (sceneDepth <= 0.0)
            continue;

        float4 sceneWorldH = mul(pushConstant.invViewProj, float4(sampleNDC, sceneDepth, 1.0));
        float3 sceneWorldPos = sceneWorldH.xyz / sceneWorldH.w;

        // Distance from the camera (not view-space Z) so this needs no assumption about the view basis' handedness
        // or which axis is "forward" -- just whichever of the two points is actually closer to the eye.
        float sampleDist = length(samplePosWorld - cameraWorldPos);
        float sceneDist = length(sceneWorldPos - cameraWorldPos);

        if (sceneDist < sampleDist - pushConstant.bias)
        {
            // Fades out occlusion from geometry far behind the sample point (a wall on the other side of a street,
            // still technically "closer to the camera" at a glancing angle, should not occlude a nearby surface).
            float rangeCheck = saturate(pushConstant.radius / max(abs(sampleDist - sceneDist), 1e-4));
            occlusion += rangeCheck;
        }
    }

    float ao = 1.0 - (occlusion / float(pushConstant.kernelSize));
    output.ao = pow(saturate(ao), pushConstant.power);
    return output;
}
