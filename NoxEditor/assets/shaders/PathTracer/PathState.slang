#pragma once

#include "Ray.slang"
#include "LobeType.slang"

namespace PathFlags
{
    static const uint active                  = (1 << 0);
    static const uint hit                     = (1 << 1);
    static const uint transmission            = (1 << 2);
    static const uint specular                = (1 << 3);
    static const uint delta                   = (1 << 4);
    static const uint insideDielectricVolume  = (1 << 5);
    static const uint terminateAtNextBounce   = (1 << 6);
    // (1 << 7), (1 << 8): the color channel the path carries after a dispersive refraction (PathState::dispersionChannel)
    static const uint deltaOnlyPath           = (1 << 12);
    static const uint deltaTree               = (1 << 13); // only delta events since the camera (RTXPT's stable planes): no firefly filter
    static const uint rrSpecHitPending        = (1 << 14); // the ray leaving RR's guide surface: its length is the specular hit distance
    static const uint rrGuidesDone            = (1 << 15); // RR's guides for this pixel are final (written at the first glass's split)
}

namespace PackedCounters
{
    static const uint DiffuseBounces   = 0;
    static const uint RejectedHits     = 1;
}

static const uint kVertexIndexBitCount = 10u;
static const uint kVertexIndexBitMask  = (1u << kVertexIndexBitCount) - 1u;

struct RayCone
{
    uint widthSpreadAngleFP16;

    static RayCone make(float width, float angle)
    {
        RayCone rc;
        rc.widthSpreadAngleFP16 = (f32tof16(width) << 16) | (f32tof16(angle) & 0xFFFF);
        return rc;
    }

    float getWidth()
    {
        return f16tof32(widthSpreadAngleFP16 >> 16);
    }

    float getSpreadAngle()
    {
        return f16tof32(widthSpreadAngleFP16 & 0xFFFF);
    }

    RayCone propagateDistance(float hitT)
    {
        float w = getWidth();
        float a = getSpreadAngle();
        return RayCone::make(w + a * hitT, a);
    }
};

struct PathState
{
    float3 origin;
    uint id;

    float3 dir;
    float sceneLength;

    float3 thp;
    float4 L; // rgb = total radiance, a = specular average
    // RTXPT's stable radiance: what L gathered along delta-only paths from the camera (emitters and the sky seen directly, through
    // glass, in mirrors), part of L too. RR's guides take it in (PathTracerRayGen.slang, RTXPT's DLSS-RR input preparation).
    float3 stableL;

    float3 diffuseRadiance;
    float3 specularRadiance;
    float firstBounceHitDist;
    float primaryViewZ;
    uint primaryLobe;

    uint interiorSlot0;
    uint interiorSlot1;
    uint packedCounters;
    uint stableBranchID;

    RayCone rayCone;
    float fireflyFilterK;
    float bsdfScatterPdf;
    uint packedMISInfo;
    float thpRuRuCorrection;

    uint flagsAndVertexIndex;

    // The other branch of the first smooth glass the camera's path met (PathTracerHit.slang): traced once this path ends
    // (PathTracerRayGen.slang). branchThp 0: none.
    float3 branchOrigin;
    float3 branchDir;
    float3 branchThp;
    uint branchRayCone;
    uint branchFlagsAndVertexIndex;

    static PathState init(uint2 pixelPos, float pixelSpreadAngle = 0.0)
    {
        PathState path;
        path.origin = float3(0.0);
        path.id = pixelPos.x | (pixelPos.y << 16);
        path.dir = float3(0, 0, 1);
        path.sceneLength = 0.0;
        path.thp = float3(1.0);
        path.L = float4(0.0);
        path.stableL = float3(0.0);
        path.diffuseRadiance = float3(0.0);
        path.specularRadiance = float3(0.0);
        path.firstBounceHitDist = 0.0;
        path.primaryViewZ = 65504.0;
        path.primaryLobe = LobeType::None;
        path.interiorSlot0 = 0;
        path.interiorSlot1 = 0;
        path.packedCounters = 0;
        path.stableBranchID = 0;
        path.rayCone = RayCone::make(0.0, pixelSpreadAngle);
        path.fireflyFilterK = 1.0;
        path.bsdfScatterPdf = 0.0;
        path.packedMISInfo = 0;
        path.thpRuRuCorrection = 1.0;
        path.flagsAndVertexIndex = 0;
        path.branchOrigin = float3(0.0);
        path.branchDir = float3(0.0, 0.0, 1.0);
        path.branchThp = float3(0.0);
        path.branchRayCone = 0;
        path.branchFlagsAndVertexIndex = 0;
        path.setActive(true);
        // Reused as the "camera path has only transmitted so far" marker for RR guide export.
        path.setFlag(PathFlags::deltaOnlyPath, true);
        path.setFlag(PathFlags::deltaTree, true);
        return path;
    }

