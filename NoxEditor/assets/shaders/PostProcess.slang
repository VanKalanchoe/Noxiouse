import Tonemapping;
    #include "shaderIO.h"

    struct PostProcessPayload
    {
        uint dummy;
    };
    groupshared PostProcessPayload gPayload;

    [shader("task")]
    [numthreads(1, 1, 1)]
    void taskMain()
    {
        DispatchMesh(1, 1, 1, gPayload);
    }

    struct MSOutput
    {
        float4 pos : SV_Position;
        float2 texCoord : TEXCOORD0;
    };

    static const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    static const float2 uvs[3] =
    {
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
        uint threadID : SV_GroupIndex
    )
    {
        SetMeshOutputCounts(3, 1);

        if (threadID < 3)
        {
            vertices[threadID].pos = float4(positions[threadID], 0.0, 1.0);
            vertices[threadID].texCoord = uvs[threadID];
        }

        if (threadID == 0)
        {
            triangles[0] = uint3(0, 1, 2);
        }
    }

    [shader("fragment")]
    float4 fragMain(
        MSOutput input,
        uniform PushConstantPostProcess pushConstant
    ) : SV_Target
    {
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
        int2 pixelCoord = int2(input.pos.xy);

        Texture2D hdrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.hdrTextureIndex, 0));
        float4 hdrColor = hdrTex.Load(int3(pixelCoord, 0));

        // When inspecting debug views (Albedo, Normals, Roughness, EntityID, DepthBuffer), pass through directly!
        if (pushConstant.debugMode != 0)
        {
            return hdrColor;
        }

        // Mode 0 (Full Lit PBR): Apply Tonemapping (Exposure + Khronos PBR Neutral + Gamma)
        TonemapParameters tonemapParams;
        tonemapParams.exposure = ubo->exposure;
        tonemapParams.gamma = ubo->gamma;
        tonemapParams.mode = (int)pushConstant.tonemapMode;;

        float3 ldrColor = toneMap(hdrColor.rgb, tonemapParams);
        return float4(ldrColor, 1.0);
    }