 #include "shaderIO.h"

    struct VisDebugPayload
    {
        uint dummy;
    };
    groupshared VisDebugPayload gPayload;

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
        uniform PushConstantVisibilityDebug pushConstant
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

    float3 hashToColor(uint2 id)
    {
        uint h = (id.x + 1u) * 0x85ebca6bu ^ (id.y * 0xc2b2ae35u);
        h ^= h >> 16;
        h *= 0x85ebca6bu;
        h ^= h >> 13;

        return float3(
            float(h & 0xFF) / 255.0,
            float((h >> 8) & 0xFF) / 255.0,
            float((h >> 16) & 0xFF) / 255.0
        );
    }

    [shader("fragment")]
    float4 fragMain(
        MSOutput input,
        uniform PushConstantVisibilityDebug pushConstant
    ) : SV_Target0
    {
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

        Texture2D<uint4> visTex = Texture2D<uint4>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.visibilityTextureIndex, 0));
        int2 pixelCoord = int2(input.pos.xy);
        uint4 rawData = visTex.Load(int3(pixelCoord, 0));
        uint2 visData = rawData.xy;

        if (visData.x == 0)
        {
            return float4(0.12, 0.12, 0.18, 1.0);
        }

        // Mode 6: Step 1 Colored Meshlets
        if (pushConstant.debugMode == 6)
        {
            return float4(hashToColor(visData), 1.0);
        }

        // Load G-Buffer targets from heap
        Texture2D albedoTex   = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferAlbedoIndex, 0));
        Texture2D normalTex   = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferNormalIndex, 0));
        Texture2D materialTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferMaterialIndex, 0));
        Texture2D emissionTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferEmissionIndex, 0));

        float4 albedoSample   = albedoTex.Load(int3(pixelCoord, 0));
        float4 normalSample   = normalTex.Load(int3(pixelCoord, 0));
        float4 materialSample = materialTex.Load(int3(pixelCoord, 0));
        float4 emissionSample = emissionTex.Load(int3(pixelCoord, 0));

        // Mode 0: G-Buffer BaseColor (Albedo RGB)
            if (pushConstant.debugMode == 0)
            {
                return float4(albedoSample.rgb, 1.0);
            }

             // Mode 1: G-Buffer Perturbed World Normal (Normal map applied) mapped to [0, 1]
            if (pushConstant.debugMode == 1)
            {
                return float4(normalSample.rgb * 0.5 + 0.5, 1.0);
            }

            // Mode 2: G-Buffer Roughness (Grayscale, Linear display matching Sascha Willems)
            if (pushConstant.debugMode == 2)
            {
                float r = materialSample.r;
                return float4(pow(r, 2.2), pow(r, 2.2), pow(r, 2.2), 1.0);
            }

            // Mode 3: G-Buffer Metallic (Grayscale, Linear display matching Sascha Willems)
            if (pushConstant.debugMode == 3)
            {
                float m = materialSample.g;
                return float4(pow(m, 2.2), pow(m, 2.2), pow(m, 2.2), 1.0);
            }

            // Mode 4: G-Buffer Emission (HDR)
            if (pushConstant.debugMode == 4)
            {
                return float4(emissionSample.rgb, 1.0);
            }

            // Mode 5: G-Buffer Ambient Occlusion (Grayscale, Linear display matching Sascha Willems)
            float ao = albedoSample.a;
            return float4(pow(ao, 2.2), pow(ao, 2.2), pow(ao, 2.2), 1.0);
    }