// Fragment stage for lit, alpha-blended (AlphaMode::Blend, non-Unlit) transparent objects.
// Mesh/Task stages are shared with the opaque forward-unlit pipeline (Material_PBR_MeshTask.slang
// / Material_PBR_Mesh.slang's meshMain, unchanged) - only this fragment stage is new.
//
// Deferred rendering (vis-buffer + G-buffer + DeferredLighting.slang, the real lighting path this
// engine uses) can't light transparent objects - a G-buffer only holds one surface per pixel, and
// blended layers need to draw after the opaque scene is already lit. That's not specific to this
// engine, every deferred renderer needs a forward pass for transparency. This shader IS that
// forward pass, but it does NOT reimplement lighting - it #includes PBRLighting.slang, the exact
// same BRDF/IBL functions DeferredLighting.slang itself uses, so there is one lighting
// implementation shared between the deferred opaque path and this forward one, not two. Without
// this, Blend objects fell back to Material_Unlit_Mesh.slang's flat baseColor pass-through (zero
// lighting math), which made them look like they were glowing/self-lit next to properly shaded
// Opaque/Mask geometry.
//
// Shadows: no G-buffer for the RT shadow pass to run against, so this shader traces its own (forwardShadowed below): the
// strongest few lights of each pixel get one hard shadow ray through a ray query, while RT shadows are on.
//
// Refraction: what is behind glass comes from the lit scene's color pyramid (screen space, any roughness), and for smooth glass
// on the high rung from a traced ray (tracedRefraction below: through the mesh's own volume, then the scene).

#include "shaderIO.h"
#include "DDGICommon.slang"
#include "PBRLighting.slang"
#include "MipFeedback.slang"
#include "RayHitShading.slang"
#include "HeightFog.slang"

import Math;

// Matches the vertex output produced by Material_PBR_Mesh.slang's meshMain.
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

static const float PBR_WORKFLOW_METALLIC_ROUGHNESS = 0.0;
static const float PBR_WORKFLOW_SPECULAR_GLOSSINESS = 1.0;

// The lights of a pixel that get a shadow ray: the strongest ones (every light would be a ray per light per pixel, and scenes
// have hundreds); the rest stay unshadowed.
static const uint ShadowedLightCount = 4;

// One shadow ray against the TLAS (ray query). Alpha-masked occluders are cut out like ShadowMask.slang does; blended and
// transmissive ones let the light through, so glass neither throws a black shadow nor shadows itself. Every candidate is
// decided here (glass and blended / masked surfaces are non-opaque instances in the TLAS; opaque ones the hardware accepts).
bool forwardShadowed(UniformBufferObject* ubo, float3 origin, float3 direction, float maxDistance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.005 * ubo->unitsPerMeter;
    ray.TMax = maxDistance;

    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(tlas, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, ray);

    GpuRayTracingInstance* rtInstances = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
    SamplerState samp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
    while (query.Proceed())
    {
        GpuRayTracingInstance occluder = rtInstances[query.CandidateRayInstanceCustomIndex()];
        if (occluder.alphaMode == 2 || ray_see_through(occluder.transmissionFactor, occluder.alphaMode))
            continue;
        if (occluder.alphaMode == 1 && occluder.baseColorTextureIndex != 0xFFFFFFFF)
        {
            uint* indices = (uint*)occluder.indexBufferAddress;
            Vertex* vertices = (Vertex*)occluder.vertexBufferAddress;
            uint primitive = query.CandidatePrimitiveIndex();
            float2 barycentrics = query.CandidateTriangleBarycentrics();
            float2 uv = vertices[indices[primitive * 3 + 0]].uv0 * (1.0 - barycentrics.x - barycentrics.y) +
                        vertices[indices[primitive * 3 + 1]].uv0 * barycentrics.x +
                        vertices[indices[primitive * 3 + 2]].uv0 * barycentrics.y;
            Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + occluder.baseColorTextureIndex, 0));
            if (baseColorTex.SampleLevel(samp, uv, RayAlphaTestLod).a * occluder.baseColorFactor.a < occluder.alphaCutoff)
                continue;
        }
        query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// Ray traced refraction (Hybrid_Renderer_Plan_2026-10.md §4 H3) for glass up to this roughness, fading into the screen-space one
// over the band below it: a single sharp ray stands for a rough lobe badly, the blurred pyramid does that better.
static const float RTRefractionFullRoughness = 0.08;
static const float RTRefractionMaxRoughness = 0.2;

// The closest hit along the ray. With `self` >= 0 only that instance counts (the glass's own far side); otherwise surfaces that
// let light through are passed (blended, transmissive) and alpha-masked ones cut out, as the shadow rays do.
bool traceRefractionRay(UniformBufferObject* ubo, float3 origin, float3 direction, float maxDistance, int self, out uint instanceSlot,
                        out uint primitive, out float2 barycentrics, out float hitT)
{
    instanceSlot = 0;
    primitive = 0;
    barycentrics = float2(0.0);
    hitT = 0.0;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = maxDistance;

    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(tlas, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);

    GpuRayTracingInstance* rtInstances = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
    SamplerState samp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
    while (query.Proceed())
    {
        const uint candidate = query.CandidateRayInstanceCustomIndex();
        if (self >= 0)
        {
            if (candidate == uint(self))
                query.CommitNonOpaqueTriangleHit();
            continue;
        }
        GpuRayTracingInstance surface = rtInstances[candidate];
        if (surface.alphaMode == 2 || ray_see_through(surface.transmissionFactor, surface.alphaMode))
            continue;
        if (surface.alphaMode == 1 && surface.baseColorTextureIndex != 0xFFFFFFFF)
        {
            const float2 uv = intersection_uv(surface, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics());
            Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + surface.baseColorTextureIndex, 0));
            if (baseColorTex.SampleLevel(samp, uv, RayAlphaTestLod).a * surface.baseColorFactor.a < surface.alphaCutoff)
                continue;
        }
        query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return false;
    instanceSlot = query.CommittedRayInstanceCustomIndex();
    primitive = query.CommittedPrimitiveIndex();
    barycentrics = query.CommittedTriangleBarycentrics();
    hitT = query.CommittedRayT();
    return true;
}

