// Order-independent transparency, both methods (Settings > Order-Independent Transparency): what a transparent fragment writes into
// the OIT targets (oit_output), and AVBOIT's volume (Adaptive Voxel-Based OIT, Michal Drobot, "Advances in Real-Time Rendering",
// SIGGRAPH 2025): the transmittance in front of every depth, built before the transparent surfaces are shaded.
//
// AVBOIT, per frame (Renderer::addAVBOITPasses):
//   1. occupancy: every transparent surface, drawn at 1/8 resolution, marks the virtual depth slices it is in (1024 along the view
//      ray on a log curve: about linear close by, logarithmic beyond).
//   2. depth warp (AVBOITBuild.slang lutMain): the virtual resolution is halved until the occupied slices fit the physical ones; empty
//      depth ranges take no physical slice (the slides' adaptive "depth warp LUT").
//   3. splat: each surface again, adding its extinction -log(transmittance) per color channel to the two physical slices around its
//      depth (linear splatting), 8 bits per channel packed with 2 overflow bits into one atomic add; a slice that overflows saves its
//      index (the slides' "overflow min depth").
//   4. integration (AVBOITBuild.slang integrateMain): the extinction summed front to back into the transmittance in front of every
//      slice (RGBA8: rgb colored -- tinted glass --, a mono).
//   5. the shading pass weights each fragment by the transmittance in front of it (two slices nearer, so its own splat is not), and
//      the composite normalizes the sum by the exact total coverage (OITComposite.slang).
// Requires shaderIO.h before it.

// ---- AVBOIT ---------------------------------------------------------------------------------------------------------------------

// Where along the slices a distance from the camera is, 0..1: a log curve, nearly linear closer than avboitLinearDistance (the slides'
// "linearization factor"), ending at avboitDistance.
float avboit_curve(UniformBufferObject* ubo, float distance)
{
    const float linear = max(ubo->avboitLinearDistance, 1e-4);
    const float range = max(ubo->avboitDistance, linear);
    return saturate(log2(1.0 + max(distance, 0.0) / linear) / log2(1.0 + range / linear));
}

// The physical slice coordinate of a distance (slice p covers [p, p + 1)): its virtual slice at the resolution that fit, through the
// depth warp. A virtual slice nothing is in takes no room: it sits where the next occupied one starts.
float avboit_physical_slice(UniformBufferObject* ubo, float distance)
{
    AvboitDepthWarp* warp = (AvboitDepthWarp*)ubo->avboitLutReference;
    const uint count = max(warp->virtualCount, 1u);
    const float v = avboit_curve(ubo, distance) * float(count);
    const uint index = min(uint(v), count - 1u);
    const uint entry = warp->entries[index];
    const float physical = float(entry & 0xFFFFu);
    return (entry & 0x80000000u) != 0u ? physical + saturate(v - float(index)) : physical;
}

// The prepasses draw at the volume's resolution, with no depth buffer of their own: a fragment behind the opaque scene (at the middle
// of its 8 x 8 pixels) adds nothing.
bool avboit_in_front_of_scene(UniformBufferObject* ubo, float2 gridPosition, float fragmentDepth)
{
    if (ubo->avboitDepthTextureIndex == 0xFFFFFFFF)
        return true;
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + ubo->avboitDepthTextureIndex, 0));
    uint width, height;
    depthTex.GetDimensions(width, height);
    const int2 pixel = int2(gridPosition * float(AVBOIT_TILE_SIZE));
    const float sceneDepth = depthTex.Load(int3(clamp(pixel, int2(0), int2(int(width) - 1, int(height) - 1)), 0));
    return fragmentDepth >= sceneDepth; // reverse Z: nearer is larger
}

// The full-resolution pixel at the middle of a prepass fragment's column (for a test that reads the scene depth itself).
float2 avboit_full_resolution_pixel(float2 gridPosition)
{
    return gridPosition * float(AVBOIT_TILE_SIZE);
}

// 1. occupancy: the finest virtual slice this distance is in.
void avboit_mark_occupied(UniformBufferObject* ubo, float distance)
{
    uint* occupancy = (uint*)ubo->avboitOccupancyReference;
    const uint v = min(uint(avboit_curve(ubo, distance) * float(AVBOIT_VIRTUAL_SLICES)), AVBOIT_VIRTUAL_SLICES - 1u);
    const uint word = v >> 5u;
    const uint bit = 1u << (v & 31u);
    // Most fragments find their slice marked already: a plain read first skips the atomic.
    if ((occupancy[word] & bit) == 0u)
    {
        uint previous;
        InterlockedOr(occupancy[word], bit, previous);
    }
}

