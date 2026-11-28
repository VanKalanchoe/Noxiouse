// Slang-only helper functions and structures shared by engine shaders (included by shaderIO.h under #ifdef __SLANG__).
// Keeping these out of the C++ side of shaderIO.h prevents Slang helper edits from triggering C++ translation-unit rebuilds.

uint payload_meshlet_index(MeshletPayload payload, uint idx)
{
    return (payload.meshletIndices[idx >> 2u] >> ((idx & 3u) * 8u)) & 0xFFu;
}

// The G-buffer material target's alpha (RGBA8): 0 for a single-sided surface; a double-sided one 0.25 + 0.75 x its diffuse
// transmission (leaves, cloth: light from behind comes through).
float gbuffer_encode_back_light(bool doubleSided, float diffuseTransmission)
{
    return doubleSided ? 0.25 + 0.75 * saturate(diffuseTransmission) : 0.0;
}

bool gbuffer_double_sided(float encoded)
{
    return encoded > 0.125;
}

// How much of a light behind a double-sided surface comes through to the side seen: its diffuse transmission, or 0.6 when the
// material sets none (the look double-sided foliage always had here).
float gbuffer_back_light(float encoded)
{
    const float diffuseTransmission = saturate((encoded - 0.25) / 0.75);
    return diffuseTransmission > 0.01 ? diffuseTransmission : 0.6;
}

// A UV through a material UV channel's transform rows (KHR_texture_transform: u' = row0 . (u, v, 1), v' = row1 . (u, v, 1)), and a UV
// difference (a gradient, a triangle edge) through its linear part.
float2 uv_transform(float3 row0, float3 row1, float2 uv)
{
    return float2(dot(row0.xy, uv) + row0.z, dot(row1.xy, uv) + row1.z);
}

float2 uv_transform_delta(float3 row0, float3 row1, float2 delta)
{
    return float2(dot(row0.xy, delta), dot(row1.xy, delta));
}

// The G-buffer material target's blue (RGBA8): the pixel's MaterialLobe* bits (which lobe targets hold data for it; low 6 bits) and its
// shading model in the top two bits -- Default Lit 0, toon 0x40 (its lobe target then holds the toon settings: gbuffer_encode_toon),
// unlit 0x80, Custom Lit 0xC0 (its lobe target holds which Material Shader lights it: gbuffer_encode_custom_lit) -- as a byte.
static const uint GBufferModelMask = 0xC0u;
static const uint GBufferToon = 0x40u;
static const uint GBufferUnlit = 0x80u;
static const uint GBufferCustomLit = 0xC0u;
// Bit 5 of material.b (with model bits 0): Subsurface shading model (step R1; Dispersion is never stored in the opaque G-buffer).
static const uint GBufferSubsurface = 0x20u;

float gbuffer_encode_flags(uint lobes, uint shadingModel)
{
    if (shadingModel == ShadingModelSubsurface)
        return float(GBufferSubsurface) / 255.0;
    const uint model = shadingModel == ShadingModelUnlit ? GBufferUnlit : shadingModel == ShadingModelToon ? GBufferToon
                     : shadingModel == ShadingModelCustomLit ? GBufferCustomLit : 0u;
    return float((model == 0u ? (lobes & 0x1Fu) : 0u) | model) / 255.0;
}

uint gbuffer_flags(float encoded)
{
    return uint(encoded * 255.0 + 0.5);
}

bool gbuffer_unlit(float encoded)
{
    return (gbuffer_flags(encoded) & GBufferModelMask) == GBufferUnlit;
}

bool gbuffer_toon(float encoded)
{
    return (gbuffer_flags(encoded) & GBufferModelMask) == GBufferToon;
}

bool gbuffer_custom_lit(float encoded)
{
    return (gbuffer_flags(encoded) & GBufferModelMask) == GBufferCustomLit;
}

bool gbuffer_subsurface(float encoded)
{
    const uint flags = gbuffer_flags(encoded);
    return (flags & GBufferModelMask) == 0u && (flags & GBufferSubsurface) != 0u;
}

// Subsurface shading model settings packed into the G-buffer lobe target (RGBA32_UINT, step R1).
struct SubsurfaceSettings
{
    float3 color;
    float opacity;
    float radius;
};

uint4 gbuffer_encode_subsurface(SubsurfaceSettings sss)
{
    const uint3 c = uint3(round(sqrt(saturate(sss.color)) * 255.0));
    const uint op = uint(round(saturate(sss.opacity) * 255.0));
    const uint rad = uint(round(saturate(sss.radius * 0.5) * 255.0));
    return uint4(c.r | (c.g << 8u) | (c.b << 16u) | (op << 24u), rad, 0u, 0u);
}

SubsurfaceSettings gbuffer_decode_subsurface(uint4 packed)
{
    SubsurfaceSettings sss;
    const float3 c = float3(packed.x & 0xFFu, (packed.x >> 8u) & 0xFFu, (packed.x >> 16u) & 0xFFu) / 255.0;
    sss.color = c * c;
    sss.opacity = float(packed.x >> 24u) / 255.0;
    sss.radius = float(packed.y & 0xFFu) / 255.0 * 2.0;
    return sss;
}

// A Custom Lit pixel's lobe target (RGBA32_UINT; it has no lobes, which are Default Lit only): x the Material Shader's slot
// (GpuMaterial::customLitSlot), y the material's record (SceneInstances::materials: its parameters and textures), z the mesh's first UV
// set (two halves), w how far that UV moves across a pixel (a half: the Material Shader's texture mips; CustomLit.slang decodes it).
uint4 gbuffer_encode_custom_lit(uint slot, uint materialIndex, float2 uv, float uvFootprint)
{
    return uint4(slot, materialIndex, f32tof16(uv.x) | (f32tof16(uv.y) << 16u), f32tof16(min(uvFootprint, 60000.0)));
}

// A toon pixel's settings in its lobe target (RGBA32_UINT; a toon pixel has no lobes -- they are Default Lit only), as Unreal keeps a
// shading model's data in its CustomData target. x: the ramp texture (0xFFFFFFFF: none); y: bands, band softness, highlight size,
// highlight softness (8 bits each); z: rim colour (sRGB-ish 8:8:8) and rim width; w: highlight and rim intensity (0..8, 8 bits each).
struct ToonSettings
{
    uint rampTexture;
    uint bands;
    float bandSoftness;
    float specularSize;
    float specularSoftness;
    float specularIntensity;
    float3 rimColor;
    float rimWidth;
    float rimIntensity;
};

uint toon_unorm8(float value) { return uint(round(saturate(value) * 255.0)); }

uint4 gbuffer_encode_toon(ToonSettings toon)
{
    const uint y = clamp(toon.bands, 1u, 8u) | (toon_unorm8(toon.bandSoftness) << 8u) | (toon_unorm8(toon.specularSize) << 16u) |
                   (toon_unorm8(toon.specularSoftness) << 24u);
    const float3 rim = sqrt(saturate(toon.rimColor)); // (a little more precision in the darks)
    const uint z = toon_unorm8(rim.r) | (toon_unorm8(rim.g) << 8u) | (toon_unorm8(rim.b) << 16u) | (toon_unorm8(toon.rimWidth) << 24u);
    const uint w = toon_unorm8(toon.specularIntensity / 8.0) | (toon_unorm8(toon.rimIntensity / 8.0) << 8u);
    return uint4(toon.rampTexture, y, z, w);
}

ToonSettings gbuffer_decode_toon(uint4 packed)
{
    ToonSettings toon;
    toon.rampTexture = packed.x;
    toon.bands = max(packed.y & 0xFFu, 1u);
    toon.bandSoftness = float((packed.y >> 8u) & 0xFFu) / 255.0;
    toon.specularSize = float((packed.y >> 16u) & 0xFFu) / 255.0;
    toon.specularSoftness = float(packed.y >> 24u) / 255.0;
    const float3 rim = float3(packed.z & 0xFFu, (packed.z >> 8u) & 0xFFu, (packed.z >> 16u) & 0xFFu) / 255.0;
    toon.rimColor = rim * rim;
    toon.rimWidth = float(packed.z >> 24u) / 255.0;
    toon.specularIntensity = float(packed.w & 0xFFu) / 255.0 * 8.0;
    toon.rimIntensity = float((packed.w >> 8u) & 0xFFu) / 255.0 * 8.0;
    return toon;
}

// A unit vector as two 16-bit octahedral coordinates (and back).
uint gbuffer_encode_octahedral(float3 v)
{
    v /= abs(v.x) + abs(v.y) + abs(v.z);
    float2 e = v.z >= 0.0 ? v.xy : (1.0 - abs(v.yx)) * select(v.xy >= 0.0, float2(1.0), float2(-1.0));
    const uint2 q = uint2(round(saturate(e * 0.5 + 0.5) * 65535.0));
    return q.x | (q.y << 16u);
}

