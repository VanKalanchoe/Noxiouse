#include "shaderIO.h"
#include "Rtxdi/PT/ReSTIRPTParameters.h"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

#ifndef RTXDI_RESERVOIR_BLOCK_SIZE
#define RTXDI_RESERVOIR_BLOCK_SIZE 16
#endif

// RTXDI_PTTemporalResampling only ever calls RTXDI_LoadPTReservoir/RTXDI_PT_RESERVOIR_BUFFER internally
// to fetch the HISTORY sample (see FindTemporalNeighbor) -- it receives the current candidate as a
// plain parameter we load ourselves. So the macro must point at the history buffer; the current buffer
// is handled manually below via direct pointer indexing + RTXDI_PackPTReservoir/UnpackPTReservoir,
// bypassing RTXDI_StorePTReservoir/RTXDI_LoadPTReservoir entirely for it. Exactly mirrors how
// ReSTIRDITemporal.slang binds RTXDI_LIGHT_RESERVOIR_BUFFER to the history buffer for the same reason
// (our 3 reservoir buffers are separate physical BDA-addressed objects, not regions within one shared
// buffer the way the vendored SDK's own reference usage assumes).
static RTXDI_PackedPTReservoir* g_HistoryReservoirBuffer;
#define RTXDI_PT_RESERVOIR_BUFFER g_HistoryReservoirBuffer

static PushConstantReSTIRPTTemporal g_PushConstant;
static UniformBufferObject* g_UBO;
static LightData* g_LightData;
static GpuRayTracingInstance* g_InstanceLUT;

// =========================================================================
// RAB_Surface / RAB_Material -- identical shape to ReSTIRPTInitial.slang's (this codebase's established
// convention of duplicating shared RAB_/SLANG-COMPAT infrastructure per entry-point file rather than
// sharing it across .slang files -- see that file's own header comment for the rationale).
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

// App-provided similarity heuristic (RTXDI leaves this to the application) -- used by temporal
// reprojection's ValidTemporalSurface to reject a reused sample when the underlying material changed
// too much (e.g. a disocclusion revealing different geometry at a similar depth/normal).
bool RAB_AreMaterialsSimilar(RAB_Material m1, RAB_Material m2)
{
    float lum1 = dot(m1.diffuseAlbedo, float3(0.2126, 0.7152, 0.0722));
    float lum2 = dot(m2.diffuseAlbedo, float3(0.2126, 0.7152, 0.0722));
    return abs(m1.roughness - m2.roughness) < 0.2 && abs(lum1 - lum2) < 0.25;
}

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
// BSDF functions -- identical to ReSTIRPTInitial.slang's, plus two new hooks Temporal resampling needs
// (RAB_SurfaceEvaluateBrdfPdf is dead code in this engine, see its own comment below; RAB_GetPTSampleTargetPdfForSurface
// is the same BSDF-times-NoL math as RAB_GetReflectedBsdfRadianceForSurface, just under PT's own hook name).
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

// App-provided hook used by ValidateInvertibilityCondition's
// RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT branch.
float RAB_SurfaceEvaluateBrdfPdf(RAB_Surface surface, float3 dir)
{
    RTXDI_BrdfRaySampleProperties continuousProps = RTXDI_DefaultBrdfRaySampleProperties();
    return RAB_SurfaceEvaluateBsdfPdf(surface, dir, continuousProps);
}

float3 RAB_GetReflectedBsdfRadianceForSurface(float3 incomingRadianceLocation, float3 incomingRadiance, RAB_Surface surface)
{
    float3 L = normalize(incomingRadianceLocation - surface.worldPos);
    return incomingRadiance * RAB_SurfaceEvaluateBsdfTimesNoL(surface, L, false);
}

// PT's own name for the same BSDF-times-NoL reflected-radiance computation above -- used by
// TemporalResampling.hlsli's EvaluateReconnection to re-evaluate a reused sample's contribution from
// the current pixel's surface.
float3 RAB_GetPTSampleTargetPdfForSurface(float3 samplePos, float3 sampleRadiance, RAB_Surface surface)
{
    return RAB_GetReflectedBsdfRadianceForSurface(samplePos, sampleRadiance, surface);
}

// =========================================================================
// RAB_LightInfo / RAB_LightSample -- identical to ReSTIRPTInitial.slang's, plus two overloads
// HybridShift's light-reconnection code needs (a Surface+UV sampler, and a LightSample-based visibility
// test) and RAB_LightSampleRadiance.
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

// Disclosed v1 simplification: our light buffer is rebuilt fresh every frame by iterating scene
// entities in stable ECS order (no dynamic add/remove mid-session in the common case), so the same
// index already refers to the same light across adjacent frames -- ignoring prevFrame here is safe
// under that assumption. A real "persistent light ID -> this frame's buffer slot" translation table
// would be needed to handle lights being added/removed between the two reconnected frames.
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

// Overload used by HybridShift's light reconnection (RTXDI_ComputeTargetFunctionWithLightPdf) -- our
// analytic point/spot/directional lights are sampled deterministically from position alone, so the
// surface/UV parameters (meant for area-light sampling) are simply unused.
RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo light, RAB_Surface surface, float2 uv)
{
    return RAB_SamplePolymorphicLight(light, surface.worldPos);
}

float3 RAB_LightSamplePosition(RAB_LightSample lightSample) { return lightSample.position; }
float RAB_LightSampleSolidAnglePdf(RAB_LightSample lightSample) { return lightSample.solidAnglePdf; }
float3 RAB_LightSampleRadiance(RAB_LightSample lightSample) { return lightSample.radiance; }

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

// Overload used by HybridShift's light reconnection -- extracts the shadow-ray target the same way
// CalculateNEE does in ReSTIRPTInitial.slang (directional lights test towards RAB_DISTANT_LIGHT_DISTANCE).
bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample lightSample)
{
    float3 visTestPos = (lightSample.lightType == 0)
        ? surface.worldPos + lightSample.normal * RAB_DISTANT_LIGHT_DISTANCE
        : lightSample.position;
    return RAB_GetConservativeVisibility(surface, visTestPos);
}

// Directional light for env-map-on-miss handling; distant enough to treat as "at infinity" for the
// path reconnection Jacobian math without special-casing it.
static const float RAB_DISTANT_LIGHT_DISTANCE = 1e4;

// =========================================================================
// RAB_RayPayload / RAB_PathTracerUserData -- identical to ReSTIRPTInitial.slang's.
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

struct RAB_PathTracerUserData
{
    uint pathType;
};

RAB_PathTracerUserData RAB_EmptyPathTracerUserData()
{
    RAB_PathTracerUserData d;
    d.pathType = RTXDI_PTPathTraceInvocationType_Temporal;
    return d;
}

void RAB_PathTracerUserDataSetPathType(inout RAB_PathTracerUserData ptud, uint pathType) { ptud.pathType = pathType; }

// No-op PSR/denoiser-guide-buffer hooks (RTXDI-provided extension points for DLSS-RR path-space
// regularization) -- this engine has no PSR support, matching ReSTIRPTInitial.slang's own disclosed
// v1 scope (no PSR/debug-viz/extra-mirror-bounce-budget).
void RAB_LastBounceDenoiserCallback(float3 lightPos, RAB_Surface surface, RAB_PathTracerUserData ptud) {}
void RAB_ReconnectionDenoiserCallback(RTXDI_PTReservoir neighborSample, RAB_Surface surface, RAB_PathTracerUserData ptud) {}

// v1 uses NEE as the sole light-sampling strategy (no BSDF-sampled-light MIS pairing, disclosed in
// ReSTIRPTInitial.slang's RAB_PathTrace header comment) -- weight 1.0 keeps light reconnection
// consistent with that same choice instead of introducing balance-heuristic MIS only here.
float RAB_GetMISWeightForNEE(uint lightIndex, RAB_LightSample lightSample, float3 lightDirection, float invSourcePdf, float scatterPdf)
{
    return 1.0;
}

// Disclosed v1 simplification: our light buffer is rebuilt fresh every frame by iterating scene
// entities in stable ECS order (no dynamic add/remove mid-session in the common case), so the same
// index already refers to the same light across adjacent frames -- identity is a safe default that
// only misbehaves if lights are added/removed between the two frames being reconnected. A real
// translation table (persistent light ID -> this frame's buffer slot) is unimplemented.
int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
    return int(lightIndex);
}

#include "Rtxdi/Utils/Math.hlsli"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"

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

