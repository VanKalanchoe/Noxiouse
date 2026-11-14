#include "shaderIO.h"

// Outlines (stylized rendering step 2; Renderer::addSceneOutlinePass, settings in PostProcessSettings): a full-screen draw over the lit
// scene at the render resolution, before TAA / DLSS (which smooth the lines and keep them stable), blended Zero / SrcColor -- the
// scene times what the pixel returns: the outline colour on an edge, white elsewhere. So black is ink, a colour tints the edge and
// keeps its shading, and no colour of the scene is ever read here (no read-write of the target).
//
// An edge, seen from the nearer side so the line lies on the object, never on what is behind it:
//   - depth: a neighbour `thickness` pixels away is farther by more than `depthThreshold` of this pixel's distance (silhouettes);
//   - normal: a neighbour at about the same distance faces away by more than `normalThreshold` (1 - cos of the angle; creases).
// The sky (depth 0) has none; with toonOnly only toon pixels (gbuffer_toon) do. Lines fade out towards `fade` distance.

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

// World position of a pixel's depth (reverse-Z) through the frame's (jittered, as the G-buffer is) inverse view-projection.
float3 outline_world(float4x4 invViewProj, float2 pixel, float2 size, float depth)
{
    const float2 ndc = float2(pixel.x / size.x * 2.0 - 1.0, 1.0 - pixel.y / size.y * 2.0);
    const float4 h = mul(invViewProj, float4(ndc, depth, 1.0));
    return h.xyz / h.w;
}

[shader("fragment")]
float4 fragMain(VSOutput input, uniform PushConstantSceneOutline pushConstant) : SV_Target0
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(pushConstant.depthIndex, 0));
    Texture2D normalTex = Texture2D.Handle(uint2(pushConstant.normalIndex, 0));
    Texture2D materialTex = Texture2D.Handle(uint2(pushConstant.materialIndex, 0));
    const int2 size = int2(pushConstant.params2.xy);
    const int2 pixel = int2(input.pos.xy);

    const float centerDepth = depthTex.Load(int3(pixel, 0));
    if (centerDepth <= 0.0)
        discard; // the sky
    if (pushConstant.toonOnly != 0u && !gbuffer_toon(materialTex.Load(int3(pixel, 0)).b))
        discard;

    const float3 camera = ubo->cameraWorldPos.xyz;
    const float centerDistance = distance(outline_world(ubo->invViewProj, float2(pixel) + 0.5, float2(size), centerDepth), camera);
    const float3 centerNormal = normalize(normalTex.Load(int3(pixel, 0)).rgb);

    // Depth steps relative to the distance (a step at 10 m is ten times one at 1 m); an orthographic view's "camera" is far back
    // along parallel rays, so there the reference is 10 m.
    const bool orthographic = ubo->nonJitteredProj[3][3] > 0.5;
    const float reference = orthographic ? 10.0 * ubo->unitsPerMeter : max(centerDistance, 1e-4);
    const float thickness = max(pushConstant.params.x, 0.5);
    const float depthThreshold = pushConstant.params.y;
    const float normalThreshold = pushConstant.params.z;
    // Eight neighbours on a ring `thickness` pixels out (rounded: whole texels, no filtering across an edge).
    static const float2 directions[8] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
                                          float2(0.7071, 0.7071), float2(-0.7071, 0.7071), float2(0.7071, -0.7071), float2(-0.7071, -0.7071) };
    float edge = 0.0;
    [unroll]
    for (uint k = 0; k < 8; ++k)
    {
        const int2 neighbour = clamp(pixel + int2(round(directions[k] * thickness)), int2(0), size - 1);
        const float neighbourDepth = depthTex.Load(int3(neighbour, 0));
        // The sky behind a surface is infinitely farther: a silhouette.
        if (neighbourDepth <= 0.0)
        {
            edge = depthThreshold > 0.0 ? 1.0 : edge;
            continue;
        }
        const float neighbourDistance = distance(outline_world(ubo->invViewProj, float2(neighbour) + 0.5, float2(size), neighbourDepth), camera);
        const float farther = (neighbourDistance - centerDistance) / reference;
        if (depthThreshold > 0.0)
            edge = max(edge, smoothstep(depthThreshold, depthThreshold * 1.5, farther));
        // A crease: about the same distance (not a silhouette, not the far side of one), facing elsewhere.
        if (normalThreshold > 0.0 && abs(farther) < max(depthThreshold, 0.02))
        {
            const float3 neighbourNormal = normalize(normalTex.Load(int3(neighbour, 0)).rgb);
            const float turn = 1.0 - dot(centerNormal, neighbourNormal);
            edge = max(edge, smoothstep(normalThreshold, normalThreshold * 1.5, turn));
        }
    }
    const float fade = pushConstant.params.w > 0.0 && !orthographic ? saturate(1.0 - centerDistance / pushConstant.params.w) : 1.0;
    edge *= fade;
    if (edge <= 0.001)
        discard;
    return float4(lerp(float3(1.0), saturate(pushConstant.color.rgb), edge), 1.0);
}
