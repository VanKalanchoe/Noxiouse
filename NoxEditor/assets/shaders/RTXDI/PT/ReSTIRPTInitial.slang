#include "shaderIO.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"
#include "Rtxdi/PT/ReSTIRPTParameters.h"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

#ifndef RTXDI_RESERVOIR_BLOCK_SIZE
#define RTXDI_RESERVOIR_BLOCK_SIZE 16
#endif

static RTXDI_PackedPTReservoir* g_ReservoirBuffer;
#define RTXDI_PT_RESERVOIR_BUFFER g_ReservoirBuffer
static RTXDI_PackedPTReservoir* g_PreservedReservoirBuffer;

static PushConstantReSTIRPTInitial g_PushConstant;
static UniformBufferObject* g_UBO;
static LightData* g_LightData;
static GpuRayTracingInstance* g_InstanceLUT;

// =========================================================================
// RAB_Surface / RAB_Material -- v1: no PSR (Path Space Regularization / DLSS-RR guide) fields, no
// tangent for anisotropy. Matches the real reference's RAB_Surface.hlsli shape (worldPos/viewDir/
// normal/geoNormal/viewDepth/diffuseProbability/material) exactly, just without the DLSS-RR extras.
// =========================================================================

struct RAB_Material
{
    float3 diffuseAlbedo;
    float3 specularF0;
    float roughness;
    float3 emissiveColor;
};

struct RAB_Surface
{
    float3 worldPos;
    float3 viewDir;
    float3 normal;
    float3 geoNormal;
    float viewDepth;
    float diffuseProbability;
    RAB_Material material;
    bool valid;
};

static const float kMinRoughness = 0.04;

RAB_Surface RAB_EmptySurface()
{
    RAB_Surface s = (RAB_Surface)0;
    s.normal = float3(0, 0, 1);
    s.valid = false;
    return s;
}

bool RAB_IsSurfaceValid(RAB_Surface surface) { return surface.valid; }
float3 RAB_GetSurfaceWorldPos(RAB_Surface surface) { return surface.worldPos; }
void RAB_SetSurfaceWorldPos(inout RAB_Surface surface, float3 worldPos) { surface.worldPos = worldPos; }
RAB_Material RAB_GetMaterial(RAB_Surface surface) { return surface.material; }
float3 RAB_GetSurfaceNormal(RAB_Surface surface) { return surface.normal; }
void RAB_SetSurfaceNormal(inout RAB_Surface surface, float3 normal) { surface.normal = normal; }
float3 RAB_GetSurfaceGeoNormal(RAB_Surface surface) { return surface.geoNormal; }
float3 RAB_GetSurfaceViewDir(RAB_Surface surface) { return surface.viewDir; }
float RAB_GetSurfaceLinearDepth(RAB_Surface surface) { return surface.viewDepth; }
float RAB_GetSurfaceRoughness(RAB_Surface surface) { return surface.material.roughness; }
float3 RAB_GetEmissiveColor(RAB_Material material) { return material.emissiveColor; }

// Duff et al. orthonormal basis construction (duplicated per-file, matching this codebase's established
// convention of not sharing helper functions across separate .slang entry-point files).
void build_orthonormal_basis(float3 n, out float3 b1, out float3 b2)
{
    float sign = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + n.z);
    float b = n.x * n.y * a;
    b1 = float3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
    b2 = float3(b, sign + n.y * n.y * a, -n.y);
}

float3 WorldToTangent(RAB_Surface surface, float3 w)
{
    float3 t, b;
    build_orthonormal_basis(surface.normal, t, b);
    return float3(dot(b, w), dot(t, w), dot(surface.normal, w));
}

float3 TangentToWorld(RAB_Surface surface, float3 h)
{
    float3 t, b;
    build_orthonormal_basis(surface.normal, t, b);
    return b * h.x + t * h.y + surface.normal * h.z;
}

