#include "shaderIO.h"

// DLSS Ray Reconstruction guides (Streamline docs/ProgrammingGuideDLSS_RR.md 4.1.1, 4.1.2, 4.2.1). RR divides the lit
// image by the diffuse and specular albedo, denoises the lighting and multiplies back, so both must be the reflectance
// the image was shaded with: the diffuse and specular color of DeferredLighting.slang, the specular pre-integrated over
// the view angle and roughness (EnvBRDFApprox2, as given by the guide). With RT reflections, also the specular hit
// distance (4.1.9): the world-space distance from the primary surface to what its reflection ray hit, which RR needs to
// move reflections with the camera.

// What Reflection.slang writes for a reflection ray that missed or was not traced (its alpha).
static const float ReflectionMissDistance = 10000.0;

// Streamline DLSS-RR programming guide 4.2.1 [Ray Tracing Gems, Chapter 32].
float3 EnvBRDFApprox2(float3 SpecularColor, float alpha, float NoV)
{
    NoV = abs(NoV);
    float4 X;
    X.x = 1.f;
    X.y = NoV;
    X.z = NoV * NoV;
    X.w = NoV * X.z;
    float4 Y;
    Y.x = 1.f;
    Y.y = alpha;
    Y.z = alpha * alpha;
    Y.w = alpha * Y.z;
    float2x2 M1 = float2x2(0.99044f, -1.28514f, 1.29678f, -0.755907f);
    float3x3 M2 = float3x3(1.f, 2.92338f, 59.4188f, 20.3225f, -27.0302f, 222.592f, 121.563f, 626.13f, 316.627f);
    float2x2 M3 = float2x2(0.0365463f, 3.32707, 9.0632f, -9.04756);
    float3x3 M4 = float3x3(1.f, 3.59685f, -1.36772f, 9.04401f, -16.3174f, 9.22949f, 5.56589f, 19.7886f, -20.2123f);
    float bias = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw));
    float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw));
    // This is a hack for specular reflectance of 0
    bias *= saturate(SpecularColor.g * 50);
    return mad(SpecularColor, max(0, scale), max(0, bias));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void guidesMain(uint3 dispatchThreadID : SV_DispatchThreadID, uniform PushConstantRRGuides pushConstant)
{
    if (dispatchThreadID.x >= pushConstant.width || dispatchThreadID.y >= pushConstant.height)
        return;

    int2 pixel = int2(dispatchThreadID.xy);
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    RWTexture2D<float4> diffuseOut = RWTexture2D<float4>.Handle(uint2(pushConstant.diffuseAlbedoStorageIndex, 0));
    RWTexture2D<float4> specularOut = RWTexture2D<float4>.Handle(uint2(pushConstant.specularAlbedoStorageIndex, 0));
    if (pushConstant.rawReflectionIndex != 0xFFFFFFFF)
    {
        Texture2D reflectionTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.rawReflectionIndex, 0));
        RWTexture2D<float> hitDistanceOut = RWTexture2D<float>.Handle(uint2(pushConstant.hitDistanceStorageIndex, 0));
        hitDistanceOut[pixel] = min(reflectionTex.Load(int3(pixel, 0)).a, ReflectionMissDistance);
    }

    // Background (reverse-Z: nothing drawn at depth 0): no reflectance, as the G-buffer clears it.
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    float depth = depthTex.Load(int3(pixel, 0));
    if (depth <= 0.0)
    {
        diffuseOut[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        specularOut[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    Texture2D albedoTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferAlbedoIndex, 0));
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferNormalIndex, 0));
    Texture2D materialTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferMaterialIndex, 0));
    float3 baseColor = albedoTex.Load(int3(pixel, 0)).rgb;
    float3 normalSample = normalTex.Load(int3(pixel, 0)).rgb;
    float4 materialSample = materialTex.Load(int3(pixel, 0));

    // Unlit (workflow 2): the lighting outputs the base color itself.
    if (materialSample.b >= 2.0)
    {
        diffuseOut[pixel] = float4(baseColor, 1.0);
        specularOut[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // As DeferredLighting.slang: dielectric F0 0.04, metals reflect their base color.
    float metallic = saturate(materialSample.g);
    float roughness = saturate(materialSample.r);
    float3 f0 = float3(0.04);
    float3 diffuseColor = baseColor * (float3(1.0) - f0) * (1.0 - metallic);
    float3 specularColor = lerp(f0, baseColor, metallic);

    // N.V from the pixel's world position (the jittered projection its depth was rasterized with).
    float2 uv = (float2(pixel) + 0.5) / float2(pushConstant.width, pushConstant.height);
    float4 worldPosH = mul(ubo->invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    float3 worldPos = worldPosH.xyz / worldPosH.w;
    float lenSqN = dot(normalSample, normalSample);
    float3 n = lenSqN > 1e-6 ? normalSample * rsqrt(lenSqN) : float3(0.0, 1.0, 0.0);
    float NoV = dot(n, normalize(ubo->cameraWorldPos.xyz - worldPos));

    diffuseOut[pixel] = float4(diffuseColor, 1.0);
    specularOut[pixel] = float4(EnvBRDFApprox2(specularColor, roughness * roughness, NoV), 1.0);
}
