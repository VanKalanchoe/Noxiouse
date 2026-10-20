#include "shaderIO.h"

// The editor's wireframe overlay (Renderer::setWireframeOverlay): the triangles' edges darkened into the lit image, from the
// meshes' own vertex output (Material_PBR_Mesh.slang meshMain) rasterized as lines.
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
    int entityID : SV_TARGET1; // not written (the write mask is off): picking keeps the surfaces
};

[shader("fragment")]
FSOutput fragMain(VSOutput vertIn, uniform PushConstantMeshlets pushConstant)
{
    FSOutput output;
    // Multiplied into the image (blend Zero / SrcColor): dark blue edges at any exposure, without knowing how bright it is.
    output.color = float4(0.12, 0.16, 0.28, 1.0);
    output.entityID = vertIn.entityID;
    return output;
}
