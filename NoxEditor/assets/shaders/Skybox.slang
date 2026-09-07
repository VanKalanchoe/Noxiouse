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
        float3 worldRay : TEXCOORD0;
    };

    // Fullscreen triangle covering the screen in NDC
    static const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    [shader("mesh")]
    [outputtopology("triangle")]
    [numthreads(3, 1, 1)]
    void meshMain(
        out indices uint3 triangles[1],
        out vertices MSOutput vertices[3],
        uint threadID : SV_GroupIndex,
        uniform PushConstantSkybox pushConstant
    )
    {
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

        SetMeshOutputCounts(3, 1);

        if (threadID < 3)
        {
            float2 screenPos = positions[threadID];

            // REVERSE-Z: Output depth at 0.0 (Far Plane), w = 1.0 (never clipped!)
            vertices[threadID].pos = float4(screenPos, 0.0, 1.0);

            // Unproject near-plane point (depth = 1.0 in Reverse-Z) to world space
            float4 unprojected = mul(ubo->invViewProj, float4(screenPos, 1.0, 1.0));
            vertices[threadID].worldRay = unprojected.xyz / unprojected.w - ubo->cameraWorldPos.xyz;
        }

        if (threadID == 0)
        {
            triangles[0] = uint3(0, 1, 2);
        }
    }

    struct FSOutput
    {
        float4 color : SV_TARGET0;
        int entityID : SV_TARGET1;
    };

    [shader("fragment")]
    FSOutput fragMain(MSOutput input, uniform PushConstantSkybox pushConstant)
    {
        FSOutput output;

        TextureCube texHandle = TextureCube.Handle(uint2(pushConstant.cubemapIndex, 0));
        SamplerState sampHandle = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));

        // Sample the environment cubemap using interpolated 3D world direction
        float3 envColor = texHandle.Sample(sampHandle, normalize(input.worldRay)).rgb;

        output.color = float4(envColor, 1.0);
        output.entityID = -1;

        return output;
    }