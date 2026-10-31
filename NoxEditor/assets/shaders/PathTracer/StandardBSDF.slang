#pragma once
#include "RTXPT/BxDF.hlsli"

// Nox interface around the upstream RTXPT FalcorBSDF. The lobe math lives in RTXPT/BxDF.hlsli.
struct BSDFSample
{
    float3 wo;
    float pdf;
    float3 weight;
    uint lobe;
    float lobeP;
};

// The glTF lobes past the core model (MaterialLobes.slang, PathTracerBridge.slang): the specular lobe's F90, anisotropy and the thin
// film live in the base's specular reflection (BxDF.hlsli); the sheen and the clear coat are layers over the base, as the spec lays
// them: material = sheen + base x (1 - max(sheen color) E(V)), then mix(material, coat, coat x F_coat(V)).
struct BSDFLobes
{
    uint flags;                 // MaterialLobe* (0: the base alone, exactly RTXPT's)
    float3 specularF90;         // KHR_materials_specular: the dielectric's weight (metals 1)
    float alphaT;               // KHR_materials_anisotropy: the width along the shading frame's T (the stretch direction)
    float alphaB;
    float iridescence;          // KHR_materials_iridescence
    float iridescenceIor;
    float iridescenceThickness;
    float coatWeight;           // KHR_materials_clearcoat: coat factor x the coat's Fresnel (IOR 1.5) at the view angle
    float coatAlpha;
    float3 coatNormal;          // world space, facing the viewer
    float3 sheenColor;          // KHR_materials_sheen
    float sheenRoughness;
    float sheenScaling;         // 1 - max(sheen color) x E(N.V): what the sheen lets through of the base

    static BSDFLobes none()
    {
        BSDFLobes lobes = (BSDFLobes)0;
        lobes.specularF90 = float3(1.0, 1.0, 1.0);
        lobes.sheenScaling = 1.0;
        return lobes;
    }
};

struct StandardBSDF
{
    StandardBSDFData data;
    BSDFLobes lobes;
    static StandardBSDF make(StandardBSDFData data) { StandardBSDF b; b.data=data; b.lobes = BSDFLobes::none(); return b; }
    static StandardBSDF make(StandardBSDFData data, BSDFLobes lobes) { StandardBSDF b; b.data=data; b.lobes = lobes; return b; }

    bool hasLayers() { return lobes.coatWeight > 0.0 || (lobes.flags & MaterialLobeSheen) != 0u; }

    // The base: RTXPT's BSDF with the specular lobe's F90, anisotropy and thin film.
    FalcorBSDF base(ShadingData sd)
    {
        FalcorBSDF b = FalcorBSDF::make(sd.mtl, sd.N, sd.V, data);
        if (lobes.flags != 0u)
        {
            b.specularReflection.f90 = lobes.specularF90;
            b.specularReflection.iridescence = lobes.iridescence;
            b.specularReflection.iridescenceIor = lobes.iridescenceIor;
            b.specularReflection.iridescenceThickness = lobes.iridescenceThickness;
            if ((lobes.flags & MaterialLobeAnisotropy) != 0u)
            {
                b.specularReflection.alpha = max(lobes.alphaT, kMinGGXAlpha);
                b.specularReflection.alphaY = max(lobes.alphaB, kMinGGXAlpha);
            }
        }
        return b;
    }

    // The layers' shares: the coat's (its Fresnel weight, chosen that often), the sheen's (its albedo under the coat), the base's.
    void layerProbabilities(out float pCoat, out float pSheen, out float pBase)
    {
        pCoat = lobes.coatWeight;
        pSheen = (1.0 - pCoat) * (1.0 - lobes.sheenScaling);
        pBase = (1.0 - pCoat) * lobes.sheenScaling;
    }

