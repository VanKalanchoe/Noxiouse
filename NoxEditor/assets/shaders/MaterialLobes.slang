// A material's lobes at one surface point (GBufferLobes, shaderIO.h): its GpuMaterial factors times its textures, as the glTF
// extensions define them (KHR_materials_clearcoat / sheen / specular / iridescence / anisotropy). The G-buffer pass, the forward
// pass and ray hits all evaluate a material's lobes through this, each with its own texture coordinates.

#ifndef MATERIAL_LOBES_SLANG
#define MATERIAL_LOBES_SLANG

#include "shaderIO.h"

// Where a surface point samples its material's textures: both mesh UV sets, with their screen gradients (raster passes) or one mip
// level for every texture (ray hits: lod >= 0). A texture reads them through its UV channel (material_texture_uv).
struct MaterialSampleCoords
{
    GpuMaterial* material;
    float2 uv[2];
    float2 ddx[2];
    float2 ddy[2];
    float lod;
    float footprint; // > 0: a ray cone's width in mesh UV units (the path tracer): each texture's mip from it and its size
    float lodBias;   // (with a footprint)
};

float4 material_sample(UniformBufferObject* ubo, uint textureIndex, int set, MaterialSampleCoords coords, SamplerState samplerState)
{
    const float2 uv = material_texture_uv(coords.material, set, coords.uv[0], coords.uv[1]);
    if (coords.footprint > 0.0)
    {
        uint width, height;
        MaterialTextureSize(ubo, textureIndex, width, height);
        const float lod = log2(max(coords.footprint * material_texture_uv_scale(coords.material, set), 1e-20)) +
                          0.5 * log2(float(max(width * height, 1u))) + coords.lodBias;
        return SampleMaterialTextureLevel(ubo, textureIndex, samplerState, uv, max(lod, 0.0));
    }
    if (coords.lod >= 0.0)
        return SampleMaterialTextureLevel(ubo, textureIndex, samplerState, uv, coords.lod);
    return SampleMaterialTextureGrad(ubo, textureIndex, samplerState, uv, material_texture_uv_delta(coords.material, set, coords.ddx[0], coords.ddx[1]),
                                     material_texture_uv_delta(coords.material, set, coords.ddy[0], coords.ddy[1]));
}

// A texture's value where the material has one, else `fallback` (the spec's "a missing texture reads 1").
float4 material_sample_or(UniformBufferObject* ubo, uint textureIndex, int set, MaterialSampleCoords coords, SamplerState samplerState, float4 fallback)
{
    return textureIndex == 0xFFFFFFFF ? fallback : material_sample(ubo, textureIndex, set, coords, samplerState);
}

// A shading normal (normal-mapped) that faces away from the viewer bent back to just facing it: such a normal sees nothing (its
// Fresnel at 1, the view below its horizon), and a coat over it covered everything with black. The bend is continuous, as Unreal's
// normal correction for reflections.
float3 lobe_facing_normal(float3 n, float3 v)
{
    const float NdotV = dot(n, v);
    return NdotV < 0.01 ? normalize(n + v * (0.01 - NdotV)) : n;
}

// Dispersion (KHR_materials_dispersion): the red, green and blue IORs of a material whose green (its own) IOR is `ior`, spread by its
// dispersion (20 / Abbe number) as the spec gives it: half of (ior - 1) / Abbe either side of green, red never below 1.
float3 dispersion_iors(float ior, float dispersion)
{
    const float halfSpread = (ior - 1.0) * 0.025 * dispersion;
    return float3(max(ior - halfSpread, 1.0), ior, ior + halfSpread);
}