float3 fresnel_schlick(float3 F0, float VdotH)
{
    return F0 + (float3(1.0) - F0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

float eval_ndf_ggx(float NdotH, float alphaSq)
{
    float f = (NdotH * alphaSq - NdotH) * NdotH + 1.0;
    return alphaSq / max(3.14159265359 * f * f, 1e-7);
}

float eval_smith_g1(float NdotV, float alphaSq)
{
    return 2.0 * NdotV / max(NdotV + sqrt(alphaSq + (1.0 - alphaSq) * (NdotV * NdotV)), 1e-7);
}

float eval_smith_g2(float NdotV, float NdotL, float alphaSq)
{
    return eval_smith_g1(NdotV, alphaSq) * eval_smith_g1(NdotL, alphaSq);
}

float3 sample_cosine_hemisphere(float u1, float u2, float3 normal)
{
    float r = sqrt(u1);
    float theta = 2.0 * 3.14159265359 * u2;
    float x = r * cos(theta);
    float y = r * sin(theta);
    float z = sqrt(max(0.0, 1.0 - u1));
    float3 t, b;
    build_orthonormal_basis(normal, t, b);
    return normalize(t * x + b * y + normal * z);
}

// GGX VNDF importance sampling (Heitz 2018) -- returns the sampled MICROFACET NORMAL, not the outgoing
// direction (caller reflects -V about it). Same formula already proven in PathTracer.slang.
float3 sample_ggx_vndf(float3 V, float3 N, float roughness, float2 u)
{
    float alpha = max(roughness * roughness, 0.001);
    float3 T, B;
    build_orthonormal_basis(N, T, B);
    float3 V_local = float3(dot(V, T), dot(V, B), dot(V, N));
    float3 Vh = normalize(float3(alpha * V_local.x, alpha * V_local.y, V_local.z));
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 T2 = cross(Vh, T1);
    float r = sqrt(u.x);
    float phi = 2.0 * 3.14159265359 * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(max(1.0 - t1 * t1, 0.0)) + s * t2;
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    float3 Ne_local = normalize(float3(alpha * Nh.x, alpha * Nh.y, max(0.0, Nh.z)));
    float3 M = Ne_local.x * T + Ne_local.y * B + Ne_local.z * N;
    return normalize(M);
}

// PDF of the GGX VNDF sample above, w.r.t. solid angle of the OUTGOING direction L (standard Heitz 2018
// VNDF PDF: D_visible(H) / (4 * dot(V,H)), D_visible(H) = G1(V) * max(0,dot(V,H)) * D(H) / dot(V,N)).
float ImportanceSampleGGX_VNDF_PDF(float roughness, float3 N, float3 V, float3 L)
{
    float3 H = normalize(V + L);
    float NdotH = saturate(dot(N, H));
    float NdotV = saturate(dot(N, V));
    float VdotH = saturate(dot(V, H));
    if (NdotV <= 0.0 || VdotH <= 0.0)
        return 0.0;
    float alpha = max(roughness * roughness, 0.001);
    float alphaSq = alpha * alpha;
    float D = eval_ndf_ggx(NdotH, alphaSq);
    float G1 = eval_smith_g1(NdotV, alphaSq);
    float pdfH = G1 * VdotH * D / NdotV;
    return pdfH / (4.0 * VdotH);
}

// =========================================================================
// BSDF functions used by ReSTIR PT (handle delta/mirror bounces, unlike ReSTIR DI's BRDF-only
// equivalents) -- ported directly from the real RTXDI SDK reference (RAB_Surface.hlsli), using the
// math helpers above instead of NVIDIA's own.
// =========================================================================

float3 RAB_SurfaceEvaluateBsdfTimesNoL(RAB_Surface surface, float3 L, bool isDelta)
{
    float3 N = surface.normal;
    float3 V = surface.viewDir;
    if (dot(L, surface.geoNormal) <= 0.0)
        return float3(0, 0, 0);

    float d = 0.0;
    float3 s = float3(0, 0, 0);
    if (isDelta)
    {
        float NoL = saturate(dot(L, N));
        float3 F = fresnel_schlick(surface.material.specularF0, NoL);
        s = F / max(1.0 - surface.diffuseProbability, 1e-4);
    }
    else
    {
        float NdotL = saturate(dot(N, L));
        d = NdotL / 3.14159265359;
        if (surface.material.roughness >= kMinRoughness)
        {
            float NdotV = saturate(dot(N, V));
            float3 H = normalize(V + L);
            float NdotH = saturate(dot(N, H));
            float VdotH = saturate(dot(V, H));
            float alphaSq = surface.material.roughness * surface.material.roughness * surface.material.roughness * surface.material.roughness;
            float3 F = fresnel_schlick(surface.material.specularF0, VdotH);
            float G = eval_smith_g2(NdotV, NdotL, alphaSq);
            float D = eval_ndf_ggx(NdotH, alphaSq);
            s = (F * G * D) * NdotL / max(4.0 * NdotL * NdotV, 1e-6);
        }
    }
    return d * surface.material.diffuseAlbedo + s;
}

bool RAB_SurfaceImportanceSampleBsdf(RAB_Surface surface, inout RTXDI_RandomSamplerState rng, out float3 dir, out RTXDI_BrdfRaySampleProperties brsp)
{
    float3 rnd;
    rnd.x = RTXDI_GetNextRandom(rng);
    rnd.y = RTXDI_GetNextRandom(rng);
    rnd.z = RTXDI_GetNextRandom(rng);

    brsp = RTXDI_DefaultBrdfRaySampleProperties();
    brsp.SetReflection();

    if (rnd.x < surface.diffuseProbability)
    {
        float3 h = sample_cosine_hemisphere(rnd.y, rnd.z, float3(0, 0, 1));
        dir = TangentToWorld(surface, h);
        brsp.SetDiffuse();
        brsp.SetContinuous();
    }
    else
    {
        brsp.SetSpecular();
        if (surface.material.roughness < kMinRoughness)
        {
            dir = reflect(-surface.viewDir, surface.normal);
            brsp.SetDelta();
        }
        else
        {
            float3 M = sample_ggx_vndf(surface.viewDir, surface.normal, surface.material.roughness, rnd.yz);
            dir = reflect(-surface.viewDir, M);
            brsp.SetContinuous();
        }
    }
    return dot(surface.normal, dir) > 0.0;
}

float RAB_SurfaceEvaluateBsdfPdf(RAB_Surface surface, float3 dir, RTXDI_BrdfRaySampleProperties brsp)
{
    float cosTheta = saturate(dot(surface.normal, dir));
    float diffusePdf = cosTheta / 3.14159265359;
    float specularPdf = 0.0;
    if (surface.material.roughness >= kMinRoughness)
    {
        specularPdf = ImportanceSampleGGX_VNDF_PDF(surface.material.roughness, surface.normal, surface.viewDir, dir);
    }
    else if (brsp.IsDelta())
    {
        specularPdf = 1.0;
    }
    return cosTheta > 0.0 ? lerp(specularPdf, diffusePdf, surface.diffuseProbability) : 0.0;
}

float3 RAB_GetReflectedBsdfRadianceForSurface(float3 incomingRadianceLocation, float3 incomingRadiance, RAB_Surface surface)
{
    float3 L = normalize(incomingRadianceLocation - surface.worldPos);
    return incomingRadiance * RAB_SurfaceEvaluateBsdfTimesNoL(surface, L, false);
}

// =========================================================================
// RAB_LightInfo / RAB_LightSample -- v1 NEE draws uniformly from the local+infinite light regions
// (matches ReSTIR DI's own original v1, before RIS/ReGIR were layered on top; same natural upgrade
// path applies here later). Reuses LightData directly, same as every other RTXDI pass this session.
// =========================================================================

typealias RAB_LightInfo = LightData;

struct RAB_LightSample
{
    float3 position;
    float3 normal;      // "L" (surface-to-light direction); for directional lights this is
                         // LightData.direction.xyz directly, unnegated (engine-wide convention)
    float3 radiance;
    float solidAnglePdf;
    uint lightType;
};

RAB_LightInfo RAB_EmptyLightInfo()
{
    LightData l;
    l.position = float4(0.0);
    l.direction = float4(0.0);
    l.color = float4(0.0);
    l.spotParams = float4(0.0);
    return l;
}

RAB_LightInfo RAB_LoadLightInfo(uint index, bool prevFrame) { return g_LightData[index]; }

RAB_LightSample RAB_EmptyLightSample()
{
    RAB_LightSample s;
    s.position = float3(0.0);
    s.normal = float3(0.0, 1.0, 0.0);
    s.radiance = float3(0.0);
    s.solidAnglePdf = 0.0;
    s.lightType = 0;
    return s;
}

RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo light, float3 worldPos)
{
    RAB_LightSample s = RAB_EmptyLightSample();
    uint lightType = (uint)light.position.w;
    s.lightType = lightType;
    s.solidAnglePdf = 1.0;

    if (lightType == 0) // Directional
    {
        float3 dir = normalize(light.direction.xyz);
        s.normal = dir;
        s.radiance = light.color.rgb * light.color.a;
        return s;
    }

    float3 lightVec = light.position.xyz - worldPos;
    float distance = length(lightVec);
    s.position = light.position.xyz;
    s.normal = distance > 0.0001 ? (lightVec / distance) : float3(0.0, 1.0, 0.0);

    float attenuation;
    float range = light.direction.w;
    float distSq = max(distance * distance, 1e-4);
    if (range > 0.0)
    {
        float dOverR = distance / range;
        float window = max(1.0 - pow(dOverR, 4.0), 0.0);
        attenuation = window / distSq;
    }
    else
    {
        attenuation = 1.0 / distSq;
    }

    if (lightType == 2) // Spot
    {
        float cd = dot(normalize(light.direction.xyz), -s.normal);
        float angular = saturate(cd * light.spotParams.x + light.spotParams.y);
        angular *= angular;
        attenuation *= angular;
    }

    s.radiance = light.color.rgb * light.color.a * attenuation;
    return s;
}

float3 RAB_LightSamplePosition(RAB_LightSample lightSample) { return lightSample.position; }
float RAB_LightSampleSolidAnglePdf(RAB_LightSample lightSample) { return lightSample.solidAnglePdf; }

// Real shadow ray for NEE visibility (unlike ReSTIR DI's initial-sampling stub, which always returns
// true and defers visibility to FinalShading's one resampled shadow ray -- PT's reference implementation
// tests visibility for every NEE candidate at initial-sampling time, see RAB_PathTracer.hlsli's CalculateNEE).
bool RAB_GetConservativeVisibility(RAB_Surface surface, float3 lightPos)
{
    if (g_UBO->tlasDeviceAddress == 0)
        return true;

    float3 toLight = lightPos - surface.worldPos;
    float dist = length(toLight);
    if (dist < 1e-4)
        return true;
    float3 dir = toLight / dist;

    RayDesc shadowRay;
    shadowRay.Origin = surface.worldPos + surface.geoNormal * 0.02;
    shadowRay.Direction = dir;
    shadowRay.TMin = 0.01;
    shadowRay.TMax = max(0.02, dist - 0.02);

    RaytracingAccelerationStructure as = RaytracingAccelerationStructure(g_UBO->tlasDeviceAddress);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> sq;
    sq.TraceRayInline(as, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE, 0xFF, shadowRay);
    sq.Proceed();
    return sq.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

// Directional light for env-map-on-miss handling; distant enough to treat as "at infinity" for the
// path reconnection Jacobian math without special-casing it.
static const float RAB_DISTANT_LIGHT_DISTANCE = 1e4;

// =========================================================================
// RAB_RayPayload -- wraps our own RayQuery committed-hit data (matches the exact fields our existing
// PathTracer.slang's bounce loop already reads), not NVIDIA's own scene/material query system.
// =========================================================================

struct RAB_RayPayload
{
    uint instanceID;
    uint primitiveIndex;
    float2 barycentrics;
    float committedRayT;
    bool frontFace;
};

bool RAB_IsValidHit(RAB_RayPayload rp) { return rp.instanceID != 0xFFFFFFFF; }
float RAB_RayPayloadGetCommittedHitT(RAB_RayPayload rp) { return rp.committedRayT; }
bool RAB_RayPayloadIsFrontFace(RAB_RayPayload rp) { return rp.frontFace; }

// =========================================================================
// RAB_PathTracerUserData -- v1 has no PSR (DLSS-RR guide buffer) fields; just tracks which invocation
// type is running (only Initial exists until temporal/spatial resampling passes are added).
// =========================================================================

struct RAB_PathTracerUserData
{
    uint pathType;
};

RAB_PathTracerUserData RAB_EmptyPathTracerUserData()
{
    RAB_PathTracerUserData d;
    d.pathType = RTXDI_PTPathTraceInvocationType_Initial;
    return d;
}

void RAB_PathTracerUserDataSetPathType(inout RAB_PathTracerUserData ptud, uint pathType) { ptud.pathType = pathType; }

#include "Rtxdi/Utils/Math.hlsli"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"
#include "Rtxdi/LightSampling/UniformSampling.hlsli"

#define RTXDI_RESERVOIR_ADDRESSING_HLSLI

uint2 RTXDI_PixelPosToReservoirPos(uint2 pixelPosition, uint activeCheckerboardField)
{
    return min(pixelPosition, uint2(g_PushConstant.viewportSize) - 1);
}

uint2 RTXDI_ReservoirPosToPixelPos(uint2 reservoirIndex, uint activeCheckerboardField)
{
    return reservoirIndex;
}

uint RTXDI_ReservoirPositionToPointer(
    RTXDI_ReservoirBufferParameters reservoirParams,
    uint2 reservoirPosition,
    uint reservoirArrayIndex)
{
    uint2 clampedPos = min(reservoirPosition, uint2(g_PushConstant.viewportSize) - 1);
    uint2 blockIdx = clampedPos / RTXDI_RESERVOIR_BLOCK_SIZE;
    uint2 positionInBlock = clampedPos % RTXDI_RESERVOIR_BLOCK_SIZE;

    uint ptr = reservoirArrayIndex * reservoirParams.reservoirArrayPitch
        + blockIdx.y * reservoirParams.reservoirBlockRowPitch
        + blockIdx.x * (RTXDI_RESERVOIR_BLOCK_SIZE * RTXDI_RESERVOIR_BLOCK_SIZE)
        + positionInBlock.y * RTXDI_RESERVOIR_BLOCK_SIZE
        + positionInBlock.x;
    uint maxElements = (reservoirArrayIndex + 1u) * reservoirParams.reservoirArrayPitch;
    return min(ptr, max(maxElements, 1u) - 1u);
}

// =========================================================================
// SLANG COMPAT NOTICE (applies to everything down to the "end SLANG COMPAT" marker below): the
// vendored RTXDI SDK is unmodifiable here (cloned fresh, not a local patch target), and Slang -- unlike
// the HLSL/DXC toolchain this SDK targets -- requires an explicit [mutating] attribute on any struct
// method that writes to its own fields (confirmed directly: even a trivial `struct Foo { void Set(int
// v) { value = v; } }` fails to compile under Slang without it). The RTXDI PT module's
// RTXDI_BrdfRaySampleProperties (Utils/BrdfRaySample.hlsli), RTXDI_PathTracerState (PT/PathTracerState.
// hlsli, which is also declared `class` there -- translated to `struct` here since HLSL's `class` is
// value-semantic like `struct`, while Slang's `class` is a heap-allocated reference type that would
// change the algorithm's actual behavior, not just its syntax), and RTXDI_InitialSamplingPathTracerContext
// (PT/InitialSamplingPathTracerContext.hlsli) all rely on implicit self-mutation throughout, so all
// three are ported below verbatim -- same field names, same logic, same order -- with ONLY [mutating]
// added and `class` changed to `struct`. Each one's real include guard is pre-defined immediately
// before it so any transitive #include of the real vendored file becomes a no-op instead of a
// redefinition error. This is NOT an approximation of the algorithm: every line of logic below is a
// direct copy of the vendored source, already verified during this session's earlier deep read of it.
// =========================================================================

// ---- RTXDI_BrdfRaySampleProperties / RTXDI_BrdfRaySample (real file: Utils/BrdfRaySample.hlsli) ----
#define RTXDI_BRDF_RAY_SAMPLE_HLSLI

#define RTXDI_BrdfRaySampleProperties_ContinuousDelta_Bit 0
#define RTXDI_BrdfRaySampleProperties_DiffuseSpecular_Bit 1
#define RTXDI_BrdfRaySampleProperties_ReflectionTransmission_Bit 2

struct RTXDI_BrdfRaySampleProperties
{
    uint flags;

    [mutating] void SetContinuous() { SetBit(flags, RTXDI_BrdfRaySampleProperties_ContinuousDelta_Bit, false); }
    bool IsContinuous() { return !GetBit(flags, RTXDI_BrdfRaySampleProperties_ContinuousDelta_Bit); }
    [mutating] void SetDelta() { SetBit(flags, RTXDI_BrdfRaySampleProperties_ContinuousDelta_Bit, true); }
    bool IsDelta() { return GetBit(flags, RTXDI_BrdfRaySampleProperties_ContinuousDelta_Bit); }

    [mutating] void SetDiffuse() { SetBit(flags, RTXDI_BrdfRaySampleProperties_DiffuseSpecular_Bit, false); }
    bool IsDiffuse() { return !GetBit(flags, RTXDI_BrdfRaySampleProperties_DiffuseSpecular_Bit); }
    [mutating] void SetSpecular() { SetBit(flags, RTXDI_BrdfRaySampleProperties_DiffuseSpecular_Bit, true); }
    bool IsSpecular() { return GetBit(flags, RTXDI_BrdfRaySampleProperties_DiffuseSpecular_Bit); }

    [mutating] void SetReflection() { SetBit(flags, RTXDI_BrdfRaySampleProperties_ReflectionTransmission_Bit, false); }
    bool IsReflection() { return !GetBit(flags, RTXDI_BrdfRaySampleProperties_ReflectionTransmission_Bit); }
    [mutating] void SetTransmission() { SetBit(flags, RTXDI_BrdfRaySampleProperties_ReflectionTransmission_Bit, true); }
    bool IsTransmission() { return GetBit(flags, RTXDI_BrdfRaySampleProperties_ReflectionTransmission_Bit); }
};

RTXDI_BrdfRaySampleProperties RTXDI_DefaultBrdfRaySampleProperties()
{
    RTXDI_BrdfRaySampleProperties properties = (RTXDI_BrdfRaySampleProperties)0;
    properties.SetContinuous();
    properties.SetDiffuse();
    properties.SetReflection();
    return properties;
}

struct RTXDI_BrdfRaySample
{
    float3 outDirection;
    float outPdf;
    float3 brdfTimesNoL;
    RTXDI_BrdfRaySampleProperties properties;
};

RTXDI_BrdfRaySample RTXDI_EmptyBrdfRaySample()
{
    RTXDI_BrdfRaySample brs = (RTXDI_BrdfRaySample)0;
    brs.properties = RTXDI_DefaultBrdfRaySampleProperties();
    return brs;
}

#include "Rtxdi/PT/PathReconnectibility.hlsli"
#include "Rtxdi/PT/PathTracerContextParameters.hlsli"
#include "Rtxdi/Utils/SampledLightData.hlsli"

// ---- RTXDI_PathTracerState (real file: PT/PathTracerState.hlsli, declared `class` there) ----
#define RTXDI_PT_PATH_TRACING_STATE_HLSLI

#define PT_STATE_LAST_VERTEX_FAR           0x02
#define PT_STATE_LAST_VERTEX_ROUGH         0x04
#define PT_STATE_PATH_TERMINATION          0x08
#define PT_STATE_LAST_VERTEX_NEARDELTA     0x10

struct RTXDI_PathTracerState
{
    bool IsSecondaryBounce() { return bounceDepth == 2; }

    [mutating] void BeginPathState()
    {
        lastHitT = RAB_RayPayloadGetCommittedHitT(traceResult);
        brdfRaySample = (RTXDI_BrdfRaySample)0;
        traceResult = (RAB_RayPayload)0;
        continuationRay = (RayDesc)0;
    }

    [mutating] void SetPathTermination(bool value)
    {
        ptStateFlags &= ~PT_STATE_PATH_TERMINATION;
        ptStateFlags |= value ? PT_STATE_PATH_TERMINATION : 0x0;
    }
    bool IsPathTermination() { return (ptStateFlags & PT_STATE_PATH_TERMINATION) != 0; }

    [mutating] void SetIsLastVertexFar(bool value)
    {
        ptStateFlags &= ~PT_STATE_LAST_VERTEX_FAR;
        ptStateFlags |= value ? PT_STATE_LAST_VERTEX_FAR : 0x0;
    }
    bool IsLastVertexFar() { return (ptStateFlags & PT_STATE_LAST_VERTEX_FAR) != 0; }

    [mutating] void SetIsLastVertexRough(bool value)
    {
        ptStateFlags &= ~PT_STATE_LAST_VERTEX_ROUGH;
        ptStateFlags |= value ? PT_STATE_LAST_VERTEX_ROUGH : 0x0;
    }
    bool IsLastVertexRough() { return (ptStateFlags & PT_STATE_LAST_VERTEX_ROUGH) != 0; }

    bool NoMainPathRcVertex() { return rcVertexLength >= bounceDepth; }

    uint16_t initialBounceDepth;
    uint16_t maxPathBounce;
    uint16_t bounceDepth;
    float3 pathThroughput;
    RTXDI_BrdfRaySample brdfRaySample;
    RayDesc continuationRay;
    RAB_Surface intersectionSurface;
    uint16_t ptStateFlags;
    float3 continuationRayBrdfOverPdf;
    RAB_RayPayload traceResult;
    RAB_LightSample lightSampleForDI;
    float lastHitT;
    float3 prevNormal;
    float3 rcPathThroughput;
    uint16_t rcVertexLength;
    RTXDI_SampledLightData sampledLightDataForDI;
    float candidateReservoirWeightSum;
};

RTXDI_PathTracerState RTXDI_EmptyPathTracerState()
{
    return (RTXDI_PathTracerState)0;
}

// ---- RTXDI_InitialSamplingPathTracerContext (real file: PT/InitialSamplingPathTracerContext.hlsli) ----
#define RTXDI_INITIAL_SAMPLING_PATH_TRACING_CONTEXT_HLSLI

struct RTXDI_InitialSamplingPathTracerContext
{
    [mutating] void SetBrdfRaySample(RTXDI_BrdfRaySample raySample)
    {
        ptState.brdfRaySample = raySample;
        SetContinuationRayBrdfOverPdf(raySample.brdfTimesNoL / raySample.outPdf);
    }

    [mutating] void SetMaxPathBounce(uint16_t newMax) { ptState.maxPathBounce = newMax; }

    [mutating] void SetMaxRcVertexLengthIfUnset(uint16_t newMax)
    {
        if (!foundRcVertex)
            ptState.rcVertexLength = newMax;
    }

    bool ShouldRunRussianRoulette() { return ptState.bounceDepth != ptState.initialBounceDepth; }

    [mutating] void RecordRussianRouletteProbability(float rrProb) { russianRoulettePdf *= rrProb; }

    [mutating] void MultiplyPathThroughput(float3 multiplicationFactor) { ptState.pathThroughput *= multiplicationFactor; }

    [mutating] void SetContinuationRay(RayDesc continuationRay) { ptState.continuationRay = continuationRay; }

    [mutating] bool AnalyzePathReconnectibilityBeforeTrace()
    {
        if (ptState.NoMainPathRcVertex())
        {
            bool isCurrentVertexRoughForConnection = false;
            if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FIXED_THRESHOLD)
            {
                isCurrentVertexRoughForConnection = RAB_GetSurfaceRoughness(ptState.intersectionSurface) > roughnessThreshold;
            }
            else if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT)
            {
                const float inverseFootprint = RTXDI_CalculateRayFootprint(RAB_GetSurfaceNormal(mainPathRcVertexSurface), RAB_GetSurfaceViewDir(ptState.intersectionSurface), ptState.lastHitT, ptState.brdfRaySample);
                isCurrentVertexRoughForConnection = inverseFootprint > footprintThreshold;
            }
            if (isCurrentVertexRoughForConnection && ptState.IsLastVertexRough() && ptState.IsLastVertexFar())
            {
                ptState.rcVertexLength = ptState.bounceDepth - 1;
                foundRcVertex = true;
            }
        }

        if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FIXED_THRESHOLD)
        {
            ptState.SetIsLastVertexRough(RAB_GetSurfaceRoughness(ptState.intersectionSurface) > roughnessThreshold);
        }
        else if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT)
        {
            ptState.SetIsLastVertexRough(ptState.brdfRaySample.outPdf <= pdfThreshold && !ptState.brdfRaySample.properties.IsDelta());
        }

        if (ptState.rcVertexLength == ptState.bounceDepth - 1)
        {
            mainPathRcWiPdf = ptState.brdfRaySample.outPdf;
            mainPathPartialJacobian = RTXDI_CalculatePartialJacobian(ptState.lastHitT, -RAB_GetSurfaceViewDir(ptState.intersectionSurface), RAB_GetSurfaceNormal(ptState.intersectionSurface));
        }

        if (ptState.rcVertexLength >= ptState.bounceDepth - 1)
        {
            mainPathRcVertexSurface = ptState.intersectionSurface;
        }

        if (ptState.rcVertexLength > ptState.bounceDepth - 1)
        {
            brdfPdfBeforeRcVertex = ptState.brdfRaySample.outPdf;
        }

        if (ptState.bounceDepth > ptState.rcVertexLength)
        {
            ptState.rcPathThroughput *= ptState.continuationRayBrdfOverPdf;
        }

        return !IsPathTerminated();
    }

    [mutating] void SetTraceResult(RAB_RayPayload rp) { ptState.traceResult = rp; }

    [mutating] void SetIntersectionSurface(const RAB_Surface intersectionSurface)
    {
        ptState.intersectionSurface = intersectionSurface;
        RAB_SetSurfaceNormal(ptState.intersectionSurface, normalize(RAB_GetSurfaceNormal(ptState.intersectionSurface)));
    }

    [mutating] void UpdateReconnectionStateForPathIntersection()
    {
        if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FIXED_THRESHOLD)
        {
            ptState.SetIsLastVertexFar(RAB_RayPayloadGetCommittedHitT(ptState.traceResult) > distanceThreshold && !ptState.brdfRaySample.properties.IsDelta());
        }
        else if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT)
        {
            const float rayFootprint = RTXDI_CalculateRayFootprint(RAB_GetSurfaceNormal(ptState.intersectionSurface), RAB_GetSurfaceViewDir(ptState.intersectionSurface), RAB_RayPayloadGetCommittedHitT(ptState.traceResult), ptState.brdfRaySample);
            ptState.SetIsLastVertexFar(rayFootprint > footprintThreshold);
        }
    }

    [mutating] void RecordPathIntersection(const RAB_Surface intersectionSurface)
    {
        SetIntersectionSurface(intersectionSurface);
        UpdateReconnectionStateForPathIntersection();
    }

    bool ShouldSampleEmissiveSurfaces() { return true; }

    [mutating] bool RecordEmissiveLightSample(float3 radianceFromEmissiveSurface, RAB_Surface prevSurface, inout RTXDI_RandomSamplerState rng)
    {
        const uint candidateRcVertexLength = (ptState.NoMainPathRcVertex() && ptState.IsLastVertexFar()) ? ptState.bounceDepth : ptState.rcVertexLength;

        float partialJacobianToLight = 0.f;
        if (candidateRcVertexLength == ptState.bounceDepth)
        {
            partialJacobianToLight = RTXDI_CalculatePartialJacobian(RAB_RayPayloadGetCommittedHitT(ptState.traceResult), ptState.continuationRay.Direction, RAB_GetSurfaceNormal(ptState.intersectionSurface));
        }

        float3 Le = radianceFromEmissiveSurface;
        bool accepted = RISStreamPathSample(ptState.bounceDepth, Le, rng, candidateRcVertexLength, false, 1.f, ptState.brdfRaySample.outDirection, partialJacobianToLight);
        return accepted;
    }

    bool ShouldSampleNee() { return true; }

    [mutating] bool RecordNeeLightSample(in const RTXDI_SampledLightData sampledLightData,
                              in const float3 radianceFromLights,
                              in const float neePdf,
                              in const float scatterPdf,
                              in const RAB_LightSample lightSample,
                              inout RTXDI_RandomSamplerState rng)
    {
        bool selected = false;
        ptState.sampledLightDataForDI = sampledLightData;
        ptState.lightSampleForDI = lightSample;
        if (any(radianceFromLights > 0.f) && RTXDI_SampledLightData_IsValidLightData(ptState.sampledLightDataForDI))
        {
            RTXDI_SampledLightData localLightData = ptState.sampledLightDataForDI;
            RAB_LightSample localLightSample = ptState.lightSampleForDI;

            const uint pathLength = ptState.bounceDepth + 1;
            if (pathLength > GetMaxPathBounce())
                return false;

            bool isCurrentVertexRoughForConnection = false;
            if (ptState.NoMainPathRcVertex())
            {
                if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FIXED_THRESHOLD)
                {
                    isCurrentVertexRoughForConnection = RAB_GetSurfaceRoughness(ptState.intersectionSurface) > roughnessThreshold;
                }
                else if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT)
                {
                    RTXDI_BrdfRaySample brs = (RTXDI_BrdfRaySample)0;
                    brs.outPdf = scatterPdf;
                    brs.properties.SetContinuous();
                    float inverseFootprint = RTXDI_CalculateRayFootprint(RAB_GetSurfaceNormal(mainPathRcVertexSurface), RAB_GetSurfaceViewDir(ptState.intersectionSurface), RAB_RayPayloadGetCommittedHitT(ptState.traceResult), brs);
                    isCurrentVertexRoughForConnection = inverseFootprint > footprintThreshold;
                }
            }

            const uint rcVertexLength = ptState.NoMainPathRcVertex() ?
                (ptState.IsLastVertexRough() && ptState.IsLastVertexFar() && isCurrentVertexRoughForConnection ? pathLength - 1 : pathLength) :
                ptState.rcVertexLength;

            float pathPartialJacobian = rcVertexLength == pathLength ? RAB_LightSampleSolidAnglePdf(localLightSample) : 0.f;
            if (rcVertexLength == pathLength - 1)
            {
                pathPartialJacobian = RTXDI_CalculatePartialJacobian(RAB_RayPayloadGetCommittedHitT(ptState.traceResult), ptState.continuationRay.Direction, RAB_GetSurfaceNormal(ptState.intersectionSurface));
            }

            float3 Le = radianceFromLights;

            selected = RISStreamPathSample(pathLength, Le, rng,
                rcVertexLength, true, neePdf,
                float3((rcVertexLength == pathLength ? 1.f / 0.f : 1.f), asfloat(ptState.sampledLightDataForDI.lightData), asfloat(ptState.sampledLightDataForDI.uvData)),
                pathPartialJacobian);

            if (selected && rcVertexLength == pathLength - 1)
            {
                selectedRcWiPdf = scatterPdf;
            }
        }

        return selected;
    }

    [mutating] void RecordPathRadianceMiss(inout RTXDI_RandomSamplerState rng)
    {
        ptState.SetPathTermination(true);
    }

    [mutating] bool RecordEnvironmentMapLightSample(const float3 environmentMapRadiance,
                                         RAB_Surface prevSurface,
                                         inout RTXDI_RandomSamplerState rng)
    {
        const float FakeDistanceHitT = RAB_DISTANT_LIGHT_DISTANCE;
        const float3 FakeWorldPosition = RAB_GetSurfaceWorldPos(ptState.intersectionSurface) + FakeDistanceHitT * ptState.continuationRay.Direction;

        float3 Le = environmentMapRadiance;
        const uint envRcVertexLength = (ptState.NoMainPathRcVertex() && ptState.IsLastVertexRough()) ? ptState.bounceDepth : ptState.rcVertexLength;

        float partialJacobianToLightEnv = 0.f;
        if (envRcVertexLength == ptState.bounceDepth)
        {
            RAB_SetSurfaceWorldPos(ptState.intersectionSurface, FakeWorldPosition);
            RAB_SetSurfaceNormal(ptState.intersectionSurface, -ptState.continuationRay.Direction);
            partialJacobianToLightEnv = RTXDI_CalculatePartialJacobian(FakeDistanceHitT, ptState.continuationRay.Direction, RAB_GetSurfaceNormal(ptState.intersectionSurface));
        }

        bool envSelected = RISStreamPathSample(ptState.bounceDepth, Le, rng, envRcVertexLength, false, 1.f, ptState.continuationRay.Direction, partialJacobianToLightEnv);
        return envSelected;
    }

    [mutating] void Init(RTXDI_PathTracerContextParameters ptParams, const RAB_Surface primarySurface, inout RTXDI_PathTracerRandomContext ptRandContext)
    {
        ptState.initialBounceDepth = 2;
        ptState.bounceDepth = ptState.initialBounceDepth;
        ptState.maxPathBounce = (uint16_t)ptParams.maxBounces;
        ptState.pathThroughput = float3(1.0f, 1.0f, 1.0f);
        ptState.intersectionSurface = primarySurface;

        runningWeightSum = 0.f;
        selectedPathLength = 0;
        brdfPdfBeforeRcVertex = 1.f;
        russianRoulettePdf = 1.f;
        mainPathRcVertexSurface = RAB_EmptySurface();
        selectedRcSurface = RAB_EmptySurface();

        ptState.rcPathThroughput = float3(1.f, 1.f, 1.f);
        ptState.rcVertexLength = ptParams.maxRcVertexLength;

        foundRcVertex = false;

        reconnectionMode = ptParams.rcParams.reconnectionMode;
        RTXDI_CalculateFootprintParameters(ptParams.rcParams, ptParams.rcrParams, primarySurface, ptRandContext, footprintThreshold, pdfThreshold);
        distanceThreshold = ptParams.rcParams.distanceThreshold;
        roughnessThreshold = ptParams.rcParams.roughnessThreshold;
    }

    float CalculateRISWeight(float3 targetFunctionOverP)
    {
        float risWeight = RTXDI_Luminance(targetFunctionOverP);
        risWeight = (isnan(risWeight) || isinf(risWeight)) ? 0.f : risWeight;
        return risWeight;
    }

    [mutating] bool RISStreamPathSample(uint pathLength,
                             float3 currentLightRadiance,
                             inout RTXDI_RandomSamplerState rng,
                             uint rcVertexLength,
                             bool isNee,
                             float neePdf,
                             float3 packedLightData,
                             float partialJacobian)
    {
        const float3 targetFunctionOverP = currentLightRadiance * ptState.pathThroughput;
        float risWeight = RTXDI_Luminance(targetFunctionOverP);
        risWeight = (isnan(risWeight) || isinf(risWeight)) ? 0.f : risWeight;

        runningWeightSum += risWeight;

        if (runningWeightSum * RTXDI_GetNextRandom(rng) < risWeight)
        {
            bool connectBeforeLight = rcVertexLength == pathLength - 1;
            bool connectToLight = rcVertexLength == pathLength;

            selectedPathLength = pathLength;
            radiance = currentLightRadiance * ptState.rcPathThroughput;

            float pdfToBeExcludedFromTargetFunction = russianRoulettePdf * ((connectToLight && isNee) ? neePdf : (rcVertexLength > pathLength ? 1.f : brdfPdfBeforeRcVertex));
            selectedTargetFunction = pdfToBeExcludedFromTargetFunction * currentLightRadiance * ptState.pathThroughput;

            selectedRcVertexLength = rcVertexLength;
            selectedRcWiPdf = mainPathRcWiPdf;

            if ((connectToLight && !isNee) || (connectBeforeLight && isNee))
            {
                selectedRcSurface = ptState.intersectionSurface;
            }
            else
            {
                selectedRcSurface = mainPathRcVertexSurface;
            }

            selectedPartialJacobian = partialJacobian == 0.f ? mainPathPartialJacobian : partialJacobian;
            radiance = isinf(packedLightData.x) ? packedLightData : radiance;

            return true;
        }

        return false;
    }

    bool IsPathTerminated() { return ptState.IsPathTermination(); }

    float3 GetSelectedTargetFunction() { return selectedTargetFunction; }
    // Ported from the RTXDI_PathTracerContext<T> wrapper's pass-through getters (PathTracerContext.hlsli)
    // -- exposed as methods here too since we call these from GenerateInitialSamples()'s ported body.
    float GetSelectedPartialJacobian() { return selectedPartialJacobian; }
    uint GetSelectedPathLength() { return selectedPathLength; }
    RAB_Surface GetSelectedRcSurface() { return selectedRcSurface; }
    uint GetSelectedRcVertexLength() { return selectedRcVertexLength; }
    float GetSelectedRcWiPdf() { return selectedRcWiPdf; }

    RTXDI_PTReconnectionMode GetReconnectionMode() { return reconnectionMode; }
    float GetRoughnessThreshold() { return roughnessThreshold; }
    float GetDistanceThreshold() { return distanceThreshold; }
    float GetFootprintThreshold() { return footprintThreshold; }
    float GetPdfThreshold() { return pdfThreshold; }

    float GetCheckPreRcInverseGeoTerm() { return 0.0; }
    RAB_Surface GetRcPrevSurface() { return RAB_EmptySurface(); }
    [mutating] void SetRcVertexLength(uint length) { }
    [mutating] void SetSelectedPathLength(uint length) { }

    float3 GetRadiance() { return radiance; }

    uint GetBounceDepth() { return ptState.bounceDepth; }
    uint GetMaxPathBounce() { return ptState.maxPathBounce; }
    RTXDI_BrdfRaySample GetBrdfRaySample() { return ptState.brdfRaySample; }
    RAB_Surface GetIntersectionSurface() { return ptState.intersectionSurface; }

    [mutating] void SetContinuationRayBrdfOverPdf(float3 brdfOverPdf) { ptState.continuationRayBrdfOverPdf = brdfOverPdf; }
    float3 GetContinuationRayBrdfOverPdf() { return ptState.continuationRayBrdfOverPdf; }
    RayDesc GetContinuationRay() { return ptState.continuationRay; }
    float3 GetPathThroughput() { return ptState.pathThroughput; }
    RAB_RayPayload GetTraceResult() { return ptState.traceResult; }

    [mutating] void IncreaseBounceDepth() { ptState.bounceDepth++; }

    bool ValidContinuationRayBrdfOverPdf()
    {
        float lum = RTXDI_Luminance(ptState.continuationRayBrdfOverPdf);
        return lum > 0 && !isinf(lum) && !isnan(lum);
    }

    uint16_t GetRcVertexLength() { return ptState.rcVertexLength; }

    bool IsSecondaryBounce() { return ptState.IsSecondaryBounce(); }
    [mutating] void BeginPathState() { ptState.BeginPathState(); }

    [mutating] void SetPathTermination(bool value) { ptState.SetPathTermination(value); }
    bool IsPathTermination() { return ptState.IsPathTermination(); }

    [mutating] void SetIsLastVertexFar(bool value) { ptState.SetIsLastVertexFar(value); }
    bool IsLastVertexFar() { return ptState.IsLastVertexFar(); }
    [mutating] void SetIsLastVertexRough(bool value) { ptState.SetIsLastVertexRough(value); }
    bool IsLastVertexRough() { return ptState.IsLastVertexRough(); }

    bool NoMainPathRcVertex() { return ptState.NoMainPathRcVertex(); }

    float3              radiance;
    RAB_Surface         mainPathRcVertexSurface;
    float               mainPathRcWiPdf;
    float               mainPathPartialJacobian;
    float               brdfPdfBeforeRcVertex;
    float               runningWeightSum;
    float3              selectedTargetFunction;
    float               selectedRcWiPdf;
    uint                selectedPathLength;
    uint                selectedRcVertexLength;
    RAB_Surface         selectedRcSurface;
    float               selectedPartialJacobian;
    float               russianRoulettePdf;
    float               footprintThreshold;
    float               pdfThreshold;
    RTXDI_PTReconnectionMode reconnectionMode;
    float               roughnessThreshold;
    float               distanceThreshold;
    bool                evaluateEnvMapOnMiss;
    uint                lightSamplingMode;
    bool                foundRcVertex;
    RTXDI_PathTracerState ptState;
};

