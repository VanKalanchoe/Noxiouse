#include "shaderIO.h"
#include "NRDFrontEnd.slang"

// NRD's guides for every denoiser that runs this frame (shadows, reflections, GI, direct lighting, path tracing): linear
// view Z and the packed normal + roughness (NRD_FrontEnd_PackNormalAndRoughness), from the depth buffer and the G-buffer.

struct GuidesPayload
{
    uint dummy;
};
groupshared GuidesPayload gPayload;

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

struct GuidesOutput
{
    float viewZ : SV_Target0;              // linear view depth (positive)
    float4 normalRoughness : SV_Target1;   // R10G10B10A2 (NRD_NORMAL_ENCODING 2, linear roughness)
};

[shader("fragment")]
GuidesOutput fragMain(MSOutput input, uniform PushConstantNRDGuides pushConstant)
{
    GuidesOutput output;
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    int2 pixel = int2(input.pos.xy);

    // Reverse-Z: nothing drawn at 0 -- infinitely far, facing up, fully rough.
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    float depth = depthTex.Load(int3(pixel, 0));
    if (depth <= 0.0)
    {
        output.viewZ = NRD_FP16_MAX;
        output.normalRoughness = NRD_FrontEnd_PackNormalAndRoughness(float3(0.0, 1.0, 0.0), 1.0, 0.0);
        return output;
    }

    float2 uv = (float2(pixel) + 0.5) / pushConstant.viewportSize;
    float4 worldPosH = mul(pushConstant.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    float3 worldPos = worldPosH.xyz / worldPosH.w;
    output.viewZ = abs(mul(ubo->view, float4(worldPos, 1.0)).z);

    // The G-buffer normal's alpha is the roughness the lighting uses.
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferNormalIndex, 0));
    float4 normalSample = normalTex.Load(int3(pixel, 0));
    float lenSqN = dot(normalSample.rgb, normalSample.rgb);
    float3 normal = lenSqN > 1e-6 ? normalSample.rgb * rsqrt(lenSqN) : float3(0.0, 1.0, 0.0);
    output.normalRoughness = NRD_FrontEnd_PackNormalAndRoughness(normal, saturate(normalSample.a), 0.0);
    return output;
}
