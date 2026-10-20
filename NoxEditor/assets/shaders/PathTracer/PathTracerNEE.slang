#pragma once

#include "PathTracerBridge.slang"
#include "EmissiveLights.slang"

namespace PathTracer
{
    struct NEECandidate
    {
        float3 L;
        float distance;
        float3 diffuse;
        float3 specular;
        float lightPdf;
        float bsdfPdf;
        float weight;
        bool valid;
    };

    NEECandidate generateNEECandidate(ShadingData sd, StandardBSDF bsdf,
        UniformBufferObject* ubo, inout uint rngState, uint skyboxTextureIndex, uint importanceIndex)
    {
        NEECandidate c = (NEECandidate)0;
        c.distance = 1e9;
        uint analyticCount = ubo->lightCount;
        bool hasEnvironment = skyboxTextureIndex != 0xFFFFFFFF;
        uint lightCount = analyticCount + (hasEnvironment ? 1u : 0u);
        // The emissive meshes (EmissiveLights.slang) take this share of the samples, the analytic lights and the environment the rest.
        const float emissiveProbability = EmissiveLights::branchProbability(ubo, hasEnvironment);
        if (lightCount == 0 && emissiveProbability <= 0.0) return c;

        const bool emissiveSample = Bridge::rand(rngState) < emissiveProbability;
        uint lightIndex = min(uint(Bridge::rand(rngState) * float(lightCount)), max(lightCount, 1u) - 1);
        float lightSelectPdf = emissiveSample ? emissiveProbability : (1.0 - emissiveProbability) / float(max(lightCount, 1u));
        bool environmentSample = !emissiveSample && lightIndex == analyticCount;
        float3 Li = 0.0;
        float directionalPdf = 1.0;

        if (emissiveSample)
        {
            const EmissiveLights::Sample sample = EmissiveLights::sample(ubo, sd.posW,
                float4(Bridge::rand(rngState), Bridge::rand(rngState), Bridge::rand(rngState), Bridge::rand(rngState)));
            if (!sample.valid) return c;
            c.L = sample.direction;
            c.distance = sample.distance;
            Li = sample.radiance;
            directionalPdf = sample.pdf;
        }
        else if (environmentSample)
        {
            EnvMapSampler sampler = Bridge::environmentSampler(ubo, skyboxTextureIndex, importanceIndex);
            DistantLightSample sample = sampler.MIPDescentSample(float2(Bridge::rand(rngState), Bridge::rand(rngState)));
            c.L = sample.Dir;
            Li = sample.Le;
            directionalPdf = sample.Pdf;
        }
        else
        {
            // The engine's light sampling (shaderIO.h): the sun's cone, a sphere light's disc, a rect light's area, with the
            // falloff, cone and a rect's area / distance^2 folded in -- the same light the raster and GI passes see.
            LightData* lights = (LightData*)ubo->lightDataReference;
            const LightPointSample lightPoint = sample_light_point(lights[lightIndex], sd.posW,
                                                                   float2(Bridge::rand(rngState), Bridge::rand(rngState)));
            c.L = lightPoint.direction;
            const bool directional = uint(lights[lightIndex].position.w) == LightType::Directional;
            c.distance = directional ? 1e8 : lightPoint.distance;
            Li = lightPoint.radiance;
            if (directional)
                Li *= cloud_shadow(ubo, sd.posW, lightIndex, c.L); // the clouds' shadow, as the raster lighting has it
        }

        if (all(Li <= float3(0.0))) return c;

        float3 diffuseBsdf, specularBsdf;
        bsdf.evalSplit(sd, c.L, diffuseBsdf, specularBsdf);
        c.bsdfPdf = bsdf.evalPdf(sd, c.L);
        c.lightPdf = lightSelectPdf * directionalPdf;
        float misWeight = 1.0;
        if (environmentSample || emissiveSample)
        {
            float lightPdf2 = c.lightPdf * c.lightPdf;
            float bsdfPdf2 = c.bsdfPdf * c.bsdfPdf;
            misWeight = lightPdf2 / max(lightPdf2 + bsdfPdf2, 1e-30);
        }
        float3 incoming = Li * misWeight / max(c.lightPdf, 1e-30);
        c.diffuse = diffuseBsdf * incoming;
        c.specular = specularBsdf * incoming;
        c.weight = max(Luminance(c.diffuse + c.specular), 0.0);
        c.valid = c.weight > 0.0 && all(isfinite(c.diffuse + c.specular));
        return c;
    }

    // The light a candidate's shadow ray lets through: 0 behind something opaque, tinted behind glass and blended surfaces
    // (Bridge::OccluderTransmittance). Every candidate is looked at (forced non-opaque: opaque-mode glass is a FORCE_OPAQUE
    // instance in the TLAS); an opaque one ends the search.
    float3 visibleNEECandidate(NEECandidate c, ShadingData sd,
        RaytracingAccelerationStructure as, UniformBufferObject* ubo, inout uint rngState)
    {
        const float offset = 0.002 * ubo->unitsPerMeter;
        RayDesc shadowRay;
        shadowRay.Origin = sd.posW + sd.N * (dot(sd.N, c.L) >= 0 ? offset : -offset);
        shadowRay.Direction = c.L;
        shadowRay.TMin = 0.5 * offset;
        shadowRay.TMax = max(c.distance - 2.0 * offset, offset);
        RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> rq;
        rq.TraceRayInline(as, RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, shadowRay);
        float3 transmittance = float3(1.0);
        while (rq.Proceed())
        {
            transmittance *= Bridge::OccluderTransmittance(rq.CandidateRayInstanceCustomIndex(), rq.CandidatePrimitiveIndex(),
                                                           rq.CandidateTriangleBarycentrics(), ubo);
            if (all(transmittance < 1e-3))
                rq.CommitNonOpaqueTriangleHit();
        }
        return rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? float3(0.0) : transmittance;
    }

    float3 evaluateNEE(ShadingData sd, StandardBSDF bsdf,
        RaytracingAccelerationStructure as, UniformBufferObject* ubo, inout uint rngState,
        uint candidateCount, uint fullSampleCount, uint skyboxTextureIndex, uint importanceIndex,
        out float3 diffuseDirect, out float3 specularDirect)
    {
        diffuseDirect = 0.0;
        specularDirect = 0.0;
        candidateCount = max(candidateCount, 1u);
        fullSampleCount = max(fullSampleCount, 1u);
        for (uint fullSample = 0; fullSample < fullSampleCount; ++fullSample)
        {
            NEECandidate selected = (NEECandidate)0;
            float weightSum = 0.0;
            for (uint i = 0; i < candidateCount; ++i)
            {
                NEECandidate candidate = generateNEECandidate(sd, bsdf, ubo, rngState, skyboxTextureIndex, importanceIndex);
                if (!candidate.valid) continue;
                weightSum += candidate.weight;
                if (Bridge::rand(rngState) * weightSum < candidate.weight)
                    selected = candidate;
            }
            if (!selected.valid || weightSum <= 0.0)
                continue;
            const float3 visibility = visibleNEECandidate(selected, sd, as, ubo, rngState);
            if (all(visibility <= 0.0))
                continue;
            float normalization = weightSum / max(float(candidateCount) * selected.weight, 1e-30);
            diffuseDirect += selected.diffuse * visibility * normalization;
            specularDirect += selected.specular * visibility * normalization;
        }
        float invFullSamples = 1.0 / float(fullSampleCount);
        diffuseDirect *= invFullSamples;
        specularDirect *= invFullSamples;
        return diffuseDirect + specularDirect;
    }
}