// ---- end SLANG COMPAT ----

#include "Rtxdi/PT/Reservoir.hlsli"

// =========================================================================
// Secondary-hit surface construction (mirrors PathTracer.slang's existing inline hit-material code
// exactly, just packaged as a function returning RAB_Surface instead of writing loose locals).
// =========================================================================

float2 intersection_uv(GpuRayTracingInstance lut, uint primIndex, float2 barycentrics)
{
    uint32_t* indices = (uint32_t*)lut.indexBufferAddress;
    uint i0 = indices[primIndex * 3 + 0];
    uint i1 = indices[primIndex * 3 + 1];
    uint i2 = indices[primIndex * 3 + 2];
    Vertex* vertices = (Vertex*)lut.vertexBufferAddress;
    float w0 = 1.0 - barycentrics.x - barycentrics.y;
    float w1 = barycentrics.x;
    float w2 = barycentrics.y;
    return w0 * vertices[i0].uv0 + w1 * vertices[i1].uv0 + w2 * vertices[i2].uv0;
}

float3 perturb_normal(GpuRayTracingInstance lut, Vertex v0, Vertex v1, Vertex v2, float3 geomNormal, float2 hitUV, SamplerState textureSampler)
{
    if (lut.normalTextureIndex == 0xFFFFFFFF)
        return geomNormal;

    float3 dp1 = mul((float3x3)lut.normalMatrix, v1.pos - v0.pos);
    float3 dp2 = mul((float3x3)lut.normalMatrix, v2.pos - v0.pos);
    float2 duv1 = v1.uv0 - v0.uv0;
    float2 duv2 = v2.uv0 - v0.uv0;

    float det = duv1.x * duv2.y - duv2.x * duv1.y;
    if (abs(det) < 1e-12)
        return geomNormal;

    float3 T = dp1 * duv2.y - dp2 * duv1.y;
    float3 B = dp2 * duv1.x - dp1 * duv2.x;
    float3 N = geomNormal;

    float lenT = length(T);
    if (lenT < 1e-6) return geomNormal;

    T = normalize(T - N * dot(N, T));
    float3 B_ortho = normalize(cross(N, T));
    if (dot(B_ortho, B) < 0.0)
        B_ortho = -B_ortho;

    float3x3 TBN = float3x3(T, B_ortho, N);
    Texture2D normalTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + lut.normalTextureIndex, 0));
    float3 tangentNormal = normalTex.SampleLevel(textureSampler, hitUV, 0).xyz * 2.0 - 1.0;
    return normalize(mul(tangentNormal, TBN));
}

