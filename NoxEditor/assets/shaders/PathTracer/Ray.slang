#pragma once

// Ray type matching RTXPT Utils/Math/Ray.hlsli
struct Ray
{
    float3 origin;
    float tMin;
    float3 dir;
    float tMax;

    static Ray make(float3 origin, float3 dir, float tMin = 0.001, float tMax = 1e9)
    {
        Ray r;
        r.origin = origin;
        r.dir = dir;
        r.tMin = tMin;
        r.tMax = tMax;
        return r;
    }

    RayDesc toRayDesc()
    {
        RayDesc desc;
        desc.Origin = origin;
        desc.TMin = tMin;
        desc.Direction = dir;
        desc.TMax = tMax;
        return desc;
    }

    float3 eval(float t)
    {
        return origin + t * dir;
    }
};