// Ported verbatim from the real vendored file (Rtxdi/Utils/ReservoirAddressing.hlsli) -- that whole
// file is normally app-overridable (guarded by the same RTXDI_RESERVOIR_ADDRESSING_HLSLI macro defined
// above), and this one extra function lives in it alongside the addressing overrides, so it's copied
// here too rather than included (matches ReSTIRGITemporal.slang's identical approach).
void RTXDI_ApplyPermutationSampling(inout int2 prevPixelPos, uint uniformRandomNumber)
{
    int2 offset = int2(uniformRandomNumber & 3, (uniformRandomNumber >> 2) & 3);
    prevPixelPos += offset;
    prevPixelPos.x ^= 3;
    prevPixelPos.y ^= 3;
    prevPixelPos -= offset;
}

// =========================================================================
// SLANG COMPAT NOTICE (applies to everything down to the "end SLANG COMPAT" marker below): same
// reasoning as ReSTIRPTInitial.slang's own notice -- the vendored SDK is unmodifiable, and Slang
// requires [mutating] on self-mutating struct methods where HLSL/DXC didn't. RTXDI_PathTracerState is
// ported identically to the Initial file's copy (needed by both context types). RTXDI_HybridShiftPathTracerContext
// (PT/HybridShiftPathTracerContext.hlsli) is new here -- same struct in the real file already (not
// `class`), ported verbatim with ONLY [mutating] added.
// =========================================================================

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

// ---- RTXDI_HybridShiftPathTracerContext (real file: PT/HybridShiftPathTracerContext.hlsli) ----
#define RTXDI_HYBRID_SHIFT_PATH_TRACING_CONTEXT_HLSLI

struct RTXDI_HybridShiftPathTracerContext
{
    [mutating] void SetBrdfRaySample(RTXDI_BrdfRaySample raySample)
    {
        ptState.brdfRaySample = raySample;
        SetContinuationRayBrdfOverPdf(raySample.brdfTimesNoL / raySample.outPdf);
    }

    [mutating] void SetMaxPathBounce(uint16_t newMax)
    {
        ptState.maxPathBounce = newMax;
    }

    [mutating] void SetMaxRcVertexLengthIfUnset(uint16_t newMax)
    {
        // Not applicable to hybrid shift
    }

    bool ShouldRunRussianRoulette()
    {
        return false;
    }

    [mutating] void RecordRussianRouletteProbability(float rrProb)
    {
    }

    [mutating] void MultiplyPathThroughput(float3 multiplicationFactor)
    {
        ptState.pathThroughput *= multiplicationFactor;
    }

    [mutating] void SetContinuationRay(RayDesc continuationRay)
    {
        ptState.continuationRay = continuationRay;
    }

    // Record data before tracing the continuation ray
    [mutating] bool AnalyzePathReconnectibilityBeforeTrace()
    {
        if (ptState.IsLastVertexRough() && ptState.IsLastVertexFar())
        {
            bool isCurrentVertexRoughForConnection = false;
            if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FIXED_THRESHOLD)
            {
                isCurrentVertexRoughForConnection = RAB_GetSurfaceRoughness(ptState.intersectionSurface) > roughnessThreshold;
            }
            else if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT)
            {
                float inverseFootprint = RTXDI_CalculateRayFootprint(ptState.prevNormal, RAB_GetSurfaceViewDir(ptState.intersectionSurface), ptState.lastHitT, ptState.brdfRaySample);
                isCurrentVertexRoughForConnection = inverseFootprint > footprintThreshold;
            }

            if (isCurrentVertexRoughForConnection)
            {
                radiance = 0.f; // invalid shift
                ptState.SetPathTermination(true);
                return !IsPathTerminated();
            }
        }

        // mark the current vertex to be rough for next vertex's reconnection condition test
        // the current vertex is "last vertex" in the next vertex's eyes
        if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FIXED_THRESHOLD)
        {
            ptState.SetIsLastVertexRough(RAB_GetSurfaceRoughness(ptState.intersectionSurface) > roughnessThreshold);
        }
        else if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT)
        {
            ptState.SetIsLastVertexRough(ptState.brdfRaySample.outPdf <= pdfThreshold && !ptState.brdfRaySample.properties.IsDelta());
        }

        ptState.prevNormal = RAB_GetSurfaceNormal(ptState.intersectionSurface);

        return !IsPathTerminated();
    }

    [mutating] void SetTraceResult(RAB_RayPayload rp)
    {
        ptState.traceResult = rp;
    }

    [mutating] void RecordPathIntersection(const RAB_Surface intersectionSurface)
    {
        ptState.intersectionSurface = intersectionSurface;
        RAB_SetSurfaceNormal(ptState.intersectionSurface, normalize(RAB_GetSurfaceNormal(ptState.intersectionSurface)));

        if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FIXED_THRESHOLD)
        {
            ptState.SetIsLastVertexFar(RAB_RayPayloadGetCommittedHitT(ptState.traceResult) > distanceThreshold && !ptState.brdfRaySample.properties.IsDelta());
        }
        else if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT)
        {
            const float rayFootprint = RTXDI_CalculateRayFootprint(RAB_GetSurfaceNormal(ptState.intersectionSurface), RAB_GetSurfaceViewDir(ptState.intersectionSurface), RAB_RayPayloadGetCommittedHitT(ptState.traceResult), ptState.brdfRaySample);
            ptState.SetIsLastVertexFar(rayFootprint > footprintThreshold);
        }

        // Check invertibility conditions since we have arrived at the last vertex in random replay
        // also populate some variables since we will about to terminate
        if (ptState.bounceDepth == ptState.maxPathBounce)
        {
            if ((ptState.IsLastVertexRough() || (replayRcVertexLength > selectedPathLength)) && ptState.IsLastVertexFar())
            {
                // case 1
                if (replayRcVertexLength <= selectedPathLength)
                {
                    if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FIXED_THRESHOLD)
                    {
                        if (ptState.IsLastVertexFar() && ptState.IsLastVertexRough() && RAB_GetSurfaceRoughness(intersectionSurface) > roughnessThreshold)
                        {
                            radiance = float3(0.0f, 0.0f, 0.0f);
                            ptState.SetPathTermination(true);
                            return;
                        }
                    }
                    else if (reconnectionMode == RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT)
                    {
                        checkPreRcInverseGeoTerm = RAB_RayPayloadGetCommittedHitT(ptState.traceResult) * RAB_RayPayloadGetCommittedHitT(ptState.traceResult) / abs(dot(ptState.prevNormal, ptState.continuationRay.Direction));
                    }
                }
                else // case 2. This is non-invertible
                {
                    radiance = float3(0.0f, 0.0f, 0.0f);
                    ptState.SetPathTermination(true);
                    return;
                }
            }

            // If the path could be invertible, we store IntersectionSurface of the current vertex, which is the vertex before the rcVertex
            if (replayRcVertexLength <= selectedPathLength)
            {
                rcPrevSurface = ptState.intersectionSurface;
                radiance = ptState.pathThroughput; // partial path throughput

                ptState.SetPathTermination(true);
                return;
            }
        }
    }

    bool ShouldSampleEmissiveSurfaces()
    {
        // See RecordPathIntersection and RecordEmissiveLightSample
        return ptState.bounceDepth == ptState.maxPathBounce;
    }

    [mutating] bool RecordEmissiveLightSample(float3 radianceFromEmissiveSurface, RAB_Surface prevSurface, inout RTXDI_RandomSamplerState rng)
    {
        if (ptState.bounceDepth == ptState.maxPathBounce)
        {
            float3 Le = radianceFromEmissiveSurface;
            Le = (ptState.bounceDepth == selectedPathLength) ? Le : 0.f;

            radiance += Le * ptState.pathThroughput;
        }
        return true;
    }

    bool ShouldSampleNee()
    {
        return false;
    }

    bool RecordNeeLightSample(in const RTXDI_SampledLightData sampledLightData,
                              in const float3 radianceFromLights,
                              in const float neePdf,
                              in const float scatterPdf,
                              in const RAB_LightSample lightSample,
                              inout RTXDI_RandomSamplerState rng)
    {
        return false;
    }

    [mutating] void RecordPathRadianceMiss(inout RTXDI_RandomSamplerState rng)
    {
        ptState.SetPathTermination(true);
    }

    [mutating] bool RecordEnvironmentMapLightSample(const float3 environmentMapRadiance,
                                         RAB_Surface prevSurface,
                                         inout RTXDI_RandomSamplerState rng)
    {
        const bool bSecondaryBounce = ptState.IsSecondaryBounce();
        if (ptState.bounceDepth == selectedPathLength && !ptState.IsLastVertexRough() && !bSecondaryBounce)
        {
            float3 Le = environmentMapRadiance;
            radiance += Le * ptState.pathThroughput;
        }
        return true;
    }

    bool IsPathTerminated()
    {
        return ptState.IsPathTermination();
    }

    [mutating] void Init(const RTXDI_PathTracerContextParameters ptParams, const RAB_Surface primarySurface, inout RTXDI_PathTracerRandomContext ptRandContext)
    {
        ptState.initialBounceDepth = 2;
        ptState.bounceDepth = ptState.initialBounceDepth;
        ptState.maxPathBounce = (uint16_t)min(ptParams.rcVertexLength - 1, ptParams.selectedPathLength);
        ptState.prevNormal = float3(0.0f, 0.0f, 0.0f);
        ptState.pathThroughput = float3(1.0f, 1.0f, 1.0f);
        ptState.intersectionSurface = primarySurface;

        reconnectionMode = ptParams.rcParams.reconnectionMode;
        RTXDI_CalculateFootprintParameters(ptParams.rcParams, ptParams.rcrParams, primarySurface, ptRandContext, footprintThreshold, pdfThreshold);
        distanceThreshold = ptParams.rcParams.distanceThreshold;
        roughnessThreshold = ptParams.rcParams.roughnessThreshold;

        radiance = float3(0.0f, 0.0f, 0.0f);
        rcPrevSurface = RAB_EmptySurface();
    }

    //
    // Hybrid-shift specific functions
    //

    float GetCheckPreRcInverseGeoTerm()
    {
        return checkPreRcInverseGeoTerm;
    }

    //
    // Unused interface functions (i.e. InitialSampling version functions)
    //

    float CalculateRISWeight(float3 targetFunctionOverP)
    {
        return 0.0;
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
        return false;
    }

    RTXDI_PTReconnectionMode GetReconnectionMode()
    {
        return reconnectionMode;
    }

    float GetRoughnessThreshold()
    {
        return roughnessThreshold;
    }

    float GetDistanceThreshold()
    {
        return distanceThreshold;
    }

    float GetPdfThreshold()
    {
        return pdfThreshold;
    }

    float GetFootprintThreshold()
    {
        return footprintThreshold;
    }

    //
    // Hybrid-Shift only functions
    //

    RAB_Surface GetRcPrevSurface()
    {
        return rcPrevSurface;
    }

    [mutating] void SetRcVertexLength(uint length)
    {
        replayRcVertexLength = length;
    }

    [mutating] void SetSelectedPathLength(uint length)
    {
        selectedPathLength = length;
    }

    float3 GetSelectedTargetFunction()
    {
        return float3(0.0, 0.0, 0.0);
    }

    float3 GetRadiance()
    {
        return radiance;
    }

    uint GetBounceDepth()
    {
        return ptState.bounceDepth;
    }

    uint GetMaxPathBounce()
    {
        return ptState.maxPathBounce;
    }

    RTXDI_BrdfRaySample GetBrdfRaySample()
    {
        return ptState.brdfRaySample;
    }

    RAB_Surface GetIntersectionSurface()
    {
        return ptState.intersectionSurface;
    }

    float3 GetContinuationRayBrdfOverPdf()
    {
        return ptState.continuationRayBrdfOverPdf;
    }

    [mutating] void SetContinuationRayBrdfOverPdf(float3 brdfOverPdf)
    {
        ptState.continuationRayBrdfOverPdf = brdfOverPdf;
    }

    RayDesc GetContinuationRay()
    {
        return ptState.continuationRay;
    }

    float3 GetPathThroughput()
    {
        return ptState.pathThroughput;
    }

    RAB_RayPayload GetTraceResult()
    {
        return ptState.traceResult;
    }

    [mutating] void IncreaseBounceDepth()
    {
        ptState.bounceDepth++;
    }

    bool ValidContinuationRayBrdfOverPdf()
    {
        float lum = RTXDI_Luminance(ptState.continuationRayBrdfOverPdf);
        return lum > 0 && !isinf(lum) && !isnan(lum);
    }

    uint16_t GetRcVertexLength()
    {
        return ptState.rcVertexLength;
    }

    bool IsSecondaryBounce()
    {
        return ptState.IsSecondaryBounce();
    }

    [mutating] void BeginPathState()
    {
        ptState.BeginPathState();
    }

    [mutating] void SetPathTermination(bool value)
    {
        ptState.SetPathTermination(value);
    }

    bool IsPathTermination()
    {
        return ptState.IsPathTermination();
    }

    [mutating] void SetIsLastVertexFar(bool value)
    {
        ptState.SetIsLastVertexFar(value);
    }

    bool IsLastVertexFar()
    {
        return ptState.IsLastVertexFar();
    }

    [mutating] void SetIsLastVertexRough(bool value)
    {
        ptState.SetIsLastVertexRough(value);
    }

    bool IsLastVertexRough()
    {
        return ptState.IsLastVertexRough();
    }

    bool NoMainPathRcVertex()
    {
        return ptState.NoMainPathRcVertex();
    }

    // Totally radiance carried from the path tree to the primary intersection divided by
    // the pdfs of producing the path tree
    float3 radiance;

    uint selectedPathLength;

    uint replayRcVertexLength;

    // Secondary intersection
    RAB_Surface rcPrevSurface;

    float footprintThreshold;
    float pdfThreshold;

    float checkPreRcInverseGeoTerm;

    RAB_Surface selectedRcSurface;
    uint selectedRcVertexLength;
    float selectedRcWiPdf;
    float3 selectedRcWiTargetFunction;
    float runningWeightSum;
    float selectedPartialJacobian;

    RTXDI_PTReconnectionMode reconnectionMode;
    float distanceThreshold;
    float roughnessThreshold;

    RTXDI_PathTracerState ptState;
};

