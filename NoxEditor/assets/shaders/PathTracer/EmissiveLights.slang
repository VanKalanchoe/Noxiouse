#pragma once

#include "../shaderIO.h"

// Emissive meshes as lights for the path tracer's next event estimation (UniformBufferObject::emissiveLights*). An instance is
// picked from the light grid's cell around the point (EmissiveLightGrid.slang: the lights that matter there) LocalShare of the
// time, else from the whole list by weight (its emission's luminance times its approximate area: GpuScene::CollectEmissiveLights);
// then one of its triangles uniformly, a point on that triangle uniformly. A BSDF ray that hits an emissive surface weighs its
// emission against this sampling with the same pdf (PathTracerHit.slang), so the two add up to the emission once.
namespace EmissiveLights
{
    // How often a light sample goes to the emissive meshes rather than the analytic lights and the environment: half the time when
    // both exist (their own selection inside each), all of it when nothing else lights the scene.
    float branchProbability(UniformBufferObject* ubo, bool hasEnvironment)
    {
        if (ubo->emissiveLightCount == 0 || ubo->emissiveLightTotalWeight <= 0.0)
            return 0.0;
        return (ubo->lightCount + (hasEnvironment ? 1u : 0u)) == 0 ? 1.0 : 0.5;
    }

    static const float LocalShare = 0.75;

    // The light grid's cell of a point (its first entry's index), ~0 outside the grid or where no light reaches.
    uint gridCell(UniformBufferObject* ubo, float3 position)
    {
        if (ubo->emissiveGridReference == 0 || ubo->emissiveGridCellSize <= 0.0)
            return 0xFFFFFFFF;
        const float3 local = (position - ubo->emissiveGridOrigin) / ubo->emissiveGridCellSize;
        if (any(local < 0.0) || local.x >= float(EmissiveGridSizeX) || local.y >= float(EmissiveGridSizeY) || local.z >= float(EmissiveGridSizeZ))
            return 0xFFFFFFFF;
        const uint3 coord = uint3(local);
        const uint first = ((coord.z * EmissiveGridSizeY + coord.y) * EmissiveGridSizeX + coord.x) * EmissiveGridEntries;
        return ((uint*)ubo->emissiveGridReference)[first] != 0xFFFFFFFF ? first : 0xFFFFFFFF;
    }

    // The probability that the sampling picks list entry `entry` at a point in cell `first` (gridCell).
    float entryProbability(UniformBufferObject* ubo, uint entry, uint first)
    {
        const EmissiveLightInstance light = ((EmissiveLightInstance*)ubo->emissiveLightsReference)[entry];
        const float global = light.weight / ubo->emissiveLightTotalWeight;
        if (first == 0xFFFFFFFF)
            return global;
        uint* entries = (uint*)ubo->emissiveGridReference;
        uint count = 0;
        for (uint k = 0; k < EmissiveGridEntries; ++k)
            count += entries[first + k] == entry ? 1u : 0u;
        return LocalShare * float(count) / float(EmissiveGridEntries) + (1.0 - LocalShare) * global;
    }

    // The list entry of an instance (the list is sorted by slot); ~0 when it is not an emissive light.
    uint find(UniformBufferObject* ubo, uint instanceSlot)
    {
        if (ubo->emissiveLightCount == 0)
            return 0xFFFFFFFF;
        EmissiveLightInstance* lights = (EmissiveLightInstance*)ubo->emissiveLightsReference;
        uint low = 0;
        uint high = ubo->emissiveLightCount;
        while (low < high)
        {
            const uint middle = (low + high) / 2;
            if (lights[middle].instanceSlot < instanceSlot)
                low = middle + 1;
            else
                high = middle;
        }
        return low < ubo->emissiveLightCount && lights[low].instanceSlot == instanceSlot ? low : 0xFFFFFFFF;
    }

    // The solid angle pdf of the emissive sampling (without the branch probability) from `origin` for a point of an instance's
    // triangle of `triangleArea`, seen from `distance` away at `cosLight` to its face. 0 when the instance is not an emissive light.
    float pdf(UniformBufferObject* ubo, float3 origin, uint instanceSlot, float triangleArea, float distance, float cosLight)
    {
        const uint entry = find(ubo, instanceSlot);
        if (entry == 0xFFFFFFFF || triangleArea <= 0.0 || cosLight <= 1e-6)
            return 0.0;
        const EmissiveLightInstance light = ((EmissiveLightInstance*)ubo->emissiveLightsReference)[entry];
        const float areaPdf = entryProbability(ubo, entry, gridCell(ubo, origin)) / (float(light.triangleCount) * triangleArea);
        return areaPdf * distance * distance / cosLight;
    }