RAB_Surface LoadSurfaceFromRayPayload(RAB_RayPayload rp, float3 rayOrigin, float3 rayDir)
{
    RAB_Surface surface = RAB_EmptySurface();

    GpuRayTracingInstance lut = g_InstanceLUT[rp.instanceID];
    uint32_t* indices = (uint32_t*)lut.indexBufferAddress;
    uint i0 = indices[rp.primitiveIndex * 3 + 0];
    uint i1 = indices[rp.primitiveIndex * 3 + 1];
    uint i2 = indices[rp.primitiveIndex * 3 + 2];
    Vertex* vertices = (Vertex*)lut.vertexBufferAddress;

    float w0 = 1.0 - rp.barycentrics.x - rp.barycentrics.y;
    float w1 = rp.barycentrics.x;
    float w2 = rp.barycentrics.y;
    float2 hitUV = w0 * vertices[i0].uv0 + w1 * vertices[i1].uv0 + w2 * vertices[i2].uv0;

    float3 localNormal = normalize(w0 * vertices[i0].normal + w1 * vertices[i1].normal + w2 * vertices[i2].normal);
    float3 hitNormal = normalize(mul((float3x3)lut.normalMatrix, localNormal));
    if (lut.doubleSided != 0 && dot(hitNormal, -rayDir) < 0.0)
        hitNormal = -hitNormal;

    float3 hitPos = rayOrigin + rayDir * rp.committedRayT;

    SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

    float3 albedo = lut.baseColorFactor.rgb;
    if (lut.baseColorTextureIndex != 0xFFFFFFFF)
    {
        Texture2D colTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + lut.baseColorTextureIndex, 0));
        albedo *= colTex.SampleLevel(textureSampler, hitUV, 0).rgb;
    }

    float metallic = clamp(lut.metallicFactor, 0.0, 1.0);
    float roughness = clamp(lut.roughnessFactor, kMinRoughness, 1.0);
    if (lut.metallicRoughnessTextureIndex != 0xFFFFFFFF)
    {
        Texture2D mrTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + lut.metallicRoughnessTextureIndex, 0));
        float4 mrSample = mrTex.SampleLevel(textureSampler, hitUV, 0);
        roughness = clamp(roughness * mrSample.g, kMinRoughness, 1.0);
        metallic = clamp(metallic * mrSample.b, 0.0, 1.0);
    }

    hitNormal = perturb_normal(lut, vertices[i0], vertices[i1], vertices[i2], hitNormal, hitUV, textureSampler);

    surface.worldPos = hitPos;
    surface.geoNormal = normalize(mul((float3x3)lut.normalMatrix, localNormal));
    surface.normal = hitNormal;
    surface.viewDir = -rayDir;
    surface.material.diffuseAlbedo = albedo * (1.0 - metallic);
    surface.material.specularF0 = lerp(float3(0.04), albedo, metallic);
    surface.material.roughness = roughness;
    surface.material.emissiveColor = lut.emissiveFactor.rgb * lut.emissiveFactor.a;
    surface.diffuseProbability = GetSurfaceDiffuseProbability(surface);
    surface.viewDepth = 1.0; // unused for secondary surfaces
    surface.valid = true;

    return surface;
}

