#include "shaderIO.h"
#include "NRDFrontEnd.slang"

// The path tracer's light as NRD wants it (NRDFrontEnd.slang): what the primary vertex emits (or the sky it sees) stays
// apart; everything else is diffuse or specular -- at the primary vertex by its own BRDF lobes, after it by the lobe the
// primary vertex sampled -- de-modulated by the pixel's G-buffer surface. PathTracer.slang writes the signals,
// PTComposite.slang modulates the denoised ones back.

static const uint PT_LOBE_NONE = 0;     // still at the primary vertex
static const uint PT_LOBE_DIFFUSE = 1;  // the primary vertex sampled its diffuse lobe (or transmission)
static const uint PT_LOBE_SPECULAR = 2;

// Light found along the path, by where the path stands.
void PT_AddRadiance(uint lobe, float3 contribution, inout float3 emission, inout float3 diffuse, inout float3 specular)
{
    if (lobe == PT_LOBE_NONE)
        emission += contribution;
    else if (lobe == PT_LOBE_SPECULAR)
        specular += contribution;
    else
        diffuse += contribution;
}

// The de-modulation factors of the G-buffer surface at a pixel, shaded as DeferredLighting.slang shades it (dielectric F0
// 0.04, roughness clamped to 0.04): the path tracer divides by them, the composite multiplies by them, ReSTIR DI's signals
// already carry them. Background pixels: 1 (nothing to de-modulate). outRoughness: the specular signal's roughness.
void PT_SurfaceFactors(UniformBufferObject* ubo, int2 pixel, uint depthIndex, uint albedoIndex, uint normalIndex, uint materialIndex,
                       out float3 diffFactor, out float3 specFactor, out float outRoughness)
{
    diffFactor = float3(1.0);
    specFactor = float3(1.0);
    outRoughness = 1.0;

    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + depthIndex, 0));
    float depth = depthTex.Load(int3(pixel, 0));
    if (depth <= 0.0)
        return;

    Texture2D albedoTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + albedoIndex, 0));
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + normalIndex, 0));
    Texture2D materialTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + materialIndex, 0));
    float3 baseColor = albedoTex.Load(int3(pixel, 0)).rgb;
    float3 normalSample = normalTex.Load(int3(pixel, 0)).rgb;
    float4 material = materialTex.Load(int3(pixel, 0));
    float lenSqN = dot(normalSample, normalSample);
    if (lenSqN < 1e-6)
        return;

    uint width, height;
    depthTex.GetDimensions(width, height);
    float2 uv = (float2(pixel) + 0.5) / float2(width, height);
    float4 worldPosH = mul(ubo->invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    float metallic = saturate(material.g);
    outRoughness = clamp(material.r, 0.04, 1.0);
    float3 f0 = float3(0.04);
    NRD_MaterialFactors(normalSample * rsqrt(lenSqN), normalize(ubo->cameraWorldPos.xyz - worldPos),
                        baseColor * (float3(1.0) - f0) * (1.0 - metallic), lerp(f0, baseColor, metallic), outRoughness, diffFactor, specFactor);
}
