#include "shaderIO.h"

// Transparent surfaces drawn after DLSS Ray Reconstruction (Renderer::addTransparentsAfterRRPasses): their layer (render
// resolution, premultiplied color, alpha = how much they cover) over the upscaled image. Blended with One / SrcAlpha:
// color = layer + output * (1 - coverage). The layer was drawn with the camera's jitter: read where the unjittered image's
// pixel is in it, so the glass holds still on the upscaled image.

struct VSOutput
{
    float4 pos : SV_Position;
};

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID)
{
    static const float2 positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    VSOutput output;
    output.pos = float4(positions[vertexID], 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input, uniform PushConstantTransparencyComposite pushConstant) : SV_Target0
{
    Texture2D<float4> layer = Texture2D<float4>.Handle(uint2(pushConstant.layerIndex, 0));
    const float2 uv = input.pos.xy * pushConstant.inverseOutputSize + pushConstant.jitterUv;
    const float4 transparent = layer.SampleLevel(SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0)), uv, 0.0);
    if (transparent.a <= 0.0 && all(transparent.rgb <= 0.0))
        discard; // nothing transparent here
    if (any(isnan(transparent)) || any(isinf(transparent)))
        discard;
    return float4(transparent.rgb, 1.0 - saturate(transparent.a));
}
