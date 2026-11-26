// Camera-facing Ribbons, Trails & Lightning Beams (docs/VFX_And_Volumes_Plan_2026-11.md step V3a):
// Expands GpuVfxRibbonNode strips into screen-aligned quad ribbons with Ribbon UV (u along length, v across width),
// Reverse-Z scene depth test + soft depth fade, and procedural energy/lightning core when no texture is bound.
#include "shaderIO.h"

struct RibbonVSOut
{
    float4 pos : SV_Position;
    float3 worldPos : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
};

float3 ribbon_node_tangent(BufferRef<GpuVfxRibbonNode> nodes, uint firstNode, uint nodeCount, uint localIdx)
{
    // At the live head (localIdx == 0), span across the last full anchored segment (nodes[2] - nodes[0]) so tiny
    // sub-pixel mouse steps right after anchoring a node never whip the wide head quad sideways.
    uint prevIdx = localIdx > 0u ? (localIdx - 1u) : 0u;
    uint nextIdx = (localIdx + 1u < nodeCount) ? (localIdx + 1u) : (nodeCount - 1u);
    if (localIdx == 0u && nodeCount >= 3u)
        nextIdx = 2u;
    else if (localIdx + 1u == nodeCount && nodeCount >= 3u)
        prevIdx = nodeCount - 3u;

    float3 dir = nodes[firstNode + nextIdx].position - nodes[firstNode + prevIdx].position;
    float lenSq = dot(dir, dir);
    if (lenSq <= 1e-8 && nodeCount >= 2u)
    {
        dir = nodes[firstNode + nodeCount - 1u].position - nodes[firstNode].position;
        lenSq = dot(dir, dir);
    }
    return lenSq > 1e-8 ? dir * rsqrt(lenSq) : float3(0.0, 1.0, 0.0);
}

[shader("vertex")]
RibbonVSOut vertMain(uint vertexID : SV_VertexID, uniform PushConstantVfxRibbon pushConstant)
{
    RibbonVSOut outVert;
    const uint segIdx = vertexID / 6u;
    const uint cornerIdx = vertexID % 6u;
    // Two triangles per quad segment: (0,0), (1,0), (1,1), (0,0), (1,1), (0,1)
    const uint isEnd = (cornerIdx == 1u || cornerIdx == 2u || cornerIdx == 4u) ? 1u : 0u;
    const float side = (cornerIdx == 2u || cornerIdx == 4u || cornerIdx == 5u) ? 1.0 : -1.0;

    const uint localIdx = min(segIdx + isEnd, pushConstant.nodeCount - 1u);
    BufferRef<GpuVfxRibbonNode> nodes = BufferRef<GpuVfxRibbonNode>(pushConstant.nodesReference);
    const GpuVfxRibbonNode node = nodes[pushConstant.firstNode + localIdx];

    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    const float3 tangent = ribbon_node_tangent(nodes, pushConstant.firstNode, pushConstant.nodeCount, localIdx);
    const float3 toCam = ubo.cameraWorldPos.xyz - node.position;
    float3 right = cross(tangent, toCam);
    const float rLenSq = dot(right, right);
    right = rLenSq > 1e-8 ? right * rsqrt(rLenSq) : float3(1.0, 0.0, 0.0);

    const float3 worldPos = node.position + right * (side * 0.5 * node.width);
    outVert.pos = mul(ubo.proj, mul(ubo.view, float4(worldPos, 1.0)));
    outVert.worldPos = worldPos;
    outVert.uv = float2(node.u, side * 0.5 + 0.5);
    outVert.color = node.color;
    return outVert;
}