// The world-space normal of a hit, interpolated from its vertices.
float3 rayHitNormal(GpuRayTracingInstance surface, uint primitive, float2 barycentrics)
{
    uint i0 = primitive * 3 + 0, i1 = primitive * 3 + 1, i2 = primitive * 3 + 2;
    if (surface.indexBufferAddress != 0)
    {
        uint* indices = (uint*)surface.indexBufferAddress;
        i0 = indices[i0];
        i1 = indices[i1];
        i2 = indices[i2];
    }
    const float3 local = LoadRayTracingVertex(surface, i0).normal * (1.0 - barycentrics.x - barycentrics.y) +
                         LoadRayTracingVertex(surface, i1).normal * barycentrics.x + LoadRayTracingVertex(surface, i2).normal * barycentrics.y;
    return normalize(mul((float3x3)surface.normalMatrix, local));
}

// What is seen through the glass at `position`, like the path tracer sees it for a smooth surface. A thin-walled material
// (thickness 0) passes the view ray on unbent. A volume (KHR_materials_volume) bends it in here and out where it leaves the
// mesh -- the glass's own far side, found with a second ray that only sees this instance (total internal reflection mirrors it
// back instead); a mesh that is not closed falls back to a slab of the given thickness (out parallel to the way in). The ray
// then goes on through the scene, past other see-through surfaces; a miss is the environment.
float3 tracedRefraction(UniformBufferObject* ubo, InstanceData instance, uint selfSlot, float3 position, float3 n, float3 v, float ior, float modelScale)
{
    const float bias = 0.002 * ubo->unitsPerMeter;
    const float maxDistance = 1000.0 * ubo->unitsPerMeter;
    float3 origin = position - v * bias;
    float3 direction = -v;
    // What the volume absorbs on the way through it (KHR_materials_volume attenuation, its distance scaled with the model).
    float3 absorbed = float3(1.0);

    if (instance.thickness > 0.0)
    {
        float3 inside = refract(-v, n, 1.0 / ior);
        inside = dot(inside, inside) > 1e-6 ? normalize(inside) : -v;
        const float3 entry = position - n * bias;

        uint slot, primitive;
        float2 barycentrics;
        float hitT;
        if (traceRefractionRay(ubo, entry, inside, maxDistance, int(selfSlot), slot, primitive, barycentrics, hitT))
        {
            GpuRayTracingInstance* rtInstances = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
            float3 outward = rayHitNormal(rtInstances[slot], primitive, barycentrics);
            if (dot(outward, inside) < 0.0)
                outward = -outward;
            const float3 exitPoint = entry + inside * hitT;
            absorbed = volume_transmittance(instance.attenuationColor, instance.attenuationDistance * modelScale, hitT);
            float3 outside = refract(inside, -outward, ior);
            direction = dot(outside, outside) > 1e-6 ? normalize(outside) : reflect(inside, -outward);
            origin = exitPoint + direction * bias;
        }
        else
        {
            const float slab = instance.thickness * modelScale / max(dot(inside, -n), 0.1);
            origin = entry + inside * slab - v * bias;
            absorbed = volume_transmittance(instance.attenuationColor, instance.attenuationDistance * modelScale, slab);
        }
    }

    uint slot, primitive;
    float2 barycentrics;
    float hitT;
    if (traceRefractionRay(ubo, origin, direction, maxDistance, -1, slot, primitive, barycentrics, hitT))
        return shadeRayHit(ubo, slot, primitive, barycentrics, origin, direction, hitT) * absorbed;

    if (ubo->prefilteredMapIndex == 0xFFFFFFFF)
        return float3(0.0);
    TextureCube environment = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->prefilteredMapIndex, 0));
    SamplerState samplerPrefiltered = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));
    return environment.SampleLevel(samplerPrefiltered, direction, 0.0).rgb * ubo->scaleIBLAmbient * absorbed;
}

float3 getNormal(UniformBufferObject* ubo, InstanceData instance, VSOutput vertIn, SamplerState samplerState)
{
    Texture2D normalTexture = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.normalTextureIndex, 0));
    float3 tangentNormal = normalTexture.SampleBias(samplerState, instance.normalTextureSet == 0 ? vertIn.uv0 : vertIn.uv1, ubo->textureLodBias).xyz * 2.0 - 1.0;

    float3 q1 = ddx(vertIn.worldPos);
    float3 q2 = ddy(vertIn.worldPos);
    float2 st1 = ddx(vertIn.uv0);
    float2 st2 = ddy(vertIn.uv0);

    float3 N = normalize(vertIn.normal);
    float3 T = normalize(q1 * st2.y - q2 * st1.y);
    float3 B = normalize(cross(N, T));
    float3x3 TBN = float3x3(T, B, N);

    return normalize(mul(tangentNormal, TBN));
}

