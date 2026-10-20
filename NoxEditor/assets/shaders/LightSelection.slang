// Direct light at a ray's hit, for the GI and reflection rays (ScreenProbeTrace, DDGIRadiance, RayHitShading). Every light's
// unshadowed light is cheap to sum, the shadow rays are what costs, and a scene can have hundreds of lights. So the strongest
// few at the hit (and every directional light) are traced exactly, and one more, picked from all the others in proportion to
// what they give (weighted reservoir sampling), stands for the rest. Exact wherever a few lights dominate -- almost everywhere --
// where picking two among all of them made the GI flash in the lights' colors at night.
#pragma once

#include "shaderIO.h"

static const uint LightSelectionExact = 4;
static const uint LightSelectionMax = LightSelectionExact + 1;

// The lights to shadow-test at a hit: irradiance at the surface (unshadowed, already weighted), and the shadow ray to it.
struct LightSelection
{
    uint count;
    float3 irradiance[LightSelectionMax];
    float3 direction[LightSelectionMax];
    float distance[LightSelectionMax];
};

float light_selection_random(inout uint state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return float(state >> 8) * (1.0 / 16777216.0);
}

// minDistance: point / spot lights give at most what they give at this distance (world units; 0 = their source radius only).
// The GI rays use ~10 cm: a ray that grazes a lamp's own fixture right next to the bulb would otherwise be a firefly the probes
// cannot average away.
LightSelection select_lights(UniformBufferObject* ubo, float3 position, float3 normal, float minDistance, inout uint rng)
{
    LightSelection selection;
    selection.count = 0;
    float keys[LightSelectionExact];
    for (uint slot = 0; slot < LightSelectionMax; ++slot)
    {
        selection.irradiance[slot] = float3(0.0);
        selection.direction[slot] = float3(0.0, 1.0, 0.0);
        selection.distance[slot] = 0.0;
    }
    for (uint slot = 0; slot < LightSelectionExact; ++slot)
        keys[slot] = 0.0;

    // The reservoir of the lights that are not among the strongest.
    float restSum = 0.0;
    float restWeight = 0.0;
    float3 restIrradiance = float3(0.0);
    float3 restDirection = float3(0.0, 1.0, 0.0);
    float restDistance = 0.0;
    const float3 luma = float3(0.2126, 0.7152, 0.0722);

    LightData* lights = (LightData*)ubo->lightDataReference;
    for (uint i = 0; i < ubo->lightCount; ++i)
    {
        const LightData light = lights[i];
        const uint type = (uint)light.position.w;
        LightPointSample sample;
        const float2 u = float2(light_selection_random(rng), light_selection_random(rng));
        float3 irradiance = light_surface_irradiance(light, position, normal, u, sample);
        if (type == 0u)
            irradiance *= cloud_shadow(ubo, position, i, sample.direction);
        if (minDistance > 0.0 && (type == 1u || type == 2u))
        {
            const float centreDistanceSq = max(dot(light.position.xyz - position, light.position.xyz - position), light.spotParams.z * light.spotParams.z);
            irradiance *= centreDistanceSq / max(centreDistanceSq, minDistance * minDistance);
        }
        float weight = dot(irradiance, luma);
        if (!(weight > 0.0))
            continue;
        // Directional lights (the sun, the moon) are always traced: they are most of the light where they reach.
        float key = type == 0u ? 1e30 : weight;

        // Into the strongest, pushing the weakest of them out to the reservoir when full.
        float3 outIrradiance = irradiance;
        float3 outDirection = sample.direction;
        float outDistance = sample.distance;
        float outWeight = weight;
        bool spilled = true;
        if (selection.count < LightSelectionExact)
        {
            const uint slot = selection.count++;
            selection.irradiance[slot] = irradiance;
            selection.direction[slot] = sample.direction;
            selection.distance[slot] = sample.distance;
            keys[slot] = key;
            spilled = false;
        }
        else
        {
            uint weakest = 0;
            for (uint slot = 1; slot < LightSelectionExact; ++slot)
            {
                if (keys[slot] < keys[weakest])
                    weakest = slot;
            }
            if (key > keys[weakest])
            {
                outIrradiance = selection.irradiance[weakest];
                outDirection = selection.direction[weakest];
                outDistance = selection.distance[weakest];
                outWeight = dot(outIrradiance, luma);
                selection.irradiance[weakest] = irradiance;
                selection.direction[weakest] = sample.direction;
                selection.distance[weakest] = sample.distance;
                keys[weakest] = key;
            }
        }
        if (spilled && outWeight > 0.0)
        {
            restSum += outWeight;
            if (light_selection_random(rng) * restSum < outWeight)
            {
                restWeight = outWeight;
                restIrradiance = outIrradiance;
                restDirection = outDirection;
                restDistance = outDistance;
            }
        }
    }

    if (restWeight > 0.0)
    {
        // The one picked stands for all the rest: its light / its probability (weight / sum).
        const uint slot = selection.count++;
        selection.irradiance[slot] = restIrradiance * (restSum / restWeight);
        selection.direction[slot] = restDirection;
        selection.distance[slot] = restDistance;
    }
    return selection;
}
