// The path tracer's emissive triangle lights (PathTracer/EmissiveLights.slang), every frame, as RTXPT bakes its emissive triangles:
//  trianglesMain: one thread per triangle of the emissive instances (UniformBufferObject::emissiveLights*, in list order): its
//                 instance and triangle, world area and centroid, and its power -- the luminance of its average emission (the
//                 emissive factor times the emissive texture read at the triangle's centroid at the mip that covers the triangle)
//                 times its area. A lamp's fixture triangles have no emission in the texture: power 0, never sampled.
//  scanMain:      the running sum of the powers (one workgroup), the global distribution.
//  buildMain:     one thread per cell of the grid around the camera (shaderIO.h EmissiveGridSizeX/Y/Z): EmissiveGridCandidates
//                 triangles drawn by power, of which EmissiveGridEntries are kept by the light each gives the cell (its power over
//                 its squared distance, held at half the cell's diagonal) -- the lamp a metre away over the bright street a block
//                 away. The sampling counts a cell's entries for its probabilities, mixed with the global distribution (every
//                 triangle stays possible, the estimate unbiased).
#include "shaderIO.h"

static float hash11(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return float(x) * (1.0 / 4294967296.0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void trianglesMain(uint3 tid : SV_DispatchThreadID, uniform PushConstantEmissiveGrid pc)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pc.matrixReference;
    const uint triangle = tid.x;
    if (triangle >= ubo->emissiveTriangleCount)
        return;

    // The instance: the last list entry starting at or before this triangle.
    EmissiveLightInstance* lights = (EmissiveLightInstance*)ubo->emissiveLightsReference;
    uint low = 0;
    uint high = ubo->emissiveLightCount - 1;
    while (low < high)
    {
        const uint middle = (low + high + 1) / 2;
        if (lights[middle].triangleOffset <= triangle)
            low = middle;
        else
            high = middle - 1;
    }
    const EmissiveLightInstance light = lights[low];
    const uint primitive = triangle - light.triangleOffset;

    EmissiveTriangle record;
    record.instanceSlot = light.instanceSlot;
    record.primitive = primitive;
    record.power = 0.0;
    record.area = 0.0;
    record.centroid = float3(0.0);
    record.emissiveTrianglePadding = 0.0;

    if (primitive < light.triangleCount)
    {
        GpuRayTracingInstance instance = ((GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference)[light.instanceSlot];
        const float4x4 world = ((GpuTransform*)ubo->sceneTransformsReference)[light.instanceSlot].world;
        uint3 index = instance.indexBufferAddress != 0 ? ((uint3*)instance.indexBufferAddress)[primitive]
                                                       : uint3(primitive * 3 + 0, primitive * 3 + 1, primitive * 3 + 2);
        const Vertex v0 = LoadRayTracingVertex(instance, index.x);
        const Vertex v1 = LoadRayTracingVertex(instance, index.y);
        const Vertex v2 = LoadRayTracingVertex(instance, index.z);
        const float3 p0 = mul(world, float4(v0.pos, 1.0)).xyz;
        const float3 p1 = mul(world, float4(v1.pos, 1.0)).xyz;
        const float3 p2 = mul(world, float4(v2.pos, 1.0)).xyz;
        record.area = 0.5 * length(cross(p1 - p0, p2 - p0));
        record.centroid = (p0 + p1 + p2) / 3.0;

        float3 emission = instance.emissiveFactor.rgb * instance.emissiveFactor.a;
        if (instance.emissiveTextureIndex != 0xFFFFFFFF)
        {
            Texture2D emissive = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.emissiveTextureIndex, 0));
            uint width, height, levels;
            emissive.GetDimensions(0, width, height, levels);
            const float2 e1 = v1.uv0 - v0.uv0;
            const float2 e2 = v2.uv0 - v0.uv0;
            const float uvArea = 0.5 * abs(e1.x * e2.y - e1.y * e2.x);
            // The mip whose texel covers the triangle: its average emission.
            const float lod = clamp(0.5 * log2(max(uvArea * float(width) * float(height), 1e-8)), 0.0, float(levels - 1));
            emission *= emissive.SampleLevel(SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0)),
                                             (v0.uv0 + v1.uv0 + v2.uv0) / 3.0, lod).rgb;
        }
        const float power = dot(max(emission, float3(0.0)), float3(0.2126, 0.7152, 0.0722)) * record.area;
        record.power = isfinite(power) ? power : 0.0;
    }
    ((EmissiveTriangle*)ubo->emissiveTrianglesReference)[triangle] = record;
}