    // The coat's GGX reflection (cosine included) and its sampling pdf, around the coat normal.
    float3 coatFrameLocal(float3 v, out float3 t, out float3 b)
    {
        const float3 n = lobes.coatNormal;
        const float s = n.z >= 0.0 ? 1.0 : -1.0;
        const float a = -1.0 / (s + n.z);
        const float c = n.x * n.y * a;
        t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
        b = float3(c, s + n.y * n.y * a, -n.y);
        return float3(dot(v, t), dot(v, b), dot(v, n));
    }

    float3 evalCoat(ShadingData sd, float3 wo)
    {
        float3 t, b;
        const float3 wi = coatFrameLocal(sd.V, t, b);
        const float3 w = coatFrameLocal(wo, t, b);
        if (min(wi.z, w.z) < kMinCosTheta || dot(wo, sd.N) <= 0.0) return float3(0.0, 0.0, 0.0);
        const float3 h = normalize(wi + w);
        const float D = evalNdfGGX(lobes.coatAlpha, h.z);
        const float G = evalMaskingSmithGGXCorrelated(lobes.coatAlpha, wi.z, w.z);
        const float v = D * G * 0.25 / wi.z;
        return float3(v, v, v);
    }

    float evalCoatPdf(ShadingData sd, float3 wo)
    {
        float3 t, b;
        const float3 wi = coatFrameLocal(sd.V, t, b);
        const float3 w = coatFrameLocal(wo, t, b);
        if (min(wi.z, w.z) < kMinCosTheta || dot(wo, sd.N) <= 0.0) return 0.0;
        return evalPdfGGX_VNDF(lobes.coatAlpha, wi, normalize(wi + w));
    }

    // The sheen (Charlie, KHR_materials_sheen; cosine included) and its sampling pdf (cosine-weighted around the shading normal).
    float3 evalSheen(ShadingData sd, float3 wo)
    {
        const float NdotL = dot(sd.N, wo), NdotV = dot(sd.N, sd.V);
        if (min(NdotL, NdotV) < kMinCosTheta) return float3(0.0, 0.0, 0.0);
        const float NdotH = saturate(dot(sd.N, normalize(sd.V + wo)));
        return lobes.sheenColor * (D_charlie(lobes.sheenRoughness, NdotH) * V_charlie(NdotL, NdotV, lobes.sheenRoughness) * NdotL);
    }

    float evalSheenPdf(ShadingData sd, float3 wo)
    {
        const float NdotL = dot(sd.N, wo);
        return NdotL < kMinCosTheta ? 0.0 : NdotL * K_1_PI;
    }