float GetSurfaceDiffuseProbability(RAB_Surface surface)
{
    float diffuseWeight = dot(surface.material.diffuseAlbedo, float3(0.2126, 0.7152, 0.0722));
    float specularWeight = dot(fresnel_schlick(surface.material.specularF0, saturate(dot(surface.viewDir, surface.normal))), float3(0.2126, 0.7152, 0.0722));
    float sumWeights = diffuseWeight + specularWeight;
    return sumWeights < 1e-7 ? 1.0 : (diffuseWeight / sumWeights);
}

float2 intersection_uv_candidate(GpuRayTracingInstance lut, uint primIndex, float2 barycentrics)
{
    return intersection_uv(lut, primIndex, barycentrics);
}

RayDesc setupContinuationRay(float3 worldNormal, float3 worldPos, float3 outDir)
{
    RayDesc ray = (RayDesc)0;
    float NoL = dot(outDir, worldNormal);
    float3 offsetNormal = NoL > 0.0 ? worldNormal : -worldNormal;
    ray.Origin = worldPos + offsetNormal * 0.001;
    ray.Direction = outDir;
    ray.TMin = 0.0001;
    ray.TMax = 1e8;
    return ray;
}

RAB_RayPayload TraceNextBounce(RayDesc ray)
{
    RAB_RayPayload result = (RAB_RayPayload)0;
    result.instanceID = 0xFFFFFFFF;

    RaytracingAccelerationStructure as = RaytracingAccelerationStructure(g_UBO->tlasDeviceAddress);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    rq.TraceRayInline(as, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);

    SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
    while (rq.Proceed())
    {
        uint instID = rq.CandidateRayInstanceCustomIndex();
        uint primIndex = rq.CandidatePrimitiveIndex();
        GpuRayTracingInstance lut = g_InstanceLUT[instID];

        if (lut.alphaMode == 0 || lut.baseColorTextureIndex == 0xFFFFFFFF)
        {
            rq.CommitNonOpaqueTriangleHit();
        }
        else
        {
            float2 uv = intersection_uv_candidate(lut, primIndex, rq.CandidateTriangleBarycentrics());
            Texture2D tex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + lut.baseColorTextureIndex, 0));
            float4 col = tex.SampleLevel(textureSampler, uv, 0);
            if (col.a >= lut.alphaCutoff)
                rq.CommitNonOpaqueTriangleHit();
        }
    }

    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        result.instanceID = rq.CommittedRayInstanceCustomIndex();
        result.primitiveIndex = rq.CommittedPrimitiveIndex();
        result.barycentrics = rq.CommittedTriangleBarycentrics();
        result.committedRayT = rq.CommittedRayT();
        result.frontFace = rq.CommittedTriangleFrontFace();
    }
    return result;
}

