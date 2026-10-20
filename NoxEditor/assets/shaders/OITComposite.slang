#include "shaderIO.h"

// Weighted blended order-independent transparency, the resolve (McGuire and Bavoil 2013): the weighted average of the transparent
// fragments of a pixel over what is behind them, by how much of it they leave (the revealage). Blended into the HDR scene with
// One / SrcAlpha: color = average * (1 - revealage) + scene * revealage (Renderer::addOITCompositePass).

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
float4 fragMain(VSOutput input, uniform PushConstantOITComposite pushConstant) : SV_Target0
{
    const int3 pixel = int3(int2(input.pos.xy), 0);
    Texture2D<float> revealTex = Texture2D<float>.Handle(uint2(pushConstant.revealIndex, 0));
    const float revealage = revealTex.Load(pixel);
    if (revealage >= 1.0 - 1e-4)
        discard; // nothing transparent here

    Texture2D<float4> accumTex = Texture2D<float4>.Handle(uint2(pushConstant.accumIndex, 0));
    const float4 accum = accumTex.Load(pixel);
    float3 average = accum.rgb / max(accum.a, 1e-5);
    if (any(isnan(average)) || any(isinf(average)))
        average = float3(0.0);
    return float4(average * (1.0 - revealage), revealage);
}
