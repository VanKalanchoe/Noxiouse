// The local light grid of the emissive lights (shaderIO.h EmissiveGridSizeX/Y/Z): one thread per cell. Each cell keeps
// EmissiveGridEntries entries of the emissive light list (UniformBufferObject::emissiveLightsReference), drawn stratified by each
// light's power over its squared distance from the cell (its bounds' nearest distance, held at half the cell's diagonal), so a
// lamp a metre away is picked over the bright street a block away. PathTracer/EmissiveLights.slang samples a cell's entries,
// mixed with the global list (which keeps every light possible, so the estimate stays unbiased).
#include "shaderIO.h"

float emissive_grid_weight(EmissiveLightInstance light, float3 cellCenter, float minDistance)
{
    const float distance = max(length(cellCenter - light.center) - light.radius, minDistance);
    return light.weight / (distance * distance);
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
    EmissiveLightInstance* lights = (EmissiveLightInstance*)ubo->emissiveLightsReference;
    const uint lightCount = ubo->emissiveLightCount;

    const uint3 coord = uint3(cell % EmissiveGridSizeX, (cell / EmissiveGridSizeX) % EmissiveGridSizeY, cell / (EmissiveGridSizeX * EmissiveGridSizeY));
    const float cellSize = ubo->emissiveGridCellSize;
    const float3 center = ubo->emissiveGridOrigin + (float3(coord) + 0.5) * cellSize;
    const float minDistance = 0.866 * cellSize; // half the cell's diagonal

    float total = 0.0;
    for (uint i = 0; i < lightCount; ++i)
        total += emissive_grid_weight(lights[i], center, minDistance);
    if (!(total > 0.0) || !isfinite(total))
    {
        for (uint k = 0; k < EmissiveGridEntries; ++k)
            entries[first + k] = 0xFFFFFFFF;
        return;
    }

    // Stratified over the running sum: the k-th pick is where it passes (k + u) / entries of the total. The offset changes every
    // frame (the picks are then unbiased over frames as well); a light worth more than one stratum takes several entries.
    const float u = frac(0.5 + float(pc.frameIndex % 4096u) * 0.6180339887 + float(cell) * 0.7548776662);
    uint picked = 0;
    float running = 0.0;
    uint lastLight = 0;
    for (uint i = 0; i < lightCount && picked < EmissiveGridEntries; ++i)
    {
        const float weight = emissive_grid_weight(lights[i], center, minDistance);
        if (weight <= 0.0)
            continue;
        running += weight;
        lastLight = i;
        while (picked < EmissiveGridEntries && (float(picked) + u) / float(EmissiveGridEntries) * total < running)
        {
            entries[first + picked] = i;
            ++picked;
        }
    }
    for (; picked < EmissiveGridEntries; ++picked) // rounding at the end of the sum
        entries[first + picked] = lastLight;
}