// v1 NEE: uniform draw across the combined local+infinite light region (matches ReSTIR DI's own v1,
// before RIS/ReGIR were layered on top -- same upgrade path applies here later). A REAL shadow ray
// tests visibility here (unlike DI's initial-sampling stub), matching the reference's CalculateNEE.
void CalculateNEE(RAB_Surface surface, inout RTXDI_RandomSamplerState rng,
    out float3 radianceFromLights, out RTXDI_SampledLightData sampledLightData,
    out float neePdf, out float scatterPdf, out RAB_LightSample lightSample)
{
    radianceFromLights = float3(0.0, 0.0, 0.0);
    sampledLightData = RTXDI_SampledLightData_CreateInvalidData();
    neePdf = 0.0;
    scatterPdf = 0.0;
    lightSample = RAB_EmptyLightSample();

    RTXDI_LightBufferRegion region;
    region.firstLightIndex = g_PushConstant.firstLocalLightIndex;
    region.numLights = g_PushConstant.numLocalLights + g_PushConstant.numInfiniteLights;
    region.pad1 = 0;
    region.pad2 = 0;
    if (region.numLights == 0)
        return;

    float rnd = RTXDI_GetNextRandom(rng);
    RAB_LightInfo lightInfo;
    uint lightIndex;
    float invSourcePdf;
    RTXDI_RandomlySelectLightUniformly(rnd, region, lightInfo, lightIndex, invSourcePdf);
    if (invSourcePdf <= 0.0)
        return;

    lightSample = RAB_SamplePolymorphicLight(lightInfo, surface.worldPos);
    if (lightSample.solidAnglePdf <= 0.0)
        return;

    float3 L = lightSample.normal; // engine convention: always the surface-to-light direction
    float3 bsdfTimesNoL = RAB_SurfaceEvaluateBsdfTimesNoL(surface, L, false);
    if (all(bsdfTimesNoL <= 0.0))
        return;

    float3 visTestPos = (lightSample.lightType == 0)
        ? surface.worldPos + L * RAB_DISTANT_LIGHT_DISTANCE
        : lightSample.position;
    if (!RAB_GetConservativeVisibility(surface, visTestPos))
        return;

    radianceFromLights = lightSample.radiance * bsdfTimesNoL;
    RTXDI_SampledLightData_SetLightData(sampledLightData, lightIndex);
    sampledLightData.uvData = 0;
    neePdf = 1.0 / invSourcePdf;
    RTXDI_BrdfRaySampleProperties continuousProps = RTXDI_DefaultBrdfRaySampleProperties();
    scatterPdf = RAB_SurfaceEvaluateBsdfPdf(surface, L, continuousProps);
}

RTXDI_PTReconnectionParameters GetReconnectionParams()
{
    RTXDI_PTReconnectionParameters p = (RTXDI_PTReconnectionParameters)0;
    p.minConnectionFootprint = 0.02f;
    p.minConnectionFootprintSigma = 0.2f;
    p.minPdfRoughness = 0.1f;
    p.minPdfRoughnessSigma = 0.01f;
    p.roughnessThreshold = g_PushConstant.roughnessThreshold;
    p.distanceThreshold = g_PushConstant.distanceThreshold;
    p.reconnectionMode = RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT;
    return p;
}

// Ported from Rtxdi/PT/InitialSampling.hlsli (a plain POD struct + factory, no mutation issue -- ported
// here only because we're not including that file, per the SLANG COMPAT notice above).
struct RTXDI_PTInitialSamplingRuntimeParameters
{
    float3 cameraPos;
    float3 prevCameraPos;
    float3 prevPrevCameraPos;
};

RTXDI_PTInitialSamplingRuntimeParameters RTXDI_EmptyPTInitialSamplingRuntimeParameters()
{
    return (RTXDI_PTInitialSamplingRuntimeParameters)0;
}

RTXDI_PathTracerContextParameters GetPTContextParams(
    RTXDI_PTInitialSamplingParameters initialSamplingParams,
    RTXDI_PTInitialSamplingRuntimeParameters isrParams,
    RTXDI_PTReconnectionParameters rcParams)
{
    RTXDI_PathTracerContextParameters ctxParams = (RTXDI_PathTracerContextParameters)0;
    ctxParams.maxBounces = (uint16_t)initialSamplingParams.maxBounceDepth;
    ctxParams.maxRcVertexLength = (uint16_t)initialSamplingParams.maxRcVertexLength;
    ctxParams.rcParams = rcParams;
    ctxParams.rcrParams.cameraPos = isrParams.cameraPos;
    ctxParams.rcrParams.prevCameraPos = isrParams.prevCameraPos;
    ctxParams.rcrParams.prevPrevCameraPos = isrParams.prevPrevCameraPos;
    ctxParams.rcrParams.isPrevFrame = false;
    return ctxParams;
}

// Ported verbatim from Rtxdi/PT/InitialSampling.hlsli's InitializePTReservoirFromPTContext / Finalize
// Resampling / GenerateInitialSamples, adapted only to call RTXDI_InitialSamplingPathTracerContext's
// methods directly instead of through the templated RTXDI_PathTracerContext<T> wrapper (see the include
// comment above) -- the resampling math itself (RIS streaming across numInitialSamples full paths,
// FinalizeResampling's normalization) is completely unchanged from the reference.
RTXDI_PTReservoir InitializePTReservoirFromPTContext(RTXDI_InitialSamplingPathTracerContext ctx, RTXDI_RandomSamplerState cachedReplayRng)
{
    RTXDI_PTReservoir reservoir = RTXDI_MakePTReservoir(
        ctx.GetSelectedTargetFunction(),
        cachedReplayRng.seed,
        cachedReplayRng.index,
        ctx.GetSelectedRcVertexLength(),
        ctx.GetSelectedPathLength(),
        ctx.GetSelectedPartialJacobian(),
        ctx.GetSelectedRcWiPdf(),
        RAB_GetSurfaceWorldPos(ctx.selectedRcSurface),
        RAB_GetSurfaceNormal(ctx.selectedRcSurface),
        ctx.GetRadiance(),
        0.0);

    const float pHat = RTXDI_Luminance(ctx.GetSelectedTargetFunction());
    reservoir.weightSum = (pHat == 0.0) ? 0.0 : (ctx.runningWeightSum / pHat);
    return reservoir;
}

