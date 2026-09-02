 #include "shaderIO.h"
    #include "Math.slang"

    // Matches the exact vertex output from meshMain in Meshlet.slang
    struct VSOutput
    {
        float4 pos : SV_Position;
        float3 worldPos;
        float3 normal;
        float2 uv0;
        float2 uv1;
        float4 color;
        nointerpolation uint drawID;
        nointerpolation int entityID;
    };

    struct FSOutput
    {
        float4 color : SV_TARGET0;
        int entityID : SV_TARGET1;
    };

    [shader("fragment")]
    FSOutput fragMain(VSOutput vertIn, uniform PushConstantMeshlets pushConstant)
    {
        FSOutput output;

        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
        InstanceData* instances = (InstanceData*)pushConstant.instanceReference;
        InstanceData instance = instances[vertIn.drawID];

        SamplerState sampLinearHandle = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

        float4 baseColor;

        // 1. Fetch Base Color (matches Sascha material_unlit.frag lines 76-80)
        if (instance.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
            baseColor = SRGBtoLINEAR(baseColorTex.Sample(sampLinearHandle, instance.baseColorTextureSet == 0 ? vertIn.uv0 : vertIn.uv1)) * instance.baseColorFactor;
        }
        else
        {
            baseColor = instance.baseColorFactor;
        }

        output.color = baseColor;
        output.entityID = vertIn.entityID;
        return output;
    }