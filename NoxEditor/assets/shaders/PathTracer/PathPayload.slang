#pragma once

#include "PathState.slang"

struct PathPayload
{
    uint4 packed[8];

    // Unit vector <-> two 16-bit octahedral coordinates.
    static uint packDirection(float3 d)
    {
        float2 p = d.xy / (abs(d.x) + abs(d.y) + abs(d.z));
        if (d.z < 0.0)
            p = (1.0 - abs(p.yx)) * float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
        const uint2 q = uint2(round(saturate(p * 0.5 + 0.5) * 65535.0));
        return q.x | (q.y << 16);
    }

    static float3 unpackDirection(uint u)
    {
        const float2 p = float2(u & 0xFFFF, u >> 16) / 65535.0 * 2.0 - 1.0;
        float3 d = float3(p, 1.0 - abs(p.x) - abs(p.y));
        if (d.z < 0.0)
            d.xy = (1.0 - abs(d.yx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0);
        return normalize(d);
    }

    static uint packHalf2x16(float2 v)
    {
        return (f32tof16(v.y) << 16) | (f32tof16(v.x) & 0xFFFF);
    }

    static float2 unpackHalf2x16(uint u)
    {
        return float2(f16tof32(u & 0xFFFF), f16tof32(u >> 16));
    }

    static PathPayload pack(PathState path)
    {
        PathPayload p;

        // 0: Origin & ID
        p.packed[0].x = asuint(path.origin.x);
        p.packed[0].y = asuint(path.origin.y);
        p.packed[0].z = asuint(path.origin.z);
        p.packed[0].w = path.id;

        // 1: Direction & SceneLength
        p.packed[1].x = asuint(path.dir.x);
        p.packed[1].y = asuint(path.dir.y);
        p.packed[1].z = asuint(path.dir.z);
        p.packed[1].w = asuint(path.sceneLength);

        // 2: Throughput & Radiance
        p.packed[2].x = packHalf2x16(path.thp.xy);
        p.packed[2].y = (f32tof16(path.thp.z) & 0xFFFF) | (path.primaryLobe << 16);
        p.packed[2].z = packHalf2x16(path.L.xy);
        p.packed[2].w = packHalf2x16(path.L.zw);

        // 3: Diffuse & Specular Radiance + NRD HitDist & PrimaryViewZ
        p.packed[3].x = packHalf2x16(path.diffuseRadiance.xy);
        p.packed[3].y = (f32tof16(path.diffuseRadiance.z) & 0xFFFF) | (f32tof16(path.specularRadiance.x) << 16);
        p.packed[3].z = packHalf2x16(path.specularRadiance.yz);
        p.packed[3].w = (f32tof16(path.firstBounceHitDist) & 0xFFFF) | (f32tof16(path.primaryViewZ) << 16);

        // 4: RayCone, MIS, RR, Counters & Flags
        p.packed[4].x = path.rayCone.widthSpreadAngleFP16;
        p.packed[4].y = (f32tof16(path.fireflyFilterK) << 16) | (f32tof16(path.bsdfScatterPdf) & 0xFFFF);
        p.packed[4].z = (path.packedMISInfo << 16) | (f32tof16(path.thpRuRuCorrection) & 0xFFFF);
        p.packed[4].w = path.flagsAndVertexIndex;
        p.packed[5] = uint4(asuint(path.bsdfScatterPdf), path.packedCounters, path.interiorSlot0, path.interiorSlot1);

        // 6, 7: the pending branch
        p.packed[6] = uint4(asuint(path.branchOrigin), packDirection(path.branchDir));
        p.packed[7] = uint4(packHalf2x16(path.branchThp.xy), f32tof16(path.branchThp.z), path.branchRayCone, path.branchFlagsAndVertexIndex);

        return p;
    }

    static PathState unpack(PathPayload p)
    {
        PathState path;

        // 0: Origin & ID
        path.origin.x = asfloat(p.packed[0].x);
        path.origin.y = asfloat(p.packed[0].y);
        path.origin.z = asfloat(p.packed[0].z);
        path.id       = p.packed[0].w;

        // 1: Direction & SceneLength
        path.dir.x      = asfloat(p.packed[1].x);
        path.dir.y      = asfloat(p.packed[1].y);
        path.dir.z      = asfloat(p.packed[1].z);
        path.sceneLength= asfloat(p.packed[1].w);

        // 2: Throughput & Radiance
        float2 thpXY = unpackHalf2x16(p.packed[2].x);
        path.thp = float3(thpXY.x, thpXY.y, f16tof32(p.packed[2].y & 0xFFFF));
        path.primaryLobe = p.packed[2].y >> 16;
        path.L = float4(unpackHalf2x16(p.packed[2].z), unpackHalf2x16(p.packed[2].w));

        // 3: Diffuse & Specular Radiance + NRD Signals
        float2 diffXY = unpackHalf2x16(p.packed[3].x);
        path.diffuseRadiance = float3(diffXY.x, diffXY.y, f16tof32(p.packed[3].y & 0xFFFF));
        float specX = f16tof32(p.packed[3].y >> 16);
        float2 specYZ = unpackHalf2x16(p.packed[3].z);
        path.specularRadiance = float3(specX, specYZ.x, specYZ.y);
        path.firstBounceHitDist = f16tof32(p.packed[3].w & 0xFFFF);
        path.primaryViewZ = f16tof32(p.packed[3].w >> 16);

        // 4: RayCone, MIS, RR, Flags
        path.rayCone.widthSpreadAngleFP16 = p.packed[4].x;
        path.fireflyFilterK               = f16tof32(p.packed[4].y >> 16);
        path.bsdfScatterPdf               = asfloat(p.packed[5].x);
        path.packedMISInfo                = p.packed[4].z >> 16;
        path.thpRuRuCorrection            = f16tof32(p.packed[4].z & 0xFFFF);
        path.flagsAndVertexIndex          = p.packed[4].w;

        path.interiorSlot0 = p.packed[5].z;
        path.interiorSlot1 = p.packed[5].w;
        path.packedCounters = p.packed[5].y;
        path.stableBranchID = 0;

        path.branchOrigin = asfloat(p.packed[6].xyz);
        path.branchDir = unpackDirection(p.packed[6].w);
        path.branchThp = float3(unpackHalf2x16(p.packed[7].x), f16tof32(p.packed[7].y));
        path.branchRayCone = p.packed[7].z;
        path.branchFlagsAndVertexIndex = p.packed[7].w;

        return path;
    }
};
