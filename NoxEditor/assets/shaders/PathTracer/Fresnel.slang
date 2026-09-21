#pragma once

// Evaluates the Fresnel term using Schlick's approximation (RTXPT Fresnel.hlsli)
float3 evalFresnelSchlick(float3 f0, float3 f90, float cosTheta)
{
    return f0 + (f90 - f0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

float evalFresnelSchlick(float f0, float f90, float cosTheta)
{
    return f0 + (f90 - f0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// Evaluates dielectric Fresnel term with Snell's law (returns reflection ratio, outputs transmitted cosThetaT)
float evalFresnelDielectric(float eta, float cosThetaI, out float cosThetaT)
{
    float etaRel = eta;
    float cI = cosThetaI;
    if (cI < 0.0)
    {
        etaRel = 1.0 / etaRel;
        cI = -cI;
    }

    float sinThetaTSq = etaRel * etaRel * (1.0 - cI * cI);
    // Total internal reflection (TIR)
    if (sinThetaTSq > 1.0)
    {
        cosThetaT = 0.0;
        return 1.0;
    }

    cosThetaT = sqrt(max(0.0, 1.0 - sinThetaTSq));

    float Rs = (etaRel * cI - cosThetaT) / (etaRel * cI + cosThetaT);
    float Rp = (etaRel * cosThetaT - cI) / (etaRel * cosThetaT + cI);

    return 0.5 * (Rs * Rs + Rp * Rp);
}

float evalFresnelDielectric(float eta, float cosThetaI)
{
    float cosThetaT;
    return evalFresnelDielectric(eta, cosThetaI, cosThetaT);
}

// Evaluates conductor Fresnel term
float evalFresnelConductor(float eta, float k, float cosThetaI)
{
    float cosThetaISq = cosThetaI * cosThetaI;
    float sinThetaISq = max(1.0 - cosThetaISq, 0.0);

    float innerTerm = eta * eta - k * k - sinThetaISq;
    float aSqPlusBSq = sqrt(max(innerTerm * innerTerm + 4.0 * eta * eta * k * k, 0.0));
    float a = sqrt(max(0.5 * (aSqPlusBSq + innerTerm), 0.0));

    float Rs = ((aSqPlusBSq + cosThetaISq) - (2.0 * a * cosThetaI)) /
               ((aSqPlusBSq + cosThetaISq) + (2.0 * a * cosThetaI));
    float Rp = ((cosThetaISq * aSqPlusBSq + sinThetaISq * sinThetaISq) - (2.0 * a * cosThetaI * sinThetaISq)) /
               ((cosThetaISq * aSqPlusBSq + sinThetaISq * sinThetaISq) + (2.0 * a * cosThetaI * sinThetaISq));

    return 0.5 * (Rs + Rp);
}