    float3 eval(ShadingData sd, float3 wo)
    {
        FalcorBSDF b = base(sd);
        const float3 baseValue = b.eval(sd.toLocal(sd.V), sd.toLocal(wo)).rgb;
        if (!hasLayers())
            return baseValue;
        return (1.0 - lobes.coatWeight) * (lobes.sheenScaling * baseValue + evalSheen(sd, wo)) + lobes.coatWeight * evalCoat(sd, wo);
    }
    void evalSplit(ShadingData sd, float3 wo, out float3 diffuse, out float3 specular)
    {
        FalcorBSDF b = base(sd);
        float3 wi=sd.toLocal(sd.V), w=sd.toLocal(wo);
        diffuse = (1-b.specTrans) * ((1-b.diffTrans)*b.diffuseReflection.eval(wi,w) + b.diffTrans*b.diffuseTransmission.eval(wi,w));
        specular = (1-b.specTrans)*b.specularReflection.eval(wi,w) + b.specTrans*b.specularReflectionTransmission.eval(wi,w);
        if (!hasLayers())
            return;
        // The sheen's wide lobe goes with the diffuse, the coat with the specular (the denoisers' two signals).
        const float below = 1.0 - lobes.coatWeight;
        diffuse = below * (lobes.sheenScaling * diffuse + evalSheen(sd, wo));
        specular = below * lobes.sheenScaling * specular + lobes.coatWeight * evalCoat(sd, wo);
    }
    float evalPdf(ShadingData sd, float3 wo)
    {
        FalcorBSDF b = base(sd);
        const float basePdf = b.evalPdf(sd.toLocal(sd.V), sd.toLocal(wo));
        if (!hasLayers())
            return basePdf;
        float pCoat, pSheen, pBase;
        layerProbabilities(pCoat, pSheen, pBase);
        return pBase * basePdf + pCoat * evalCoatPdf(sd, wo) + pSheen * evalSheenPdf(sd, wo);
    }
    bool sample(ShadingData sd, float4 u, out BSDFSample result)
    {
        FalcorBSDF b = base(sd);
        float3 local;
        if (!hasLayers())
        {
            bool valid=b.sample(sd.toLocal(sd.V),local,result.pdf,result.weight,result.lobe,result.lobeP,u.xyz);
            result.wo=sd.fromLocal(local);
            return valid;
        }

        // One layer by its share (u.w), then the direction from it; the weight and pdf of the whole (one-sample MIS over the layers).
        float pCoat, pSheen, pBase;
        layerProbabilities(pCoat, pSheen, pBase);
        result.lobeP = 1.0;
        if (u.w < pBase)
        {
            if (!b.sample(sd.toLocal(sd.V), local, result.pdf, result.weight, result.lobe, result.lobeP, u.xyz))
                return false;
            result.wo = sd.fromLocal(local);
            if (result.pdf <= 0.0)
            {
                // A delta lobe of the base (a mirror, smooth glass): what the layers above let through of it, over the chance of the base.
                result.weight *= (1.0 - lobes.coatWeight) * lobes.sheenScaling / pBase;
                result.lobeP *= pBase;
                return true;
            }
        }
        else if (u.w < pBase + pCoat)
        {
            float3 t, bt;
            const float3 wi = coatFrameLocal(sd.V, t, bt);
            if (wi.z < kMinCosTheta)
                return false;
            const float3 h = sampleGGX_VNDF(lobes.coatAlpha, wi, u.xy);
            const float3 w = 2.0 * dot(wi, h) * h - wi;
            result.wo = t * w.x + bt * w.y + lobes.coatNormal * w.z;
            result.lobe = (uint)LobeType::SpecularReflection;
        }
        else
        {
            float pdf;
            result.wo = sd.fromLocal(sample_cosine_hemisphere_concentric(u.xy, pdf));
            result.lobe = (uint)LobeType::DiffuseReflection;
        }
        result.pdf = evalPdf(sd, result.wo);
        if (result.pdf <= 0.0)
            return false;
        result.weight = eval(sd, result.wo) / result.pdf;
        return any(result.weight > 0.0);
    }
    // RTXPT's delta lobes (its stable planes): [0] transmission, [1] reflection, each with its world direction and its whole weight
    // (lobe probability times albedo); nonDeltaPart: what the rough lobes hold. Exactly RTXPT's: a thin pane's transmission keeps
    // its whole weight (its "TODO adjust F for thin surface hack"), as RTXPT shows its windows. Under a coat or sheen the delta lobes
    // keep what those let through, and what they reflect is rough (not a stable plane).
    void evalDeltaLobes(ShadingData sd, out DeltaLobe lobes[cMaxDeltaLobes], out int count, out float nonDeltaPart)
    {
        FalcorBSDF b = base(sd);
        const float3 wi = sd.toLocal(sd.V);
        b.evalDeltaLobes(wi, lobes, count, nonDeltaPart);
        for (int i = 0; i < count; ++i)
            lobes[i].dir = sd.fromLocal(lobes[i].dir);
        if (hasLayers())
        {
            const float through = (1.0 - this.lobes.coatWeight) * this.lobes.sheenScaling;
            for (int i = 0; i < count; ++i)
                lobes[i].thp *= through;
            nonDeltaPart = nonDeltaPart * through + (1.0 - through);
        }
    }

};
