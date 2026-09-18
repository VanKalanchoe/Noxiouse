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
// asks for more than the image holds.
float relativeMipLod(UniformBufferObject* ubo, uint textureIndex, float2 ddxUV, float2 ddyUV)
{
    Texture2D texture = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + textureIndex, 0));
    uint width, height;
    texture.GetDimensions(width, height);
    const float2 size = float2(width, height);
    const float2 dx = ddxUV * size;
    const float2 dy = ddyUV * size;
    return 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-12));
}

// Debug view "Texture Streaming Mips": red wants mips the image does not hold yet, green holds what it needs, blue holds
// more than it needs (kept while the pool has room). Shaded by the surface's brightness so the scene stays readable.
float3 mipStreamingDebugColor(float relativeLod, float3 baseColor)
{
    const float3 color = relativeLod < -0.5 ? float3(1.0, 0.15, 0.1) : (relativeLod < 1.5 ? float3(0.2, 0.9, 0.2) : float3(0.2, 0.4, 1.0));
    return color * (0.35 + 0.65 * dot(baseColor, float3(0.2126, 0.7152, 0.0722)));
}

// The mip the texture is asked for, min-ed into its slot (the CPU adds the image's first mip back).
void reportMipFeedback(UniformBufferObject* ubo, uint textureIndex, float2 ddxUV, float2 ddyUV)
{
    if (textureIndex == 0xFFFFFFFF || textureIndex >= MipFeedbackSlots)
        return;

    const float lod = relativeMipLod(ubo, textureIndex, ddxUV, ddyUV);

    // Most reports repeat what another pixel already wrote: a plain read first skips the atomic (and its contention on
    // the few addresses a screen full of the same textures hits). A stale read only costs the atomic.
    uint32_t* feedback = (uint32_t*)ubo->mipFeedbackReference;
    const uint value = uint(clamp(floor(lod) + float(MipFeedbackBias), 0.0, 31.0));
    if (feedback[textureIndex] <= value)
        return;
    uint previous;
    InterlockedMin(feedback[textureIndex], value, previous);
}
