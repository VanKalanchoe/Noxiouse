#include "shaderIO.h"

// Order-independent transparency, the resolve (Renderer::addOITCompositePass), in two draws over the HDR scene:
//   OIT_COMPOSITE_MULTIPLY: the scene times what the layers let through of it, per color channel (tinted glass tints it): blended
//                           Zero / SrcColor.
//   OIT_COMPOSITE_ADD:      the layers' light, the weighted sum normalized by the exact total coverage (1 - the mono revealage):
//                           blended One / One.
// Weighted blended OIT (McGuire and Bavoil 2013) and AVBOIT (Drobot 2025) resolve alike; they differ in the weights (OITShared.slang).

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
    Texture2D<float4> revealTex = Texture2D<float4>.Handle(uint2(pushConstant.revealIndex, 0));
    const float4 revealage = revealTex.Load(pixel);
    if (all(revealage >= float4(1.0 - 1e-4)))
        discard; // nothing transparent here

    if (pushConstant.mode == OIT_COMPOSITE_MULTIPLY)
        return float4(revealage.rgb, 1.0);

    Texture2D<float4> accumTex = Texture2D<float4>.Handle(uint2(pushConstant.accumIndex, 0));
    const float4 accum = accumTex.Load(pixel);
    float3 color = accum.rgb / max(accum.a, 1e-5) * (1.0 - revealage.a);
    if (any(isnan(color)) || any(isinf(color)))
        color = float3(0.0);
    return float4(color, 0.0);
}
