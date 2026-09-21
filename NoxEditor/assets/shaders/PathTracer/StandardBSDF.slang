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
struct StandardBSDF
{
    StandardBSDFData data;
    static StandardBSDF make(StandardBSDFData data) { StandardBSDF b; b.data=data; return b; }
    float3 eval(ShadingData sd, float3 wo)
    {
        FalcorBSDF b = FalcorBSDF::make(sd.mtl, sd.N, sd.V, data);
        return b.eval(sd.toLocal(sd.V), sd.toLocal(wo)).rgb;
    }
    void evalSplit(ShadingData sd, float3 wo, out float3 diffuse, out float3 specular)
    {
        FalcorBSDF b = FalcorBSDF::make(sd.mtl, sd.N, sd.V, data);
        float3 wi=sd.toLocal(sd.V), w=sd.toLocal(wo);
        diffuse = (1-b.specTrans) * ((1-b.diffTrans)*b.diffuseReflection.eval(wi,w) + b.diffTrans*b.diffuseTransmission.eval(wi,w));
        specular = (1-b.specTrans)*b.specularReflection.eval(wi,w) + b.specTrans*b.specularReflectionTransmission.eval(wi,w);
    }
    float evalPdf(ShadingData sd, float3 wo)
    {
        FalcorBSDF b = FalcorBSDF::make(sd.mtl, sd.N, sd.V, data);
        return b.evalPdf(sd.toLocal(sd.V), sd.toLocal(wo));
    }
    bool sample(ShadingData sd, float4 u, out BSDFSample result)
    {
        FalcorBSDF b = FalcorBSDF::make(sd.mtl, sd.N, sd.V, data);
        float3 local;
        bool valid=b.sample(sd.toLocal(sd.V),local,result.pdf,result.weight,result.lobe,result.lobeP,u.xyz);
        result.wo=sd.fromLocal(local);
        return valid;
    }

};