float convertMetallic(float3 diffuseCol, float3 specularCol, float maxSpecular)
{
    float perceivedDiffuse = sqrt(0.299 * diffuseCol.r * diffuseCol.r + 0.587 * diffuseCol.g * diffuseCol.g + 0.114 * diffuseCol.b * diffuseCol.b);
    float perceivedSpecular = sqrt(0.299 * specularCol.r * specularCol.r + 0.587 * specularCol.g * specularCol.g + 0.114 * specularCol.b * specularCol.b);
    if (perceivedSpecular < c_MinRoughness)
    {
        return 0.0;
    }
    float a = c_MinRoughness;
    float b = perceivedDiffuse * (1.0 - maxSpecular) / (1.0 - c_MinRoughness) + perceivedSpecular - 2.0 * c_MinRoughness;
    float c = c_MinRoughness - perceivedSpecular;
    float D = max(b * b - 4.0 * a * c, 0.0);
    return clamp((-b + sqrt(D)) / (2.0 * a), 0.0, 1.0);
}

struct FSOutput
{
    float4 color : SV_TARGET0;
    int entityID : SV_TARGET1;
};

struct FSOutputOIT
{
    float4 accum : SV_TARGET0;
    float reveal : SV_TARGET1;
    int entityID : SV_TARGET2;
};

// layerOpacity: DLSS-RR's transparency layer only (MESHLET_TRANSPARENCY_LAYER) -- how much of the scene behind it the surface hides,
// per channel (1 - what it lets through, tinted by its base color); negative otherwise.
FSOutput shadeTransparent(VSOutput vertIn, bool isFrontFace, PushConstantMeshlets pushConstant, out float3 layerOpacity);

[shader("fragment")]
FSOutput fragMain(VSOutput vertIn, bool isFrontFace : SV_IsFrontFace, uniform PushConstantMeshlets pushConstant)
{
    float3 layerOpacity;
    return shadeTransparent(vertIn, isFrontFace, pushConstant, layerOpacity);
}

// DLSS-RR's transparency layer (Renderer::addRRTransparencyLayerPass): the color with one opacity for blending the glass over
// glass, and the opacity per channel (Streamline kBufferTypeTransparencyLayerOpacity) -- a tinted pane (the wall lamps' warm glass)
// tints the scene behind it, as the path tracer has it; one channel lost the tint.
struct FSOutputLayer
{
    float4 color : SV_TARGET0;
    int entityID : SV_TARGET1;
    float4 opacity : SV_TARGET2;
};

[shader("fragment")]
FSOutputLayer fragMainLayer(VSOutput vertIn, bool isFrontFace : SV_IsFrontFace, uniform PushConstantMeshlets pushConstant)
{
    float3 layerOpacity;
    const FSOutput shaded = shadeTransparent(vertIn, isFrontFace, pushConstant, layerOpacity);
    FSOutputLayer output;
    output.color = shaded.color;
    output.entityID = shaded.entityID;
    output.opacity = float4(all(layerOpacity >= 0.0) ? layerOpacity : float3(shaded.color.a), 1.0);
    return output;
}

// Order-independent transparency (Renderer::setOITEnabled): the same surface into the weighted blended targets.
[shader("fragment")]
FSOutputOIT fragMainOIT(VSOutput vertIn, bool isFrontFace : SV_IsFrontFace, uniform PushConstantMeshlets pushConstant)
{
    float3 layerOpacity;
    const FSOutput shaded = shadeTransparent(vertIn, isFrontFace, pushConstant, layerOpacity);
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    const OITOutput oit = oit_output(shaded.color, length(vertIn.worldPos - ubo->cameraWorldPos.xyz) / ubo->unitsPerMeter);
    FSOutputOIT output;
    output.accum = oit.accum;
    output.reveal = oit.reveal;
    output.entityID = shaded.entityID;
    return output;
}