void FinalizeResampling(inout RTXDI_PTReservoir selectedReservoir, float3 selectedTargetFunction)
{
    const float normalizationNumerator = 1.0;
    const float normalizationDenominator = RTXDI_Luminance(selectedTargetFunction) * selectedReservoir.M;
    RTXDI_FinalizePTResampling(selectedReservoir, normalizationNumerator, normalizationDenominator);
}

RTXDI_PTReservoir GenerateInitialSamples(RTXDI_PTInitialSamplingParameters initialSamplingParams,
    RTXDI_PTInitialSamplingRuntimeParameters isrParams,
    RTXDI_PTReconnectionParameters rcParams,
    RTXDI_PathTracerRandomContext ptRandContext,
    RAB_Surface surface,
    inout RAB_PathTracerUserData ptud)
{
    RTXDI_PTReservoir selectedReservoir = RTXDI_EmptyPTReservoir();
    RAB_PathTracerUserDataSetPathType(ptud, RTXDI_PTPathTraceInvocationType_Initial);
    float3 selectedTargetFunction = float3(0.0, 0.0, 0.0);

    for (int i = 0; i < initialSamplingParams.numInitialSamples; i++)
    {
        RTXDI_RandomSamplerState cachedRrRNG = ptRandContext.replayRandomSamplerState;

        RTXDI_PathTracerContextParameters ctxParams = GetPTContextParams(initialSamplingParams, isrParams, rcParams);
        RTXDI_InitialSamplingPathTracerContext ctx = (RTXDI_InitialSamplingPathTracerContext)0;
        ctx.Init(ctxParams, surface, ptRandContext);

        RAB_PathTrace(ctx, ptRandContext, ptud);

        RTXDI_PTReservoir candidateReservoir = InitializePTReservoirFromPTContext(ctx, cachedRrRNG);

        if (RTXDI_CombinePTReservoirs(selectedReservoir, candidateReservoir, RTXDI_GetNextRandom(ptRandContext.initialRandomSamplerState), ctx.GetSelectedTargetFunction()))
        {
            selectedTargetFunction = ctx.GetSelectedTargetFunction();
        }
    }

    FinalizeResampling(selectedReservoir, selectedTargetFunction);
    return selectedReservoir;
}

// =========================================================================
// RAB_PathTrace -- our bounce loop, instrumented with SDK context calls at each step (ported/adapted
// from the real reference RAB_PathTracer.hlsli). v1 scope: emissive-hit sampling + uniform-NEE sampling
// (no MIS weighting between the two -- both stream independently into the same path reservoir via RIS,
// which is still statistically valid, just without variance-optimal MIS weights), no PSR/debug-viz/
// extra-mirror-bounce-budget/min-throughput-cutoff (all real reference features, all disclosed
// omissions rather than approximations of the core resampling algorithm itself).
// =========================================================================

void RAB_PathTrace(inout RTXDI_InitialSamplingPathTracerContext ctx,
    inout RTXDI_PathTracerRandomContext ptRandContext, inout RAB_PathTracerUserData ptud)
{
    while (ctx.GetBounceDepth() <= ctx.GetMaxPathBounce())
    {
        ctx.BeginPathState();

        RAB_Surface surface = ctx.GetIntersectionSurface();
        float3 outDir;
        RTXDI_BrdfRaySampleProperties brsp;
        bool validSample = RAB_SurfaceImportanceSampleBsdf(surface, ptRandContext.replayRandomSamplerState, outDir, brsp);

        RTXDI_BrdfRaySample brs = RTXDI_EmptyBrdfRaySample();
        if (validSample)
        {
            brs.outDirection = outDir;
            brs.properties = brsp;
            brs.outPdf = RAB_SurfaceEvaluateBsdfPdf(surface, outDir, brsp);
            brs.brdfTimesNoL = RAB_SurfaceEvaluateBsdfTimesNoL(surface, outDir, brsp.IsDelta());
        }
        ctx.SetBrdfRaySample(brs);

        if (!ctx.ValidContinuationRayBrdfOverPdf())
            break;

        ctx.MultiplyPathThroughput(ctx.GetContinuationRayBrdfOverPdf());

        if (ctx.ShouldRunRussianRoulette())
        {
            float contProb = clamp(RTXDI_Luminance(ctx.GetPathThroughput()), 0.05, 0.95);
            if (RTXDI_GetNextRandom(ptRandContext.initialRandomSamplerState) > contProb)
                break;
            ctx.MultiplyPathThroughput(1.0 / contProb);
            ctx.RecordRussianRouletteProbability(contProb);
        }

        ctx.SetContinuationRay(setupContinuationRay(RAB_GetSurfaceGeoNormal(surface), RAB_GetSurfaceWorldPos(surface), brs.outDirection));

        if (!ctx.AnalyzePathReconnectibilityBeforeTrace())
            break;

        RAB_Surface prevSurface = surface;
        RayDesc continuationRay = ctx.GetContinuationRay();
        ctx.SetTraceResult(TraceNextBounce(continuationRay));

        if (RAB_IsValidHit(ctx.GetTraceResult()))
        {
            RAB_Surface hitSurface = LoadSurfaceFromRayPayload(ctx.GetTraceResult(), continuationRay.Origin, continuationRay.Direction);
            ctx.RecordPathIntersection(hitSurface);

            if (ctx.IsPathTerminated())
                break;

            if (ctx.ShouldSampleEmissiveSurfaces())
            {
                float3 emissive = RAB_RayPayloadIsFrontFace(ctx.GetTraceResult())
                    ? RAB_GetEmissiveColor(RAB_GetMaterial(ctx.GetIntersectionSurface()))
                    : float3(0.0, 0.0, 0.0);
                if (any(emissive > 0.0))
                    ctx.RecordEmissiveLightSample(emissive, prevSurface, ptRandContext.initialRandomSamplerState);
            }

            if (ctx.ShouldSampleNee() && g_PushConstant.numNeeSamples > 0 &&
                RAB_GetSurfaceRoughness(ctx.GetIntersectionSurface()) >= kMinRoughness)
            {
                for (uint neeIdx = 0; neeIdx < g_PushConstant.numNeeSamples; neeIdx++)
                {
                    float3 radianceFromLights;
                    RTXDI_SampledLightData sampledLightData;
                    float neePdf, scatterPdf;
                    RAB_LightSample lightSample;
                    CalculateNEE(ctx.GetIntersectionSurface(), ptRandContext.initialRandomSamplerState,
                        radianceFromLights, sampledLightData, neePdf, scatterPdf, lightSample);

                    if (neePdf != 0.0)
                        ctx.RecordNeeLightSample(sampledLightData, radianceFromLights, neePdf, scatterPdf, lightSample, ptRandContext.initialRandomSamplerState);
                }
            }
        }
        else
        {
            ctx.RecordPathRadianceMiss(ptRandContext.initialRandomSamplerState);

            if (g_PushConstant.skyboxTextureIndex != 0xFFFFFFFF)
            {
                TextureCube skyTex = TextureCube.Handle(uint2(g_PushConstant.skyboxTextureIndex, 0));
                SamplerState skySamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));
                float3 envRadiance = skyTex.SampleLevel(skySamp, continuationRay.Direction, 0.0).rgb;
                ctx.RecordEnvironmentMapLightSample(envRadiance, prevSurface, ptRandContext.initialRandomSamplerState);
            }
            break;
        }

        ctx.IncreaseBounceDepth();
    }
}

// =========================================================================
// Dispatch entry point -- full-screen mesh-shader triangle, matching every other RTXDI pass in this
// engine. Primary surface comes from the existing G-buffer (cheap, already rasterized); the bounce
// loop above ray-traces everything from there onward.
// =========================================================================

struct ReSTIRPTInitialPayload
{
    uint dummy;
};
groupshared ReSTIRPTInitialPayload gInitialPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gInitialPayload);
}

