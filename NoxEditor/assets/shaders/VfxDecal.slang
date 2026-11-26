// Projected VFX Decals (docs/VFX_And_Volumes_Plan_2026-11.md step V3a):
// Renders a unit bounding cube (front-face culled so it works when camera is inside), reconstructs scene surface
// world position from Reverse-Z Depth, projects into decal local space along -Y, fades by normal angle & box edge,
// and blends an optional texture or procedural glowing arcane rune circle onto HDRScene.
#include "shaderIO.h"

struct DecalVSOut
{
    float4 pos : SV_Position;
    nointerpolation uint decalIndex : TEXCOORD0;
};

// Inward-wound unit cube so NRI_CULL_MODE_BACK renders only the far inside faces (works even when camera is inside the box).
static const float3 kCubeVerts[36] = {
    // -Z
    float3(-0.5,-0.5,-0.5), float3( 0.5,-0.5,-0.5), float3( 0.5, 0.5,-0.5),
    float3(-0.5,-0.5,-0.5), float3( 0.5, 0.5,-0.5), float3(-0.5, 0.5,-0.5),
    // +Z
    float3(-0.5,-0.5, 0.5), float3( 0.5, 0.5, 0.5), float3( 0.5,-0.5, 0.5),
    float3(-0.5,-0.5, 0.5), float3(-0.5, 0.5, 0.5), float3( 0.5, 0.5, 0.5),
    // -X
    float3(-0.5,-0.5,-0.5), float3(-0.5, 0.5, 0.5), float3(-0.5,-0.5, 0.5),
    float3(-0.5,-0.5,-0.5), float3(-0.5, 0.5,-0.5), float3(-0.5, 0.5, 0.5),
    // +X
    float3( 0.5,-0.5,-0.5), float3( 0.5,-0.5, 0.5), float3( 0.5, 0.5, 0.5),
    float3( 0.5,-0.5,-0.5), float3( 0.5, 0.5, 0.5), float3( 0.5, 0.5,-0.5),
    // -Y
    float3(-0.5,-0.5,-0.5), float3( 0.5,-0.5, 0.5), float3( 0.5,-0.5,-0.5),
    float3(-0.5,-0.5,-0.5), float3(-0.5,-0.5, 0.5), float3( 0.5,-0.5, 0.5),
    // +Y
    float3(-0.5, 0.5,-0.5), float3( 0.5, 0.5,-0.5), float3( 0.5, 0.5, 0.5),
    float3(-0.5, 0.5,-0.5), float3( 0.5, 0.5, 0.5), float3(-0.5, 0.5, 0.5)
};

[shader("vertex")]
DecalVSOut vertMain(uint vertexID : SV_VertexID, uniform PushConstantVfxDecal pushConstant)
{
    DecalVSOut outVert;
    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    const GpuVfxDecal decal = BufferRef<GpuVfxDecal>(pushConstant.decalsReference)[pushConstant.decalIndex];
    const float3 localPos = kCubeVerts[vertexID % 36u];
    const float3 worldPos = mul(decal.model, float4(localPos, 1.0)).xyz;
    outVert.pos = mul(ubo.proj, mul(ubo.view, float4(worldPos, 1.0)));
    outVert.decalIndex = pushConstant.decalIndex;
    return outVert;
}

// Procedural glowing arcane rune ring when no custom decal texture is assigned.
float4 decal_procedural_rune(float2 uv, float time)
{
    const float2 p = uv * 2.0 - 1.0;
    const float r = length(p);
    if (r > 1.0)
        return float4(0.0);
    const float angle = atan2(p.y, p.x);

    const float outerRing = exp(-pow((r - 0.88) * 28.0, 2.0));
    const float innerRing = exp(-pow((r - 0.62) * 32.0, 2.0));
    const float coreRing  = exp(-pow((r - 0.25) * 24.0, 2.0));
    const float ticks = smoothstep(0.6, 0.92, sin(angle * 12.0 + time * 1.4)) * exp(-pow((r - 0.75) * 18.0, 2.0));
    const float star = exp(-pow((r - (0.44 + 0.14 * cos(angle * 6.0 - time * 0.9))) * 26.0, 2.0));
    const float softGlow = pow(saturate(1.0 - r), 2.2) * 0.35;

    const float mask = saturate(outerRing + innerRing * 0.85 + coreRing * 0.7 + ticks * 0.9 + star * 0.8 + softGlow);
    const float hot = saturate(outerRing + star) * 0.7;
    return float4(1.0 + hot, 1.0 + hot, 1.0 + hot, mask);
}