// The lobes of `material` at a surface point. `geometricNormal` is the interpolated vertex normal (the coat's, without a coat normal
// map), `tangent` / `tangentSign` the vertex tangent frame (zero length: the mesh has none, so no anisotropy and no coat normal map),
// `backFace` a double-sided surface seen from behind (its normals flipped, as the base's). `anisotropicT` is the world direction the
// highlights stretch along (with MaterialLobeAnisotropy).
GBufferLobes evaluate_material_lobes(UniformBufferObject* ubo, GpuMaterial* material, MaterialSampleCoords coords, SamplerState samplerState,
                                     float3 geometricNormal, float3 tangent, float tangentSign, bool backFace, out float3 anisotropicT)
{
    GBufferLobes lobes = gbuffer_default_lobes();
    anisotropicT = float3(0.0);
    const uint flags = material.lobes;
    lobes.flags = flags;
    if (flags == 0u)
        return lobes;

    // The vertex tangent frame (as the base's normal mapping builds it), when the mesh has tangents.
    const float3 N = geometricNormal;
    const bool hasTangents = dot(tangent, tangent) > 1e-12;
    float3 T = float3(1.0, 0.0, 0.0);
    float3 B = float3(0.0, 1.0, 0.0);
    if (hasTangents)
    {
        T = normalize(tangent - N * dot(N, tangent));
        B = cross(N, T) * tangentSign;
    }

    if ((flags & MaterialLobeSpecular) != 0u)
    {
        // KHR_materials_specular with KHR_materials_ior: the IOR's F0 tinted by the specular color (at most 1), and the weight.
        const float iorF0 = ((material.ior - 1.0) / (material.ior + 1.0)) * ((material.ior - 1.0) / (material.ior + 1.0));
        const float3 color = material.specularColorFactor *
                             material_sample_or(ubo, material.specularColorTextureIndex, material.specularColorTextureSet, coords, samplerState, float4(1.0)).rgb;
        lobes.dielectricF0 = min(iorF0 * color, float3(1.0));
        lobes.specularWeight = material.specularStrength *
                               material_sample_or(ubo, material.specularStrengthTextureIndex, material.specularStrengthTextureSet, coords, samplerState, float4(1.0)).a;
    }

    if ((flags & MaterialLobeClearcoat) != 0u)
    {
        lobes.coat = material.clearcoatFactor *
                     material_sample_or(ubo, material.clearcoatTextureIndex, material.clearcoatTextureSet, coords, samplerState, float4(1.0)).r;
        lobes.coatRoughness = material.clearcoatRoughnessFactor *
                              material_sample_or(ubo, material.clearcoatRoughnessTextureIndex, material.clearcoatRoughnessTextureSet, coords, samplerState, float4(1.0)).g;
        // Without a coat normal map the coat is flat: the base's normal map stays below it.
        float3 coatNormal = N;
        if (material.clearcoatNormalTextureIndex != 0xFFFFFFFF && hasTangents)
        {
            const float3 texel = material_sample(ubo, material.clearcoatNormalTextureIndex, material.clearcoatNormalTextureSet, coords, samplerState).xyz;
            float3 tangentNormal;
            tangentNormal.xy = texel.xy * 2.0 - 1.0;
            // A two-channel (BC5) normal map rebuilds its z, as the base's does.
            tangentNormal.z = texel.z <= 0.0 ? sqrt(saturate(1.0 - dot(tangentNormal.xy, tangentNormal.xy))) : abs(texel.z * 2.0 - 1.0);
            tangentNormal.xy *= material.clearcoatNormalScale; // the coat normal map's strength (its textureInfo scale)
            coatNormal = normalize(T * tangentNormal.x + B * tangentNormal.y + N * tangentNormal.z);
        }
        lobes.coatNormal = backFace ? -coatNormal : coatNormal;
        if (lobes.coat <= 0.0)
            lobes.flags &= ~MaterialLobeClearcoat;
    }

    if ((flags & MaterialLobeSheen) != 0u)
    {
        const float4 colorTexel = material_sample_or(ubo, material.sheenColorTextureIndex, material.sheenColorTextureSet, coords, samplerState, float4(1.0));
        lobes.sheenColor = material.sheenColorFactor * colorTexel.rgb;
        lobes.sheenRoughness = material.sheenRoughnessFactor *
                               material_sample_or(ubo, material.sheenRoughnessTextureIndex, material.sheenRoughnessTextureSet, coords, samplerState, float4(1.0)).a;
        if (max(max(lobes.sheenColor.r, lobes.sheenColor.g), lobes.sheenColor.b) <= 0.0)
            lobes.flags &= ~MaterialLobeSheen;
    }

    if ((flags & MaterialLobeIridescence) != 0u)
    {
        lobes.iridescence = material.iridescenceFactor *
                            material_sample_or(ubo, material.iridescenceTextureIndex, material.iridescenceTextureSet, coords, samplerState, float4(1.0)).r;
        lobes.iridescenceIor = material.iridescenceIor;
        // From the minimum (texture 0) to the maximum (texture 1); without a texture the maximum.
        const float thickness = material_sample_or(ubo, material.iridescenceThicknessTextureIndex, material.iridescenceThicknessTextureSet,
                                                   coords, samplerState, float4(1.0)).g;
        lobes.iridescenceThickness = lerp(material.iridescenceThicknessMinimum, material.iridescenceThicknessMaximum, thickness);
        if (lobes.iridescence <= 0.0)
            lobes.flags &= ~MaterialLobeIridescence;
    }

    if ((flags & MaterialLobeAnisotropy) != 0u)
    {
        // The direction in tangent space (the texture's RG in [-1, 1], else +X), turned by the rotation; the strength times the
        // texture's B. A mesh without tangents has no direction for it.
        float2 direction = material.anisotropyDirection;
        float strength = material.anisotropyStrength;
        if (material.anisotropyTextureIndex != 0xFFFFFFFF)
        {
            const float3 texel = material_sample(ubo, material.anisotropyTextureIndex, material.anisotropyTextureSet, coords, samplerState).rgb;
            const float2 textureDirection = texel.rg * 2.0 - 1.0;
            const float2 d = dot(textureDirection, textureDirection) > 1e-8 ? normalize(textureDirection) : float2(1.0, 0.0);
            const float2 r = material.anisotropyDirection; // (cos, sin) of the rotation
            direction = float2(r.x * d.x - r.y * d.y, r.y * d.x + r.x * d.y);
            strength *= texel.b;
        }
        lobes.anisotropy = hasTangents ? strength : 0.0;
        anisotropicT = normalize(T * direction.x + B * direction.y);
        if (backFace)
            anisotropicT = -anisotropicT;
        if (lobes.anisotropy <= 0.0)
            lobes.flags &= ~MaterialLobeAnisotropy;
    }
    return lobes;
}

#endif
