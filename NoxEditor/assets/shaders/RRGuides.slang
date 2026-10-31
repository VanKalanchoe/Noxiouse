#include "shaderIO.h"
#include "PBRLighting.slang"

// DLSS Ray Reconstruction guides (Streamline docs/ProgrammingGuideDLSS_RR.md 4.1.1, 4.1.2, 4.2.1). RR divides the lit
// image by the diffuse and specular albedo, denoises the lighting and multiplies back, so both must be the reflectance
// the image was shaded with: the diffuse and specular color of DeferredLighting.slang, the specular pre-integrated over
// the view angle and roughness (EnvBRDFApprox2, as given by the guide). And how reflections move: RTXPT's specular motion
// vectors (PostProcess.hlsl, ComputeSpecularMotionVector), not the specular hit distance -- RTXPT turns that input off ("it's
// buggy"). Tagged with the hit distance, RR moved the reflections' history from the wrong places: a string light's reflection
// in a smooth floor came out as the sign's letter above it, where NRD (and the path tracer) showed the bulb.

// Streamline DLSS-RR programming guide 4.2.1 [Ray Tracing Gems, Chapter 32].
// `f90`: the grazing reflectance (KHR_materials_specular's weight; 1 otherwise).
float3 EnvBRDFApprox2(float3 SpecularColor, float alpha, float NoV, float3 f90 = float3(1.0))
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
    return mad(SpecularColor, max(0, scale), max(0, bias) * f90);
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
    // The specular motion vectors: the surface's own where nothing better is known (RTXPT's default).
    Texture2D<float2> velocityTex = Texture2D<float2>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.velocityTextureIndex, 0));
    RWTexture2D<float2> specularMotionOut = RWTexture2D<float2>.Handle(uint2(pushConstant.specularMotionStorageIndex, 0));
    specularMotionOut[pixel] = velocityTex.Load(int3(pixel, 0));

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

    // Unlit: the lighting outputs the base color itself.
    if (gbuffer_unlit(materialSample.b))
    {
        diffuseOut[pixel] = float4(baseColor, 1.0);
        specularOut[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // As DeferredLighting.slang: the dielectric F0 (0.04, or the specular lobe's), metals reflect their base color.
    float metallic = saturate(materialSample.g);
    float roughness = saturate(materialSample.r);
    const GBufferLobes lobes = gbuffer_load_lobes(ubo, pixel, materialSample.b);
    float3 diffuseColor, specularColor, f90;
    lobe_base_reflectance(lobes, baseColor, metallic, diffuseColor, specularColor, f90);

    // N.V from the pixel's world position (the jittered projection its depth was rasterized with).
    float2 uv = (float2(pixel) + 0.5) / float2(pushConstant.width, pushConstant.height);
    float4 worldPosH = mul(ubo->invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    float3 worldPos = worldPosH.xyz / worldPosH.w;
    float lenSqN = dot(normalSample, normalSample);
    float3 n = lenSqN > 1e-6 ? normalSample * rsqrt(lenSqN) : float3(0.0, 1.0, 0.0);
    const float3 toCamera = normalize(ubo->cameraWorldPos.xyz - worldPos);
    float NoV = dot(n, toCamera);

    // Under a clear coat what the base reflects is what the coat lets through, plus the coat's own reflection (lobe_ambient's
    // layering); the reflection's guides follow the reflection layer (gbuffer_reflection_layer).
    float3 specularAlbedo = EnvBRDFApprox2(specularColor, roughness * roughness, NoV, (lobes.flags & MaterialLobeSpecular) != 0u ? f90 : float3(1.0));
    if (lobes.coat > 0.0)
    {
        const float coatNoV = saturate(dot(lobes.coatNormal, toCamera));
        const float coatWeight = lobes.coat * (0.04 + 0.96 * pow(1.0 - coatNoV, 5.0));
        const float coatRoughness = max(lobes.coatRoughness, 0.04);
        diffuseColor *= 1.0 - coatWeight;
        specularAlbedo = specularAlbedo * (1.0 - coatWeight) + coatWeight * EnvBRDFApprox2(float3(1.0), coatRoughness * coatRoughness, coatNoV);
        float3 reflectionNormal = n;
        gbuffer_reflection_layer(lobes, reflectionNormal, roughness);
    }
    diffuseOut[pixel] = float4(diffuseColor, 1.0);
    specularOut[pixel] = float4(specularAlbedo, 1.0);

    // RTXPT's specular motion vectors (PostProcess.hlsl): on a smooth surface (roughness below 0.25, kSpecularRoughnessThreshold)
    // with a traced reflection, the reflected point's mirror image behind the surface -- as far along the camera's ray as the
    // reflection ray went (a miss: the sky, far away) -- moves as the camera does; its motion is the reflection's.
    if (pushConstant.rawReflectionIndex != 0xFFFFFFFF && roughness < 0.25)
    {
        Texture2D reflectionTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.rawReflectionIndex, 0));
        // A ray that missed: Reflection.slang wrote reflection_miss_distance (shaderIO.h) in its alpha.
        const float hitDistance = min(reflectionTex.Load(int3(pixel, 0)).a, reflection_miss_distance(ubo));
        if (hitDistance > 1e-3 * ubo->unitsPerMeter)
        {
            const float3 image = worldPos + normalize(worldPos - ubo->cameraWorldPos.xyz) * hitDistance;
            const float4 currClip = mul(ubo->nonJitteredProj, mul(ubo->view, float4(image, 1.0)));
            const float4 prevClip = mul(ubo->prevProj, mul(ubo->prevView, float4(image, 1.0)));
            if (currClip.w > 1e-6 && prevClip.w > 1e-6)
                specularMotionOut[pixel] = (prevClip.xy / prevClip.w - currClip.xy / currClip.w) * float2(0.5, -0.5); // as the G-buffer's
        }
    }
}