groupshared float gScan[EmissiveScanGroupSize];

[shader("compute")]
[numthreads(EmissiveScanGroupSize, 1, 1)]
void scanMain(uint3 tid : SV_GroupThreadID, uniform PushConstantEmissiveGrid pc)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pc.matrixReference;
    EmissiveTriangle* triangles = (EmissiveTriangle*)ubo->emissiveTrianglesReference;
    float* cdf = (float*)ubo->emissiveTriangleCdfReference;
    const uint count = ubo->emissiveTriangleCount;
    const uint thread = tid.x;

    float running = 0.0;
    for (uint base = 0; base < count; base += EmissiveScanGroupSize)
    {
        const uint i = base + thread;
        gScan[thread] = i < count ? triangles[i].power : 0.0;
        GroupMemoryBarrierWithGroupSync();
        // Inclusive Hillis-Steele scan of the block.
        for (uint offset = 1; offset < EmissiveScanGroupSize; offset <<= 1)
        {
            const float add = thread >= offset ? gScan[thread - offset] : 0.0;
            GroupMemoryBarrierWithGroupSync();
            gScan[thread] += add;
            GroupMemoryBarrierWithGroupSync();
        }
        if (i < count)
            cdf[i] = running + gScan[thread];
        running += gScan[EmissiveScanGroupSize - 1];
        GroupMemoryBarrierWithGroupSync();
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void buildMain(uint3 tid : SV_DispatchThreadID, uniform PushConstantEmissiveGrid pc)
{
    const uint cellCount = EmissiveGridSizeX * EmissiveGridSizeY * EmissiveGridSizeZ;
    const uint cell = tid.x;
    if (cell >= cellCount)
        return;

    UniformBufferObject* ubo = (UniformBufferObject*)pc.matrixReference;
    uint* entries = (uint*)ubo->emissiveGridReference;
    const uint first = cell * EmissiveGridEntries;
    const uint count = ubo->emissiveTriangleCount;
    EmissiveTriangle* triangles = (EmissiveTriangle*)ubo->emissiveTrianglesReference;
    float* cdf = (float*)ubo->emissiveTriangleCdfReference;
    const float total = count > 0 ? cdf[count - 1] : 0.0;
    if (!(total > 0.0) || !isfinite(total))
    {
        for (uint k = 0; k < EmissiveGridEntries; ++k)
            entries[first + k] = 0xFFFFFFFF;
        return;
    }

    const uint3 coord = uint3(cell % EmissiveGridSizeX, (cell / EmissiveGridSizeX) % EmissiveGridSizeY, cell / (EmissiveGridSizeX * EmissiveGridSizeY));
    const float cellSize = ubo->emissiveGridCellSize;
    const float3 center = ubo->emissiveGridOrigin + (float3(coord) + 0.5) * cellSize;
    const float minDistanceSq = (0.866 * cellSize) * (0.866 * cellSize); // half the cell's diagonal

    // Candidates by power (stratified over the running sum); each weighs its light at the cell over its power -- the resampling
    // weight of a power-drawn candidate for a target of power / distance^2.
    uint candidates[EmissiveGridCandidates];
    float weights[EmissiveGridCandidates];
    float weightSum = 0.0;
    const float offset = hash11(cell * 9781u + pc.frameIndex * 6271u);
    for (uint m = 0; m < EmissiveGridCandidates; ++m)
    {
        const float target = (float(m) + offset) / float(EmissiveGridCandidates) * total;
        uint low = 0;
        uint high = count - 1;
        while (low < high)
        {
            const uint middle = (low + high) / 2;
            if (cdf[middle] > target)
                high = middle;
            else
                low = middle + 1;
        }
        const float3 toLight = triangles[low].centroid - center;
        candidates[m] = low;
        weights[m] = 1.0 / max(dot(toLight, toLight), minDistanceSq);
        weightSum += weights[m];
    }

    // Kept by weight, stratified.
    const float keepOffset = hash11(cell * 26699u + pc.frameIndex * 7919u + 17u);
    uint kept = 0;
    float running = 0.0;
    for (uint m = 0; m < EmissiveGridCandidates && kept < EmissiveGridEntries; ++m)
    {
        running += weights[m];
        while (kept < EmissiveGridEntries && (float(kept) + keepOffset) / float(EmissiveGridEntries) * weightSum < running)
        {
            entries[first + kept] = candidates[m];
            ++kept;
        }
    }
    for (; kept < EmissiveGridEntries; ++kept) // rounding at the end of the sum
        entries[first + kept] = candidates[EmissiveGridCandidates - 1];
}