    bool hasBranch() { return any(branchThp > 0.0); }

    // Continues with the stored branch: its ray, weight, vertex and flags; what the path gathered (L) stays.
    [mutating]
    void resumeBranch()
    {
        origin = branchOrigin;
        dir = branchDir;
        thp = branchThp;
        rayCone.widthSpreadAngleFP16 = branchRayCone;
        flagsAndVertexIndex = branchFlagsAndVertexIndex;
        bsdfScatterPdf = 0.0;
        fireflyFilterK = 1.0;
        packedCounters = 0; // only delta events before the split: no diffuse bounce yet
        branchThp = float3(0.0);
        setActive(true);
    }

    // --- Accessors matching RTXPT ---
    bool isActive()
    {
        return (flagsAndVertexIndex & (PathFlags::active << kVertexIndexBitCount)) != 0;
    }

    [mutating]
    void setActive(bool active = true)
    {
        uint bit = PathFlags::active << kVertexIndexBitCount;
        if (active) flagsAndVertexIndex |= bit;
        else flagsAndVertexIndex &= ~bit;
    }

    [mutating]
    void terminate()
    {
        setActive(false);
    }

    bool isTerminatingAtNextBounce()
    {
        return (flagsAndVertexIndex & (PathFlags::terminateAtNextBounce << kVertexIndexBitCount)) != 0;
    }

    [mutating]
    void setTerminateAtNextBounce(bool v = true)
    {
        uint bit = PathFlags::terminateAtNextBounce << kVertexIndexBitCount;
        if (v) flagsAndVertexIndex |= bit;
        else flagsAndVertexIndex &= ~bit;
    }

    // KHR_materials_dispersion: the one color channel (1 red, 2 green, 3 blue; 0 all) the path carries since its first dispersive
    // refraction, which every later dispersive refraction keeps (the flag bits 7 and 8).
    uint dispersionChannel()
    {
        return (flagsAndVertexIndex >> (kVertexIndexBitCount + 7u)) & 3u;
    }

    [mutating]
    void setDispersionChannel(uint channel)
    {
        const uint shift = kVertexIndexBitCount + 7u;
        flagsAndVertexIndex = (flagsAndVertexIndex & ~(3u << shift)) | ((channel & 3u) << shift);
    }

    uint getVertexIndex()
    {
        return flagsAndVertexIndex & kVertexIndexBitMask;
    }

    [mutating]
    void setVertexIndex(uint vi)
    {
        flagsAndVertexIndex = (flagsAndVertexIndex & ~kVertexIndexBitMask) | (vi & kVertexIndexBitMask);
    }

    [mutating]
    void incrementVertexIndex()
    {
        uint vi = getVertexIndex();
        setVertexIndex(vi + 1);
    }

    uint getCounter(uint counterIdx)
    {
        return (packedCounters >> (counterIdx * 8)) & 0xFF;
    }

    [mutating]
    void setCounter(uint counterIdx, uint val)
    {
        uint shift = counterIdx * 8;
        packedCounters = (packedCounters & ~(0xFF << shift)) | ((val & 0xFF) << shift);
    }

    [mutating]
    void incrementCounter(uint counterIdx)
    {
        setCounter(counterIdx, getCounter(counterIdx) + 1);
    }

    bool hasFlag(uint flag)
    {
        return (flagsAndVertexIndex & (flag << kVertexIndexBitCount)) != 0;
    }

    [mutating]
    void setFlag(uint flag, bool value = true)
    {
        uint bit = flag << kVertexIndexBitCount;
        if (value) flagsAndVertexIndex |= bit;
        else flagsAndVertexIndex &= ~bit;
    }

    RayDesc getScatterRay(float tMin = 0.001, float tMax = 1e9)
    {
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = dir;
        ray.TMin = tMin;
        ray.TMax = tMax;
        return ray;
    }
};