void avboit_splat_slice(UniformBufferObject* ubo, uint2 column, int slice, float3 amount)
{
    const uint3 values = uint3(min(amount + 0.5, float3(255.0)));
    if (all(values == uint3(0u)))
        return;
    const uint s = uint(clamp(slice, 0, int(ubo->avboitSlices) - 1));
    const uint packed = values.r | (values.g << 10u) | (values.b << 20u);
    uint* extinction = (uint*)ubo->avboitExtinctionReference;
    uint previous;
    InterlockedAdd(extinction[(s * ubo->avboitGridHeight + column.y) * ubo->avboitGridWidth + column.x], packed, previous);
    // A channel past 255 in this slice: what is behind it there sees nothing of that channel (the integration saturates from the first
    // such slice on); the 2 spare bits keep the carry out of the next channel for a few more layers.
    uint* overflow = (uint*)ubo->avboitOverflowReference;
    const uint columnIndex = (column.y * ubo->avboitGridWidth + column.x) * 3u;
    [unroll]
    for (uint c = 0; c < 3u; ++c)
    {
        if (((previous >> (10u * c)) & 0x3FFu) + values[c] > 255u)
        {
            uint ignored;
            InterlockedMin(overflow[columnIndex + c], s, ignored);
        }
    }
}

// 3. splat: this surface's extinction per channel, 255 = transmittance 1/255 (nothing left), split between the two physical slices
// around its depth by how near it is to each (the slides' linear splatting).
void avboit_splat(UniformBufferObject* ubo, uint2 column, float distance, float3 transmittance)
{
    if (column.x >= ubo->avboitGridWidth || column.y >= ubo->avboitGridHeight)
        return;
    const float3 extinction = saturate(-log(max(saturate(transmittance), float3(1e-6))) / log(255.0)) * 255.0;
    if (all(extinction < 0.5))
        return;
    const float s = avboit_physical_slice(ubo, distance) - 0.5;
    const float base = floor(s);
    const float fraction = s - base;
    avboit_splat_slice(ubo, column, int(base), extinction * (1.0 - fraction));
    avboit_splat_slice(ubo, column, int(base) + 1, extinction * fraction);
}

// 5. the transmittance in front of a surface at this full-resolution pixel and distance (rgb, a mono): the volume between its columns
// (bilinear) and between its slices (linear), two slices nearer than the surface (the slides' bias for linear splatting and sampling).
float4 avboit_transmittance(UniformBufferObject* ubo, float2 pixel, float distance)
{
    if (ubo->avboitTransmittanceTextureIndex == 0xFFFFFFFF)
        return float4(1.0);
    Texture2DArray volume = Texture2DArray.Handle(uint2(ubo->imageHeapIndexOffset + ubo->avboitTransmittanceTextureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    const float2 uv = pixel / float2(float(ubo->avboitGridWidth * AVBOIT_TILE_SIZE), float(ubo->avboitGridHeight * AVBOIT_TILE_SIZE));
    const float slice = avboit_physical_slice(ubo, distance) - 2.0;
    if (slice <= 0.0)
        return float4(1.0); // nothing can be in front of the first slice
    const float layer = floor(slice);
    const float fraction = slice - layer;
    const float lastLayer = float(ubo->avboitSlices); // the volume holds avboitSlices + 1 layers: in front of each slice, and behind all
    const float4 front = volume.SampleLevel(linearClamp, float3(uv, min(layer, lastLayer)), 0.0);
    const float4 back = volume.SampleLevel(linearClamp, float3(uv, min(layer + 1.0, lastLayer)), 0.0);
    return lerp(front, back, fraction);
}

// ---- Both methods: what a fragment writes --------------------------------------------------------------------------------------

// accum (summed): its light times its weight, and its coverage times the same weight; reveal (multiplied in: dst * (1 - src)): its
// coverage per color channel (rgb: what it lets through, tinted) and mono (a). The weight: the transmittance in front of it (AVBOIT),
// or the depth weight of weighted blended OIT (McGuire and Bavoil 2013, eq. 9). OITComposite.slang:
//   scene * reveal.rgb + accum.rgb / accum.a * (1 - reveal.a).
struct OITOutput
{
    float4 accum;
    float4 reveal;
};

// `premultiplied`: the surface's own light (already times its coverage); `transmittance`: what it lets through of what is behind it,
// per channel (1 - alpha for a surface that only covers; tinted for glass); `pixel`: the full-resolution SV_Position.xy.
OITOutput oit_output(UniformBufferObject* ubo, float3 premultiplied, float3 transmittance, float3 worldPos, float2 pixel)
{
    const float3 alpha = 1.0 - saturate(transmittance);
    const float alphaMono = dot(alpha, float3(1.0 / 3.0));
    const float distance = length(worldPos - ubo->cameraWorldPos.xyz);
    OITOutput output;
    output.reveal = float4(alpha, alphaMono);
    if (ubo->oitMode == OIT_AVBOIT)
    {
        const float4 front = avboit_transmittance(ubo, pixel, distance);
        output.accum = float4(premultiplied * front.rgb, alphaMono * front.a);
        return output;
    }
    const float z = max(distance / ubo->unitsPerMeter, 0.0);
    // Eq. 9 of the paper, z in meters; the cap is lower than its 3e3: HDR colors in a half-float target overflow sooner.
    float weight = alphaMono * clamp(10.0 / (1e-5 + pow(z / 5.0, 2.0) + pow(z / 200.0, 6.0)), 1e-2, 3e2);
    const float brightest = max(max(premultiplied.r, premultiplied.g), premultiplied.b);
    if (brightest * weight > 2048.0) // a few overlapping layers of the brightest still fit under the half float's 65504
        weight = 2048.0 / brightest;
    output.accum = float4(premultiplied, alphaMono) * weight;
    return output;
}