float3 gbuffer_decode_octahedral(uint packed)
{
    const float2 e = float2(packed & 0xFFFFu, packed >> 16u) / 65535.0 * 2.0 - 1.0;
    float3 v = float3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = saturate(-v.z);
    v.xy += select(v.xy >= 0.0, float2(-t), float2(t));
    return normalize(v);
}

// An orthonormal basis around a unit normal (Duff et al. 2017, "Building an Orthonormal Basis, Revisited"): the reference a
// direction in the surface's plane is stored against as an angle.
void gbuffer_normal_basis(float3 n, out float3 t, out float3 b)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// The normal the G-buffer normal target gives back (its half floats, normalized as the readers do): an anisotropy direction is
// stored against this one's basis, so the reader rebuilds exactly the same basis.
float3 gbuffer_stored_normal(float3 n)
{
    const float3 h = f16tof32(f32tof16(n));
    const float lengthSq = dot(h, h);
    return lengthSq > 1e-6 ? h * rsqrt(lengthSq) : float3(0.0, 1.0, 0.0);
}

// The lobes of one G-buffer pixel (the MaterialLobe* bits say which hold data), as GBufferMaterial.slang evaluates them.
struct GBufferLobes
{
    uint flags;
    float3 dielectricF0;     // min(F0 of the IOR x specular color, 1) (MaterialLobeSpecular; else 0.04)
    float specularWeight;    // (else 1)
    float coat;              // clear coat factor
    float coatRoughness;
    float3 coatNormal;
    float3 sheenColor;
    float sheenRoughness;
    float anisotropy;        // strength
    float anisotropyAngle;   // the direction, radians in [0, pi) against gbuffer_normal_basis of the stored normal
    float iridescence;       // factor
    float iridescenceIor;
    float iridescenceThickness; // nanometers
};

GBufferLobes gbuffer_default_lobes()
{
    GBufferLobes lobes;
    lobes.flags = 0u;
    lobes.dielectricF0 = float3(0.04);
    lobes.specularWeight = 1.0;
    lobes.coat = 0.0;
    lobes.coatRoughness = 0.0;
    lobes.coatNormal = float3(0.0, 0.0, 1.0);
    lobes.sheenColor = float3(0.0);
    lobes.sheenRoughness = 0.0;
    lobes.anisotropy = 0.0;
    lobes.anisotropyAngle = 0.0;
    lobes.iridescence = 0.0;
    lobes.iridescenceIor = 1.3;
    lobes.iridescenceThickness = 0.0;
    return lobes;
}

// The lobe target (RGBA32_UINT): x the coat normal (octahedral 16:16); y coat factor, coat roughness, sheen roughness, anisotropy
// strength (8 bits each); z the sheen color (square root, 8 bits a channel) and the anisotropy angle (8 bits over pi); w the
// iridescence factor (8), its IOR (8 bits over 1..3) and thickness (16 bits, nanometers).
uint4 gbuffer_encode_lobes(GBufferLobes lobes)
{
    uint4 packed;
    packed.x = gbuffer_encode_octahedral(lobes.coatNormal);
    const uint4 y = uint4(round(saturate(float4(lobes.coat, lobes.coatRoughness, lobes.sheenRoughness, lobes.anisotropy)) * 255.0));
    packed.y = y.x | (y.y << 8u) | (y.z << 16u) | (y.w << 24u);
    const uint3 sheen = uint3(round(sqrt(saturate(lobes.sheenColor)) * 255.0));
    const uint angle = uint(round(frac(lobes.anisotropyAngle / 3.14159265) * 255.0)) & 0xFFu;
    packed.z = sheen.x | (sheen.y << 8u) | (sheen.z << 16u) | (angle << 24u);
    const uint iridescence = uint(round(saturate(lobes.iridescence) * 255.0));
    const uint ior = uint(round(saturate((lobes.iridescenceIor - 1.0) * 0.5) * 255.0));
    const uint thickness = uint(round(clamp(lobes.iridescenceThickness, 0.0, 65535.0)));
    packed.w = iridescence | (ior << 8u) | (thickness << 16u);
    return packed;
}

// The specular target (RGBA8): the dielectric F0's square root (precision where F0 lives, a few percent) and the weight.
float4 gbuffer_encode_specular(GBufferLobes lobes)
{
    return float4(sqrt(saturate(lobes.dielectricF0)), saturate(lobes.specularWeight));
}

GBufferLobes gbuffer_decode_lobes(uint flags, uint4 packed, float4 specular)
{
    GBufferLobes lobes = gbuffer_default_lobes();
    lobes.flags = flags & 0x3Fu;
    if ((lobes.flags & MaterialLobeSpecular) != 0u)
    {
        lobes.dielectricF0 = specular.rgb * specular.rgb;
        lobes.specularWeight = specular.a;
    }
    lobes.coatNormal = gbuffer_decode_octahedral(packed.x);
    const float4 y = float4(packed.y & 0xFFu, (packed.y >> 8u) & 0xFFu, (packed.y >> 16u) & 0xFFu, packed.y >> 24u) / 255.0;
    lobes.coat = (lobes.flags & MaterialLobeClearcoat) != 0u ? y.x : 0.0;
    lobes.coatRoughness = y.y;
    lobes.sheenRoughness = y.z;
    lobes.anisotropy = (lobes.flags & MaterialLobeAnisotropy) != 0u ? y.w : 0.0;
    const float3 sheen = float3(packed.z & 0xFFu, (packed.z >> 8u) & 0xFFu, (packed.z >> 16u) & 0xFFu) / 255.0;
    lobes.sheenColor = (lobes.flags & MaterialLobeSheen) != 0u ? sheen * sheen : float3(0.0);
    lobes.anisotropyAngle = float(packed.z >> 24u) / 255.0 * 3.14159265;
    lobes.iridescence = (lobes.flags & MaterialLobeIridescence) != 0u ? float(packed.w & 0xFFu) / 255.0 : 0.0;
    lobes.iridescenceIor = 1.0 + 2.0 * float((packed.w >> 8u) & 0xFFu) / 255.0;
    lobes.iridescenceThickness = float(packed.w >> 16u);
    return lobes;
}

// The specular layer one reflection ray per pixel traces (Reflection.slang) and its denoiser guides describe: with a clear coat
// UE's blend, the coat's roughness by its factor over the base's and the normal likewise; without one, the surface's own.
void gbuffer_reflection_layer(GBufferLobes lobes, inout float3 normal, inout float roughness)
{
    if (lobes.coat <= 0.0)
        return;
    roughness = lerp(roughness, lobes.coatRoughness, lobes.coat);
    normal = normalize(lerp(normal, lobes.coatNormal, lobes.coat));
}

// Beer-Lambert absorption over `length` (world units) of a volume whose light keeps attenuationColor over attenuationDistance.
float3 volume_transmittance(float3 attenuationColor, float attenuationDistance, float length)
{
    if (attenuationDistance <= 0.0 || length <= 0.0)
        return float3(1.0);
    return pow(max(attenuationColor, float3(1e-6)), float3(length / attenuationDistance));
}

// Cross-API UniformBufferObject loader (BDA on Vulkan, raw ByteAddressBuffer SRV on D3D12).
UniformBufferObject loadUbo(uint64_t ref)
{
    return BufferRef<UniformBufferObject>(ref).Load(0);
}