[shader("fragment")]
float4 fragMain(RibbonVSOut inVert, uniform PushConstantVfxRibbon pushConstant) : SV_Target0
{
    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);

    // Reverse-Z scene depth test + optional soft depth fade.
    float softFade = 1.0;
    if (pushConstant.depthTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D depthTex = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
        const int2 px = int2(inVert.pos.xy);
        const float sceneDepthNdc = depthTex.Load(int3(px, 0)).r;
        // Reverse-Z: larger NDC z is closer to camera. Discard if behind solid geometry.
        if (inVert.pos.z < sceneDepthNdc)
            discard;

        if ((pushConstant.flags & VFX_RIBBON_SOFT) != 0u && pushConstant.softDistance > 1e-4 && sceneDepthNdc > 1e-6)
        {
            uint depthW = 1u;
            uint depthH = 1u;
            depthTex.GetDimensions(depthW, depthH);
            const float2 ndcXY = float2((inVert.pos.x / max(float(depthW), 1.0)) * 2.0 - 1.0,
                                        1.0 - (inVert.pos.y / max(float(depthH), 1.0)) * 2.0);
            const float4 sceneH = mul(ubo.invViewProj, float4(ndcXY, sceneDepthNdc, 1.0));
            const float3 sceneWorld = sceneH.xyz / max(abs(sceneH.w), 1e-6);
            const float particleViewZ = abs(mul(ubo.view, float4(inVert.worldPos, 1.0)).z);
            const float sceneViewZ = abs(mul(ubo.view, float4(sceneWorld, 1.0)).z);
            softFade = saturate((sceneViewZ - particleViewZ) / pushConstant.softDistance);
        }
    }

    float4 texColor = float4(1.0);
    if (pushConstant.textureIndex != 0xFFFFFFFFu)
    {
        Texture2D tex = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + pushConstant.textureIndex, 0));
        SamplerState samp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
        const float2 uv = float2(inVert.uv.x * pushConstant.uvTileU + ubo.timeSeconds * pushConstant.uvScrollU, inVert.uv.y);
        texColor = tex.Sample(samp, uv);
    }
    else
    {
        // Crisp procedural glowing core (bright white-hot center fading smoothly to edges).
        const float distFromCenter = abs(inVert.uv.y * 2.0 - 1.0);
        const bool isBeam = (pushConstant.flags & VFX_RIBBON_BEAM) != 0u;
        const float edgeMask = pow(saturate(1.0 - distFromCenter), isBeam ? 2.4 : 1.6);
        const float hotCore = pow(saturate(1.0 - distFromCenter), isBeam ? 8.0 : 5.0);
        // Subtle tail fade for trails
        const float endFade = isBeam ? (smoothstep(0.0, 0.04, inVert.uv.x) * smoothstep(1.0, 0.96, inVert.uv.x)) : 1.0;
        texColor = float4(1.0 + hotCore * (isBeam ? 1.8 : 0.7), 1.0 + hotCore * (isBeam ? 1.8 : 0.7), 1.0 + hotCore * (isBeam ? 1.8 : 0.7), edgeMask * endFade);
    }

    float alpha = saturate(inVert.color.a * texColor.a * softFade);
    float3 rgb = inVert.color.rgb * texColor.rgb;

#ifdef NOX_MATERIAL_GRAPH
    if (pushConstant.materialSlot != 0xFFFFFFFFu && ubo.materialDataReference != 0)
    {
        const GpuMaterial mat = BufferRef<GpuMaterial>(ubo.materialDataReference)[pushConstant.materialSlot];
        if (mat.graphSlot != 0xFFFFFFFFu)
        {
            MaterialGraphInputs gi = (MaterialGraphInputs)0;
            gi.uv0 = inVert.uv;
            gi.uv0Dx = ddx(inVert.uv);
            gi.uv0Dy = ddy(inVert.uv);
            gi.position = inVert.worldPos + ubo.renderOrigin;
            gi.view = normalize(ubo.cameraWorldPos.xyz - inVert.worldPos);
            gi.normal = gi.view;
            gi.time = ubo.timeSeconds;
            gi.cameraPosition = ubo.cameraWorldPos.xyz + ubo.renderOrigin;
            gi.vertexColor = inVert.color;
            gi.hasParticleData = 1u;
            gi.particleAge = inVert.uv.x;
            gi.particleColor = inVert.color;
            gi.ribbonUV = inVert.uv;
            gi.material = mat;
            gi.ubo = ubo;

            MaterialGraphOutputs go = (MaterialGraphOutputs)0;
            go.baseColor = rgb;
            go.emissive = float3(0.0);
            go.opacity = alpha;
            material_graph_surface(mat.graphSlot, gi, go);
            rgb = go.baseColor + go.emissive;
            alpha = saturate(go.opacity * softFade);
        }
    }
#endif

    if (alpha <= 1e-4)
        discard;

    // Premultiplied output matching Particles pass blend equations (Additive: ONE/ONE; Alpha: ONE/ONE_MINUS_SRC_ALPHA).
    rgb *= alpha;
    return float4(rgb, (pushConstant.flags & VFX_RIBBON_ADDITIVE) != 0u ? 0.0 : alpha);
}
