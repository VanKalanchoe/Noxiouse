#pragma once

#include "../shaderIO.h"

// Emissive meshes as lights (UniformBufferObject::emissiveLights*).
//
// The path tracer samples emissive TRIANGLES, as RTXPT does (its LightsBaker turns every emissive triangle into a light weighed by its
// area times its average emission, the emissive texture included): EmissiveLightGrid.slang writes each triangle's record and power
// every frame, their running sum, and a grid of cells around the camera that each keep EmissiveGridEntries triangles picked for the
// light they give there. A light sample takes a triangle from the cell around the point LocalShare of the time, else from all of them
// by power; then a point on it uniformly. A BSDF ray that hits an emissive triangle weighs its emission against this sampling with
// the same probability (PathTracerHit.slang), so the two add up to the emission once.
//
// Picking an instance and then any of its triangles uniformly (the screen-probe GI's sampling below, which the path tracer used too)
// spent nearly every sample of a lamp on its dark fixture -- the Bistro's wall lights have 3584 triangles, the ceiling lamps 1692,
// only their bulbs emit (the emissive texture) -- and the rare sample on a bulb was a firefly the filter clamped away: lamp-lit
// walls and rooms came out black.
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

    struct Sample
    {
        float3 direction;  // towards the light point
        float distance;
        float3 radiance;   // its emission
        float pdf;         // solid angle, without the branch probability
        float solidAngle;  // the triangle's solid angle as this point estimates it (area x cosLight / distance^2)
        bool valid;
    };

    // A point of an emissive instance's triangle seen from `position`: direction, distance, emission (texture at `textureLod`) and the
    // solid angle pdf for an area pdf of `areaProbability` / its area. `barycentricU`: two uniform numbers.
    Sample sampleTrianglePoint(UniformBufferObject* ubo, float3 position, uint instanceSlot, uint triangle, float2 barycentricU,
                               float areaProbability, float textureLod)
    {
        Sample result = (Sample)0;
        GpuRayTracingInstance instance = ((GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference)[instanceSlot];
        const float4x4 world = ((GpuTransform*)ubo->sceneTransformsReference)[instanceSlot].world;
        uint3 index = instance.indexBufferAddress != 0 ? ((uint3*)instance.indexBufferAddress)[triangle]
                                                       : uint3(triangle * 3 + 0, triangle * 3 + 1, triangle * 3 + 2);
        const Vertex v0 = LoadRayTracingVertex(instance, index.x);
        const Vertex v1 = LoadRayTracingVertex(instance, index.y);
        const Vertex v2 = LoadRayTracingVertex(instance, index.z);
        const float3 p0 = mul(world, float4(v0.pos, 1.0)).xyz;
        const float3 p1 = mul(world, float4(v1.pos, 1.0)).xyz;
        const float3 p2 = mul(world, float4(v2.pos, 1.0)).xyz;

        // Uniform on the triangle.
        const float root = sqrt(barycentricU.x);
        const float3 barycentrics = float3(1.0 - root, root * (1.0 - barycentricU.y), root * barycentricU.y);
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
            GpuMaterial* material = &((GpuMaterial*)ubo->sceneMaterialsReference)[instance.materialIndex];
            const float2 uv = material_texture_uv(material, material.emissiveTextureSet, v0.uv0 * barycentrics.x + v1.uv0 * barycentrics.y + v2.uv0 * barycentrics.z,
                                                  v0.uv1 * barycentrics.x + v1.uv1 * barycentrics.y + v2.uv1 * barycentrics.z);
            result.radiance *= SampleMaterialTextureLevel(ubo, instance.emissiveTextureIndex, SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0)), uv, textureLod).rgb;
        }
        const float areaPdf = areaProbability / (0.5 * doubleArea);
        result.pdf = areaPdf * distanceSquared / cosLight;
        result.solidAngle = 0.5 * doubleArea * cosLight / distanceSquared;
        result.valid = any(result.radiance > 0.0) && result.pdf > 0.0 && isfinite(result.pdf);
        return result;
    }

    // ---- The screen-probe GI's sampling: an instance by weight (its emission's luminance times its approximate area:
    // GpuScene::CollectEmissiveLights), one of its triangles uniformly.

    Sample sample(UniformBufferObject* ubo, float3 position, float4 u)
    {
        EmissiveLightInstance* lights = (EmissiveLightInstance*)ubo->emissiveLightsReference;
        // The instance: the first whose running weight passes u.x of the total.
        const float target = u.x * ubo->emissiveLightTotalWeight;
        uint low = 0;
        uint high = ubo->emissiveLightCount - 1;
        while (low < high)
        {
            const uint middle = (low + high) / 2;
            if (lights[middle].cdf > target)
                high = middle;
            else
                low = middle + 1;
        }
        const EmissiveLightInstance light = lights[low];
        const uint triangle = min(uint(u.y * float(light.triangleCount)), light.triangleCount - 1);
        const float probability = (light.weight / ubo->emissiveLightTotalWeight) / float(light.triangleCount);
        return sampleTrianglePoint(ubo, position, light.instanceSlot, triangle, u.zw, probability, 2.0);
    }

    // Its solid angle pdf for a point of an instance's triangle of `triangleArea`, `distance` away at `cosLight` to its face.
    float pdf(UniformBufferObject* ubo, uint instanceSlot, float triangleArea, float distance, float cosLight)
    {
        const uint entry = find(ubo, instanceSlot);
        if (entry == 0xFFFFFFFF || triangleArea <= 0.0 || cosLight <= 1e-6)
            return 0.0;
        const EmissiveLightInstance light = ((EmissiveLightInstance*)ubo->emissiveLightsReference)[entry];
        const float areaPdf = (light.weight / ubo->emissiveLightTotalWeight) / (float(light.triangleCount) * triangleArea);
        return areaPdf * distance * distance / cosLight;
    }

    // ---- The path tracer's sampling: emissive triangles (EmissiveLightGrid.slang).

    static const float LocalShare = 0.75;

    bool hasTriangleLights(UniformBufferObject* ubo)
    {
        return ubo->emissiveTrianglesReference != 0 && ubo->emissiveTriangleCount > 0;
    }

    float triangleTotalPower(UniformBufferObject* ubo)
    {
        return ((float*)ubo->emissiveTriangleCdfReference)[ubo->emissiveTriangleCount - 1];
    }

    // The light grid's cell of a point (its first entry's index), ~0 outside the grid or where no triangle was picked.
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

    // The probability that the sampling picks emissive triangle `triangle` at a point in cell `first` (gridCell).
    float triangleProbability(UniformBufferObject* ubo, uint triangle, uint first)
    {
        const float global = ((EmissiveTriangle*)ubo->emissiveTrianglesReference)[triangle].power / triangleTotalPower(ubo);
        if (first == 0xFFFFFFFF)
            return global;
        uint* entries = (uint*)ubo->emissiveGridReference;
        uint count = 0;
        for (uint k = 0; k < EmissiveGridEntries; ++k)
            count += entries[first + k] == triangle ? 1u : 0u;
        return LocalShare * float(count) / float(EmissiveGridEntries) + (1.0 - LocalShare) * global;
    }

    Sample sampleTriangles(UniformBufferObject* ubo, float3 position, float4 u)
    {
        const float total = triangleTotalPower(ubo);
        if (!(total > 0.0))
            return (Sample)0;
        const uint first = gridCell(ubo, position);
        uint triangle = 0;
        if (first != 0xFFFFFFFF && u.x < LocalShare)
        {
            triangle = ((uint*)ubo->emissiveGridReference)[first + min(uint(u.x / LocalShare * float(EmissiveGridEntries)), EmissiveGridEntries - 1)];
        }
        else
        {
            const float globalU = first != 0xFFFFFFFF ? (u.x - LocalShare) / (1.0 - LocalShare) : u.x;
            const float target = globalU * total;
            float* cdf = (float*)ubo->emissiveTriangleCdfReference;
            uint low = 0;
            uint high = ubo->emissiveTriangleCount - 1;
            while (low < high)
            {
                const uint middle = (low + high) / 2;
                if (cdf[middle] > target)
                    high = middle;
                else
                    low = middle + 1;
            }
            triangle = low;
        }
        const EmissiveTriangle record = ((EmissiveTriangle*)ubo->emissiveTrianglesReference)[triangle];
        if (!(record.power > 0.0))
            return (Sample)0;
        return sampleTrianglePoint(ubo, position, record.instanceSlot, record.primitive, u.yz, triangleProbability(ubo, triangle, first), 0.0);
    }

    // The solid angle pdf of the path tracer's emissive sampling from `origin` (without the branch probability) for a point of
    // instance `instanceSlot`'s triangle `primitive`, `distance` away at `cosLight` to its face. 0 when it is not sampled.
    float trianglePdf(UniformBufferObject* ubo, float3 origin, uint instanceSlot, uint primitive, float distance, float cosLight)
    {
        const uint entry = find(ubo, instanceSlot);
        if (entry == 0xFFFFFFFF || cosLight <= 1e-6)
            return 0.0;
        const EmissiveLightInstance light = ((EmissiveLightInstance*)ubo->emissiveLightsReference)[entry];
        if (primitive >= light.triangleCount)
            return 0.0;
        const uint triangle = light.triangleOffset + primitive;
        const EmissiveTriangle record = ((EmissiveTriangle*)ubo->emissiveTrianglesReference)[triangle];
        if (!(record.power > 0.0) || !(record.area > 0.0))
            return 0.0;
        return triangleProbability(ubo, triangle, gridCell(ubo, origin)) / record.area * distance * distance / cosLight;
    }

    // trianglePdf for a ray from `origin` along `direction` that hit instance `instanceSlot`'s triangle `primitive` `distance` away,
    // the cosine from the triangle's own face (for passes that do not have it: the hybrid reflections).
    float trianglePdfAlong(UniformBufferObject* ubo, float3 origin, float3 direction, uint instanceSlot, uint primitive, float distance)
    {
        if (!hasTriangleLights(ubo) || find(ubo, instanceSlot) == 0xFFFFFFFF)
            return 0.0;
        GpuRayTracingInstance instance = ((GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference)[instanceSlot];
        const float4x4 world = ((GpuTransform*)ubo->sceneTransformsReference)[instanceSlot].world;
        uint3 index = instance.indexBufferAddress != 0 ? ((uint3*)instance.indexBufferAddress)[primitive]
                                                       : uint3(primitive * 3 + 0, primitive * 3 + 1, primitive * 3 + 2);
        const float3 p0 = mul(world, float4(LoadRayTracingVertex(instance, index.x).pos, 1.0)).xyz;
        const float3 p1 = mul(world, float4(LoadRayTracingVertex(instance, index.y).pos, 1.0)).xyz;
        const float3 p2 = mul(world, float4(LoadRayTracingVertex(instance, index.z).pos, 1.0)).xyz;
        const float3 cross01 = cross(p1 - p0, p2 - p0);
        const float doubleArea = length(cross01);
        if (doubleArea <= 0.0)
            return 0.0;
        return trianglePdf(ubo, origin, instanceSlot, primitive, distance, abs(dot(cross01 / doubleArea, direction)));
    }
}
