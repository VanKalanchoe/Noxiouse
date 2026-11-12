#include "shaderIO.h"

// AVBOIT's two compute steps (OITShared.slang has the whole method; Renderer::addAVBOITPasses runs them).

// Is any of the finest virtual slices in [v << shift, (v + 1) << shift) occupied?
bool avboit_virtual_occupied(uint words[AVBOIT_VIRTUAL_SLICES / 32], uint v, uint shift)
{
    const uint first = v << shift;
    const uint size = 1u << shift;
    if (size >= 32u)
    {
        for (uint w = first >> 5u; w < (first + size) >> 5u; ++w)
        {
            if (words[w] != 0u)
                return true;
        }
        return false;
    }
    const uint mask = ((1u << size) - 1u) << (first & 31u);
    return (words[first >> 5u] & mask) != 0u;
}

// 2. the depth warp: the finest virtual resolution whose occupied slices fit the physical ones (halved until they do), each occupied
// virtual slice one physical slice in order, an empty one pointing at where the next occupied one starts. One thread: 1024 slices.
[shader("compute")]
[numthreads(1, 1, 1)]
void lutMain(uniform PushConstantAVBOIT pushConstant)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    uint* occupancy = (uint*)ubo->avboitOccupancyReference;
    AvboitDepthWarp* warp = (AvboitDepthWarp*)ubo->avboitLutReference;

    uint words[AVBOIT_VIRTUAL_SLICES / 32];
    for (uint w = 0; w < AVBOIT_VIRTUAL_SLICES / 32u; ++w)
        words[w] = occupancy[w];

    const uint budget = max(ubo->avboitSlices, 1u);
    uint shift = 0;
    for (; shift < 10u; ++shift)
    {
        const uint virtualCount = AVBOIT_VIRTUAL_SLICES >> shift;
        uint count = 0;
        for (uint v = 0; v < virtualCount; ++v)
            count += avboit_virtual_occupied(words, v, shift) ? 1u : 0u;
        if (count <= budget)
            break;
    }

    const uint virtualCount = AVBOIT_VIRTUAL_SLICES >> shift;
    uint physical = 0;
    for (uint v = 0; v < virtualCount; ++v)
    {
        if (avboit_virtual_occupied(words, v, shift))
        {
            warp->entries[v] = physical | 0x80000000u;
            ++physical;
        }
        else
            warp->entries[v] = physical;
    }
    warp->virtualShift = shift;
    warp->virtualCount = virtualCount;
    warp->usedSlices = physical;
}

// 4. integration: per column, the extinction summed front to back; layer p of the volume is the transmittance in front of slice p
// (layer usedSlices: behind everything). A channel that overflowed in a slice has nothing left from there on.
[shader("compute")]
[numthreads(8, 8, 1)]
void integrateMain(uint3 id : SV_DispatchThreadID, uniform PushConstantAVBOIT pushConstant)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    const uint width = ubo->avboitGridWidth;
    const uint height = ubo->avboitGridHeight;
    if (id.x >= width || id.y >= height)
        return;
    AvboitDepthWarp* warp = (AvboitDepthWarp*)ubo->avboitLutReference;
    uint* extinction = (uint*)ubo->avboitExtinctionReference;
    uint* overflow = (uint*)ubo->avboitOverflowReference;
    RWTexture2DArray<float4> volume = RWTexture2DArray<float4>.Handle(uint2(pushConstant.transmittanceStorageIndex, 0));

    const uint columnIndex = (id.y * width + id.x) * 3u;
    const uint3 firstOverflow = uint3(overflow[columnIndex], overflow[columnIndex + 1u], overflow[columnIndex + 2u]);
    const uint used = min(warp->usedSlices, ubo->avboitSlices);
    // 255 units of extinction = transmittance 1/255: optical depth ln(255).
    const float unit = log(255.0) / 255.0;
    float3 opticalDepth = float3(0.0);
    for (uint p = 0; p <= used; ++p)
    {
        const float3 transmittance = exp(-opticalDepth * unit);
        volume[uint3(id.xy, p)] = float4(transmittance, dot(transmittance, float3(1.0 / 3.0)));
        if (p == used)
            break;
        const uint packed = extinction[(p * height + id.y) * width + id.x];
        [unroll]
        for (uint c = 0; c < 3u; ++c)
            opticalDepth[c] = p >= firstOverflow[c] ? 1e4 : opticalDepth[c] + float((packed >> (10u * c)) & 0x3FFu);
    }
}