struct MSOutput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float2 positions[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

static const float2 uvs[3] = {
    float2(0.0, 0.0),
    float2(2.0, 0.0),
    float2(0.0, 2.0)
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(3, 1, 1)]
void meshMain(
    out indices uint3 triangles[1],
    out vertices MSOutput vertices[3],
    uint threadID : SV_GroupIndex,
    uniform PushConstantReSTIRPTInitial pushConstant
)
{
    SetMeshOutputCounts(3, 1);
    if (threadID < 3)
    {
        vertices[threadID].pos = float4(positions[threadID], 0.0, 1.0);
        vertices[threadID].uv = uvs[threadID];
    }
    if (threadID == 0)
    {
        triangles[0] = uint3(0, 1, 2);
    }
}

RAB_Surface RAB_GetGBufferSurface(int2 pixelPos)
{
    RAB_Surface s = RAB_EmptySurface();
    if (pixelPos.x < 0 || pixelPos.y < 0 ||
        pixelPos.x >= int(g_PushConstant.viewportSize.x) || pixelPos.y >= int(g_PushConstant.viewportSize.y))
        return s;

    if (g_PushConstant.depthTextureIndex == 0xFFFFFFFF || g_PushConstant.gbufferNormalIndex == 0xFFFFFFFF ||
        g_PushConstant.gbufferAlbedoIndex == 0xFFFFFFFF || g_PushConstant.gbufferMaterialIndex == 0xFFFFFFFF)
        return s;

    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(g_UBO->imageHeapIndexOffset + g_PushConstant.depthTextureIndex, 0));
    float rawDepth = depthTex.Load(int3(pixelPos, 0)).r;
    if (rawDepth <= 0.00001)
        return s;

    Texture2D normalTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + g_PushConstant.gbufferNormalIndex, 0));
    float4 normalSample = normalTex.Load(int3(pixelPos, 0));
    float lenSqN = dot(normalSample.rgb, normalSample.rgb);
    if (lenSqN < 1e-6)
        return s;
    s.normal = normalSample.rgb * rsqrt(lenSqN);
    s.geoNormal = s.normal; // G-buffer doesn't track a separate geometric normal

    float2 uv = (float2(pixelPos) + 0.5) / g_PushConstant.viewportSize;
    float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 ndc = float4(ndc_xy, rawDepth, 1.0);
    float4 worldPosH = mul(g_PushConstant.invViewProj, ndc);
    s.worldPos = worldPosH.xyz / worldPosH.w;
    s.viewDir = normalize(g_PushConstant.cameraWorldPos.xyz - s.worldPos);
    s.viewDepth = length(s.worldPos - g_PushConstant.cameraWorldPos.xyz);

    Texture2D albedoTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + g_PushConstant.gbufferAlbedoIndex, 0));
    Texture2D materialTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + g_PushConstant.gbufferMaterialIndex, 0));
    float4 albedoSample = albedoTex.Load(int3(pixelPos, 0));
    float4 materialSample = materialTex.Load(int3(pixelPos, 0));

    float roughness = clamp(materialSample.r, kMinRoughness, 1.0);
    float metallic = clamp(materialSample.g, 0.0, 1.0);
    float3 baseColor = albedoSample.rgb;

    s.material.diffuseAlbedo = baseColor * (1.0 - metallic);
    s.material.specularF0 = lerp(float3(0.04), baseColor, metallic);
    s.material.roughness = roughness;
    s.material.emissiveColor = float3(0.0, 0.0, 0.0); // primary-surface emissive already handled by the deferred composite
    s.diffuseProbability = GetSurfaceDiffuseProbability(s);
    s.valid = true;
    return s;
}

struct PSOutput
{
    float4 debugOut : SV_Target0;
    float4 primaryDirect : SV_Target1;
};

[shader("fragment")]
PSOutput fragMain(
    MSOutput input,
    uniform PushConstantReSTIRPTInitial pushConstant
)
{
    PSOutput outVal;
    outVal.debugOut = float4(0.0, 0.0, 0.0, 1.0);
    outVal.primaryDirect = float4(0.0, 0.0, 0.0, 1.0);

    uint2 pixelCoord = uint2(input.pos.xy);
    if (pixelCoord.x >= uint(pushConstant.viewportSize.x) || pixelCoord.y >= uint(pushConstant.viewportSize.y))
        return outVal;

    if (pushConstant.matrixReference == 0 || pushConstant.lightDataReference == 0 ||
        pushConstant.reservoirBufferReference == 0)
        return outVal;

    g_PushConstant = pushConstant;
    g_UBO = (UniformBufferObject*)pushConstant.matrixReference;
    g_LightData = (LightData*)pushConstant.lightDataReference;
    g_InstanceLUT = (GpuRayTracingInstance*)g_UBO->sceneRayTracingInstancesReference;
    g_ReservoirBuffer = (RTXDI_PackedPTReservoir*)pushConstant.reservoirBufferReference;
    g_PreservedReservoirBuffer = (RTXDI_PackedPTReservoir*)pushConstant.preservedReservoirReference;

    if (g_UBO->tlasDeviceAddress == 0)
        return outVal;

    RTXDI_ReservoirBufferParameters resParams;
    resParams.reservoirBlockRowPitch = pushConstant.reservoirBlockRowPitch;
    resParams.reservoirArrayPitch = pushConstant.reservoirArrayPitch;
    resParams.pad1 = 0;
    resParams.pad2 = 0;

    RAB_Surface primarySurface = RAB_GetGBufferSurface(int2(pixelCoord));
    if (!RAB_IsSurfaceValid(primarySurface))
    {
        // Primary ray hit nothing (sky) -- there's no surface to bounce a path from, but the pixel
        // still needs to show the skybox instead of black. Matches Skybox.slang's own convention
        // (unproject the NEAR plane, depth=1.0 in this engine's Reverse-Z, to get the world-space ray
        // direction; TextureCube handles are NOT offset by imageHeapIndexOffset, same as the miss
        // branch inside RAB_PathTrace below).
        RTXDI_PTReservoir emptyReservoir = RTXDI_EmptyPTReservoir();
        RTXDI_StorePTReservoir(emptyReservoir, resParams, pixelCoord, 0);
        if (pushConstant.preservedReservoirReference != 0)
            g_PreservedReservoirBuffer[RTXDI_ReservoirPositionToPointer(resParams, pixelCoord, 0)] = RTXDI_PackPTReservoir(emptyReservoir);
        if (pushConstant.skyboxTextureIndex != 0xFFFFFFFF)
        {
            float2 skyUv = (float2(pixelCoord) + 0.5) / pushConstant.viewportSize;
            float2 skyNdc = float2(skyUv.x * 2.0 - 1.0, 1.0 - skyUv.y * 2.0);
            float4 skyUnprojected = mul(pushConstant.invViewProj, float4(skyNdc, 1.0, 1.0));
            float3 skyRayDir = normalize(skyUnprojected.xyz / skyUnprojected.w - pushConstant.cameraWorldPos.xyz);

            TextureCube skyTex = TextureCube.Handle(uint2(pushConstant.skyboxTextureIndex, 0));
            SamplerState skySamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));
            float3 skyColor = skyTex.SampleLevel(skySamp, skyRayDir, 0.0).rgb;

            outVal.debugOut = float4(skyColor, 1.0);
            outVal.primaryDirect = float4(skyColor, 1.0);
        }
        return outVal;
    }

    // Named per-pass seed constants (not arbitrary numbers) -- these keep this pass's RNG stream
    // decorrelated from every other RTXDI pass's stream (DI/GI/PT each get their own disjoint seed
    // range in RandomSamplerPerPassSeeds.hlsli); matches the real FullSample's GenerateInitialSamples.hlsl.
    RTXDI_PathTracerRandomContext ptRandContext = RTXDI_InitializePathTracerRandomContext(pixelCoord, pushConstant.frameIndex, RTXDI_PT_GENERATE_INITIAL_SAMPLES_RANDOM_SEED, RTXDI_PT_GENERATE_INITIAL_SAMPLES_REPLAY_RANDOM_SEED);
    RAB_PathTracerUserData ptud = RAB_EmptyPathTracerUserData();

    // Bounce-1 (primary surface) direct lighting -- RAB_PathTrace below only ever resamples INDIRECT
    // light (bounce 2+), by the RTXDI SDK's own design (see RTXDI_PathTracerState's initialBounceDepth=2
    // comment in the SLANG COMPAT block above), so the primary surface's own direct lighting has to be
    // evaluated here separately, exactly like this engine's ReSTIR GI is already summed with
    // DeferredLighting's direct term. A plain averaged NEE estimate (same math as CalculateNEE's use
    // inside RAB_PathTrace) is sufficient here -- no RIS needed since there's only one surface to shade,
    // not many competing bounce candidates.
    if (pushConstant.numNeeSamples > 0)
    {
        float3 primaryDirectRadiance = float3(0.0, 0.0, 0.0);
        for (uint pns = 0; pns < pushConstant.numNeeSamples; pns++)
        {
            float3 radianceFromLights;
            RTXDI_SampledLightData sampledLightData;
            float neePdf, scatterPdf;
            RAB_LightSample lightSample;
            CalculateNEE(primarySurface, ptRandContext.initialRandomSamplerState,
                radianceFromLights, sampledLightData, neePdf, scatterPdf, lightSample);
            if (neePdf > 0.0)
                primaryDirectRadiance += radianceFromLights / neePdf;
        }
        outVal.primaryDirect = float4(primaryDirectRadiance / float(pushConstant.numNeeSamples), 1.0);
    }

    RTXDI_PTInitialSamplingParameters initialParams;
    initialParams.numInitialSamples = max(pushConstant.numInitialSamples, 1u);
    initialParams.maxBounceDepth = max(pushConstant.maxBounceDepth, 1u);
    initialParams.maxRcVertexLength = max(pushConstant.maxRcVertexLength, 1u);
    initialParams.pad0 = 0;

    RTXDI_PTInitialSamplingRuntimeParameters isrParams = RTXDI_EmptyPTInitialSamplingRuntimeParameters();
    isrParams.cameraPos = pushConstant.cameraWorldPos.xyz;
    isrParams.prevCameraPos = pushConstant.cameraWorldPos.xyz;
    isrParams.prevPrevCameraPos = pushConstant.cameraWorldPos.xyz;

    RTXDI_PTReservoir reservoir = GenerateInitialSamples(initialParams, isrParams, GetReconnectionParams(),
        ptRandContext, primarySurface, ptud);

    RTXDI_StorePTReservoir(reservoir, resParams, pixelCoord, 0);
    if (pushConstant.preservedReservoirReference != 0)
        g_PreservedReservoirBuffer[RTXDI_ReservoirPositionToPointer(resParams, pixelCoord, 0)] = RTXDI_PackPTReservoir(reservoir);

    if (RTXDI_IsValidPTReservoir(reservoir))
    {
        outVal.debugOut = float4(reservoir.radiance * reservoir.weightSum, 1.0);
    }

    return outVal;
}