// ---- end SLANG COMPAT ----

#include "Rtxdi/PT/Reservoir.hlsli"

// =========================================================================
// Secondary-hit surface construction -- identical to ReSTIRPTInitial.slang's (needed here for
// RandomReplay's re-tracing of bounces through the scene).
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

float GetSurfaceDiffuseProbability(RAB_Surface surface)
{
    float diffuseWeight = dot(surface.material.diffuseAlbedo, float3(0.2126, 0.7152, 0.0722));
    float specularWeight = dot(fresnel_schlick(surface.material.specularF0, saturate(dot(surface.viewDir, surface.normal))), float3(0.2126, 0.7152, 0.0722));
    float sumWeights = diffuseWeight + specularWeight;
    return sumWeights < 1e-7 ? 1.0 : (diffuseWeight / sumWeights);
}

RAB_Surface LoadSurfaceFromRayPayload(RAB_RayPayload rp, float3 rayOrigin, float3 rayDir)
{
    RAB_Surface surface = RAB_EmptySurface();

    GpuRayTracingInstance lut = g_InstanceLUT[rp.instanceID];
    uint32_t* indices = (uint32_t*)lut.indexBufferAddress;
    uint i0 = indices[rp.primitiveIndex * 3 + 0];
    uint i1 = indices[rp.primitiveIndex * 3 + 1];
    uint i2 = indices[rp.primitiveIndex * 3 + 2];
    Vertex vertex0 = LoadRayTracingVertex(lut, i0);
    Vertex vertex1 = LoadRayTracingVertex(lut, i1);
    Vertex vertex2 = LoadRayTracingVertex(lut, i2);

    float w0 = 1.0 - rp.barycentrics.x - rp.barycentrics.y;
    float w1 = rp.barycentrics.x;
    float w2 = rp.barycentrics.y;
    float2 hitUV = w0 * vertex0.uv0 + w1 * vertex1.uv0 + w2 * vertex2.uv0;

    float3 localNormal = normalize(w0 * vertex0.normal + w1 * vertex1.normal + w2 * vertex2.normal);
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

    hitNormal = perturb_normal(lut, vertex0, vertex1, vertex2, hitNormal, hitUV, textureSampler);

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

// =========================================================================
// RAB_PathTrace overload for RTXDI_HybridShiftPathTracerContext -- same bounce-loop STRUCTURE as
// ReSTIRPTInitial.slang's RAB_PathTrace (ported from the same reference RAB_PathTracer.hlsli), but the
// NEE block is omitted entirely: HybridShiftPathTracerContext.ShouldSampleNee() always returns false,
// so that block would never execute -- omitting it is behaviorally identical, not an approximation.
// Slang resolves this as a normal overload (by concrete parameter type), avoiding the templated
// RTXDI_PathTracerContext<T> wrapper Slang can't parse (see ReSTIRPTInitial.slang's SLANG COMPAT notice).
// =========================================================================

void RAB_PathTrace(inout RTXDI_HybridShiftPathTracerContext ctx,
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
// HybridShift -- ported from Rtxdi/PT/HybridShift.hlsli. The generic `RTXDI_PathTracerContext<RTXDI_HybridShiftPathTracerContext>`
// wrapper type used throughout the real file is replaced with the concrete `RTXDI_HybridShiftPathTracerContext`
// directly (same reasoning as ReSTIRPTInitial.slang: the wrapper is a pure pass-through, so calling the
// concrete type's methods is behaviorally identical). `InitializePathTracerContext` (real file's own
// name) below directly constructs+Inits the concrete type instead of going through the generic
// `RTXDI_InitializePathTracerContext<T>` factory (avoided for the same reason).
// =========================================================================

struct RTXDI_PTHybridShiftRuntimeParameters
{
    bool isPrevFrame;
    bool isBasePathInPrevFrame;

    float3 cameraPos;
    float3 prevCameraPos;
    float3 prevPrevCameraPos;
};

RTXDI_HybridShiftPathTracerContext InitializePathTracerContext(const RTXDI_PTHybridShiftPerFrameParameters hspfParams,
                                                      const RTXDI_PTHybridShiftRuntimeParameters hsrParams,
                                                      const RTXDI_PTReconnectionParameters rcParams,
                                                      const RTXDI_PTReservoir neighborSample,
                                                      const RAB_Surface surfaceForResampling,
                                                      inout RTXDI_PathTracerRandomContext ptRandContext)
{
    RTXDI_PathTracerContextParameters ptParams = (RTXDI_PathTracerContextParameters) 0;
    ptParams.maxBounces = (uint16_t) hspfParams.maxBounceDepth;
    ptParams.maxRcVertexLength = (uint16_t) hspfParams.maxRcVertexLength;
    ptParams.rcVertexLength = (uint16_t) neighborSample.rcVertexLength;
    ptParams.selectedPathLength = (uint16_t) neighborSample.pathLength;
    ptParams.rcParams = rcParams;
    ptParams.rcrParams.isPrevFrame = hsrParams.isPrevFrame;
    ptParams.rcrParams.cameraPos = hsrParams.cameraPos;
    ptParams.rcrParams.prevCameraPos = hsrParams.prevCameraPos;
    ptParams.rcrParams.prevPrevCameraPos = hsrParams.prevPrevCameraPos;

    RTXDI_HybridShiftPathTracerContext ctx = (RTXDI_HybridShiftPathTracerContext)0;
    ctx.Init(ptParams, surfaceForResampling, ptRandContext);

    return ctx;
}

bool NeedToRunRandomReplayPathTracer(const RTXDI_PTReservoir neighborSample)
{
    return neighborSample.rcVertexLength > 2;
}

void RandomReplay(inout RTXDI_HybridShiftPathTracerContext ctx,
                  const RTXDI_PTReservoir neighborSample,
                  inout RAB_Surface surfaceForResampling,
                  inout RTXDI_PathTracerRandomContext ptRandContext,
                  inout float3 targetFunction,
                  inout RAB_PathTracerUserData ptud)
{
    // set the RcVertexLength and the path length here
    // such that we know to which bounce we need to replay our path to
    ctx.SetRcVertexLength(neighborSample.rcVertexLength);
    ctx.SetSelectedPathLength(neighborSample.pathLength);

    RAB_PathTrace(ctx, ptRandContext, ptud);

    // surfaceForResampling is now the surface of the vertex before rcVertex
    surfaceForResampling = ctx.GetRcPrevSurface();

    // This is now the "prefix" throughput
    targetFunction = ctx.GetRadiance();
}

void CalculatePartialJacobian(const float3 receiverPos,
                              const float3 samplePos,
                              const float3 sampleNormal,
                              inout float distanceToSurfaceSqr,
                              inout float cosineEmissionAngle)
{
    const float3 vec = receiverPos - samplePos;

    distanceToSurfaceSqr = dot(vec, vec);
    cosineEmissionAngle = saturate(dot(sampleNormal, vec * rsqrt(distanceToSurfaceSqr)));
}

float CalculateJacobianWithCachedJacobian(const float3 receiverPos,
                                          inout RTXDI_PTReservoir neighborReservoir)
{
    const float originalInversePartialJacobian = neighborReservoir.partialJacobian;
    float newDistanceSqr = 0.0f;
    float newCosine = 0.0f;
    CalculatePartialJacobian(receiverPos, neighborReservoir.translatedWorldPosition, neighborReservoir.worldNormal, newDistanceSqr, newCosine);

    const float newPartialJacobian = newCosine / newDistanceSqr;
    float jacobian = newPartialJacobian * originalInversePartialJacobian;

    neighborReservoir.partialJacobian = 1.f / newPartialJacobian;

    if (isinf(jacobian) || isnan(jacobian))
    {
        jacobian = 0;
    }

    return jacobian;
}

bool ConnectsToNEELight(RTXDI_PTReservoir neighborSample)
{
    // neighborSample.radiance.x == INF is used as an indicator for NEE-sample lights
    return (neighborSample.rcVertexLength == neighborSample.pathLength) &&
            isinf(neighborSample.radiance.x);
}

bool ShouldComputeReconnectionJacobian(const RTXDI_PTReservoir neighborSample,
                                       const bool connectToRtxdiLight)
{
    return neighborSample.rcVertexLength <= neighborSample.pathLength && !connectToRtxdiLight;
}

float ComputeReconnectionJacobian(const RTXDI_HybridShiftPathTracerContext ctx,
                                  inout RTXDI_PTReservoir neighborSample,
                                  const RAB_Surface surfaceForResampling,
                                  inout float jacobian)
{
    if (neighborSample.rcVertexLength <= neighborSample.pathLength)
    {
        jacobian = CalculateJacobianWithCachedJacobian(RAB_GetSurfaceWorldPos(surfaceForResampling), neighborSample);
    }

    return jacobian;
}

void ValidateInvertibilityCondition(const RTXDI_HybridShiftPathTracerContext ctx,
                                    const RTXDI_PTReservoir neighborSample,
                                    const RAB_Surface surfaceForResampling,
                                    inout float jacobian)
{
    float3 reconnectionDir = neighborSample.translatedWorldPosition - RAB_GetSurfaceWorldPos(surfaceForResampling);
    float reconnectionDist = length(reconnectionDir);
    reconnectionDir /= reconnectionDist;

    if (ctx.GetReconnectionMode() == RTXDI_RESTIRPT_RECONNECTION_MODE_FIXED_THRESHOLD)
    {
        if (neighborSample.rcVertexLength == neighborSample.pathLength)
        {
            if (isinf(neighborSample.radiance.x))
            {
                return;
            }
            else if (reconnectionDist < ctx.GetDistanceThreshold() || RAB_GetSurfaceRoughness(surfaceForResampling) < ctx.GetRoughnessThreshold())
            {
                jacobian = 0.0f;
            }
        }
        else if (neighborSample.rcVertexLength < neighborSample.pathLength)
        {
            if (reconnectionDist < ctx.GetDistanceThreshold() || RAB_GetSurfaceRoughness(surfaceForResampling) < ctx.GetRoughnessThreshold())
            {
                jacobian = 0.0f;
            }
        }
    }
    else if (ctx.GetReconnectionMode() == RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT)
    {
        float scatterPdf = 0.f;
        if (neighborSample.rcVertexLength <= neighborSample.pathLength)
        {
            scatterPdf = RAB_SurfaceEvaluateBrdfPdf(surfaceForResampling, reconnectionDir);
        }

        float geometryFactor = abs(dot(neighborSample.worldNormal, reconnectionDir)) / (reconnectionDist * reconnectionDist);
        float rayfootprint = 1.f / (geometryFactor * scatterPdf);

        if (rayfootprint <= ctx.GetFootprintThreshold())
        {
            jacobian = 0.f;
        }

        if (ctx.GetCheckPreRcInverseGeoTerm() > 0.f && ctx.GetCheckPreRcInverseGeoTerm() / scatterPdf > ctx.GetFootprintThreshold())
        {
            jacobian = 0.f;
        }

        if (scatterPdf > ctx.GetPdfThreshold() && neighborSample.rcVertexLength < neighborSample.pathLength)
        {
            jacobian = 0.f;
        }

        if (neighborSample.rcVertexLength < neighborSample.pathLength)
        {
            float rcScatterPdf = neighborSample.rcWiPdf;
            float inverseGeometryFactor = abs(dot(RAB_GetSurfaceNormal(surfaceForResampling), reconnectionDir)) / (reconnectionDist * reconnectionDist);
            float inversefootprint = 1.f / (inverseGeometryFactor * rcScatterPdf);
            if (inversefootprint <= ctx.GetFootprintThreshold())
                jacobian = 0.f;
        }
    }
}

void ValidateLightIDAndReservoir(const RTXDI_PTHybridShiftRuntimeParameters hsrParams,
                                 inout RTXDI_PTReservoir neighborSample,
                                 inout RTXDI_SampledLightData sampledLightData,
                                 inout float3 targetFunction)
{
    if (RTXDI_SampledLightData_IsValidLightData(sampledLightData))
    {
        int mappedLightID = -1;
        bool remapped = false;

        if (hsrParams.isBasePathInPrevFrame)
        {
            mappedLightID = RAB_TranslateLightIndex(RTXDI_SampledLightData_GetLightIndex(sampledLightData), false);
            neighborSample.radiance.y = asfloat(RTXDI_LightIndexToLightData(mappedLightID));
            remapped = true;
        }

        if (remapped)
        {
            if (mappedLightID == -1)
            {
                targetFunction *= 0.f;
                sampledLightData = RTXDI_SampledLightData_CreateInvalidData();
            }
            else
            {
                RTXDI_SampledLightData_SetLightData(sampledLightData, mappedLightID);
            }
        }
    }
}

float3 RTXDI_ComputeTargetFunctionWithLightPdf(const RTXDI_PTHybridShiftRuntimeParameters hsrParams,
                                               const RTXDI_SampledLightData sampledLightData,
                                               const RAB_Surface surface,
                                               inout float lightPdf,
                                               inout float3 lightPos,
                                               inout RAB_LightSample lightSample)
{
    RAB_LightInfo lightInfo = RAB_LoadLightInfo(RTXDI_SampledLightData_GetLightIndex(sampledLightData), hsrParams.isPrevFrame);
    lightSample = RAB_SamplePolymorphicLight(lightInfo, surface, RTXDI_SampledLightData_GetUVDataFloat2(sampledLightData));

    const bool isVisible = RAB_GetConservativeVisibility(surface, lightSample);

    float3 targetFunction = isVisible ? RAB_GetReflectedBsdfRadianceForSurface(RAB_LightSamplePosition(lightSample), RAB_LightSampleRadiance(lightSample), surface) : float3(0.0f, 0.0f, 0.0f);

    float3 surfaceToLightVector = RAB_LightSamplePosition(lightSample) - RAB_GetSurfaceWorldPos(surface);
    float distanceToLight = length(surfaceToLightVector);
    float3 lightDir = surfaceToLightVector / distanceToLight;

    lightPdf = RAB_LightSampleSolidAnglePdf(lightSample);
    lightPos = RAB_LightSamplePosition(lightSample);

    return targetFunction / RAB_LightSampleSolidAnglePdf(lightSample);
}

void UpdateReconnectionForRTXDIConnectedLight(const RTXDI_PTHybridShiftRuntimeParameters hsrParams,
                                              const RTXDI_HybridShiftPathTracerContext ctx,
                                              const RAB_Surface surfaceForResampling,
                                              inout float jacobian,
                                              inout float3 targetFunction,
                                              inout RTXDI_PTReservoir neighborSample,
                                              inout RAB_PathTracerUserData ptud)
{
    RTXDI_SampledLightData sampledLightData = RTXDI_GetSampledLightData(neighborSample);

    ValidateLightIDAndReservoir(hsrParams, neighborSample, sampledLightData, targetFunction);

    float invPartialJacobian = 1.f;
    float3 lightPos = float3(0.0f, 0.0f, 0.0f);
    RAB_LightSample lightSample = RAB_EmptyLightSample();
    float3 lightReflectedRadiance = RTXDI_ComputeTargetFunctionWithLightPdf(hsrParams, sampledLightData, surfaceForResampling, invPartialJacobian, lightPos, lightSample);

    const float originalInvPartialJacobian = neighborSample.partialJacobian;
    targetFunction *= lightReflectedRadiance;
    jacobian = invPartialJacobian == 0.f ? 0.f : originalInvPartialJacobian / invPartialJacobian;
    neighborSample.partialJacobian = invPartialJacobian;

    const float3 lightDirection = normalize(lightPos - RAB_GetSurfaceWorldPos(surfaceForResampling));

    RAB_LastBounceDenoiserCallback(lightPos, surfaceForResampling, ptud);

    float scatterPdf = 0.f;
    if (ctx.GetReconnectionMode() == RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT)
        scatterPdf = RAB_SurfaceEvaluateBrdfPdf(surfaceForResampling, lightDirection);

    if (ctx.GetReconnectionMode() == RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT)
    {
        if (ctx.GetCheckPreRcInverseGeoTerm() > 0.f && ctx.GetCheckPreRcInverseGeoTerm() / scatterPdf > ctx.GetFootprintThreshold())
        {
            jacobian = 0.f;
        }
    }

    targetFunction *= RAB_GetMISWeightForNEE(RTXDI_SampledLightData_GetLightIndex(sampledLightData), lightSample, lightDirection, invPartialJacobian, scatterPdf);
}

void RTXDI_ComputeHybridShift(inout RAB_Surface surfaceForResampling,
                              inout RTXDI_PTReservoir neighborSample,
                              RTXDI_PTHybridShiftPerFrameParameters hspfParams,
                              RTXDI_PTHybridShiftRuntimeParameters hsrParams,
                              RTXDI_PTReconnectionParameters rcParams,
                              inout float3 targetFunction,
                              inout float jacobian,
                              inout RAB_PathTracerUserData ptud)
{
    jacobian = 1.f;
    targetFunction = float3(1.0f, 1.0f, 1.0f);

    RTXDI_PathTracerRandomContext ptrRandContext = (RTXDI_PathTracerRandomContext) 0;
    ptrRandContext.replayRandomSamplerState = RTXDI_CreateRandomSamplerFromDirectSeed(neighborSample.randomSeed, neighborSample.randomIndex);

    RTXDI_HybridShiftPathTracerContext ctx = InitializePathTracerContext(hspfParams, hsrParams, rcParams, neighborSample, surfaceForResampling, ptrRandContext);

    bool needToRunRandomReplayPathTracer = NeedToRunRandomReplayPathTracer(neighborSample);

    if (needToRunRandomReplayPathTracer)
    {
        RandomReplay(ctx, neighborSample, surfaceForResampling, ptrRandContext, targetFunction, ptud);
    }

    const bool connectToRtxdiLight = ConnectsToNEELight(neighborSample);
    if (ShouldComputeReconnectionJacobian(neighborSample, connectToRtxdiLight))
    {
        jacobian = ComputeReconnectionJacobian(ctx, neighborSample, surfaceForResampling, jacobian);
        ValidateInvertibilityCondition(ctx, neighborSample, surfaceForResampling, jacobian);

        RAB_ReconnectionDenoiserCallback(neighborSample, surfaceForResampling, ptud);
    }

    if (connectToRtxdiLight)
    {
        UpdateReconnectionForRTXDIConnectedLight(hsrParams, ctx, surfaceForResampling, jacobian, targetFunction, neighborSample, ptud);
    }
}

// =========================================================================
// TemporalResampling -- ported from Rtxdi/PT/TemporalResampling.hlsli. Duplication-based history
// reduction is omitted (requires a whole separate DuplicationMap system this engine doesn't have --
// disclosed v1 scope cut, matching every other "real but secondary" feature already deferred for PT),
// debug retrace is omitted (RTXDI_DEBUG never defined here), NEIGHBOR_FIND_MODE_GI is the only mode
// ported (matches the real file's own default).
// =========================================================================

struct RTXDI_PTTemporalResamplingRuntimeParameters
{
    uint2 pixelPosition;
    uint2 reservoirPosition;
    float3 motionVector;

    float3 cameraPos;
    float3 prevCameraPos;
    float3 prevPrevCameraPos;
};

// Generates a pattern of offsets for looking closely around a given pixel.
int2 RTXDI_CalculateTemporalResamplingOffset(int sampleIdx, int radius)
{
    sampleIdx &= 7;

    int mask2 = sampleIdx >> 1 & 0x01;
    int mask4 = 1 - (sampleIdx >> 2 & 0x01);
    int tmp0 = -1 + 2 * (sampleIdx & 0x01);
    int tmp1 = 1 - 2 * mask2;
    int tmp2 = mask4 | mask2;
    int tmp3 = mask4 | (1 - mask2);

    return int2(tmp0, tmp0 * tmp1) * int2(tmp2, tmp3) * radius;
}

int2 CalculateTemporalSurfacePosition(RTXDI_PTTemporalResamplingParameters tParams, RTXDI_RuntimeParameters rParams, int i, int temporalSampleStartIdx, int2 pixelPos, float radius, int2 prevPos, bool isFallbackSample)
{
    const bool isFirstSample = i == 0;
    int2 offset = int2(0, 0);
    if (isFallbackSample)
    {
        prevPos = int2(pixelPos);
    }
    else if (!isFirstSample)
    {
        offset = RTXDI_CalculateTemporalResamplingOffset(temporalSampleStartIdx + i, int(radius));
    }

    int2 idx = prevPos + offset;
    if ((tParams.enablePermutationSampling && isFirstSample) || isFallbackSample)
    {
        RTXDI_ApplyPermutationSampling(idx, tParams.uniformRandomNumber);
    }

    return idx;
}

bool ValidTemporalSurface(RTXDI_PTTemporalResamplingParameters tParams, RAB_Surface curSurface, RAB_Surface temporalSurface, float expectedPrevLinearDepth, bool isFallbackSample)
{
    if (!RAB_IsSurfaceValid(temporalSurface))
    {
        return false;
    }

    if (!isFallbackSample && !RTXDI_IsValidNeighbor(
        RAB_GetSurfaceNormal(curSurface), RAB_GetSurfaceNormal(temporalSurface),
        expectedPrevLinearDepth, RAB_GetSurfaceLinearDepth(temporalSurface),
        tParams.normalThreshold, tParams.depthThreshold))
    {
        return false;
    }

    if (!RAB_AreMaterialsSimilar(RAB_GetMaterial(curSurface), RAB_GetMaterial(temporalSurface)))
    {
        return false;
    }

    return true;
}

bool FindTemporalNeighbor(const RTXDI_PTTemporalResamplingParameters tParams,
                             const RTXDI_RuntimeParameters rParams,
                             const RTXDI_ReservoirBufferParameters reservoirBufferParams,
                             const RTXDI_PTBufferIndices bufferIndices,
                             inout RTXDI_RandomSamplerState rng,
                             const int2 pixelPos,
                             const int2 prevPos,
                             const RAB_Surface curSurface,
                             const float expectedPrevLinearDepth,
                             inout RAB_Surface temporalSurface,
                             inout RTXDI_PTReservoir temporalReservoir)
{
    // Try to find a matching surface in the neighborhood of the reprojected pixel
    const int temporalSampleCount = 9;
    const int sampleCount = temporalSampleCount + (tParams.enableFallbackSampling ? 1 : 0);
    const int radius = 1;
    const int temporalSampleStartIdx = int(RTXDI_GetNextRandom(rng) * 8);
    for (int i = 0; i < sampleCount; i++)
    {
        const bool isFallbackSample = i == temporalSampleCount;

        int2 temporalSurfacePos = CalculateTemporalSurfacePosition(tParams, rParams, i, temporalSampleStartIdx, pixelPos, radius, prevPos, isFallbackSample);
        temporalSurface = RAB_GetGBufferSurface(temporalSurfacePos, true);

        if (!ValidTemporalSurface(tParams, curSurface, temporalSurface, expectedPrevLinearDepth, isFallbackSample))
        {
            continue;
        }

        // Array index is always 0: g_HistoryReservoirBuffer already points at the one physical buffer
        // that holds history (see its declaration comment) -- bufferIndices.temporalResamplingInputBufferIndex
        // is not a valid array-index-within-one-buffer for our per-object buffer layout.
        uint2 prevReservoirPos = RTXDI_PixelPosToReservoirPos(uint2(temporalSurfacePos), rParams.activeCheckerboardField);
        temporalReservoir = RTXDI_LoadPTReservoir(reservoirBufferParams, prevReservoirPos, 0);

        if (!RTXDI_IsValidPTReservoir(temporalReservoir))
        {
            continue;
        }

        return true;
    }

    return false;
}

bool ValidateTemporalNeighbor(const RTXDI_PTTemporalResamplingParameters tParams,
                              inout RTXDI_PTReservoir prevSample,
                              const RAB_Surface curSurface,
                              const RAB_Surface prevSurface,
                              inout int reducedMaxTemporalHistory)
{
    // No-RcVertex paths are not valid temporal reuse candidates for hybrid shift.
    // Random replay can otherwise clobber the inout resampling surface with an empty
    // RcPrevSurface, which poisons the subsequent bias-correction jacobian.
    if (prevSample.rcVertexLength > prevSample.pathLength)
    {
        return false;
    }

    prevSample.M = min(prevSample.M, min(float(0xffff), float(reducedMaxTemporalHistory)));
    prevSample.age = min(RTXDI_PTRESERVOIR_AGE_MAX, prevSample.age + 1);

    bool foundNeighbor = true;
    if (tParams.enableAgeBasedRejection && prevSample.age > min((uint)RTXDI_PTRESERVOIR_AGE_MAX, tParams.maxReservoirAge))
    {
        foundNeighbor = false;
    }

    return foundNeighbor;
}

bool ShouldEvaluateTemporalReconnection(RTXDI_PTReservoir prevSample)
{
    return !(((prevSample.rcVertexLength == prevSample.pathLength) && RTXDI_ConnectsToNeeLight(prevSample)) || (prevSample.rcVertexLength > prevSample.pathLength));
}

bool RTXDI_PTHasReconnectionVertex(RTXDI_PTReservoir sample)
{
    return sample.rcVertexLength <= sample.pathLength;
}

void EvaluateReconnection(const RTXDI_PTHybridShiftPerFrameParameters hspfParams,
                          inout float3 targetFunction,
                          const RAB_Surface curSurface,
                          const RTXDI_PTReservoir prevSample,
                          const bool checkVisibility)
{
    targetFunction *= RAB_GetPTSampleTargetPdfForSurface(prevSample.translatedWorldPosition, prevSample.radiance, curSurface);

    if (checkVisibility && any(targetFunction > 0.f))
    {
        targetFunction *= RAB_GetConservativeVisibility(curSurface, prevSample.translatedWorldPosition);
    }
}

RTXDI_PTHybridShiftRuntimeParameters BuildHSRShiftParams(const RTXDI_PTTemporalResamplingRuntimeParameters trrParams)
{
    RTXDI_PTHybridShiftRuntimeParameters hsrParams = (RTXDI_PTHybridShiftRuntimeParameters)0;
    hsrParams.isBasePathInPrevFrame = true;
    hsrParams.isPrevFrame = false;
    hsrParams.cameraPos = trrParams.cameraPos;
    hsrParams.prevCameraPos = trrParams.prevCameraPos;
    hsrParams.prevPrevCameraPos = trrParams.prevPrevCameraPos;
    return hsrParams;
}

RTXDI_PTHybridShiftRuntimeParameters BuildHSRInverseShiftParams(const RTXDI_PTTemporalResamplingRuntimeParameters trrParams)
{
    RTXDI_PTHybridShiftRuntimeParameters hsrParams = (RTXDI_PTHybridShiftRuntimeParameters)0;
    hsrParams.isBasePathInPrevFrame = false;
    hsrParams.isPrevFrame = true;
    hsrParams.cameraPos = trrParams.cameraPos;
    hsrParams.prevCameraPos = trrParams.prevCameraPos;
    hsrParams.prevPrevCameraPos = trrParams.prevPrevCameraPos;
    return hsrParams;
}

// Shift previous sample - random replay to new location
void ResampleTemporalNeighbor(const RTXDI_PTTemporalResamplingParameters tParams,
                              const RTXDI_PTTemporalResamplingRuntimeParameters trrParams,
                              const RTXDI_PTHybridShiftPerFrameParameters hspfParams,
                              const RTXDI_PTReconnectionParameters rcParams,
                              inout RTXDI_RandomSamplerState rng,
                              inout RTXDI_PTReservoir targetReservoir,
                              inout RAB_Surface curSurface,
                              inout RTXDI_PTReservoir prevSample,
                              inout float jacobian,
                              inout float3 selectedTargetFunction,
                              inout bool selectedPrevSample,
                              inout RAB_PathTracerUserData ptud)
{
    float3 targetFunction = float3(1.0f, 1.0f, 1.0f);
    bool shouldEvaluateReconnection = ShouldEvaluateTemporalReconnection(prevSample);

    RAB_PathTracerUserDataSetPathType(ptud, RTXDI_PTPathTraceInvocationType_Temporal);
    RTXDI_ComputeHybridShift(curSurface, prevSample, hspfParams, BuildHSRShiftParams(trrParams), rcParams, targetFunction, jacobian, ptud);

    prevSample.weightSum *= jacobian;

    if (shouldEvaluateReconnection)
    {
        EvaluateReconnection(hspfParams, targetFunction, curSurface, prevSample, tParams.enableVisibilityBeforeCombine != 0);
    }

    if (RTXDI_CombinePTReservoirs(targetReservoir, prevSample, RTXDI_GetNextRandom(rng), targetFunction))
    {
        selectedPrevSample = true;
        selectedTargetFunction = targetFunction;
    }
}

void BiasCorrection(const RTXDI_PTTemporalResamplingRuntimeParameters trrParams,
                    const RTXDI_PTHybridShiftPerFrameParameters hspfParams,
                    const RTXDI_PTReconnectionParameters rcParams,
                    inout RTXDI_RandomSamplerState rng,
                    inout RTXDI_PTReservoir targetReservoir,
                    inout RAB_Surface curSurface,
                    inout RAB_Surface prevSurface,
                    inout RTXDI_PTReservoir prevSample,
                    const bool selectedPrevSample,
                    inout float pi,
                    inout float piSum,
                    inout RAB_PathTracerUserData ptud)
{
    float3 targetFunction = float3(1.0f, 1.0f, 1.0f);
    bool shouldEvaluateReconnection = ShouldEvaluateTemporalReconnection(targetReservoir);

    float jacobian = RTXDI_CalculateJacobian(RAB_GetSurfaceWorldPos(prevSurface), RAB_GetSurfaceWorldPos(curSurface), targetReservoir.translatedWorldPosition, targetReservoir.worldNormal);
    float backupPartialJacobian = targetReservoir.partialJacobian;

    RAB_PathTracerUserDataSetPathType(ptud, RTXDI_PTPathTraceInvocationType_TemporalInverse);
    RTXDI_ComputeHybridShift(prevSurface, targetReservoir, hspfParams, BuildHSRInverseShiftParams(trrParams), rcParams, targetFunction, jacobian, ptud);

    targetReservoir.partialJacobian = backupPartialJacobian;

    if (shouldEvaluateReconnection)
    {
        EvaluateReconnection(hspfParams, targetFunction, prevSurface, targetReservoir, true);
    }

    float temporalP = RTXDI_Luminance(targetFunction) * jacobian;

    pi = selectedPrevSample ? temporalP : pi;
    piSum += temporalP * prevSample.M;
}

// Adapted from the real reference: takes `curSample` as a plain parameter (already loaded by the
// caller) instead of loading it internally via RTXDI_LoadPTReservoir(bufferIndices.initialPathTracerOutputBufferIndex)
// -- that call assumes one shared buffer sliced by array index, but our engine's reservoir "slots" are
// separate physical BDA-addressed buffers (see g_HistoryReservoirBuffer's declaration comment above),
// so the current-frame candidate has to come from a different pointer than the macro-bound history
// buffer this function's internal RTXDI_LoadPTReservoir calls (inside FindTemporalNeighbor) read from.
// Exactly the same adaptation ReSTIRDITemporal.slang already makes for RTXDI_DITemporalResampling.
RTXDI_PTReservoir RTXDI_PTTemporalResampling(RTXDI_PTTemporalResamplingParameters tParams,
                                             RTXDI_PTTemporalResamplingRuntimeParameters trrParams,
                                             RTXDI_PTHybridShiftPerFrameParameters hspfParams,
                                             RTXDI_PTReconnectionParameters rcParams,
                                             RTXDI_RuntimeParameters rParams,
                                             RTXDI_ReservoirBufferParameters bufferParams,
                                             RTXDI_RandomSamplerState rng,
                                             RTXDI_PTBufferIndices bufferIndices,
                                             RTXDI_PTReservoir curSample,
                                             inout bool selectedPrevSample,
                                             inout RAB_PathTracerUserData ptud)
{
    RAB_Surface curSurface = RAB_GetGBufferSurface(int2(trrParams.pixelPosition), false);
    if (!RAB_IsSurfaceValid(curSurface))
    {
        return RTXDI_EmptyPTReservoir();
    }

    RTXDI_PTReservoir targetReservoir = RTXDI_EmptyPTReservoir();
    float3 selectedTargetFunction = float3(0.0f, 0.0f, 0.0f);

    if (RTXDI_IsValidPTReservoir(curSample))
    {
        // Keep no-RcVertex current paths as single-frame samples. Temporal hybrid shift and its
        // inverse bias-correction pass require a real reconnection vertex; otherwise random replay
        // can leave the resampling surface empty and inject structured history noise.
        if (!RTXDI_PTHasReconnectionVertex(curSample))
        {
            selectedPrevSample = false;
            return curSample;
        }

        if (RTXDI_CombinePTReservoirs(targetReservoir, curSample, /* random = */ 0.5, curSample.targetFunction))
        {
            selectedTargetFunction = curSample.targetFunction;
        }
    }

    int2 prevPos = int2(round(float2(trrParams.pixelPosition) + trrParams.motionVector.xy));

    // RAB_GetSurfaceLinearDepth returns distance to the camera. The engine's velocity buffer only
    // stores UV motion in RG, but RTXDI expects motionVector.z to be previousDepth - currentDepth.
    // Derive the matching previous-frame depth from the current world position and previous camera.
    float expectedPrevLinearDepth = length(RAB_GetSurfaceWorldPos(curSurface) - trrParams.prevCameraPos);
    RTXDI_PTReservoir prevSample = RTXDI_EmptyPTReservoir();
    RAB_Surface prevSurface = RAB_EmptySurface();

    bool foundNeighbor = FindTemporalNeighbor(tParams, rParams, bufferParams, bufferIndices, rng, int2(trrParams.pixelPosition), prevPos, curSurface, expectedPrevLinearDepth, prevSurface, prevSample);

    int reducedMaxTemporalHistory = int(tParams.maxHistoryLength);

    float jacobian = 1.f;
    if (foundNeighbor)
    {
        jacobian = RTXDI_CalculateJacobian(RAB_GetSurfaceWorldPos(curSurface), RAB_GetSurfaceWorldPos(prevSurface), prevSample.translatedWorldPosition, prevSample.worldNormal);
        foundNeighbor = ValidateTemporalNeighbor(tParams, prevSample, curSurface, prevSurface, reducedMaxTemporalHistory);
    }

    selectedPrevSample = false;
    if (foundNeighbor)
    {
        ResampleTemporalNeighbor(tParams, trrParams, hspfParams, rcParams, rng, targetReservoir, curSurface, prevSample, jacobian, selectedTargetFunction, selectedPrevSample, ptud);
    }

    float pi = RTXDI_Luminance(selectedTargetFunction);
    float piSum = RTXDI_Luminance(selectedTargetFunction) * curSample.M;
    if (RTXDI_IsValidPTReservoir(targetReservoir) && foundNeighbor && RTXDI_PTHasReconnectionVertex(targetReservoir))
    {
        BiasCorrection(trrParams, hspfParams, rcParams, rng, targetReservoir, curSurface, prevSurface, prevSample, selectedPrevSample, pi, piSum, ptud);
    }

    const float normalizationNumerator = pi;
    const float normalizationDenominator = piSum * RTXDI_Luminance(selectedTargetFunction);
    RTXDI_FinalizePTResampling(targetReservoir, normalizationNumerator, normalizationDenominator);

    return targetReservoir;
}

// =========================================================================
// RAB_GetGBufferSurface -- mirrors ReSTIRGITemporal.slang's exact two-frame pattern (prevDepth/
// prevNormal/prevInvViewProj for the previous frame's textures, unprojected with the MATCHING frame's
// matrix -- unprojecting last frame's depth with this frame's matrix reconstructs the wrong world
// position the instant the camera moves), extended with prevAlbedo/prevMaterial since PT's RAB_Surface
// carries a full RAB_Material unlike GI's.
// =========================================================================

RAB_Surface RAB_GetGBufferSurface(int2 pixelPos, bool isPreviousFrame)
{
    RAB_Surface s = RAB_EmptySurface();
    if (pixelPos.x < 0 || pixelPos.y < 0 ||
        pixelPos.x >= int(g_PushConstant.viewportSize.x) || pixelPos.y >= int(g_PushConstant.viewportSize.y))
        return s;

    uint depthTexIndex = (isPreviousFrame && g_PushConstant.prevDepthTextureIndex != 0xFFFFFFFF)
        ? g_PushConstant.prevDepthTextureIndex
        : g_PushConstant.depthTextureIndex;
    if (depthTexIndex == 0xFFFFFFFF)
        return s;

    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(g_UBO->imageHeapIndexOffset + depthTexIndex, 0));
    float rawDepth = depthTex.Load(int3(pixelPos, 0)).r;
    if (rawDepth <= 0.00001)
        return s;

    uint normalTexIndex = (isPreviousFrame && g_PushConstant.prevNormalTextureIndex != 0xFFFFFFFF)
        ? g_PushConstant.prevNormalTextureIndex
        : g_PushConstant.gbufferNormalIndex;
    if (normalTexIndex == 0xFFFFFFFF)
        return s;

    Texture2D normalTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + normalTexIndex, 0));
    float4 normalSample = normalTex.Load(int3(pixelPos, 0));
    float lenSqN = dot(normalSample.rgb, normalSample.rgb);
    if (lenSqN < 1e-6)
        return s;
    s.normal = normalSample.rgb * rsqrt(lenSqN);
    s.geoNormal = s.normal; // G-buffer doesn't track a separate geometric normal

    float2 uv = (float2(pixelPos) + 0.5) / g_PushConstant.viewportSize;
    float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 ndc = float4(ndc_xy, rawDepth, 1.0);
    float4x4 unprojectMatrix = isPreviousFrame ? g_PushConstant.prevInvViewProj : g_UBO->invViewProj;
    float4 worldPosH = mul(unprojectMatrix, ndc);
    s.worldPos = worldPosH.xyz / worldPosH.w;

    float3 camPos = isPreviousFrame ? g_PushConstant.prevCameraWorldPos.xyz : g_PushConstant.cameraWorldPos.xyz;
    s.viewDir = normalize(camPos - s.worldPos);
    s.viewDepth = length(s.worldPos - camPos);

    uint albedoTexIndex = (isPreviousFrame && g_PushConstant.prevAlbedoTextureIndex != 0xFFFFFFFF)
        ? g_PushConstant.prevAlbedoTextureIndex
        : g_PushConstant.gbufferAlbedoIndex;
    uint materialTexIndex = (isPreviousFrame && g_PushConstant.prevMaterialTextureIndex != 0xFFFFFFFF)
        ? g_PushConstant.prevMaterialTextureIndex
        : g_PushConstant.gbufferMaterialIndex;
    if (albedoTexIndex == 0xFFFFFFFF || materialTexIndex == 0xFFFFFFFF)
        return s;

    Texture2D albedoTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + albedoTexIndex, 0));
    Texture2D materialTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + materialTexIndex, 0));
    float4 albedoSample = albedoTex.Load(int3(pixelPos, 0));
    float4 materialSample = materialTex.Load(int3(pixelPos, 0));

    float roughness = clamp(materialSample.r, kMinRoughness, 1.0);
    float metallic = clamp(materialSample.g, 0.0, 1.0);
    float3 baseColor = albedoSample.rgb;

    s.material.diffuseAlbedo = baseColor * (1.0 - metallic);
    s.material.specularF0 = lerp(float3(0.04), baseColor, metallic);
    s.material.roughness = roughness;
    s.material.emissiveColor = float3(0.0, 0.0, 0.0);
    s.diffuseProbability = GetSurfaceDiffuseProbability(s);
    s.valid = true;
    return s;
}

