// The lobes' math with nothing else (no textures, no engine types): the raster lighting (PBRLighting.slang) and the path tracer's BSDF
// (PathTracer/RTXPT/BxDF.hlsli, StandardBSDF.slang) evaluate the glTF lobes with the same functions.
#ifndef MATERIAL_LOBE_MATH_SLANG
#define MATERIAL_LOBE_MATH_SLANG

static const float LobePi = 3.141592653589793;

// Anisotropic GGX (KHR_materials_anisotropy, Burley's extended distribution and its height-correlated visibility).
float D_GGX_anisotropic(float NdotH, float TdotH, float BdotH, float at, float ab)
{
    const float a2 = at * ab;
    const float3 f = float3(ab * TdotH, at * BdotH, a2 * NdotH);
    const float w2 = a2 / max(dot(f, f), 1e-12);
    return a2 * w2 * w2 / LobePi;
}

float V_GGX_anisotropic(float NdotL, float NdotV, float BdotV, float TdotV, float TdotL, float BdotL, float at, float ab)
{
    const float GGXV = NdotL * length(float3(at * TdotV, ab * BdotV, NdotV));
    const float GGXL = NdotV * length(float3(at * TdotL, ab * BdotL, NdotL));
    // Not clamped to 1 (the spec's sample code does): the isotropic visibility is not, and at grazing angles it is above 1.
    return 0.5 / max(GGXV + GGXL, 1e-8);
}

// Sheen (KHR_materials_sheen): the "Charlie" distribution and visibility of Conty & Kulla 2017, as the spec gives them.
float D_charlie(float sheenRoughness, float NdotH)
{
    const float alphaG = max(sheenRoughness * sheenRoughness, 1e-6);
    const float invR = 1.0 / alphaG;
    const float sin2h = max(1.0 - NdotH * NdotH, 0.0);
    return (2.0 + invR) * pow(sin2h, invR * 0.5) / (2.0 * LobePi);
}

float sheen_lambda_curve(float x, float alphaG)
{
    const float oneMinusAlphaSq = (1.0 - alphaG) * (1.0 - alphaG);
    const float a = lerp(21.5473, 25.3245, oneMinusAlphaSq);
    const float b = lerp(3.82987, 3.32435, oneMinusAlphaSq);
    const float c = lerp(0.19823, 0.16801, oneMinusAlphaSq);
    const float d = lerp(-1.97760, -1.27393, oneMinusAlphaSq);
    const float e = lerp(-4.32054, -4.85967, oneMinusAlphaSq);
    return a / (1.0 + b * pow(max(x, 0.0), c)) + d * x + e;
}

float sheen_lambda(float cosTheta, float alphaG)
{
    return abs(cosTheta) < 0.5 ? exp(sheen_lambda_curve(cosTheta, alphaG))
                               : exp(2.0 * sheen_lambda_curve(0.5, alphaG) - sheen_lambda_curve(1.0 - cosTheta, alphaG));
}

float V_charlie(float NdotL, float NdotV, float sheenRoughness)
{
    const float alphaG = max(sheenRoughness * sheenRoughness, 1e-6);
    return clamp(1.0 / ((1.0 + sheen_lambda(NdotV, alphaG) + sheen_lambda(NdotL, alphaG)) * (4.0 * NdotV * NdotL)), 0.0, 1.0);
}

// Iridescence (KHR_materials_iridescence): the thin film's Fresnel (Belcour & Barla 2017), as the spec's implementation notes give it.
float3 iridescence_sensitivity(float opd, float3 shift)
{
    const float phase = 2.0 * LobePi * opd * 1.0e-9;
    const float3 val = float3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    const float3 pos = float3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    const float3 var = float3(4.3278e+09, 9.3046e+09, 6.6121e+09);
    float3 xyz = val * sqrt(2.0 * LobePi * var) * cos(pos * phase + shift) * exp(-phase * phase * var);
    xyz.x += 9.7470e-14 * sqrt(2.0 * LobePi * 4.5282e+09) * cos(2.2399e+06 * phase + shift.x) * exp(-4.5282e+09 * phase * phase);
    xyz /= 1.0685e-7;
    // XYZ to linear Rec. 709 (the matrix's rows).
    return float3(dot(float3(3.2404542, -1.5371385, -0.4985314), xyz),
                  dot(float3(-0.9692660, 1.8760108, 0.0415560), xyz),
                  dot(float3(0.0556434, -0.2040259, 1.0572252), xyz));
}

float3 iridescence_fresnel(float outsideIor, float filmIor, float3 baseF0, float thickness, float cosTheta1)
{
    // The film's IOR goes to the outside's as it thins to nothing (the Khronos reference renderer's evalIridescence).
    const float filmIorBlended = lerp(outsideIor, filmIor, smoothstep(0.0, 0.03, thickness));
    const float sinTheta2Sq = (outsideIor / filmIorBlended) * (outsideIor / filmIorBlended) * (1.0 - cosTheta1 * cosTheta1);
    const float cosTheta2Sq = 1.0 - sinTheta2Sq;
    if (cosTheta2Sq < 0.0)
        return float3(1.0); // total internal reflection
    const float cosTheta2 = sqrt(cosTheta2Sq);

    // First interface (outside to film), second (film to base, its IOR from its F0).
    const float R0 = ((filmIorBlended - outsideIor) / (filmIorBlended + outsideIor)) * ((filmIorBlended - outsideIor) / (filmIorBlended + outsideIor));
    const float R12 = R0 + (1.0 - R0) * pow(1.0 - cosTheta1, 5.0);
    const float T121 = 1.0 - R12;
    const float phi12 = filmIorBlended < outsideIor ? LobePi : 0.0;
    const float phi21 = LobePi - phi12;

    const float3 sqrtF0 = sqrt(clamp(baseF0, 0.0, 0.9999));
    const float3 baseIor = (1.0 + sqrtF0) / (1.0 - sqrtF0);
    const float3 R1 = ((baseIor - filmIorBlended) / (baseIor + filmIorBlended)) * ((baseIor - filmIorBlended) / (baseIor + filmIorBlended));
    const float3 R23 = R1 + (1.0 - R1) * pow(1.0 - cosTheta2, 5.0);
    const float3 phi23 = select(baseIor < filmIorBlended, float3(LobePi), float3(0.0));

    const float opd = 2.0 * filmIorBlended * thickness * cosTheta2;
    const float3 phi = float3(phi21) + phi23;

    const float3 R123 = clamp(R12 * R23, 1e-5, 0.9999);
    const float3 r123 = sqrt(R123);
    const float3 Rs = T121 * T121 * R23 / (1.0 - R123);
    float3 I = R12 + Rs; // m = 0
    float3 Cm = Rs - T121;
    for (int m = 1; m <= 2; ++m)
    {
        Cm *= r123;
        I += Cm * 2.0 * iridescence_sensitivity(float(m) * opd, float(m) * phi);
    }
    return max(I, float3(0.0));
}

#endif