    struct Sample
    {
        float3 direction;  // towards the light point
        float distance;
        float3 radiance;   // its emission
        float pdf;         // solid angle, without the branch probability
        bool valid;
    };

    Sample sample(UniformBufferObject* ubo, float3 position, float4 u)
    {
        Sample result = (Sample)0;
        EmissiveLightInstance* lights = (EmissiveLightInstance*)ubo->emissiveLightsReference;

        // The instance: one of the cell's entries, else the first whose running weight passes u.x of the total.
        const uint first = gridCell(ubo, position);
        uint low = 0;
        if (first != 0xFFFFFFFF && u.x < LocalShare)
        {
            low = ((uint*)ubo->emissiveGridReference)[first + min(uint(u.x / LocalShare * float(EmissiveGridEntries)), EmissiveGridEntries - 1)];
        }
        else
        {
            const float globalU = first != 0xFFFFFFFF ? (u.x - LocalShare) / (1.0 - LocalShare) : u.x;
            const float target = globalU * ubo->emissiveLightTotalWeight;
            uint high = ubo->emissiveLightCount - 1;
            while (low < high)
            {
                const uint middle = (low + high) / 2;
                if (lights[middle].cdf > target)
                    high = middle;
                else
                    low = middle + 1;
            }
        }
        const EmissiveLightInstance light = lights[low];
        const uint triangle = min(uint(u.y * float(light.triangleCount)), light.triangleCount - 1);

        GpuRayTracingInstance instance = ((GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference)[light.instanceSlot];
        const float4x4 world = ((GpuTransform*)ubo->sceneTransformsReference)[light.instanceSlot].world;
        uint3 index = instance.indexBufferAddress != 0 ? ((uint3*)instance.indexBufferAddress)[triangle]
                                                       : uint3(triangle * 3 + 0, triangle * 3 + 1, triangle * 3 + 2);
        const Vertex v0 = LoadRayTracingVertex(instance, index.x);
        const Vertex v1 = LoadRayTracingVertex(instance, index.y);
        const Vertex v2 = LoadRayTracingVertex(instance, index.z);
        const float3 p0 = mul(world, float4(v0.pos, 1.0)).xyz;
        const float3 p1 = mul(world, float4(v1.pos, 1.0)).xyz;
        const float3 p2 = mul(world, float4(v2.pos, 1.0)).xyz;

        // Uniform on the triangle.
        const float root = sqrt(u.z);
        const float3 barycentrics = float3(1.0 - root, root * (1.0 - u.w), root * u.w);
        const float3 point = p0 * barycentrics.x + p1 * barycentrics.y + p2 * barycentrics.z;
        const float3 cross01 = cross(p1 - p0, p2 - p0);
        const float doubleArea = length(cross01);
        if (doubleArea <= 0.0)
            return result;
        const float3 faceNormal = cross01 / doubleArea;

        const float3 toLight = point - position;
        const float distanceSquared = dot(toLight, toLight);
        if (distanceSquared <= 1e-12)
            return result;
        result.distance = sqrt(distanceSquared);
        result.direction = toLight / result.distance;
        // Both faces emit, as a BSDF ray that hits either side picks the emission up (PathTracerHit.slang).
        const float cosLight = abs(dot(faceNormal, result.direction));
        if (cosLight <= 1e-6)
            return result;

        result.radiance = instance.emissiveFactor.rgb * instance.emissiveFactor.a;
        if (instance.emissiveTextureIndex != 0xFFFFFFFF)
        {
            const float2 uv = v0.uv0 * barycentrics.x + v1.uv0 * barycentrics.y + v2.uv0 * barycentrics.z;
            Texture2D emissive = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.emissiveTextureIndex, 0));
            result.radiance *= emissive.SampleLevel(SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0)), uv, 2.0).rgb;
        }
        const float areaPdf = entryProbability(ubo, low, first) / (float(light.triangleCount) * 0.5 * doubleArea);
        result.pdf = areaPdf * distanceSquared / cosLight;
        result.valid = any(result.radiance > 0.0) && result.pdf > 0.0 && isfinite(result.pdf);
        return result;
    }
}
