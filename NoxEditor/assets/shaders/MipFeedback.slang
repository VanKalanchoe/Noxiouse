#pragma once
#include "shaderIO.h"

// Texture streaming feedback (§5.12): which mip the visible pixels want of each texture. One pixel per 8x8 tile reports,
// a different one each frame (37 is coprime with 64, so 64 frames visit every pixel of the tile).
bool isMipFeedbackPixel(int2 pixel, uint frame)
{
    const uint pick = (frame * 37u) & 63u;
    return all((uint2(pixel) & 7u) == uint2(pick & 7u, pick >> 3u));
}

// The mip the UV derivatives ask of the texture, relative to the image it samples (the texture's resident mips): below 0
// asks for more than the image holds. As the anisotropic sampler (16x) picks it: the long axis of the footprint over the probes
// along it, not the long axis alone (that asked up to 4 mips too coarse on streets and walls seen at an angle, which then never
// got their sharp mips), and with this frame's LOD bias (UniformBufferObject::mipFeedbackLodBias).
float relativeMipLod(UniformBufferObject* ubo, uint textureIndex, float2 ddxUV, float2 ddyUV)
{
    uint width, height;
    MaterialTextureSize(ubo, textureIndex, width, height);
    const float2 size = float2(width, height);
    const float2 dx = ddxUV * size;
    const float2 dy = ddyUV * size;
    const float lengthX = dot(dx, dx);
    const float lengthY = dot(dy, dy);
    const float major = max(max(lengthX, lengthY), 1e-12);
    const float minor = max(min(lengthX, lengthY), 1e-12);
    const float anisotropy = min(ceil(sqrt(major / minor)), 16.0);
    return 0.5 * log2(major) - log2(anisotropy) + ubo->mipFeedbackLodBias;
}

// Debug view "Texture Streaming Mips": red wants mips the image does not hold yet, green holds what it needs, blue holds
// more than it needs (kept while the pool has room). Shaded by the surface's brightness so the scene stays readable.
float3 mipStreamingDebugColor(float relativeLod, float3 baseColor)
{
    const float3 color = relativeLod < -0.5 ? float3(1.0, 0.15, 0.1) : (relativeLod < 1.5 ? float3(0.2, 0.9, 0.2) : float3(0.2, 0.4, 1.0));
    return color * (0.35 + 0.65 * dot(baseColor, float3(0.2126, 0.7152, 0.0722)));
}

// The mip a texture is asked for (relative to its resident image, bias included), min-ed into its slot (the CPU adds the image's
// first mip back). The path tracer reports this way, with its ray cone LOD (PathTracerHit.slang).
void reportMipFeedbackLod(UniformBufferObject* ubo, uint textureIndex, float lod)
{
    if (textureIndex == 0xFFFFFFFF || textureIndex >= MipFeedbackSlots || ubo->mipFeedbackReference == 0)
        return;

    // Most reports repeat what another pixel already wrote: a plain read first skips the atomic (and its contention on
    // the few addresses a screen full of the same textures hits). A stale read only costs the atomic.
    uint32_t* feedback = (uint32_t*)ubo->mipFeedbackReference;
    const uint value = uint(clamp(floor(lod) + float(MipFeedbackBias), 0.0, 31.0));
    if (feedback[textureIndex] <= value)
        return;
    uint previous;
    InterlockedMin(feedback[textureIndex], value, previous);
}

// The mip the texture is asked for by these UV derivatives (raster passes).
void reportMipFeedback(UniformBufferObject* ubo, uint textureIndex, float2 ddxUV, float2 ddyUV)
{
    if (textureIndex == 0xFFFFFFFF || textureIndex >= MipFeedbackSlots)
        return;
    reportMipFeedbackLod(ubo, textureIndex, relativeMipLod(ubo, textureIndex, ddxUV, ddyUV));
}
