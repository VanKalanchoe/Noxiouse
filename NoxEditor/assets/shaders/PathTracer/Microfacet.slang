#pragma once

static const float M_PI = 3.14159265358979323846;
static const float K_1_PI = 0.31830988618379067154;
static const float kMinCosTheta = 1e-6;
static const float kMinGGXAlpha = 0.002;

// Evaluates the GGX (Trowbridge-Reitz) normal distribution function D(h)
float evalNdfGGX(float alpha, float cosTheta)
{
    float a2 = max(alpha * alpha, 1e-6);
    float d = (cosTheta * a2 - cosTheta) * cosTheta + 1.0;
    return a2 / max(d * d * M_PI, 1e-7);
}

// Smith GGX masking function G1
float evalG1GGX(float alphaSqr, float cosTheta)
{
    if (cosTheta <= 0.0) return 0.0;
    float cosThetaSq = cosTheta * cosTheta;
    float tanThetaSq = max(1.0 - cosThetaSq, 0.0) / cosThetaSq;
    return 2.0 / (1.0 + sqrt(1.0 + alphaSqr * tanThetaSq));
}

// Smith correlated height-correlated masking-shadowing G2(wi, wo)
float evalG2_Smith(float3 wi, float3 wo, float alpha)
{
    if (wi.z <= 0.0 || wo.z <= 0.0) return 0.0;
    float a2 = alpha * alpha;
    float lambdaV = wo.z * sqrt(wi.z * (wi.z - a2 * wi.z) + a2);
    float lambdaL = wi.z * sqrt(wo.z * (wo.z - a2 * wo.z) + a2);
    return 2.0 * wi.z * wo.z / max(lambdaV + lambdaL, 1e-7);
}

// Walter et al. 2007 sampling of GGX NDF
float3 sampleGGX_NDF(float alpha, float2 u)
{
    float alphaSqr = alpha * alpha;
    float phi = u.y * (2.0 * M_PI);
    float tanThetaSqr = alphaSqr * u.x / max(1.0 - u.x, 1e-7);
    float cosTheta = 1.0 / sqrt(1.0 + tanThetaSqr);
    float r = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));

    return float3(cos(phi) * r, sin(phi) * r, cosTheta);
}

// Evaluates PDF for sampling GGX NDF
float evalPdfGGX_NDF(float alpha, float3 wi, float3 h)
{
    float cosTheta = h.z;
    if (cosTheta <= 0.0) return 0.0;
    return evalNdfGGX(alpha, cosTheta) * cosTheta / max(dot(wi, h) * 4.0, 1e-7);
}

// Samples the GGX distribution of visible normals (VNDF - Heitz 2018)
float3 sampleGGX_VNDF(float3 wi, float2 alpha, float2 u)
{
    // 1. Transform wi to hemisphere configuration
    float3 Vh = normalize(float3(alpha.x * wi.x, alpha.y * wi.y, wi.z));

    // 2. Construct orthonormal basis
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 T2 = cross(Vh, T1);

    // 3. Parameterize projected disk
    float r = sqrt(u.x);
    float phi = 2.0 * M_PI * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(max(1.0 - t1 * t1, 0.0)) + s * t2;

    // 4. Reproject onto hemisphere
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;

    // 5. Transform back to ellipsoid and normalize
    return normalize(float3(alpha.x * Nh.x, alpha.y * Nh.y, max(0.0, Nh.z)));
}

// Evaluates PDF for sampling GGX VNDF
float evalPdfGGX_VNDF(float alpha, float3 wi, float3 h)
{
    if (wi.z <= 0.0 || h.z <= 0.0) return 0.0;
    float G1 = evalG1GGX(alpha * alpha, wi.z);
    float D = evalNdfGGX(alpha, h.z);
    float dotWiH = max(dot(wi, h), 0.0);
    return G1 * D * dotWiH / max(wi.z * 4.0 * dotWiH, 1e-7);
}