// Material textures (§5.12.2): every lookup of a material's texture -- raster, ray traced, path traced -- goes through these, by the
// texture index the material holds: the image's bindless slot, or a streaming virtual texture's (MaterialTextureVirtual), resolved here
// through its page table -- no shader has to know which kind a material uses.
bool isVirtualMaterialTexture(uint textureIndex)
{
    return textureIndex != 0xFFFFFFFF && (textureIndex & MaterialTextureVirtual) != 0;
}
// The image: a virtual texture's tail.
Texture2D MaterialTexture(UniformBufferObject ubo, uint textureIndex)
{
    const uint slot = textureIndex == 0xFFFFFFFFu ? 0u : (ubo.imageHeapIndexOffset + (textureIndex & ~MaterialTextureVirtual));
#ifdef NOX_VULKAN
    return Texture2D.Handle(uint2(slot, 0));
#else
    return Texture2D.Handle(uint2(NonUniformResourceIndex(slot), 0));
#endif
}
GpuVirtualTexture virtualTextureOf(UniformBufferObject ubo, uint textureIndex)
{
    return BufferRef<GpuVirtualTexture>(ubo.virtualTexturesReference)[textureIndex & ~MaterialTextureVirtual];
}
uint2 virtualMipSize(GpuVirtualTexture vt, uint mip)
{
    return uint2(max(vt.width >> mip, 1u), max(vt.height >> mip, 1u));
}
// The page table entry index of the tile holding uv (repeat addressing) at a tiled mip.
uint virtualTileIndex(GpuVirtualTexture vt, float2 uv, uint mip)
{
    const uint2 size = virtualMipSize(vt, mip);
    const uint2 tiles = (size + VirtualTileSize - 1) / VirtualTileSize;
    const uint2 tile = min(uint2(frac(uv) * float2(size)) / VirtualTileSize, tiles - 1);
    return vt.pageTableOffset + vt.firstTile[mip] + tile.y * tiles.x + tile.x;
}
// The mip these UV derivatives ask of a virtual texture: as the anisotropic sampler picks it (the footprint's long axis over its
// probes), with the anisotropy capped at what the tile borders hold.
float virtualTextureLod(GpuVirtualTexture vt, float2 ddxUV, float2 ddyUV)
{
    const float2 size = float2(vt.width, vt.height);
    const float2 dx = ddxUV * size;
    const float2 dy = ddyUV * size;
    const float major = max(max(dot(dx, dx), dot(dy, dy)), 1e-12);
    const float minor = max(min(dot(dx, dx), dot(dy, dy)), 1e-12);
    const float anisotropy = min(ceil(sqrt(major / minor)), float(VirtualTextureMaxAnisotropy));
    return 0.5 * log2(major) - log2(anisotropy);
}
// One mip of a virtual texture at uv: the texels of the tile its page table entry maps (its own, or the finest resident ancestor's),
// sampled in the pool with the derivatives scaled to that mip (anisotropic filtering inside the tile's border); the tail when no
// tile at or above it is resident.
float4 sampleVirtualMip(UniformBufferObject ubo, uint textureIndex, GpuVirtualTexture vt, SamplerState samplerState, float2 uv, uint mip,
                        float2 ddxUV, float2 ddyUV, bool gradients)
{
    if (mip < vt.tiledMips)
    {
        const uint entry = BufferRef<uint>(ubo.virtualPageTableReference)[virtualTileIndex(vt, uv, mip)];
        if (entry != VirtualPageUnmapped)
        {
            const uint slot = entry & VirtualPageSlotMask;
            const float2 size = float2(virtualMipSize(vt, entry >> VirtualPageMipShift));
            const float2 texel = frac(uv) * size;
            const float2 inTile = texel - floor(texel / float(VirtualTileSize)) * float(VirtualTileSize);
            const float2 origin = float2(uint2(slot % vt.poolTilesPerRow, slot / vt.poolTilesPerRow) * VirtualTileStride + VirtualTileBorder);
            const float2 physical = (origin + inTile) * vt.poolInvSize;
#ifdef NOX_VULKAN
            Texture2D pool = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + vt.poolTextureIndex, 0));
#else
            Texture2D pool = Texture2D.Handle(uint2(NonUniformResourceIndex(ubo.imageHeapIndexOffset + vt.poolTextureIndex), 0));
#endif
            if (!gradients)
                return pool.SampleLevel(samplerState, physical, 0.0);
            float2 dx = ddxUV * size;
            float2 dy = ddyUV * size;
            const float major = sqrt(max(dot(dx, dx), dot(dy, dy)));
            if (major > float(VirtualTextureMaxAnisotropy))
            {
                dx *= float(VirtualTextureMaxAnisotropy) / major;
                dy *= float(VirtualTextureMaxAnisotropy) / major;
            }
            return pool.SampleGrad(samplerState, physical, dx * vt.poolInvSize, dy * vt.poolInvSize);
        }
    }
    return MaterialTexture(ubo, textureIndex).SampleLevel(samplerState, uv, max(float(mip) - float(vt.tiledMips), 0.0));
}
// A virtual texture at a LOD (mip 0's texels): trilinear, the two mips around it blended (each one lookup); past the tiled mips the tail
// alone, filtered by the hardware.
float4 sampleVirtualTexture(UniformBufferObject ubo, uint textureIndex, SamplerState samplerState, float2 uv, float lod, float2 ddxUV, float2 ddyUV,
                            bool gradients)
{
    GpuVirtualTexture vt = virtualTextureOf(ubo, textureIndex);
    lod = max(lod, 0.0);
    if (lod >= float(vt.tiledMips))
    {
        Texture2D tail = MaterialTexture(ubo, textureIndex);
        return gradients ? tail.SampleGrad(samplerState, uv, ddxUV, ddyUV) : tail.SampleLevel(samplerState, uv, lod - float(vt.tiledMips));
    }
    const uint mip = uint(lod);
    const float blend = lod - float(mip);
    float4 color = sampleVirtualMip(ubo, textureIndex, vt, samplerState, uv, mip, ddxUV, ddyUV, gradients);
    if (blend > 1.0 / 256.0)
        color = lerp(color, sampleVirtualMip(ubo, textureIndex, vt, samplerState, uv, mip + 1, ddxUV, ddyUV, gradients), blend);
    return color;
}

float4 SampleMaterialTextureGrad(UniformBufferObject ubo, uint textureIndex, SamplerState samplerState, float2 uv, float2 ddxUV, float2 ddyUV)
{
    if (isVirtualMaterialTexture(textureIndex))
        return sampleVirtualTexture(ubo, textureIndex, samplerState, uv, virtualTextureLod(virtualTextureOf(ubo, textureIndex), ddxUV, ddyUV),
                                    ddxUV, ddyUV, true);
    return MaterialTexture(ubo, textureIndex).SampleGrad(samplerState, uv, ddxUV, ddyUV);
}
float4 SampleMaterialTexture(UniformBufferObject ubo, uint textureIndex, SamplerState samplerState, float2 uv)
{
    if (isVirtualMaterialTexture(textureIndex))
        return SampleMaterialTextureGrad(ubo, textureIndex, samplerState, uv, ddx(uv), ddy(uv));
    return MaterialTexture(ubo, textureIndex).Sample(samplerState, uv);
}
float4 SampleMaterialTextureBias(UniformBufferObject ubo, uint textureIndex, SamplerState samplerState, float2 uv, float bias)
{
    if (isVirtualMaterialTexture(textureIndex))
    {
        const float scale = exp2(bias);
        return SampleMaterialTextureGrad(ubo, textureIndex, samplerState, uv, ddx(uv) * scale, ddy(uv) * scale);
    }
    return MaterialTexture(ubo, textureIndex).SampleBias(samplerState, uv, bias);
}
float4 SampleMaterialTextureLevel(UniformBufferObject ubo, uint textureIndex, SamplerState samplerState, float2 uv, float lod)
{
    if (isVirtualMaterialTexture(textureIndex))
        return sampleVirtualTexture(ubo, textureIndex, samplerState, uv, lod, float2(0.0), float2(0.0), false);
    return MaterialTexture(ubo, textureIndex).SampleLevel(samplerState, uv, lod);
}
// Texels of the full-resolution mip and the mip count (LOD from ray cones and footprints, mip feedback).
void MaterialTextureSize(UniformBufferObject ubo, uint textureIndex, out uint width, out uint height, out uint mipCount)
{
    if (isVirtualMaterialTexture(textureIndex))
    {
        GpuVirtualTexture vt = virtualTextureOf(ubo, textureIndex);
        width = vt.width;
        height = vt.height;
        mipCount = vt.mipLevels;
        return;
    }
    MaterialTexture(ubo, textureIndex).GetDimensions(0, width, height, mipCount);
}
void MaterialTextureSize(UniformBufferObject ubo, uint textureIndex, out uint width, out uint height)
{
    uint mipCount;
    MaterialTextureSize(ubo, textureIndex, width, height, mipCount);
}

