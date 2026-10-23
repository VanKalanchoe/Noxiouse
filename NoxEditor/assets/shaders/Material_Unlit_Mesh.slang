 #include "shaderIO.h"
    #include "Math.slang"
    #include "HeightFog.slang"
    #include "TransparentCoverage.slang"

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

    struct FSOutputOIT
    {
        float4 accum : SV_TARGET0;
        float reveal : SV_TARGET1;
        int entityID : SV_TARGET2;
    };

    FSOutput shadeUnlit(VSOutput vertIn, PushConstantMeshlets pushConstant);

    [shader("fragment")]
    FSOutput fragMain(VSOutput vertIn, uniform PushConstantMeshlets pushConstant)
    {
        // Over DLSS-RR's output: blended by the share of the pixel it is in front of (the pass has no hardware depth test then).
        const float pixelCoverage = after_upscale_coverage((UniformBufferObject*)pushConstant.matrixReference, pushConstant, vertIn.pos);
        if (pixelCoverage <= 0.0)
            discard;
        FSOutput output = shadeUnlit(vertIn, pushConstant);
        output.color *= pixelCoverage;
        return output;
    }

    // Order-independent transparency (Renderer::setOITEnabled): a transparent unlit surface into the weighted blended targets.
    [shader("fragment")]
    FSOutputOIT fragMainOIT(VSOutput vertIn, uniform PushConstantMeshlets pushConstant)
    {
        const FSOutput shaded = shadeUnlit(vertIn, pushConstant);
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
        const OITOutput oit = oit_output(shaded.color, length(vertIn.worldPos - ubo->cameraWorldPos.xyz) / ubo->unitsPerMeter);
        FSOutputOIT output;
        output.accum = oit.accum;
        output.reveal = oit.reveal;
        output.entityID = shaded.entityID;
        return output;
    }

    FSOutput shadeUnlit(VSOutput vertIn, PushConstantMeshlets pushConstant)
    {
        FSOutput output;

        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
        SceneInstances instances = SceneInstances(ubo);
        InstanceData instance = instances[vertIn.drawID];

        SamplerState sampLinearHandle = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

        float4 baseColor;

        // 1. Fetch Base Color (matches Sascha material_unlit.frag lines 76-80)
        if (instance.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
            baseColor = baseColorTex.Sample(sampLinearHandle, instance.baseColorTextureSet == 0 ? vertIn.uv0 : vertIn.uv1) * instance.baseColorFactor;
        }
        else
        {
            baseColor = instance.baseColorFactor;
        }

        // The fog in front of it (aerial perspective, height fog). Blended (alpha mode 2: the transparent pass, premultiplied):
        // its color by its alpha, and the fog's scattered light only where it covers what is behind (already fogged).
        const float4 fog = sceneFogTerms(ubo, vertIn.worldPos);
        const float cover = instance.alphaMode == 2 ? baseColor.a : 1.0;
        output.color = float4((baseColor.rgb * fog.a + fog.rgb) * cover, baseColor.a);
        output.entityID = vertIn.entityID;
        return output;
    }