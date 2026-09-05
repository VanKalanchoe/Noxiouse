#include "shaderIO.h"

struct SkyboxPayload
{
    uint dummy;
};
groupshared SkyboxPayload gPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gPayload);
}

struct MSOutput
{
    float4 pos : SV_Position;
    float3 localPos : TEXCOORD0;
};

// 8 vertices of a unit cube
static const float3 cubePositions[8] =
{
    float3(-1.0, -1.0, -1.0), // 0: Front-Bottom-Left
    float3( 1.0, -1.0, -1.0), // 1: Front-Bottom-Right
    float3( 1.0,  1.0, -1.0), // 2: Front-Top-Right
    float3(-1.0,  1.0, -1.0), // 3: Front-Top-Left
    float3(-1.0, -1.0,  1.0), // 4: Back-Bottom-Left
    float3( 1.0, -1.0,  1.0), // 5: Back-Bottom-Right
    float3( 1.0,  1.0,  1.0), // 6: Back-Top-Right
    float3(-1.0,  1.0,  1.0)  // 7: Back-Top-Left
};

// 12 triangles forming the 6 cube faces (Inward-facing CCW winding)
static const uint3 cubeIndices[12] =
{
    // Front (-Z)
    uint3(1, 0, 3), uint3(1, 3, 2),
    // Back (+Z)
    uint3(4, 5, 6), uint3(4, 6, 7),
    // Left (-X)
    uint3(0, 4, 7), uint3(0, 7, 3),
    // Right (+X)
    uint3(5, 1, 2), uint3(5, 2, 6),
    // Bottom (-Y)
    uint3(4, 0, 1), uint3(4, 1, 5),
    // Top (+Y)
    uint3(7, 6, 2), uint3(7, 2, 3)
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(8, 1, 1)]
void meshMain
(
    out indices uint3 triangles[12], 
    out vertices MSOutput vertices[8],
    uint threadID : SV_GroupIndex,
    uniform PushConstantSkybox pushConstant
)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

    SetMeshOutputCounts(8, 12);

    if (threadID < 8)
    {
        float3 pos = cubePositions[threadID];
        vertices[threadID].localPos = pos;

        // Strip translation from view matrix to keep skybox centered at camera
        float3x3 rotView = (float3x3)ubo->view;
        float4 clipPos = mul(ubo->proj, float4(mul(rotView, pos), 1.0));

        // REVERSE-Z: Force depth to 0.0 (Far Plane)
        vertices[threadID].pos = float4(clipPos.xy, 0.0, clipPos.w);
    }

    if (threadID == 0)
    {
        for (uint i = 0; i < 12; i++)
        {
            triangles[i] = cubeIndices[i];
        }
    }
}

import Tonemapping;

struct FSOutput
{
    float4 color : SV_TARGET0;
    int entityID : SV_TARGET1;
};

[shader("fragment")]
FSOutput fragMain(MSOutput input, uniform PushConstantSkybox pushConstant)
{
    FSOutput output;

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

    TextureCube texHandle = TextureCube.Handle(uint2(pushConstant.cubemapIndex, 0));
    SamplerState sampHandle = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));

    // Sample the environment cubemap using interpolating 3D direction
    float3 envColor = texHandle.Sample(sampHandle, input.localPos).rgb;

    TonemapParameters tonemapParams;
    tonemapParams.exposure = ubo->exposure;
    tonemapParams.gamma = ubo->gamma;
    tonemapParams.mode = (int)TonemapMode::None;

    float3 color = toneMap(envColor, tonemapParams);

    output.color = float4(color, 1.0);
    output.entityID = -1;

    return output;
}