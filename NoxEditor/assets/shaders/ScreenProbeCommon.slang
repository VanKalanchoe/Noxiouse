// Screen-probe GI (docs/Hybrid_Renderer_Plan_2026-10.md §2c), the final gather Lumen uses: probes placed on the visible
// surfaces (one per tileSize x tileSize pixels), each tracing a hemisphere of rays against the TLAS. The hits are shaded with
// direct light + the DDGI cascades (the world radiance cache: the second bounce and beyond), the misses read the sky. Then:
//   ScreenProbePlace       where this frame's probes stand (a jittered pixel per tile)
//   ScreenProbeTrace       8x8 rays per probe
//   ScreenProbeIrradiance  per probe, the rays convolved into an octahedral irradiance map (6x6 + border)
//   ScreenProbeFilter      spatial filter between neighbouring probes on the same surface (run filterPasses times)
//   ScreenProbeIntegrate   per pixel: the probes around it on its plane, read with its normal, accumulated over frames
// The probes start on the surface they light, so nothing leaks through walls, and every pixel sees the same probes as its
// neighbours: smooth, unlike a world-space grid whose cells cut through the geometry.
//
// Stored like the irradiance cubemap and DDGI: irradiance / pi, so the light a surface reflects is albedo times it.
#pragma once

#include "shaderIO.h"
#include "DDGICommon.slang"

static const uint ScreenProbeRayRes = 8;            // rays per probe: 8 x 8, one texel each in the trace atlas
static const uint ScreenProbeRayCount = ScreenProbeRayRes * ScreenProbeRayRes;
static const uint ScreenProbeIrradianceInterior = 6; // octahedral irradiance per probe: 6 x 6 and a 1-texel border
static const uint ScreenProbeIrradianceRes = ScreenProbeIrradianceInterior + 2;
static const float ScreenProbePi = 3.14159265359;
// Emissive meshes as lights (PathTracer/EmissiveLights.slang): every ray texel (64 a probe) samples a point on one and traces a
// shadow ray to it, into a second target (ScreenProbeTrace): rgb its light's share of the probe's irradiance / pi
// before the cosine, a its direction (octahedral, 12 bits each way, exact in a float). The irradiance pass adds them with the
// cosine; the rays leave the emission of those meshes out (counted once). A lamp is a tiny target for 64 random rays: only this
// way does it light the street under it. 8 a probe (one ray texel in eight) left a wall lit by a lantern's bulb, half hidden by
// its frame, boiling in blotches the size of the spatial filter with the camera still; 64 are 8x the samples (a third of the
// noise) for one more shadow ray per probe ray.
static const uint ScreenProbeEmissiveSamples = 64;

// The ray texels that also take an emissive sample (ScreenProbeEmissiveSamples of the 8 x 8): all of them.
bool screen_probe_is_emissive_texel(uint2 local)
{
    return true;
}

float screen_probe_pack_direction(float3 direction)
{
    const float2 oct = saturate(oct_encode(direction) * 0.5 + 0.5);
    const uint2 q = uint2(round(oct * 4095.0));
    return float(q.x * 4096u + q.y);
}

float3 screen_probe_unpack_direction(float packed)
{
    const uint value = uint(packed);
    const float2 oct = float2(float(value / 4096u), float(value % 4096u)) / 4095.0;
    return oct_decode(oct * 2.0 - 1.0);
}

// Every pass is a compute dispatch over its target, 8 x 8 texels a group, and writes it through bindless storage images
// (compute, so the passes can run on the async compute queue: a compute-only queue cannot draw).
#define SCREEN_PROBE_GROUP_SIZE 8

void screen_probe_store(uint storageIndex, uint2 texel, float4 value)
{
    RWTexture2D<float4> target = RWTexture2D<float4>.Handle(uint2(storageIndex, 0));
    target[texel] = value;
}

void screen_probe_store(uint storageIndex, uint2 texel, float value)
{
    RWTexture2D<float> target = RWTexture2D<float>.Handle(uint2(storageIndex, 0));
    target[texel] = value;
}

// Whether a thread's texel is inside the pass's target (the dispatch rounds up to whole groups).
bool screen_probe_in_target(uint storageIndex, uint2 texel)
{
    RWTexture2D<float4> target = RWTexture2D<float4>.Handle(uint2(storageIndex, 0));
    uint width, height;
    target.GetDimensions(width, height);
    return texel.x < width && texel.y < height;
}