[shader("fragment")]
float4 fragMain(DecalVSOut inVert, uniform PushConstantVfxDecal pushConstant) : SV_Target0
{
    if (pushConstant.depthTextureIndex == 0xFFFFFFFFu)
        discard;

    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    const GpuVfxDecal decal = BufferRef<GpuVfxDecal>(pushConstant.decalsReference)[inVert.decalIndex];

    const int2 px = int2(inVert.pos.xy);
    Texture2D depthTex = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    const float sceneDepthNdc = depthTex.Load(int3(px, 0)).r;
    if (sceneDepthNdc <= 1e-6) // sky / far plane
        discard;

    const float2 uvScreen = inVert.pos.xy / max(pushConstant.viewportSize, float2(1.0, 1.0));
    const float2 ndcXY = float2(uvScreen.x * 2.0 - 1.0, 1.0 - uvScreen.y * 2.0);
    const float4 worldH = mul(ubo.invViewProj, float4(ndcXY, sceneDepthNdc, 1.0));
    if (abs(worldH.w) < 1e-6)
        discard;
    const float3 worldPos = worldH.xyz / worldH.w;

    // Project scene point into decal local box [-0.5, 0.5]^3.
    const float3 localPos = mul(decal.invModel, float4(worldPos, 1.0)).xyz;
    if (any(abs(localPos) > 0.5))
        discard;

    // Fade smoothly near top/bottom faces of the projection box.
    const float edgeDist = 0.5 - abs(localPos.y);
    const float edgeFade = decal.edgeFade > 1e-4 ? saturate(edgeDist / decal.edgeFade) : 1.0;

    // Angle fade against GBufferNormal so decals don't smear on vertical walls.
    float normalFade = 1.0;
    if (pushConstant.normalTextureIndex != 0xFFFFFFFFu && decal.normalFade > 0.0)
    {
        Texture2D normalTex = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + pushConstant.normalTextureIndex, 0));
        const float3 surfN = normalTex.Load(int3(px, 0)).xyz;
        if (dot(surfN, surfN) > 1e-4)
        {
            const float3 decalUp = normalize(mul((float3x3)decal.model, float3(0.0, 1.0, 0.0)));
            const float ndotu = abs(dot(normalize(surfN), decalUp));
            normalFade = smoothstep(decal.normalFade, min(decal.normalFade + 0.25, 1.0), ndotu);
        }
    }

    const float2 decalUV = localPos.xz + 0.5;
    float4 sampleCol;
    if (decal.textureIndex != 0xFFFFFFFFu)
    {
        Texture2D tex = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + decal.textureIndex, 0));
        SamplerState samp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
        sampleCol = tex.Sample(samp, decalUV);
    }
    else
    {
        sampleCol = decal_procedural_rune(decalUV, ubo.timeSeconds);
    }

    const float alpha = saturate(decal.color.a * sampleCol.a * edgeFade * normalFade);
    if (alpha <= 1e-4)
        discard;

    const float boost = max(decal.emissiveIntensity, 0.0) + ((decal.flags & 1u) != 0u ? 0.0 : 1.0);
    const float3 rgb = decal.color.rgb * sampleCol.rgb * boost * alpha;
    return float4(rgb, (decal.flags & 1u) != 0u ? 0.0 : alpha);
}
