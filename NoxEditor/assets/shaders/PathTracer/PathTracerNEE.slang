#pragma once

#include "PathTracerBridge.slang"

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
        if (lightCount == 0) return c;

        uint lightIndex = min(uint(Bridge::rand(rngState) * float(lightCount)), lightCount - 1);
        float lightSelectPdf = 1.0 / float(lightCount);
        bool environmentSample = lightIndex == analyticCount;
        float3 Li = 0.0;
        float directionalPdf = 1.0;

        if (environmentSample)
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
            c.distance = uint(lights[lightIndex].position.w) == LightType::Directional ? 1e8 : lightPoint.distance;
            Li = lightPoint.radiance;
        }

        if (all(Li <= float3(0.0))) return c;

        float3 diffuseBsdf, specularBsdf;
        bsdf.evalSplit(sd, c.L, diffuseBsdf, specularBsdf);
        c.bsdfPdf = bsdf.evalPdf(sd, c.L);
        c.lightPdf = lightSelectPdf * directionalPdf;
        float misWeight = 1.0;
        if (environmentSample)
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

    bool visibleNEECandidate(NEECandidate c, ShadingData sd,
        RaytracingAccelerationStructure as, UniformBufferObject* ubo, inout uint rngState)
    {
        RayDesc shadowRay;
        shadowRay.Origin = sd.posW + sd.N * (dot(sd.N, c.L) >= 0 ? 0.002 : -0.002);
        shadowRay.Direction = c.L;
        shadowRay.TMin = 0.001;
        shadowRay.TMax = max(c.distance - 0.004, 0.002);
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> rq;
        rq.TraceRayInline(as, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, shadowRay);
        while (rq.Proceed())
        {
            uint instID = rq.CandidateRayInstanceCustomIndex();
            uint primIdx = rq.CandidatePrimitiveIndex();
            if (Bridge::AlphaTest(instID, primIdx, rq.CandidateTriangleBarycentrics(), ubo, Bridge::rand(rngState)))
                rq.CommitNonOpaqueTriangleHit();
        }
        return rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
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
            if (!selected.valid || weightSum <= 0.0 || !visibleNEECandidate(selected, sd, as, ubo, rngState))
                continue;
            float normalization = weightSum / max(float(candidateCount) * selected.weight, 1e-30);
            diffuseDirect += selected.diffuse * normalization;
            specularDirect += selected.specular * normalization;
        }
        float invFullSamples = 1.0 / float(fullSampleCount);
        diffuseDirect *= invFullSamples;
        specularDirect *= invFullSamples;
        return diffuseDirect + specularDirect;
    }
}