uint screen_probe_hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float screen_probe_random(uint seed)
{
    return float(screen_probe_hash(seed) >> 8) * (1.0 / 16777216.0);
}

// Where in its tile (0..1) every probe of this frame stands (R2 sequence): the placement and the per-pixel interpolation agree.
float2 screen_probe_jitter(uint frameIndex)
{
    return frac(0.5 + float(frameIndex % 1024u) * float2(0.7548776662, 0.5698402910));
}

// An orthonormal frame around n (Duff et al. 2017).
void screen_probe_basis(float3 n, out float3 tangent, out float3 bitangent)
{
    const float sign = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (sign + n.z);
    const float b = n.x * n.y * a;
    tangent = float3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
    bitangent = float3(b, sign + n.y * n.y * a, -n.y);
}

// Ray `ray` (0..63) of a probe this frame: uniform over the hemisphere around the probe's normal, one per cell of an 8 x 8
// stratification of (cos theta, phi), jittered in its cell and turned by a random angle that changes every frame and probe. The
// trace and the convolution derive it the same way.
float3 screen_probe_ray_direction(float3 normal, uint2 probe, uint ray, uint frameIndex)
{
    const uint seed = (probe.x * 73856093u) ^ (probe.y * 19349663u) ^ (frameIndex * 83492791u);
    const float rotation = screen_probe_random(seed) * 2.0 * ScreenProbePi;
    const uint2 cell = uint2(ray % ScreenProbeRayRes, ray / ScreenProbeRayRes);
    const float2 jitter = float2(screen_probe_random(seed ^ (ray * 2654435761u)), screen_probe_random(seed ^ (ray * 2246822519u + 1u)));
    const float2 u = (float2(cell) + jitter) / float(ScreenProbeRayRes);

    const float cosTheta = u.x;
    const float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    const float phi = u.y * 2.0 * ScreenProbePi + rotation;
    float3 tangent, bitangent;
    screen_probe_basis(normal, tangent, bitangent);
    return normalize(tangent * (cos(phi) * sinTheta) + bitangent * (sin(phi) * sinTheta) + normal * cosTheta);
}

// Texel of a probe's octahedral irradiance tile (0..7, border included) -> the interior texel (0..5) it shows. The border repeats
// the texels across the octahedron's seams, so a bilinear read near an edge blends the right directions.
uint2 screen_probe_border_source(uint2 local)
{
    const int last = int(ScreenProbeIrradianceInterior) - 1;
    int2 t = int2(local) - 1;
    const bool outX = t.x < 0 || t.x > last;
    const bool outY = t.y < 0 || t.y > last;
    if (outX && outY)
        return uint2(t.x < 0 ? last : 0, t.y < 0 ? last : 0);
    if (outY)
        return uint2(last - t.x, t.y < 0 ? 0 : last);
    if (outX)
        return uint2(t.x < 0 ? 0 : last, last - t.y);
    return uint2(t);
}

float3 screen_probe_interior_direction(uint2 interior)
{
    const float2 f = (float2(interior) + 0.5) / float(ScreenProbeIrradianceInterior) * 2.0 - 1.0;
    return oct_decode(f);
}

// The irradiance (/ pi) a probe holds towards `direction`: bilinear inside its tile of the atlas.
float3 screen_probe_irradiance(Texture2D atlas, SamplerState linearClamp, uint2 probe, float3 direction, float2 atlasSize)
{
    const float2 oct = oct_encode(direction) * 0.5 + 0.5; // 0..1 over the interior
    const float2 texel = float2(probe * ScreenProbeIrradianceRes) + 1.0 + oct * float(ScreenProbeIrradianceInterior);
    return atlas.SampleLevel(linearClamp, texel / atlasSize, 0).rgb;
}

// How much a probe standing at probePosition (normal probeNormal) speaks for a surface point: on its plane (relative to the
// distance from the camera, as Lumen weighs it) and facing the same way.
float screen_probe_surface_weight(float3 probePosition, float3 probeNormal, float3 position, float3 normal, float viewDistance)
{
    const float planeDistance = abs(dot(probePosition - position, normal)) / max(viewDistance, 1e-4);
    const float planeWeight = exp2(-4000.0 * planeDistance * planeDistance);
    const float facing = saturate(dot(probeNormal, normal));
    return planeWeight * facing * facing;
}

float3 screen_probe_world_position(float4x4 invViewProj, float2 pixelCenter, float2 viewportSize, float depth)
{
    const float2 uv = pixelCenter / viewportSize;
    const float4 h = mul(invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return h.xyz / h.w;
}
