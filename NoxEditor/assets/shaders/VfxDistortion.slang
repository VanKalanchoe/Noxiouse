// Screen-Space Distortion Pass (docs/VFX_And_Volumes_Plan_2026-11.md step V3b):
// Warps a copy of HDRScene for Shockwave Rings, Heat Haze, and Portal Swirls with Reverse-Z depth occlusion,
// foreground anti-bleed masking (UE5/Unity standard), RGB chromatic aberration, and subtle energy rim tint.
#include "shaderIO.h"

struct DistortionVSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint distortionIndex : TEXCOORD1;
};

static const float2 kQuadCorners[6] = {
    float2(-1.0, -1.0), float2( 1.0, -1.0), float2( 1.0,  1.0),
    float2(-1.0, -1.0), float2( 1.0,  1.0), float2(-1.0,  1.0)
};

[shader("vertex")]
DistortionVSOut vertMain(uint vertexID : SV_VertexID, uniform PushConstantVfxDistortion pushConstant)
{
    DistortionVSOut outV;
    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    const GpuVfxDistortion d = BufferRef<GpuVfxDistortion>(pushConstant.distortionsReference)[pushConstant.distortionIndex];

    const float2 corner = kQuadCorners[vertexID % 6u];
    float3 right;
    float3 up;
    if (d.cameraFacing != 0u)
    {
        float3 forward = d.position - ubo.cameraWorldPos.xyz;
        const float dist = length(forward);
        forward = dist > 1e-5 ? forward / dist : float3(0.0, 0.0, -1.0);
        right = cross(forward, float3(0.0, 1.0, 0.0));
        right = dot(right, right) > 1e-8 ? normalize(right) : float3(1.0, 0.0, 0.0);
        up = cross(right, forward);
    }
    else
    {
        right = d.right;
        up = d.up;
    }

    const float3 worldPos = d.position + (right * corner.x + up * corner.y) * d.radius;
    outV.pos = mul(ubo.proj, mul(ubo.view, float4(worldPos, 1.0)));
    outV.uv = corner; // [-1, 1]
    outV.distortionIndex = pushConstant.distortionIndex;
    return outV;
}

float dist_hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float dist_noise2d(float2 p)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0 - 2.0 * f);
    const float a = dist_hash21(i);
    const float b = dist_hash21(i + float2(1.0, 0.0));
    const float c = dist_hash21(i + float2(0.0, 1.0));
    const float d = dist_hash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

[shader("fragment")]
float4 fragMain(DistortionVSOut inVert, uniform PushConstantVfxDistortion pushConstant) : SV_Target0
{
    const float r = length(inVert.uv);
    if (r >= 1.0)
        discard;

    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    const GpuVfxDistortion d = BufferRef<GpuVfxDistortion>(pushConstant.distortionsReference)[inVert.distortionIndex];

    Texture2D sceneTex = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + pushConstant.sceneColorIndex, 0));
    Texture2D depthTex = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));

    uint texW = 1u;
    uint texH = 1u;
    sceneTex.GetDimensions(texW, texH);
    const float2 invDims = 1.0 / max(float2(float(texW), float(texH)), float2(1.0, 1.0));
    const float2 screenUV = inVert.pos.xy * invDims;

    // Reverse-Z depth occlusion: discard distortion pixels behind solid foreground geometry.
    const float centerSceneDepth = depthTex.Load(int3(int2(inVert.pos.xy), 0)).r;
    if (inVert.pos.z < centerSceneDepth)
        discard;

    float2 offset = float2(0.0, 0.0);
    float crestMask = 0.0;
    const float2 radialDir = r > 1e-4 ? (inVert.uv / r) : float2(1.0, 0.0);

    if (d.mode == 0u) // Shockwave Ring
    {
        const float waveRadius = frac(ubo.timeSeconds * max(d.speed, 0.05));
        const float band = max(d.ringWidth, 0.04);
        const float distToWave = abs(r - waveRadius);
        const float ring = saturate(1.0 - distToWave / band);
        const float wave = sin((r - waveRadius) / band * 3.14159265) * ring * ring * (1.0 - waveRadius);
        offset = radialDir * (wave * d.strength);
        crestMask = ring * ring * (1.0 - waveRadius);
    }
    else if (d.mode == 1u) // Heat Haze
    {
        const float edgeMask = pow(saturate(1.0 - r), 1.6);
        const float2 p = inVert.uv * 4.5 + float2(0.0, ubo.timeSeconds * d.speed * 1.8);
        const float nx = dist_noise2d(p) * 2.0 - 1.0;
        const float ny = dist_noise2d(p + float2(17.3, 31.7)) * 2.0 - 1.0;
        offset = float2(nx, ny) * (d.strength * edgeMask);
        crestMask = edgeMask * 0.35;
    }
    else // Portal Swirl
    {
        const float edgeMask = pow(saturate(1.0 - r), 1.3);
        const float2 tangent = float2(-radialDir.y, radialDir.x);
        const float swirl = sin(r * 10.0 - ubo.timeSeconds * d.speed * 3.0) * 0.35 + 0.65;
        offset = (tangent * 0.85 + radialDir * 0.35) * (d.strength * edgeMask * swirl);
        crestMask = exp(-pow((r - 0.78) * 9.0, 2.0)) + edgeMask * 0.25;
    }

    if (dot(offset, offset) < 1e-9 && crestMask < 1e-4)
        discard;

    // Foreground anti-bleed check (UE5/Unity standard): if the refracted UV lands on a foreground object closer
    // than this distortion quad, suppress the offset so foreground silhouettes never smear into the shockwave.
    const float2 warpedUV = clamp(screenUV + offset, float2(0.001, 0.001), float2(0.999, 0.999));
    const int2 warpedPx = clamp(int2(warpedUV * float2(float(texW), float(texH))), int2(0, 0), int2(int(texW) - 1, int(texH) - 1));
    const float warpedDepth = depthTex.Load(int3(warpedPx, 0)).r;
    if (warpedDepth > inVert.pos.z)
        offset = float2(0.0, 0.0);

    const float ca = saturate(d.chromaticAberration) * 0.45;
    const float2 uvR = clamp(screenUV + offset * (1.0 + ca), float2(0.001, 0.001), float2(0.999, 0.999));
    const float2 uvG = clamp(screenUV + offset,              float2(0.001, 0.001), float2(0.999, 0.999));
    const float2 uvB = clamp(screenUV + offset * (1.0 - ca), float2(0.001, 0.001), float2(0.999, 0.999));

    float3 col;
    col.r = sceneTex.SampleLevel(linearClamp, uvR, 0).r;
    col.g = sceneTex.SampleLevel(linearClamp, uvG, 0).g;
    col.b = sceneTex.SampleLevel(linearClamp, uvB, 0).b;

    col += d.rimTint.rgb * (d.rimTint.a * crestMask);
    return float4(col, 1.0);
}
