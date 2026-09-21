#pragma once
#define RTXPT_MATERIAL_HAS_TRANSMISSION 1
#include "LobeType.slang"
#include "RTXPT/MathConstants.hlsli"
typedef float lpfloat;
typedef float3 lpfloat3;
float Luminance(float3 v) { return dot(v, float3(0.2126, 0.7152, 0.0722)); }
float Average(float3 v) { return (v.x + v.y + v.z) / 3.0; }

// Ray Tracing Gems / RTXPT scale-aware origin offset. A fixed world-space epsilon can jump
// completely through thin, closely layered geometry (the Bistro painting cover glass is one
// such case), so offset the floating-point representation by a few ULPs instead.
float3 ComputeRayOrigin(float3 worldPosition, float3 faceNormal)
{
    const float origin = 1.0 / 16.0;
    const float fScale = 3.0 / 65536.0;
    const float iScale = 3.0 * 256.0;

    int3 iOff = int3(faceNormal * iScale);
    int3 positionBits = asint(worldPosition);
    int3 offsetBits = int3(
        worldPosition.x < 0.0 ? -iOff.x : iOff.x,
        worldPosition.y < 0.0 ? -iOff.y : iOff.y,
        worldPosition.z < 0.0 ? -iOff.z : iOff.z);
    float3 iPos = asfloat(positionBits + offsetBits);
    float3 fPos = worldPosition + faceNormal * fScale;
    return float3(
        abs(worldPosition.x) < origin ? fPos.x : iPos.x,
        abs(worldPosition.y) < origin ? fPos.y : iPos.y,
        abs(worldPosition.z) < origin ? fPos.z : iPos.z);
}

float ComputeNewScatterFireflyFilterK(float currentK, float bouncePdf, float lobeP)
{
    const float minK = 0.00001;
    float angle = bouncePdf == 0.0
        ? 0.0
        : 2.0 * acos(max(-1.0, 1.0 - (1.0 / bouncePdf) / (2.0 * K_PI)));
    const float k = 32.0;
    float p = k / (k + angle * angle);
    p *= sqrt(max(lobeP, 0.0));
    return max(minK, currentK * p);
}

float ComputeRayConeSpreadExpansion(float bouncePdf)
{
    if (bouncePdf <= 0.0) return 0.0;
    return 0.3 * 2.0 * acos(max(-1.0, 1.0 - (1.0 / bouncePdf) / (2.0 * K_PI)));
}

float3 FireflyFilter(float3 signal, float threshold, float fireflyFilterK)
{
    if (threshold == 0.0) return signal;
    float cap = threshold * fireflyFilterK;
    float average = Average(signal);
    return average > cap ? signal * (cap / average) : signal;
}
// Concentric sampling from RTXPT Utils/Math/MathHelpers.hlsli.
float3 sample_cosine_hemisphere_concentric(float2 u, out float pdf)
{
    u = 2.0 * u - 1.0;
    float2 d = 0;
    if (u.x != 0 || u.y != 0)
    {
        float r, phi;
        if (abs(u.x) > abs(u.y)) { r = u.x; phi = (u.y / u.x) * K_PI_4; }
        else { r = u.y; phi = K_PI_2 - (u.x / u.y) * K_PI_4; }
        d = r * float2(cos(phi), sin(phi));
    }
    float z = sqrt(max(0.0, 1.0 - dot(d, d)));
    pdf = z * K_1_PI;
    return float3(d, z);
}
struct MaterialHeader
{
    bool thinSurface;
    bool isThinSurface() { return thinSurface; }
    uint getActiveLobes() { return LobeType::All; }
    bool isPSDExclude() { return false; }
};
struct ShadingData
{
    float3 posW, N, V, T, B;
    MaterialHeader mtl;
    float3 toLocal(float3 v) { return float3(dot(v,T), dot(v,B), dot(v,N)); }
    float3 fromLocal(float3 v) { return T*v.x + B*v.y + N*v.z; }
};
static const int cMaxDeltaLobes = 2;
struct DeltaLobe
{
    float3 dir, thp;
    float probability;
    bool transmission;
    static DeltaLobe make() { return (DeltaLobe)0; }
};