// Octahedral unit vectors (Cigolle et al., "A Survey of Efficient Representations for Independent Unit Vectors"): the vector projected
// onto the octahedron |x| + |y| + |z| = 1, the lower half folded over the upper. Decoded as packVertex (DataTypes.h) encodes.
float3 octDecode(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
Vertex unpackVertex(PackedVertex packed)
{
    Vertex vertex;
    vertex.pos = packed.pos;
    vertex.normal = octDecode(float2(packed.normal & 0xFFFFu, packed.normal >> 16) * (2.0 / 65535.0) - 1.0);
    vertex.uv0 = packed.uv0;
    vertex.uv1 = packed.uv1;
    vertex.tangent = float4(octDecode(float2(packed.tangent & 0x7FFFu, (packed.tangent >> 15) & 0x7FFFu) * (2.0 / 32767.0) - 1.0),
                            (packed.tangent & 0x80000000u) != 0 ? -1.0 : 1.0);
    vertex.boneIDs = uint4(0);
    vertex.boneWeights = float4(0.0);
    vertex.color = float4(1.0);
    return vertex;
}
// A vertex color as the color stream holds it (VertexPacking::PackColor): RGB sRGB-encoded, A linear, 8 bits each.
float4 unpackVertexColor(uint32_t packed)
{
    const float3 srgb = float3(float(packed & 0xFFu), float((packed >> 8) & 0xFFu), float((packed >> 16) & 0xFFu)) * (1.0 / 255.0);
    const float3 linear = select(srgb <= 0.04045, srgb * (1.0 / 12.92), pow((srgb + 0.055) * (1.0 / 1.055), 2.4));
    return float4(linear, float(packed >> 24) * (1.0 / 255.0));
}
// The color of a geometry stream vertex (InstanceData::vertexColors: its mesh has them), white otherwise. It multiplies the base color.
float4 loadMeshVertexColor(UniformBufferObject ubo, uint vertexIndex, uint vertexColors)
{
    if (vertexColors == 0 || ubo.geometryColorsReference == 0)
        return float4(1.0);
    return unpackVertexColor(BufferRef<uint32_t>(ubo.geometryColorsReference)[vertexIndex]);
}
void unpackSkin(SkinVertex skin, inout Vertex vertex)
{
    vertex.boneIDs = uint4(skin.boneIDs01 & 0xFFFFu, skin.boneIDs01 >> 16, skin.boneIDs23 & 0xFFFFu, skin.boneIDs23 >> 16);
    vertex.boneWeights = float4(skin.boneWeights01 & 0xFFFFu, skin.boneWeights01 >> 16, skin.boneWeights23 & 0xFFFFu, skin.boneWeights23 >> 16) *
                         (1.0 / 65535.0);
}
// A vertex of the geometry stream, with its bone influences when the caller skins it.
Vertex loadMeshVertex(UniformBufferObject ubo, uint vertexIndex, bool skinned)
{
    Vertex vertex = unpackVertex(BufferRef<PackedVertex>(ubo.geometryVerticesReference)[vertexIndex]);
    if (skinned && ubo.geometrySkinReference != 0)
        unpackSkin(BufferRef<SkinVertex>(ubo.geometrySkinReference)[vertexIndex], vertex);
    return vertex;
}

// A hit triangle's vertex indices into the instance's vertex (and color) addresses: its index buffer's, or the triangle's own three
// vertices without one.
uint3 RayTracingTriangle(GpuRayTracingInstance instance, uint primitive)
{
    if (instance.indexBufferAddress == 0)
        return uint3(primitive * 3, primitive * 3 + 1, primitive * 3 + 2);
    BufferRef<uint32_t> indices = BufferRef<uint32_t>(instance.indexBufferAddress);
    return uint3(indices[primitive * 3], indices[primitive * 3 + 1], indices[primitive * 3 + 2]);
}
// The vertex color at a hit (barycentrics of corners 1 and 2), white when the mesh has none: it multiplies the base color, as raster's.
float4 RayTracingVertexColor(GpuRayTracingInstance instance, uint3 corners, float2 barycentrics)
{
    if (instance.colorBufferAddress == 0)
        return float4(1.0);
    BufferRef<uint32_t> colors = BufferRef<uint32_t>(instance.colorBufferAddress);
    return unpackVertexColor(colors[corners.x]) * (1.0 - barycentrics.x - barycentrics.y) + unpackVertexColor(colors[corners.y]) * barycentrics.x +
           unpackVertexColor(colors[corners.z]) * barycentrics.y;
}
// The light a ray hit's surface emits: the material's emissive factor x strength through its emissive texture, read through the
// texture's own UV channel (KHR_texture_transform) at `lod`. Without the texture a mesh whose emissive map is mostly dark glowed
// everywhere -- a MagicaVoxel model is one mesh with one palette material (3 of room.vox's 256 entries emit), and DDGI saw its whole
// room glowing, which lit the screen probes' second bounce and edge fallback white.
float3 RayHitEmission(UniformBufferObject ubo, GpuRayTracingInstance instance, Vertex v0, Vertex v1, Vertex v2, float2 barycentrics,
                      SamplerState textureSampler, float lod)
{
    float3 emission = instance.emissiveFactor.rgb * instance.emissiveFactor.a;
    if (instance.emissiveTextureIndex == 0xFFFFFFFF || !any(emission > 0.0))
        return emission;
    const float w0 = 1.0 - barycentrics.x - barycentrics.y;
    const GpuMaterial material = BufferRef<GpuMaterial>(ubo.sceneMaterialsReference)[instance.materialIndex];
    const float2 uv = material_texture_uv(material, material.emissiveTextureSet, w0 * v0.uv0 + barycentrics.x * v1.uv0 + barycentrics.y * v2.uv0,
                                          w0 * v0.uv1 + barycentrics.x * v1.uv1 + barycentrics.y * v2.uv1);
    return emission * SampleMaterialTextureLevel(ubo, instance.emissiveTextureIndex, textureSampler, uv, lod).rgb;
}
// Its alpha, for the alpha tests (the base color's alpha includes it).
float RayTracingVertexAlpha(GpuRayTracingInstance instance, uint primitive, float2 barycentrics)
{
    return instance.colorBufferAddress == 0 ? 1.0 : RayTracingVertexColor(instance, RayTracingTriangle(instance, primitive), barycentrics).a;
}
// Ray tracing with the material graphs' Opacity Mask (NOX_MATERIAL_GRAPH_MASK: the ray tracing passes' graph variants, generated with
// MaterialGraphMasks.slang -- Renderer/CustomLitShaders.h): a Masked material whose graph connects Opacity Mask is tested with the
// graph's mask at the hit instead of its base colour's alpha, as the visibility buffer and the shadow maps test it. Defined in
// MaterialGraph.slang (material_graph_ray_mask); `handled` false: the material has no graph mask.
#ifdef NOX_MATERIAL_GRAPH_MASK
float material_graph_ray_mask(UniformBufferObject ubo, GpuRayTracingInstance instance, uint primitive, float2 barycentrics, out bool handled);
#endif

// Whether a hit's base color has an alpha to test: a base color texture or vertex colors (else an alpha-masked surface is solid). With
// the graphs' masks any Masked hit may have one (RayHitAlpha tells: 1 without).
bool RayHitAlphaTested(GpuRayTracingInstance instance)
{
#ifdef NOX_MATERIAL_GRAPH_MASK
    if (instance.alphaMode == 1)
        return true;
#endif
    return instance.baseColorTextureIndex != 0xFFFFFFFF || instance.colorBufferAddress != 0;
}
// That alpha at a hit: the texture's (at uv) times the vertex color's (the ray alpha tests' rule; the factor's is the caller's). With
// the graphs' masks: a graph-masked hit's mask (what is compared with the cutoff; see RayHitCoverage for the factor).
float RayHitAlpha(UniformBufferObject ubo, GpuRayTracingInstance instance, SamplerState samplerState, float2 uv, float lod, uint primitive,
                  float2 barycentrics)
{
#ifdef NOX_MATERIAL_GRAPH_MASK
    bool graphMask;
    const float mask = material_graph_ray_mask(ubo, instance, primitive, barycentrics, graphMask);
    if (graphMask)
        return mask;
#endif
    float alpha = RayTracingVertexAlpha(instance, primitive, barycentrics);
    if (instance.baseColorTextureIndex != 0xFFFFFFFF)
        alpha *= SampleMaterialTextureLevel(ubo, instance.baseColorTextureIndex, samplerState, uv, lod).a;
    return alpha;
}
// A hit's coverage with its base colour factor: factor x RayHitAlpha; a graph-masked hit's mask alone (the raster's rule: the graph's
// Opacity Mask is the whole coverage, compared with the Opacity Mask Clip Value).
float RayHitCoverage(UniformBufferObject ubo, GpuRayTracingInstance instance, SamplerState samplerState, float2 uv, float lod, uint primitive,
                     float2 barycentrics)
{
#ifdef NOX_MATERIAL_GRAPH_MASK
    bool graphMask;
    const float mask = material_graph_ray_mask(ubo, instance, primitive, barycentrics, graphMask);
    if (graphMask)
        return mask;
#endif
    return instance.baseColorFactor.a * RayHitAlpha(ubo, instance, samplerState, uv, lod, primitive, barycentrics);
}

// Ray traversal can use compact deformed positions while hit shading retains the base vertex's UVs and material data.
Vertex LoadRayTracingVertex(GpuRayTracingInstance instance, uint vertexIndex)
{
    Vertex vertex = unpackVertex(BufferRef<PackedVertex>(instance.vertexBufferAddress)[vertexIndex]);
    if (instance.skinnedVertexBufferAddress != 0)
    {
        SkinnedVertex skinned = BufferRef<SkinnedVertex>(instance.skinnedVertexBufferAddress)[vertexIndex];
        vertex.pos = skinned.position.xyz;
        vertex.normal = skinned.normal.xyz;
    }
    return vertex;
}

// The pixel a ray traced pass's fragment traces (UniformBufferObject::rtSubsample): its own at full ray tracing resolution, one of
// its block's otherwise.
// Which pixel of its block a block traces this frame: each block its own place in the 4-frame cycle (a hash of the block). The
// whole screen moving its pattern together every frame was structured noise NRD and DLSS RR kept as 2x2 blotches.
int2 rt_block_offset(UniformBufferObject ubo, int2 block)
{
    if (ubo.rtSubsample <= 1u)
        return int2(0, 0);
    static const int2 order[4] = { int2(0, 0), int2(1, 1), int2(1, 0), int2(0, 1) };
    uint h = (uint(block.x) * 0x8da6b343u) ^ (uint(block.y) * 0xd8163841u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return order[(ubo.frameIndex + h) & 3u] * int(ubo.rtSubsample) / 2;
}

int2 rt_trace_pixel(UniformBufferObject ubo, float2 fragmentPosition, float2 viewportSize)
{
    const uint subsample = max(ubo.rtSubsample, 1u);
    const int2 block = int2(fragmentPosition);
    return min(block * int(subsample) + rt_block_offset(ubo, block), int2(viewportSize) - 1);
}

// NRD's distances are the scene's (world units). The view depth it reads for the sky lies beyond its denoising range (the renderer
// sets CommonSettings::denoisingRange to NRDDenoisingRangeMeters): the sky is neither denoised nor mixed into the geometry beside it.
float nrd_sky_view_z(UniformBufferObject ubo)
{
    return 4.0 * NRDDenoisingRangeMeters * ubo.unitsPerMeter;
}

// The hit distance of a reflection ray that found nothing: far (10 km), but within half floats (the reflection targets are
// RGBA16F), so ~600 m in a centimeter project.
float reflection_miss_distance(UniformBufferObject ubo)
{
    return min(10000.0 * ubo.unitsPerMeter, 60000.0);
}

// ---- Light evaluation every shader shares (docs/Hybrid_Renderer_Plan_2026-10.md §7) ----

// The directional lights' discs a ray sees (over a procedural sky, UniformBufferObject::skyDiscs): each its illuminance over its
// solid angle, capped as the skybox's sun is (the real ratio only saturates the tonemapper harder). For the rays nothing else
// sends the lights' light along: camera rays and mirror paths -- every other path samples them directly.
float3 directional_light_discs(UniformBufferObject ubo, float3 direction)
{
    if (ubo.skyDiscs == 0)
        return float3(0.0);
    BufferRef<LightData> lights = BufferRef<LightData>(ubo.lightDataReference);
    float3 radiance = float3(0.0);
    for (uint i = 0; i < ubo.lightCount; ++i)
    {
        const LightData light = lights[i];
        if ((uint)light.position.w != 0u)
            continue;
        const float cosRadius = cos(clamp(light.spotParams.z, 0.0005, 0.2));
        if (dot(direction, normalize(light.direction.xyz)) < cosRadius)
            continue;
        const float solidAngle = 2.0 * 3.14159265 * (1.0 - cosRadius);
        radiance += light.color.rgb * light.color.w * min(1.0 / solidAngle, 64.0);
    }
    return radiance;
}

// The radius SSAO / RTAO look within at a surface point (`ndc`, `depth`, rebuilt to `worldPos`). With a traced GI that already holds
// the occlusion past its probe spacing (the screen probes), the AO is only its short range, as Unreal's Lumen short-range AO (Lumen
// does not apply SSAO, r.Lumen.DiffuseIndirect.SSAO 0): at most `shortRangePixels` pixels' width at the surface -- the occlusion
// between the probes -- else the whole `radius`. Applied over the GI's own occlusion at the full radius it counted every cavity
// twice: inside AnisotropyBarnLamp's shade the GI's sky visibility (0.345) was multiplied by an RTAO measuring the same 0.345.
float ambient_occlusion_radius(float4x4 invViewProj, float2 ndc, float depth, float3 worldPos, float2 viewportSize, float radius,
                               float shortRangePixels)
{
    if (shortRangePixels <= 0.0)
        return radius;
    // One pixel's width at the surface, from the neighbor's position at the same depth (perspective or orthographic alike).
    const float4 neighborH = mul(invViewProj, float4(ndc + float2(2.0 / max(viewportSize.x, 1.0), 0.0), depth, 1.0));
    const float pixelWidth = length(neighborH.xyz / neighborH.w - worldPos);
    return clamp(shortRangePixels * pixelWidth, 1e-6, radius);
}

// How far a ray that starts on a rasterized surface (rebuilt from the depth buffer, or a raster vertex) must start off it. The raster
// draws cluster LODs simplified by up to lodErrorThreshold of the render height at their distance; the TLAS holds the original
// triangles (§5.7.1), so the two surfaces can be that far apart. A ray starting under the traced surface hits it at once: black
// specks in RTAO, specks of shadow, darker reflections -- far away, gone up close where the raster draws the original triangles
// (Unreal offsets its ray tracing against Nanite's fallback meshes the same way). minBias: the offset the caller needs anyway.
float raster_surface_bias(UniformBufferObject ubo, float3 worldPos, float minBias)
{
    if (ubo.lodFullDetail != 0)
        return minBias;
    // The size of one render height on screen at worldPos: 2 tan(fovY / 2) x distance (perspective, proj[1][1] = 1 / tan(fovY / 2)),
    // 2 / proj[1][1] (orthographic: proj[3][3] = 1, no perspective).
    const float yScale = max(abs(ubo.nonJitteredProj[1][1]), 1e-6);
    const bool orthographic = ubo.nonJitteredProj[3][3] > 0.5;
    const float height = orthographic ? 2.0 / yScale : 2.0 * length(worldPos - ubo.cameraWorldPos.xyz) / yScale;
    return max(minBias, height * ubo.lodErrorThreshold);
}

// Where a point was in last frame's image (render-resolution textures drawn last frame: the SSR history, SSGI's and SSR's own
// accumulations): through the previous camera with last frame's jitter, as that image was drawn. Without the jitter the lookup lands
// up to half a pixel off, in another direction every frame -- sharp reflections shimmered on a still camera. w <= 0: behind it.
float4 prev_frame_uv(UniformBufferObject ubo, float3 worldPos, float2 viewportSize)
{
    const float4 clip = mul(ubo.prevProj, mul(ubo.prevView, float4(worldPos, 1.0)));
    if (clip.w <= 0.0)
        return float4(0.0, 0.0, 0.0, -1.0);
    const float2 ndc = clip.xy / clip.w + float2(2.0, -2.0) * ubo.prevJitterOffset / viewportSize;
    return float4(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, clip.z / clip.w, clip.w);
}

// How much of a light the clouds let through above worldPos: the cloud shadow map's transmittance for the shadow mask's light (the
// sun, or the moon at night), 1 for any other light and without clouds. The map holds it from the ground up: a point above the
// ground looks it up where its ray towards the light starts at ground level. Fades out towards the map's edge.
float cloud_shadow(UniformBufferObject ubo, float3 worldPos, uint lightIndex, float3 toLight)
{
    if (ubo.cloudShadowTextureIndex == 0xFFFFFFFF || lightIndex != ubo.shadowMaskLightIndex || toLight.y <= 0.0)
        return 1.0;
    const float kmPerUnit = 0.001 / max(ubo.unitsPerMeter, 1e-6);
    const float2 groundKm = worldPos.xz * kmPerUnit - toLight.xz * (worldPos.y * kmPerUnit / max(toLight.y, 0.02));
    const float2 uv = (groundKm - ubo.cloudShadowCenterKm) / (2.0 * ubo.cloudShadowExtentKm) + 0.5;
    const float2 edge = min(uv, 1.0 - uv);
    const float fade = saturate(min(edge.x, edge.y) * 10.0);
    if (fade <= 0.0)
        return 1.0;
    Texture2D<float> map = Texture2D<float>.Handle(uint2(ubo.imageHeapIndexOffset + ubo.cloudShadowTextureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    return lerp(1.0, map.SampleLevel(linearClamp, uv, 0.0), fade);
}

// A point / spot light's falloff: the inverse square law, held at the source's radius (exact for a sphere light, and no blow-up on
// a lamp's own shade right next to the bulb), smoothly windowed to 0 at Range (Karis 2013; 0 = unlimited).
float light_distance_attenuation(LightData light, float distance)
{
    const float radius = light.spotParams.z;
    float attenuation = 1.0 / max(distance * distance, max(radius * radius, 1e-8));
    const float range = light.direction.w;
    if (range > 0.0)
    {
        const float ratio = distance / range;
        const float window = saturate(1.0 - ratio * ratio * ratio * ratio);
        attenuation *= window * window;
    }
    return attenuation;
}

// A spot light's cone (KHR_lights_punctual: smooth from the inner to the outer angle); 1 for the other lights. toLight: unit, from
// the surface towards the light.
float light_spot_attenuation(LightData light, float3 toLight)
{
    if ((uint)light.position.w != 2u)
        return 1.0;
    const float cone = saturate(dot(normalize(light.direction.xyz), -toLight) * light.spotParams.x + light.spotParams.y);
    return cone * cone;
}

// Rect and disc lights are one kind (an area light: a lit face, its two half axes); these functions take either.
bool is_area_light(uint type) { return type == 3u || type == 4u; }

float3 rect_light_axis_x(LightData light) { return light.spotParams.xyz; }

float3 rect_light_axis_y(LightData light)
{
    const float3 x = light.spotParams.xyz;
    const float lengthX = length(x);
    return lengthX > 1e-12 ? normalize(cross(x / lengthX, normalize(light.direction.xyz))) * light.spotParams.w : float3(0.0);
}

float rect_light_area(LightData light)
{
    const float halfX = length(light.spotParams.xyz);
    return (uint)light.position.w == 4u ? 3.14159265 * halfX * light.spotParams.w : 4.0 * halfX * light.spotParams.w;
}

// How much a light lights a point, roughly (luminance; a ranking for UniformBufferObject::shadowRayLightLimit, not a shading term):
// a directional light its illuminance, a point / spot light its intensity over the distance squared with its range window and cone,
// a rect / disc light its intensity times its area over the distance squared.
float light_shadow_priority(LightData light, float3 position)
{
    const float strength = dot(light.color.rgb, float3(0.2126, 0.7152, 0.0722)) * light.color.w;
    const uint type = (uint)light.position.w;
    if (type == 0)
        return strength;
    const float3 toLight = light.position.xyz - position;
    const float distance = length(toLight);
    if (type == 3 || type == 4)
    {
        const float area = rect_light_area(light);
        return strength * area / max(distance * distance, area);
    }
    const float3 l = distance > 0.0001 ? toLight / distance : float3(0.0, 1.0, 0.0);
    return strength * light_distance_attenuation(light, distance) * light_spot_attenuation(light, l);
}

// Whether a point of the light's plane (offset from its centre) lies on it: inside the rectangle, or the disc.
bool area_light_contains(LightData light, float3 offset)
{
    const float3 axisX = rect_light_axis_x(light);
    const float3 axisY = rect_light_axis_y(light);
    const float x = dot(offset, axisX) / max(dot(axisX, axisX), 1e-12);
    const float y = dot(offset, axisY) / max(dot(axisY, axisY), 1e-12);
    return (uint)light.position.w == 4u ? x * x + y * y <= 1.0 : abs(x) <= 1.0 && abs(y) <= 1.0;
}

// The point of the light nearest a point of its plane (offset from its centre): clamped into the rectangle, or the disc.
float3 area_light_clamp(LightData light, float3 offset)
{
    const float3 axisX = rect_light_axis_x(light);
    const float3 axisY = rect_light_axis_y(light);
    const float halfX = length(axisX);
    const float halfY = length(axisY);
    const float3 unitX = axisX / max(halfX, 1e-12);
    const float3 unitY = axisY / max(halfY, 1e-12);
    float2 local = float2(dot(offset, unitX), dot(offset, unitY));
    if ((uint)light.position.w == 4u)
    {
        const float radius = length(local);
        if (radius > halfX)
            local *= halfX / radius;
    }
    else
    {
        local = clamp(local, float2(-halfX, -halfY), float2(halfX, halfY));
    }
    return light.position.xyz + unitX * local.x + unitY * local.y;
}

// The emitting face of a rect light a ray meets first, closer than maxT: its radiance, and maxT made its distance (left as it is,
// and 0 returned, when there is none). A rect light shines along its front: a ray sees it travelling against that.
float3 ray_rect_lights(UniformBufferObject ubo, float3 origin, float3 direction, inout float maxT)
{
    BufferRef<LightData> lights = BufferRef<LightData>(ubo.lightDataReference);
    float3 radiance = float3(0.0);
    for (uint i = 0; i < ubo.lightCount; ++i)
    {
        const LightData light = lights[i];
        if (!is_area_light((uint)light.position.w))
            continue;
        const float3 front = normalize(light.direction.xyz);
        const float facing = dot(direction, front);
        if (facing >= -1e-6)
            continue;
        const float t = dot(light.position.xyz - origin, front) / facing;
        if (t <= 0.0 || t >= maxT)
            continue;
        if (!area_light_contains(light, origin + direction * t - light.position.xyz))
            continue;
        maxT = t;
        radiance = light.color.rgb * light.color.w;
    }
    return radiance;
}

// The range window of a rect light at `distance` from its centre (1 without a Range).
float rect_light_window(LightData light, float distance)
{
    const float range = light.direction.w;
    if (range <= 0.0)
        return 1.0;
    const float ratio = distance / range;
    const float window = saturate(1.0 - ratio * ratio * ratio * ratio);
    return window * window;
}

// The cosine-weighted solid angle (steradians, at most pi) the light fills above a surface's horizon: its irradiance there is the
// light's radiance times this (Lambert's polygon formula, the polygon clipped to the surface's hemisphere first). 0 behind it.
// A disc is the octagon of the same area: within a percent or two of the circle's.
float rect_light_form_factor(LightData light, float3 position, float3 normal)
{
    const float3 front = normalize(light.direction.xyz);
    if (dot(position - light.position.xyz, front) <= 0.0)
        return 0.0;
    const float3 x = rect_light_axis_x(light);
    const float3 y = rect_light_axis_y(light);
    const float3 c = light.position.xyz - position;
    float3 corners[8];
    uint cornerCount = 4;
    if ((uint)light.position.w == 4u)
    {
        cornerCount = 8;
        const float areaScale = 1.05390; // sqrt(pi / (2 sqrt 2)): an octagon through these points has the circle's area
        for (uint k = 0; k < 8; ++k)
        {
            const float angle = 0.78539816 * float(k);
            corners[k] = c + (x * cos(angle) + y * sin(angle)) * areaScale;
        }
    }
    else
    {
        corners[0] = c - x - y;
        corners[1] = c + x - y;
        corners[2] = c + x + y;
        corners[3] = c - x + y;
    }

    // Clip to the half-space in front of the surface (Sutherland-Hodgman: one more vertex at most).
    float3 clipped[9];
    uint count = 0;
    for (uint i = 0; i < cornerCount; ++i)
    {
        const float3 a = corners[i];
        const float3 b = corners[(i + 1) % cornerCount];
        const float da = dot(a, normal);
        const float db = dot(b, normal);
        if (da >= 0.0)
            clipped[count++] = a;
        if ((da >= 0.0) != (db >= 0.0))
            clipped[count++] = a + (b - a) * (da / (da - db));
    }
    if (count < 3)
        return 0.0;

    float sum = 0.0;
    for (uint i = 0; i < count; ++i)
    {
        const float3 a = normalize(clipped[i]);
        const float3 b = normalize(clipped[(i + 1) % count]);
        const float3 edge = cross(a, b);
        const float edgeLength = length(edge);
        if (edgeLength > 1e-9)
            sum += acos(clamp(dot(a, b), -1.0, 1.0)) * dot(edge / edgeLength, normal);
    }
    return min(0.5 * abs(sum), 3.14159265);
}

// One sample of a light seen from `position` (u: two uniform random numbers): a point on it -- in the sun's cone, on a sphere
// light's disc, on a rect or disc light's area -- and the light arriving from there at a surface facing it, everything folded in
// (falloff, cone, for an area light its area / distance^2 and its own cosine). Light reflected = brdf * radiance * cos(surface),
// averaged over u.
struct LightPointSample
{
    float3 position;  // the sampled point (directional: far along the direction)
    float3 direction; // unit, from `position` towards the sample
    float distance;   // to the sample: the shadow ray's length
    float3 radiance;
};

void light_basis(float3 n, out float3 tangent, out float3 bitangent)
{
    const float sign = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (sign + n.z);
    const float b = n.x * n.y * a;
    tangent = float3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
    bitangent = float3(b, sign + n.y * n.y * a, -n.y);
}

LightPointSample sample_light_point(LightData light, float3 position, float2 u)
{
    LightPointSample s;
    const uint type = (uint)light.position.w;
    const float radiusSqrt = sqrt(saturate(u.x));
    const float phi = 6.28318530718 * frac(u.y);
    const float2 disc = radiusSqrt * float2(cos(phi), sin(phi));

    if (type == 0u)
    {
        float3 direction = normalize(light.direction.xyz);
        if (light.spotParams.z > 1e-5)
        {
            float3 tangent, bitangent;
            light_basis(direction, tangent, bitangent);
            direction = normalize(direction + (disc.x * tangent + disc.y * bitangent) * tan(light.spotParams.z));
        }
        s.direction = direction;
        s.distance = 1e7;
        s.position = position + direction * s.distance;
        s.radiance = light.color.rgb * light.color.w;
        return s;
    }

    if (is_area_light(type))
    {
        // Uniform over the area: the rectangle, or the disc (the same square-root mapping as above).
        const float2 onLight = type == 4u ? disc : float2(2.0 * u.x - 1.0, 2.0 * u.y - 1.0);
        s.position = light.position.xyz + rect_light_axis_x(light) * onLight.x + rect_light_axis_y(light) * onLight.y;
        const float3 toSample = s.position - position;
        s.distance = length(toSample);
        s.direction = s.distance > 1e-6 ? toSample / s.distance : normalize(light.direction.xyz);
        const float cosLight = dot(normalize(light.direction.xyz), -s.direction);
        // Area sampling: radiance * area * cos / d^2, which cannot exceed the hemisphere's 2 pi.
        const float solidAngle = cosLight > 0.0 ? min(rect_light_area(light) * cosLight / max(s.distance * s.distance, 1e-12), 6.28318530718) : 0.0;
        s.radiance = light.color.rgb * light.color.w * solidAngle * rect_light_window(light, length(light.position.xyz - position));
        return s;
    }

    float3 samplePosition = light.position.xyz;
    const float3 toCentre = samplePosition - position;
    const float centreDistance = length(toCentre);
    if (light.spotParams.z > 1e-5 && centreDistance > 1e-6)
    {
        float3 tangent, bitangent;
        light_basis(toCentre / centreDistance, tangent, bitangent);
        samplePosition += (disc.x * tangent + disc.y * bitangent) * light.spotParams.z;
    }
    const float3 toSample = samplePosition - position;
    s.position = samplePosition;
    s.distance = length(toSample);
    s.direction = s.distance > 1e-6 ? toSample / s.distance : float3(0.0, 1.0, 0.0);
    // The falloff by the centre's distance (the disc only spreads the shadow ray).
    s.radiance = light.color.rgb * light.color.w * light_distance_attenuation(light, centreDistance) * light_spot_attenuation(light, s.direction);
    return s;
}

// The irradiance a light gives a surface (normal `normal`, unshadowed: times the visibility of the returned sample), with the
// shadow ray to test: towards the sample. Exact for rect lights (the form factor; the sample only places the shadow ray).
float3 light_surface_irradiance(LightData light, float3 position, float3 normal, float2 u, out LightPointSample sample)
{
    sample = sample_light_point(light, position, u);
    if (is_area_light((uint)light.position.w))
        return light.color.rgb * light.color.w * rect_light_form_factor(light, position, normal) *
               rect_light_window(light, length(light.position.xyz - position));
    return sample.radiance * max(dot(normal, sample.direction), 0.0);
}

// Whether rays of the hybrid passes go through a surface (and the raster path draws it as glass): see-through transmission and
// not alpha-masked (masked leaves with transmission are cut-out solid surfaces). The same test as GpuScene's render bucket.
bool ray_see_through(float transmission, uint alphaMode)
{
    return transmission >= SeeThroughTransmission && alphaMode != 1u;
}

// A tangent-space normal map sample (0..1) as a unit tangent-space normal, as the G-buffer (GBufferMaterial.slang) and the path
// tracer (PathTracerBridge.slang) decode it: X and Y from the texture, Z rebuilt when the texture stores none (two-channel BC5 maps
// such as the Bistro's _ddna read 0 in blue -- taken as -1 it turned the normal into the surface).
// `scale`: glTF's normalTextureInfo.scale (the map's strength: X and Y scaled, then normalized).
float3 tangent_normal_from_map(float3 sampled, float scale = 1.0)
{
    float3 tangentNormal;
    tangentNormal.xy = sampled.xy * 2.0 - 1.0;
    tangentNormal.z = sampled.z <= 0.0 ? sqrt(saturate(1.0 - dot(tangentNormal.xy, tangentNormal.xy))) : abs(sampled.z * 2.0 - 1.0);
    tangentNormal.xy *= scale;
    return dot(tangentNormal, tangentNormal) > 1e-12 ? normalize(tangentNormal) : float3(0.0, 0.0, 1.0);
}

// The roughness a denoiser's guide gives an anisotropic GGX lobe (KHR_materials_anisotropy): its width along the stretch, alphaT =
// mix(alpha, 1, anisotropy^2) as perceptual roughness. The denoisers know isotropic lobes only; given the surface's roughness they
// took the stretched lobe's far samples for noise. Wider than the lobe across the stretch, which they then blur a little more.
float anisotropic_guide_roughness(float perceptualRoughness, float anisotropy)
{
    const float alpha = perceptualRoughness * perceptualRoughness;
    return sqrt(lerp(alpha, 1.0, anisotropy * anisotropy));
}

// Specular anti-aliasing: a normal that changes faster than a pixel (a fine normal map's grooves, seen from afar) makes a highlight
// no pixel can hold -- it sparkles and TAA / DLSS cannot settle it. The roughness grows by the normal's variance over the pixel
// (Tokuyoshi & Kaplanyan 2019, "Improved Geometric Specular Antialiasing"), as Godot's screen-space roughness limiter does with its
// defaults (amount 0.25, limit 0.18). `dndx`, `dndy`: the normal's change one pixel over. Perceptual roughness in and out.
float specular_aa_roughness(float perceptualRoughness, float3 dndx, float3 dndy)
{
    const float variance = 0.25 * (dot(dndx, dndx) + dot(dndy, dndy));
    const float kernelRoughness2 = min(2.0 * variance, 0.18);
    return sqrt(min(1.0, perceptualRoughness * perceptualRoughness + kernelRoughness2));
}

// A normal map sample to a world-space normal in the mesh's stored tangent frame (xyz world tangent, w bitangent handedness),
// Gram-Schmidt'ed against the interpolated normal, as the G-buffer does it.
float3 normal_from_map(float3 sampled, float3 normal, float4 tangent, float scale = 1.0)
{
    const float3 tangentNormal = tangent_normal_from_map(sampled, scale);

    const float3 N = normalize(normal);
    const float3 t = tangent.xyz - N * dot(N, tangent.xyz);
    if (dot(t, t) <= 1e-12)
        return N;
    const float3 T = normalize(t);
    const float3 B = cross(N, T) * (tangent.w < 0.0 ? -1.0 : 1.0);
    return normalize(mul(tangentNormal, float3x3(T, B, N)));
}

// The roughest surface the RT reflections trace (Reflection.slang) and the deferred lighting reads them for, faded out over the last
// 0.15: Unreal's Lumen default (r.Lumen.Reflections.MaxRoughnessToTrace 0.4). Rougher surfaces take their reflection from the GI
// (the screen probes' light around them, DeferredLighting.slang). Traced up to 0.85 under a denoiser, one GGX ray per pixel on rough
// concrete now and then found a lamp's small bright bulb; the denoiser spread those few pixels into worm-shaped blotches on matte
// walls that looked wet.
STATIC_CONST float RTReflectionMaxRoughness = 0.4;

// A material's depth bias (MaterialData::DepthBias; HDRP's Mesh Decal View Bias): the vertex drawn depthBias closer to the camera
// along its view ray -- under the reverse-Z projection its clip z scaled by distance / (distance - bias), the clip x, y and w, and so
// where it lands on screen, unchanged. Geometry modelled onto another surface (the Bistro's edge decals, trims and overlay layers,
// opaque faces 0 mm from the walls) then shows over it instead of z-fighting, as their engine's decal materials did.
float4 depth_bias_clip(float4 clipPosition, float3 worldPosition, float3 cameraPosition, float depthBias)
{
    if (depthBias <= 0.0)
        return clipPosition;
    const float distance = length(worldPosition - cameraPosition);
    clipPosition.z *= distance / max(distance - depthBias, 0.01 * distance);
    return clipPosition;
}

// Whether a surface lets light through to the hybrid passes' shadow rays: glass seen through, and what RTXPT's materials exclude
// from shadow rays (ExcludeFromNEE: the Bistro's windows). Windows that blocked the sun left the rooms behind them dark -- only
// their reflection showed.
bool ray_shadow_passes(GpuRayTracingInstance instance)
{
    return (instance.pathTracerFlags & PathTracerExcludeFromNEE) != 0 || ray_see_through(instance.transmissionFactor, instance.alphaMode);
}

// The exposure the post process applies (PostProcess.slang): the base exposure and compensation, and with auto exposure the stops
// from the adapted log2 average luminance to the key (AutoExposureKey), brightening only partly.
float scene_exposure(UniformBufferObject ubo, uint autoExposureTextureIndex, float compensation, float minEV, float maxEV)
{
    float exposure = ubo.exposure * exp2(compensation);
    if (autoExposureTextureIndex != 0xFFFFFFFF)
    {
        Texture2D<float> exposureTex = Texture2D<float>.Handle(uint2(ubo.imageHeapIndexOffset + autoExposureTextureIndex, 0));
        uint width, height, levels;
        exposureTex.GetDimensions(0, width, height, levels);
        const float logAverage = exposureTex.Load(int3(0, 0, int(levels) - 1));
        if (isfinite(logAverage))
        {
            float stops = log2(AutoExposureKey) - logAverage;
            if (stops > 0.0)
                stops *= AutoExposureDarkAdaptation;
            exposure *= exp2(clamp(stops, minEV, maxEV));
        }
    }
    return max(exposure, 1e-8);
}

// A pixel's lobes from the G-buffer lobe targets (ubo indices), default when it has none.
GBufferLobes gbuffer_load_lobes(UniformBufferObject ubo, int2 pixel, float encodedFlags)
{
    const uint raw = gbuffer_flags(encodedFlags);
    if ((raw & GBufferModelMask) != 0u || (raw & GBufferSubsurface) != 0u)
        return gbuffer_default_lobes();
    const uint flags = raw & 0x1Fu;
    if (flags == 0u || ubo.gbufferLobesTextureIndex == 0xFFFFFFFF)
        return gbuffer_default_lobes();
    Texture2D<uint4> lobesTex = Texture2D<uint4>.Handle(uint2(ubo.imageHeapIndexOffset + ubo.gbufferLobesTextureIndex, 0));
    Texture2D specularTex = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + ubo.gbufferSpecularTextureIndex, 0));
    return gbuffer_decode_lobes(flags, lobesTex.Load(int3(pixel, 0)), specularTex.Load(int3(pixel, 0)));
}

// A ray hit's texture coordinate (its mesh UV set 0) as the material's textures read it: through UV channel 0.
float2 ray_hit_uv(GpuRayTracingInstance lut, float2 uv)
{
    return uv_transform(lut.uvRow0, lut.uvRow1, uv);
}

// Where a texture is read (KHR_texture_transform): its set names one of the material's UV channels, which takes a mesh UV set (0 / 1)
// through its transform. The _delta forms carry a UV gradient (or a triangle edge) through the same channel's linear part.
float2 material_texture_uv(GpuMaterial material, int set, float2 mesh0, float2 mesh1)
{
    const uint channel = min(uint(max(set, 0)), MaterialUVChannels - 1u);
    return uv_transform(material.uvRow0[channel], material.uvRow1[channel], ((material.uvSources >> channel) & 1u) != 0u ? mesh1 : mesh0);
}

// How much a texture's UV channel scales lengths in UV (the square root of its linear part's determinant): a ray cone's footprint
// (texture LOD) on that texture.
float material_texture_uv_scale(GpuMaterial material, int set)
{
    const uint channel = min(uint(max(set, 0)), MaterialUVChannels - 1u);
    const float3 r0 = material.uvRow0[channel], r1 = material.uvRow1[channel];
    return sqrt(abs(r0.x * r1.y - r0.y * r1.x));
}

float2 material_texture_uv_delta(GpuMaterial material, int set, float2 mesh0, float2 mesh1)
{
    const uint channel = min(uint(max(set, 0)), MaterialUVChannels - 1u);
    return uv_transform_delta(material.uvRow0[channel], material.uvRow1[channel], ((material.uvSources >> channel) & 1u) != 0u ? mesh1 : mesh0);
}

// Raster view of one GPU scene instance: the instance record joined with its transform, material and mesh (ray tracing
// shaders read GpuRayTracingInstance instead).
struct InstanceData
{
    float4x4 modelMatrix;
    float4x4 normalMatrix;
    float4x4 previousModelMatrix;

    uint32_t drawsOffset;
    uint32_t boundsOffset;
    uint32_t meshletCount;
    uint32_t pageOffset; // GpuMesh::pageOffset: NoGeometryRange when every cluster is resident
    uint32_t vertexColors; // the mesh has vertex colors (loadMeshVertexColor)
    float3 boundsMin;
    float3 boundsMax;

    float workflow;
    float4 diffuseFactor;
    float4 specularFactor;
    float4 baseColorFactor;
    uint32_t baseColorTextureIndex;
    int32_t baseColorTextureSet;
    float metallicFactor;
    float roughnessFactor;
    uint32_t metallicRoughnessTextureIndex;
    int32_t physicalDescriptorTextureSet;
    uint32_t normalTextureIndex;
    int32_t normalTextureSet;
    uint32_t occlusionTextureIndex;
    int32_t occlusionTextureSet;
    float3 emissiveFactor;
    uint32_t emissiveTextureIndex;
    int32_t emissiveTextureSet;
    float emissiveStrength;
    float transmissionFactor;
    uint32_t transmissionTextureIndex;
    int32_t transmissionTextureSet;
    float ior;
    float thickness;
    float3 attenuationColor;
    float attenuationDistance;
    uint32_t alphaMode;
    float alphaMaskCutoff;
    uint32_t doubleSided;
    uint32_t shadingModel; // GpuMaterial::shadingModel
    float diffuseTransmission;
    float depthBias; // GpuMaterial::depthBias
    float3 uvRow0[MaterialUVChannels]; // GpuMaterial's UV channels (instance_texture_uv)
    float3 uvRow1[MaterialUVChannels];
    uint32_t uvSources;
    float normalScale;
    float occlusionStrength;

    uint32_t boneMatrixOffset;
    uint32_t skinnedVertexOffset;
    int entityID;
};

float2 instance_texture_uv(InstanceData instance, int set, float2 mesh0, float2 mesh1)
{
    const uint channel = min(uint(max(set, 0)), MaterialUVChannels - 1u);
    return uv_transform(instance.uvRow0[channel], instance.uvRow1[channel], ((instance.uvSources >> channel) & 1u) != 0u ? mesh1 : mesh0);
}

float2 instance_texture_uv_delta(InstanceData instance, int set, float2 mesh0, float2 mesh1)
{
    const uint channel = min(uint(max(set, 0)), MaterialUVChannels - 1u);
    return uv_transform_delta(instance.uvRow0[channel], instance.uvRow1[channel], ((instance.uvSources >> channel) & 1u) != 0u ? mesh1 : mesh0);
}

struct SceneInstances
{
    BufferRef<GpuInstance> instances;
    BufferRef<GpuTransform> transforms;
    BufferRef<GpuMaterial> materials;
    BufferRef<GpuMesh> meshes;

    __init(UniformBufferObject ubo)
    {
        instances = BufferRef<GpuInstance>(ubo.sceneInstancesReference);
        transforms = BufferRef<GpuTransform>(ubo.sceneTransformsReference);
        materials = BufferRef<GpuMaterial>(ubo.sceneMaterialsReference);
        meshes = BufferRef<GpuMesh>(ubo.sceneMeshesReference);
    }

    // The instance's material record (its lobes and their textures, which InstanceData leaves out).
    GpuMaterial material(uint slot)
    {
        return materials[instances[slot].materialIndex];
    }

    // How far (world units) a material graph's World Position Offset may move the instance's vertices past its mesh's bounds: the culling
    // grows the bounds by it (its Max World Position Offset Displacement; 0 without an offset, or with no maximum set).
    float offsetBounds(uint slot)
    {
        GpuMaterial m = material(slot);
        return m.graphSlot != 0xFFFFFFFFu && (m.graphOutputs & MaterialGraphOutputWorldPositionOffset) != 0u ? m.graphMaxDisplacement : 0.0;
    }

    __subscript(uint slot) -> InstanceData
    {
        get
        {
            GpuInstance instance = instances[slot];
            GpuTransform transform = transforms[slot];
            GpuMaterial material = materials[instance.materialIndex];
            GpuMesh mesh = meshes[instance.meshIndex];

            InstanceData data;
            data.modelMatrix = transform.world;
            data.normalMatrix = transform.normal;
            data.previousModelMatrix = transform.previousWorld;

            data.drawsOffset = mesh.drawsOffset;
            data.boundsOffset = mesh.boundsOffset;
            data.meshletCount = mesh.meshletCount;
            data.pageOffset = mesh.pageOffset;
            data.vertexColors = mesh.flags & GpuMeshVertexColors;
            data.boundsMin = mesh.boundsMin;
            data.boundsMax = mesh.boundsMax;

            data.workflow = material.workflow;
            data.diffuseFactor = material.diffuseFactor;
            data.specularFactor = material.specularFactor;
            data.baseColorFactor = material.baseColorFactor;
            data.baseColorTextureIndex = material.baseColorTextureIndex;
            data.baseColorTextureSet = material.baseColorTextureSet;
            data.metallicFactor = material.metallicFactor;
            data.roughnessFactor = material.roughnessFactor;
            data.metallicRoughnessTextureIndex = material.metallicRoughnessTextureIndex;
            data.physicalDescriptorTextureSet = material.physicalDescriptorTextureSet;
            data.normalTextureIndex = material.normalTextureIndex;
            data.normalTextureSet = material.normalTextureSet;
            data.occlusionTextureIndex = material.occlusionTextureIndex;
            data.occlusionTextureSet = material.occlusionTextureSet;
            data.emissiveFactor = material.emissiveFactor;
            data.emissiveTextureIndex = material.emissiveTextureIndex;
            data.emissiveTextureSet = material.emissiveTextureSet;
            data.emissiveStrength = material.emissiveStrength;
            data.transmissionFactor = material.transmissionFactor;
            data.transmissionTextureIndex = material.transmissionTextureIndex;
            data.transmissionTextureSet = material.transmissionTextureSet;
            data.ior = material.ior;
            data.thickness = material.thickness;
            data.attenuationColor = material.attenuationColor;
            data.attenuationDistance = material.attenuationDistance;
            data.alphaMode = material.alphaMode;
            data.alphaMaskCutoff = material.alphaMaskCutoff;
            data.doubleSided = material.doubleSided;
            data.shadingModel = material.shadingModel;
            data.diffuseTransmission = material.diffuseTransmission;
            data.depthBias = material.depthBias;
            data.uvRow0 = material.uvRow0;
            data.uvRow1 = material.uvRow1;
            data.uvSources = material.uvSources;
            data.normalScale = material.normalScale;
            data.occlusionStrength = material.occlusionStrength;

            data.boneMatrixOffset = instance.boneMatrixOffset;
            data.skinnedVertexOffset = instance.skinnedVertexOffset;
            data.entityID = instance.entityID;
            return data;
        }
    }
};