// =========================================================================
// Dispatch entry point -- full-screen mesh-shader triangle, matching every other RTXDI pass. Reads
// this frame's Initial-sampling reservoir (bufferIndices.initialPathTracerOutputBufferIndex) and last
// frame's finalized reservoir (temporalResamplingInputBufferIndex) from the SAME 3-slot buffer array
// the C++ side rotates, and overwrites the Initial slot in place with the temporal result -- exactly
// like ReSTIRGITemporal.slang's own current/previous-buffer pattern, just addressed via buffer indices
// into one array instead of two separate buffer objects.
// =========================================================================

struct ReSTIRPTTemporalPayload
{
    uint dummy;
};
groupshared ReSTIRPTTemporalPayload gTemporalPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gTemporalPayload);
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
    uniform PushConstantReSTIRPTTemporal pushConstant
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

struct PSOutput
{
    float4 debugOut : SV_Target0;
};

[shader("fragment")]
PSOutput fragMain(
    MSOutput input,
    uniform PushConstantReSTIRPTTemporal pushConstant
)
{
    PSOutput outVal;
    outVal.debugOut = float4(0.0, 0.0, 0.0, 1.0);

    uint2 pixelCoord = uint2(input.pos.xy);
    if (pixelCoord.x >= uint(pushConstant.viewportSize.x) || pixelCoord.y >= uint(pushConstant.viewportSize.y))
        return outVal;

    if (pushConstant.matrixReference == 0 || pushConstant.lightDataReference == 0 ||
        pushConstant.currentReservoirReference == 0 || pushConstant.historyReservoirReference == 0)
        return outVal;

    g_PushConstant = pushConstant;
    g_UBO = (UniformBufferObject*)pushConstant.matrixReference;
    g_LightData = (LightData*)pushConstant.lightDataReference;
    g_InstanceLUT = (GpuRayTracingInstance*)g_UBO->sceneRayTracingInstancesReference;

    if (g_UBO->tlasDeviceAddress == 0)
        return outVal;

    RTXDI_ReservoirBufferParameters resParams;
    resParams.reservoirBlockRowPitch = pushConstant.reservoirBlockRowPitch;
    resParams.reservoirArrayPitch = pushConstant.reservoirArrayPitch;
    resParams.pad1 = 0;
    resParams.pad2 = 0;

    // Kept only for RTXDI_PTTemporalResampling's signature fidelity to the real reference -- neither
    // field is actually used as a buffer-array-index any more (see g_HistoryReservoirBuffer's and
    // RTXDI_PTTemporalResampling's own declaration comments on why our per-object buffer layout needed
    // that adaptation).
    RTXDI_PTBufferIndices bufferIndices = (RTXDI_PTBufferIndices)0;

    float3 screenSpaceMotion = float3(0.0, 0.0, 0.0);
    if (pushConstant.gbufferVelocityIndex != 0xFFFFFFFF)
    {
        Texture2D velocityTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + pushConstant.gbufferVelocityIndex, 0));
        float2 velUV = velocityTex.Load(int3(pixelCoord, 0)).rg;
        screenSpaceMotion = float3(velUV * pushConstant.viewportSize, 0.0);
    }

    RTXDI_PTTemporalResamplingParameters tParams;
    tParams.depthThreshold = pushConstant.depthThreshold;
    tParams.normalThreshold = pushConstant.normalThreshold;
    tParams.enablePermutationSampling = pushConstant.enablePermutationSampling;
    tParams.maxHistoryLength = pushConstant.maxHistoryLength;
    tParams.maxReservoirAge = pushConstant.maxReservoirAge;
    tParams.enableFallbackSampling = 1;
    tParams.enableVisibilityBeforeCombine = 1;
    tParams.uniformRandomNumber = RTXDI_JenkinsHash(pushConstant.frameIndex);
    tParams.duplicationBasedHistoryReduction = 0;
    tParams.historyReductionStrength = 0.0;
    tParams.enableAgeBasedRejection = 1;
    tParams.pad1 = 0;

    RTXDI_PTTemporalResamplingRuntimeParameters trrParams = (RTXDI_PTTemporalResamplingRuntimeParameters)0;
    trrParams.pixelPosition = pixelCoord;
    trrParams.reservoirPosition = pixelCoord;
    trrParams.motionVector = screenSpaceMotion;
    trrParams.cameraPos = pushConstant.cameraWorldPos.xyz;
    trrParams.prevCameraPos = pushConstant.prevCameraWorldPos.xyz;
    trrParams.prevPrevCameraPos = pushConstant.prevPrevCameraWorldPos.xyz;

    RTXDI_PTHybridShiftPerFrameParameters hspfParams;
    hspfParams.maxBounceDepth = pushConstant.maxBounceDepth;
    hspfParams.maxRcVertexLength = pushConstant.maxRcVertexLength;
    hspfParams.pad1 = 0;
    hspfParams.pad2 = 0;

    RTXDI_PTReconnectionParameters rcParams = (RTXDI_PTReconnectionParameters)0;
    rcParams.minConnectionFootprint = 0.02f;
    rcParams.minConnectionFootprintSigma = 0.2f;
    rcParams.minPdfRoughness = 0.1f;
    rcParams.minPdfRoughnessSigma = 0.01f;
    rcParams.roughnessThreshold = pushConstant.roughnessThreshold;
    rcParams.distanceThreshold = pushConstant.distanceThreshold;
    rcParams.reconnectionMode = RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT;

    RTXDI_RuntimeParameters rParams;
    rParams.neighborOffsetMask = 0;
    rParams.activeCheckerboardField = 0;
    rParams.frameIndex = pushConstant.frameIndex;
    rParams.pad2 = 0;

    // Named per-pass seed (not an arbitrary literal 0) -- matches the real FullSample's
    // TemporalResampling.hlsl exactly, keeping this pass's RNG stream decorrelated from Initial
    // Sampling's (seeds 21/22) and every other RTXDI pass's own stream.
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixelCoord, pushConstant.frameIndex, RTXDI_PT_TEMPORAL_RESAMPLING_RANDOM_SEED);
    RAB_PathTracerUserData ptud = RAB_EmptyPathTracerUserData();
    bool selectedPrevSample = false;

    g_HistoryReservoirBuffer = (RTXDI_PackedPTReservoir*)pushConstant.historyReservoirReference;
    RTXDI_PackedPTReservoir* currentReservoirBuffer = (RTXDI_PackedPTReservoir*)pushConstant.currentReservoirReference;

    uint pointer = RTXDI_ReservoirPositionToPointer(resParams, pixelCoord, 0);
    RTXDI_PTReservoir curSample = RTXDI_UnpackPTReservoir(currentReservoirBuffer[pointer]);

    RTXDI_PTReservoir temporalResult = RTXDI_PTTemporalResampling(
        tParams, trrParams, hspfParams, rcParams, rParams, resParams, rng, bufferIndices, curSample, selectedPrevSample, ptud);

    // Store to the CURRENT buffer manually -- RTXDI_StorePTReservoir would write through the macro,
    // i.e. into the history buffer, which must stay untouched (it's still that buffer's role next frame).
    currentReservoirBuffer[pointer] = RTXDI_PackPTReservoir(temporalResult);

    outVal.debugOut = float4(temporalResult.radiance, 1.0);
    return outVal;
}