FSOutput shadeTransparent(VSOutput vertIn, bool isFrontFace, PushConstantMeshlets pushConstant, out float3 layerOpacity)
{
    FSOutput output;
    layerOpacity = float3(-1.0);

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    SceneInstances instances = SceneInstances(ubo);
    InstanceData instance = instances[vertIn.drawID];

    SamplerState sampLinearHandle = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

    // Texture streaming: transparent surfaces never reach the G-buffer, so they report here (hardware derivatives).
    if (ubo->mipFeedbackReference != 0 && isMipFeedbackPixel(int2(vertIn.pos.xy), ubo->mipFeedbackFrame))
    {
        const float2 ddxUV0 = ddx(vertIn.uv0), ddyUV0 = ddy(vertIn.uv0);
        const float2 ddxUV1 = ddx(vertIn.uv1), ddyUV1 = ddy(vertIn.uv1);
        reportMipFeedback(ubo, instance.baseColorTextureIndex, instance.baseColorTextureSet == 0 ? ddxUV0 : ddxUV1, instance.baseColorTextureSet == 0 ? ddyUV0 : ddyUV1);
        reportMipFeedback(ubo, instance.metallicRoughnessTextureIndex, instance.physicalDescriptorTextureSet == 0 ? ddxUV0 : ddxUV1, instance.physicalDescriptorTextureSet == 0 ? ddyUV0 : ddyUV1);
        reportMipFeedback(ubo, instance.normalTextureIndex, instance.normalTextureSet == 0 ? ddxUV0 : ddxUV1, instance.normalTextureSet == 0 ? ddyUV0 : ddyUV1);
        reportMipFeedback(ubo, instance.occlusionTextureIndex, instance.occlusionTextureSet == 0 ? ddxUV0 : ddxUV1, instance.occlusionTextureSet == 0 ? ddyUV0 : ddyUV1);
        reportMipFeedback(ubo, instance.emissiveTextureIndex, instance.emissiveTextureSet == 0 ? ddxUV0 : ddxUV1, instance.emissiveTextureSet == 0 ? ddyUV0 : ddyUV1);
        reportMipFeedback(ubo, instance.transmissionTextureIndex, instance.transmissionTextureSet == 0 ? ddxUV0 : ddxUV1, instance.transmissionTextureSet == 0 ? ddyUV0 : ddyUV1);
    }

    float perceptualRoughness = 1.0;
    float metallic = 0.0;
    float4 baseColor = float4(1.0);
    float3 f0 = float3(0.04);

    if (instance.alphaMode == 1) // Mask
    {
        float4 maskBaseColor = instance.baseColorFactor;
        if (instance.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
            maskBaseColor = baseColorTex.SampleBias(sampLinearHandle, instance.baseColorTextureSet == 0 ? vertIn.uv0 : vertIn.uv1, ubo->textureLodBias) * instance.baseColorFactor;
        }
        if (maskBaseColor.a < instance.alphaMaskCutoff)
        {
            discard;
        }
    }

    if (instance.workflow == PBR_WORKFLOW_METALLIC_ROUGHNESS)
    {
        perceptualRoughness = instance.roughnessFactor;
        metallic = instance.metallicFactor;
        if (instance.metallicRoughnessTextureIndex != 0xFFFFFFFF)
        {
            Texture2D mrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.metallicRoughnessTextureIndex, 0));
            float4 mrSample = mrTex.SampleBias(sampLinearHandle, instance.physicalDescriptorTextureSet == 0 ? vertIn.uv0 : vertIn.uv1, ubo->textureLodBias);
            perceptualRoughness *= mrSample.g;
            metallic *= mrSample.b;
        }
        perceptualRoughness = clamp(perceptualRoughness, c_MinRoughness, 1.0);
        metallic = clamp(metallic, 0.0, 1.0);

        if (instance.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
            baseColor = baseColorTex.SampleBias(sampLinearHandle, instance.baseColorTextureSet == 0 ? vertIn.uv0 : vertIn.uv1, ubo->textureLodBias) * instance.baseColorFactor;
        }
        else
        {
            baseColor = instance.baseColorFactor;
        }
    }
    else // PBR_WORKFLOW_SPECULAR_GLOSSINESS
    {
        // The factor is the baseline (a material with no specular-glossiness texture at all -- e.g. Bistro's flat-color
        // decals -- authors its roughness through glossinessFactor alone); a texture, where present, modulates it. This
        // used to default perceptualRoughness to a hardcoded 0.0 (mirror-smooth) and only ever replace it, never
        // combine it with the factor, so any material without a texture (glossinessFactor 0.0 = fully rough, matte)
        // rendered as a perfect mirror regardless of what it actually authored -- Bistro's dirt/decal walls reflecting
        // the environment instead of looking like flat dirty concrete, the one thing every one of them has in common.
        float glossiness = instance.specularFactor.a;
        float3 specular = instance.specularFactor.rgb;
        if (instance.metallicRoughnessTextureIndex != 0xFFFFFFFF)
        {
            // Imported as SRGBA8 (EditorAssetManager::ImportMeshTextures): RGB (specular color) is hardware sRGB-decoded
            // on sample, alpha (glossiness) is always left linear by that format -- decoding either manually here on top
            // would be a double decode.
            Texture2D specGlossTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.metallicRoughnessTextureIndex, 0));
            float4 specGlossSample = specGlossTex.SampleBias(sampLinearHandle, instance.physicalDescriptorTextureSet == 0 ? vertIn.uv0 : vertIn.uv1, ubo->textureLodBias);
            glossiness *= specGlossSample.a;
            specular *= specGlossSample.rgb;
        }
        perceptualRoughness = clamp(1.0 - glossiness, 0.0, 1.0);

        const float epsilon = 1e-6;
        float4 diffuseSample = instance.diffuseFactor;
        if (instance.baseColorTextureIndex != 0xFFFFFFFF)
        {
            // Imported as SRGBA8 (EditorAssetManager::ImportMeshTextures: importTexture(BaseColorTexturePath, true)),
            // already hardware sRGB-decoded on sample -- decoding it again here was a double decode.
            Texture2D colorMap = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
            diffuseSample = colorMap.SampleBias(sampLinearHandle, vertIn.uv0, ubo->textureLodBias) * instance.diffuseFactor;
        }

        float maxSpecular = max(max(specular.r, specular.g), specular.b);
        metallic = convertMetallic(diffuseSample.rgb, specular, maxSpecular);

        float3 baseColorDiffusePart = diffuseSample.rgb * ((1.0 - maxSpecular) / (1.0 - c_MinRoughness) / max(1.0 - metallic, epsilon));
        float3 baseColorSpecularPart = specular - (float3(c_MinRoughness) * (1.0 - metallic) * (1.0 / max(metallic, epsilon)));
        baseColor = float4(lerp(baseColorDiffusePart, baseColorSpecularPart, metallic * metallic), diffuseSample.a);
    }

    float transmission = instance.transmissionFactor;
    if (instance.transmissionTextureIndex != 0xFFFFFFFF)
    {
        Texture2D transTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.transmissionTextureIndex, 0));
        transmission *= transTex.SampleBias(sampLinearHandle, instance.transmissionTextureSet == 0 ? vertIn.uv0 : vertIn.uv1, ubo->textureLodBias).r;
    }
    // Opaque and masked surfaces drawn with this shader (the editor's extra views have no G-buffer, Renderer::addExtraViewPasses):
    // nothing shows through, whatever the base color's alpha or the transmission says.
    if ((pushConstant.meshletCulling & MESHLET_SHADE_OPAQUE) != 0)
    {
        transmission = 0.0;
        baseColor.a = 1.0;
    }
    // Refraction (Hybrid_Renderer_Plan_2026-10.md §4 H3b) when the pass has the lit scene behind it: the transmitted part comes from
    // there, bent and blurred (below), and the surface covers what is behind it by its alpha alone. Without it, transmission only
    // lets the background through unbent (lower alpha).
    const bool refraction = transmission > 0.0 && pushConstant.refractionTextureIndex != 0xFFFFFFFF;
    // A blended surface is there by its alpha (coverage), all of it -- its reflection too, as the path tracer takes alpha (a decal's
    // clear parts reflect nothing; they used to add the sky's reflection over the wall, a pale veil). Applied once to the result
    // (premultiplied output). What transmission leaves is the surface's diffuse share.
    const float coverage = instance.alphaMode == 2 ? baseColor.a : 1.0;
    const float diffuseAlpha = 1.0 - transmission;

    float3 diffuseColor = baseColor.rgb * (1.0 - metallic) * diffuseAlpha;
    float3 specularColor = lerp(f0, baseColor.rgb, metallic);
    float alphaRoughness = perceptualRoughness * perceptualRoughness;
    float reflectance = max(max(specularColor.r, specularColor.g), specularColor.b);
    float3 specularEnvironmentR0 = specularColor;
    float3 specularEnvironmentR90 = clamp(float3(reflectance * 25.0), 0.0, 1.0);

    float3 n = (instance.normalTextureIndex != 0xFFFFFFFF) ? getNormal(ubo, instance, vertIn, sampLinearHandle) : normalize(vertIn.normal);
    if (instance.doubleSided != 0 && !isFrontFace)
    {
        n = -n;
    }
    float3 v = normalize(ubo->cameraWorldPos.xyz - vertIn.worldPos);
    // Glass has no back: seen through from either side, its normal faces the viewer (as the path tracer turns a hit's normal to
    // the ray). A pane modeled facing away -- the Bistro's picture cover glass faces the picture -- was shaded as seen edge-on:
    // Fresnel 1, a perfect mirror of the room, the picture behind it gone.
    if (transmission > 0.0 && dot(instance.doubleSided != 0 && !isFrontFace ? -vertIn.normal : vertIn.normal, v) < 0.0)
        n = -n;
    // Specular anti-aliasing (Tokuyoshi & Kaplanyan 2019, "Improved Geometric Specular Antialiasing"): the roughness widened by how
    // much the normal turns across the pixel. A smooth normal-mapped pane (the street lanterns' ribbed glass) turned its reflection
    // rays a long way within a pixel, and which of them met a string light's tiny bulb changed with the camera's sub-pixel jitter:
    // single-pixel highlights that flickered, under DLSS-RR's transparency layer and over the scene alike. (Whether glass is smooth
    // enough to tint what is behind it without the color pyramid's blur stays the material's own roughness.)
    const float materialRoughness = perceptualRoughness;
    {
        const float3 dndx = ddx(n);
        const float3 dndy = ddy(n);
        const float kernelVariance = 0.25 * (dot(dndx, dndx) + dot(dndy, dndy));
        alphaRoughness = sqrt(saturate(alphaRoughness * alphaRoughness + min(2.0 * kernelVariance, 0.18)));
        perceptualRoughness = sqrt(alphaRoughness);
    }
    float3 reflection = normalize(reflect(-v, n));
    float NdotV = clamp(dot(n, v), 0.001, 1.0);

    // The glass's first pass (MESHLET_GLASS_MULTIPLY): what the pane lets through of the scene behind it, multiplied into it -- the
    // transmission past the Fresnel reflection, through the base color, where it covers the pixel. Glass that bends or blurs the view
    // (a volume, roughness: the color pyramid) lets nothing through here; its second pass adds what it shows. Smooth: up to the
    // minimum roughness the metallic-roughness path clamps to (c_MinRoughness, 0.04) and a little -- below 0.02 no glass ever was,
    // and every pane took the one-tint path. Under DLSS-RR (MESHLET_TRANSPARENCY_LAYER, into RR's input color): the base color's
    // tint alone, every pane's, whatever it bends; the Fresnel share is the transparency layer's opacity.
    const bool stackedGlass = refraction && instance.thickness <= 0.0 && materialRoughness <= c_MinRoughness + 0.01;
    if ((pushConstant.meshletCulling & MESHLET_GLASS_MULTIPLY) != 0)
    {
        const float3 fresnel = specularEnvironmentR0 + (1.0 - specularEnvironmentR0) * pow(1.0 - NdotV, 5.0);
        const float3 tint = saturate(baseColor.rgb * (transmission * (1.0 - metallic)));
        const float3 throughput = (pushConstant.meshletCulling & MESHLET_TRANSPARENCY_LAYER) != 0 ? tint
                                  : (stackedGlass ? tint * (1.0 - fresnel) : float3(0.0));
        output.color = float4(lerp(float3(1.0), throughput, coverage), 1.0);
        output.entityID = vertIn.entityID;
        return output;
    }

    // Direct analytic lights, the same evaluateDirectLight the deferred opaque path uses. The strongest ShadowedLightCount of them
    // get a shadow ray once all are summed (see file header).
    float3 directLighting = float3(0.0);
    LightData* lights = (LightData*)ubo->lightDataReference;
    const bool traceShadows = ubo->enableRTShadows != 0 && ubo->tlasDeviceAddress != 0;
    float3 shadowedContribution[ShadowedLightCount];
    float3 shadowedDirection[ShadowedLightCount];
    float shadowedDistance[ShadowedLightCount];
    float shadowedWeight[ShadowedLightCount];
    for (uint slot = 0; slot < ShadowedLightCount; ++slot)
    {
        shadowedContribution[slot] = float3(0.0);
        shadowedDirection[slot] = float3(0.0, 1.0, 0.0);
        shadowedDistance[slot] = 0.0;
        shadowedWeight[slot] = 0.0;
    }

    for (uint i = 0; i < ubo->lightCount; i++)
    {
        LightData light = lights[i];
        uint lightType = (uint)light.position.w;

        float3 l;
        float attenuation = 1.0;
        float lightDistance = 100000.0 * ubo->unitsPerMeter; // a directional light: as far as a shadow ray needs to go
        float3 contribution;

        if (is_area_light(lightType))
        {
            // Rect: exact diffuse, representative-point specular (PBRLighting.slang); its shadow ray to a point of it that moves
            // with the pixel and the frame (a soft shadow's grain the temporal upscale smooths).
            PBRInfo rectInputs = PBRInfo(1.0, NdotV, 1.0, 1.0, 1.0, perceptualRoughness, metallic, specularEnvironmentR0,
                                         specularEnvironmentR90, alphaRoughness, diffuseColor, specularColor);
            contribution = evaluateRectLight(rectInputs, light, vertIn.worldPos, n, v);
            if (!any(contribution > 0.0))
                continue;
            const float2 u = frac(float2(52.9829189 * frac(0.06711056 * vertIn.pos.x + 0.00583715 * vertIn.pos.y),
                                         0.7548776662 * float(ubo->mipFeedbackFrame % 64u) + 0.5698402910 * (vertIn.pos.x + vertIn.pos.y)));
            const LightPointSample sample = sample_light_point(light, vertIn.worldPos, u);
            l = sample.direction;
            lightDistance = sample.distance;
        }
        else
        {
            if (lightType == 0)
            {
                l = normalize(light.direction.xyz);
                attenuation = cloud_shadow(ubo, vertIn.worldPos, i, l);
            }
            else
            {
                // Inverse square held at the source radius, range window, cone (shaderIO.h).
                float3 lightVec = light.position.xyz - vertIn.worldPos;
                float distance = length(lightVec);
                lightDistance = distance;
                l = distance > 0.0001 ? lightVec / distance : float3(0.0, 1.0, 0.0);
                attenuation = light_distance_attenuation(light, distance) * light_spot_attenuation(light, l);
            }

            if (attenuation <= 0.0)
            {
                continue;
            }

            float NdotL = clamp(dot(n, l), 0.001, 1.0);
            if (dot(n, l) <= 0.0)
            {
                continue;
            }

            float3 h = normalize(l + v);
            PBRInfo pbrInputs = PBRInfo(
                NdotL, NdotV, clamp(dot(n, h), 0.0, 1.0), clamp(dot(l, h), 0.0, 1.0), clamp(dot(v, h), 0.0, 1.0),
                perceptualRoughness, metallic,
                specularEnvironmentR0, specularEnvironmentR90, alphaRoughness,
                diffuseColor, specularColor
            );

            float3 lightRadiance = light.color.rgb * light.color.w * attenuation;
            contribution = evaluateDirectLight(pbrInputs, lightRadiance);
        }
        directLighting += contribution;

        if (traceShadows)
        {
            float weight = dot(contribution, float3(0.2126, 0.7152, 0.0722));
            uint weakest = 0;
            for (uint slot = 1; slot < ShadowedLightCount; ++slot)
            {
                if (shadowedWeight[slot] < shadowedWeight[weakest])
                    weakest = slot;
            }
            if (weight > shadowedWeight[weakest])
            {
                shadowedContribution[weakest] = contribution;
                shadowedDirection[weakest] = l;
                shadowedDistance[weakest] = lightDistance;
                shadowedWeight[weakest] = weight;
            }
        }
    }

    // A light hidden from this point takes its share back out. The ray leaves a little above the surface (the normal faces every
    // light that was summed: the loop skips the others).
    if (traceShadows)
    {
        float3 shadowOrigin = vertIn.worldPos + n * (0.01 * ubo->unitsPerMeter);
        for (uint slot = 0; slot < ShadowedLightCount; ++slot)
        {
            if (shadowedWeight[slot] > 0.0 &&
                forwardShadowed(ubo, shadowOrigin, shadowedDirection[slot], max(shadowedDistance[slot] - 0.01 * ubo->unitsPerMeter, 0.0)))
                directLighting -= shadowedContribution[slot];
        }
        directLighting = max(directLighting, float3(0.0));
    }

    PBRInfo iblInputs = PBRInfo(
        1.0, NdotV, 1.0, 1.0, 1.0,
        perceptualRoughness, metallic,
        specularEnvironmentR0, specularEnvironmentR90, alphaRoughness,
        diffuseColor, specularColor
    );
    // Read ahead of getIBLContribution: it needs this for the specular occlusion approximation (see
    // iblSpecularOcclusion in PBRLighting.slang), not just to darken the final color the way it used to be used below.
    // Combined with SSAO/RTAO (whichever ran this frame -- set by Renderer::addForward3DPass, after the opaque
    // G-buffer path's own occlusion pass; this shader has no G-buffer to read it from the way DeferredLighting.slang
    // does): a material's own baked AO map is per-material detail no screen-space pass will ever pick up (grout
    // lines in a tile texture); SSAO/RTAO is contact/geometric occlusion no baked texture accounts for either.
    float ao = 1.0;
    if (instance.occlusionTextureIndex != 0xFFFFFFFF)
    {
        Texture2D occlusionTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.occlusionTextureIndex, 0));
        ao = occlusionTex.SampleBias(sampLinearHandle, instance.occlusionTextureSet == 0 ? vertIn.uv0 : vertIn.uv1, ubo->textureLodBias).r;
    }
    // The screen-space AO at this pixel is the opaque surface's behind it: right for a decal lying on that surface, wrong for glass
    // or anything standing in front of it (the background's occlusion, noisy and sliding as the camera moves). Only where this
    // surface is the one the pixel sees (the screen-probe GI's distance says so; without it, only what lets no light through).
    if (pushConstant.ssaoTextureIndex != 0xFFFFFFFF && transmission <= 0.0)
    {
        float3 unusedIrradiance;
        const bool onSurfaceBehind = ubo->screenGIDistanceTextureIndex == 0xFFFFFFFF || sample_screen_gi(ubo, vertIn.worldPos, unusedIrradiance);
        if (onSurfaceBehind)
        {
            Texture2D<float> ssaoTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.ssaoTextureIndex, 0));
            ao *= ssaoTex.Load(int3(int2(vertIn.pos.xy), 0)).r;
        }
    }

    IBLResult ibl = getIBLContribution(ubo, iblInputs, n, reflection, ao);
    float3 ambientDiffuse = ibl.diffuse;
    float3 ambientSpecular = ibl.specular;

    // With a GI technique running: the scene's diffuse light here (SceneGI.slang -- a decal gets exactly its wall's screen-probe
    // GI, glass the DDGI cascades), and the sky's reflection only as far as the sky reaches, as the deferred lighting does.
    float3 sceneIrradiance;
    if ((ubo->screenGITextureIndex != 0xFFFFFFFF || ubo->enableDDGI != 0) && sample_scene_gi(ubo, vertIn.worldPos, n, normalize(vertIn.normal), v, sceneIrradiance))
    {
        ambientDiffuse = sceneIrradiance * diffuseColor;
        ambientSpecular *= scene_gi_specular_occlusion(ubo, sceneIrradiance, n);
    }

    // Smooth glass with ray traced glass on: its reflection traced like its refraction (the room around a jar on a counter, not
    // the sky the environment map holds), fading into the occluded environment reflection with roughness.
    float tracedReflectionWeight = 0.0;
    if ((pushConstant.meshletCulling & MESHLET_RT_REFRACTION) != 0 && ubo->tlasDeviceAddress != 0 && transmission > 0.0)
        tracedReflectionWeight = saturate((RTRefractionMaxRoughness - perceptualRoughness) / (RTRefractionMaxRoughness - RTRefractionFullRoughness));
    if (tracedReflectionWeight > 0.0 && ubo->brdfLutIndex != 0xFFFFFFFF)
    {
        const float3 reflected = normalize(reflect(-v, n));
        const float3 origin = vertIn.worldPos + n * (0.002 * ubo->unitsPerMeter);
        uint slot, primitive;
        float2 barycentrics;
        float hitT;
        float3 radiance;
        if (traceRefractionRay(ubo, origin, reflected, 1000.0 * ubo->unitsPerMeter, -1, slot, primitive, barycentrics, hitT))
            radiance = shadeRayHit(ubo, slot, primitive, barycentrics, origin, reflected, hitT);
        else
        {
            TextureCube environment = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->prefilteredMapIndex, 0));
            radiance = environment.SampleLevel(SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0)), reflected, 0.0).rgb *
                       ubo->scaleIBLAmbient;
        }
        Texture2D brdfTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->brdfLutIndex, 0));
        const float2 brdf = brdfTex.SampleLevel(SamplerState.Handle(uint2(SamplerIndex::SAMPLER_BRDFLUT, 0)), float2(NdotV, 1.0 - perceptualRoughness), 0).rg;
        ambientSpecular = lerp(ambientSpecular, radiance * (specularColor * brdf.x + brdf.y), tracedReflectionWeight);
    }

    float3 color = (directLighting + ambientDiffuse + ambientSpecular) * ao;

    float3 emissive = instance.emissiveFactor.rgb * instance.emissiveStrength;
    if (instance.emissiveTextureIndex != 0xFFFFFFFF)
    {
        Texture2D emissiveTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.emissiveTextureIndex, 0));
        emissive *= emissiveTex.SampleBias(sampLinearHandle, instance.emissiveTextureSet == 0 ? vertIn.uv0 : vertIn.uv1, ubo->textureLodBias).rgb;
    }
    color += emissive;

    // The fog in front of the surface (aerial perspective, height fog) on its own light, and its scattered light where the surface
    // covers what is behind it. The background it lets through (refracted below, or blended) is already fogged at its distance.
    const float4 fog = sceneFogTerms(ubo, vertIn.worldPos);
    color = color * fog.a + fog.rgb * diffuseAlpha;

    if (refraction)
    {
        // What is behind the surface, seen along the refracted ray: through the volume's thickness (KHR_materials_volume, in model
        // units, so scaled with the model; 0 = thin-walled, no bending), blurred by the roughness the IOR leaves (as the Khronos
        // glTF viewer does), tinted by the base color, less what the surface reflects (Fresnel). Rough or thick glass reads a
        // blurrier mip; what is outside the screen is clamped to its edge.
        const float ior = max(instance.ior, 1.0);
        const float4x4 model = instance.modelMatrix;
        const float modelScale = (length(float3(model[0][0], model[1][0], model[2][0])) + length(float3(model[0][1], model[1][1], model[2][1])) +
                                  length(float3(model[0][2], model[1][2], model[2][2]))) / 3.0;

        // Smooth glass on the high rung: traced (H3); rougher glass, or the lower rungs: the color pyramid (H3b), both in between.
        // Thin-walled glass (no volume) does not bend the view ray: what it shows is the lit scene right behind it, which the
        // color pyramid already holds with everything the deferred lighting gives it (GI, reflections, shadows) -- tracing it
        // again would only shade it more simply.
        float tracedWeight = 0.0;
        if ((pushConstant.meshletCulling & MESHLET_RT_REFRACTION) != 0 && ubo->tlasDeviceAddress != 0 && instance.thickness > 0.0)
            tracedWeight = saturate((RTRefractionMaxRoughness - perceptualRoughness) / (RTRefractionMaxRoughness - RTRefractionFullRoughness));

        float3 behind = float3(0.0);
        // The glass's second pass (MESHLET_GLASS_ADD) for a thin smooth pane: its own light alone, what is behind was let through by
        // the first.
        if (stackedGlass && (pushConstant.meshletCulling & MESHLET_GLASS_ADD) != 0)
            tracedWeight = 1.0;
        else if (tracedWeight > 0.0)
            behind = tracedRefraction(ubo, instance, vertIn.drawID, vertIn.worldPos, n, v, ior, modelScale) * tracedWeight;
        if (tracedWeight < 1.0)
        {
            Texture2D refractionSource = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.refractionTextureIndex, 0));
            uint sourceWidth, sourceHeight, sourceLevels;
            refractionSource.GetDimensions(0, sourceWidth, sourceHeight, sourceLevels);

            float3 refracted = refract(-v, n, 1.0 / ior);
            if (dot(refracted, refracted) < 1e-6)
                refracted = -v;
            const float3 exitPoint = vertIn.worldPos + normalize(refracted) * instance.thickness * modelScale;

            // The exit point's offset on screen from the surface's own pixel (the same projection the HDR image was drawn with).
            const float4x4 viewProjection = mul(ubo->proj, ubo->view);
            const float4 surfaceClip = mul(viewProjection, float4(vertIn.worldPos, 1.0));
            const float4 exitClip = mul(viewProjection, float4(exitPoint, 1.0));
            float2 offset = float2(0.0);
            if (surfaceClip.w > 1e-5 && exitClip.w > 1e-5)
                offset = (exitClip.xy / exitClip.w - surfaceClip.xy / surfaceClip.w) * float2(0.5, -0.5);
            const float2 uv = saturate(vertIn.pos.xy / float2(sourceWidth, sourceHeight) + offset);

            float lod = log2(float(max(sourceWidth, sourceHeight))) * perceptualRoughness * saturate(ior * 2.0 - 2.0);
            // Under DLSS Ray Reconstruction the scene behind is its raw ray traced lighting (NRD is off), and RR's guides at a glass
            // pixel describe what is seen through it (RTXPT's convention for transmission). A blur mip of that raw image is noise
            // grown into blotches RR takes for detail -- the glass that stayed noisy. A sharp read leaves the noise where the guides
            // are, for RR to remove; rough glass is a little less blurred.
            if (ubo->rayReconstructionActive != 0)
                lod = min(lod, 1.0);
            SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
            // Through the volume's thickness: what it absorbs on the way (KHR_materials_volume attenuation).
            const float3 absorbed = volume_transmittance(instance.attenuationColor, instance.attenuationDistance * modelScale,
                                                         instance.thickness * modelScale / max(dot(normalize(refracted), -n), 0.1));
            behind += refractionSource.SampleLevel(linearClamp, uv, min(lod, float(sourceLevels - 1))).rgb * absorbed * (1.0 - tracedWeight);
        }

        const float3 fresnel = specularEnvironmentR0 + (1.0 - specularEnvironmentR0) * pow(1.0 - NdotV, 5.0);
        color += behind * baseColor.rgb * (transmission * (1.0 - metallic)) * (1.0 - fresnel);
        output.color = float4(color * coverage, coverage); // added in the glass's second pass (the alpha unused)
        output.entityID = vertIn.entityID;
        return output;
    }

    // DLSS-RR's transparency layer (MESHLET_TRANSPARENCY_LAYER, Renderer::addRRTransparencyLayerPass): the light the surface gives
    // itself, premultiplied, and how much of the scene behind it hides. The scene is already tinted by every pane's base color in
    // RR's input color (the glass's multiply pass, MESHLET_GLASS_MULTIPLY): what is left to hide is the Fresnel reflection's share,
    // per channel in layerOpacity (RR's opacity layer) and averaged in alpha (glass over glass). RR composites it over the scene it
    // denoised: color + (1 - opacity) * scene. The tint itself is not left to RR's compositing: carried in the opacity only, it did
    // not show.
    if ((pushConstant.meshletCulling & MESHLET_TRANSPARENCY_LAYER) != 0)
    {
        const float3 fresnel = specularEnvironmentR0 + (1.0 - specularEnvironmentR0) * pow(1.0 - NdotV, 5.0);
        layerOpacity = coverage * saturate(fresnel);
        output.color = float4(color * coverage, dot(layerOpacity, float3(1.0 / 3.0)));
        output.entityID = vertIn.entityID;
        return output;
    }

    // Linear HDR, not tonemapped - this pipeline writes into the same m_hdrSceneResource as
    // everything else in the forward pass, which is only tonemapped once at the end of the frame.
    // Without the refraction source, the transmitted part lets the background through unbent.
    output.color = float4(color * coverage, coverage * diffuseAlpha);
    output.entityID = vertIn.entityID;

    return output;
}
