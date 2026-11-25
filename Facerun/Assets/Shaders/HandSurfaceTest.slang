#include "CustomLit.slang"
#include "MaterialGraph.slang"

void surface(MaterialGraphInputs i, inout MaterialGraphOutputs o)
{
    float bands = 0.5 + 0.5 * sin(i.position.y * 0.15 + i.time * 3.0);
    o.baseColor = lerp(float3(0.05, 0.15, 0.45), float3(0.1, 0.95, 0.85), bands);
    o.metallic = 0.8;
    o.roughness = lerp(0.1, 0.5, bands);
    o.emissive = float3(0.0, 0.6, 0.8) * smoothstep(0.85, 1.0, bands) * 2.0;
}