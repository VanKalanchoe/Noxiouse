#include "shaderIO.h"

// Procedural sky: Sebastien Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020),
// the model behind Unreal's SkyAtmosphere, after the paper's reference implementation (github.com/sebh/UnrealEngineSkyAtmosphere).
// Earth's atmosphere as Rayleigh (air), Mie (aerosols) and ozone layers over a spherical planet, lengths in km. Three passes:
//   1. transmittanceMain:   transmittance from a height towards the top of the atmosphere, for every height and zenith angle.
//   2. multiScatteringMain: the paper's multiple-scattering approximation (Psi_ms, eq. 10), per height and sun angle.
//   3. skyCubemapMain:      the sky radiance seen from the ground in every direction, written into the environment cubemap.
// The cubemap is the scene's environment like an HDR map would be (skybox, IBL, reflections, DDGI and path tracer misses),
// so raster and path tracer see the same sky. The sun itself (its disc) is not in it: the directional light is the sun.

static const float PI = 3.14159265358979;

static const float BottomRadius = 6360.0; // km
static const float TopRadius = 6460.0;    // km: 100 km of atmosphere

// Earth (the paper's / sebh's defaults), per km at sea level.
static const float3 RayleighScattering = float3(5.802, 13.558, 33.1) * 1e-3;
static const float RayleighHeightScale = 8.0;
static const float MieScattering = 3.996e-3;
static const float MieExtinction = 4.40e-3;
static const float MieHeightScale = 1.2;
static const float3 OzoneAbsorption = float3(0.650, 1.881, 0.085) * 1e-3;
static const float OzoneCenterKm = 25.0;
static const float OzoneHalfWidthKm = 15.0;

// Above the ground by a hair, so a point on the ground does not count as below it.
static const float PlanetRadiusOffset = 0.01;

struct Medium
{
    float3 scattering;  // Rayleigh + Mie
    float3 rayleigh;
    float mie;
    float3 extinction;
};

Medium sampleMedium(float heightKm, PushConstantSkyAtmosphere push)
{
    const float densityRayleigh = exp(-heightKm / RayleighHeightScale);
    const float densityMie = exp(-heightKm / MieHeightScale);
    const float densityOzone = max(0.0, 1.0 - abs(heightKm - OzoneCenterKm) / OzoneHalfWidthKm);

    Medium medium;
    medium.rayleigh = RayleighScattering * densityRayleigh * push.rayleighScale;
    medium.mie = MieScattering * densityMie * push.mieScale;
    medium.scattering = medium.rayleigh + medium.mie;
    const float mieAbsorption = (MieExtinction - MieScattering) * densityMie * push.mieScale;
    medium.extinction = medium.scattering + mieAbsorption + OzoneAbsorption * densityOzone * push.ozoneScale;
    return medium;
}

// Distance along the ray to the sphere of `radius` around the planet's center (the origin): the nearest hit in front, or -1.
float raySphere(float3 origin, float3 direction, float radius)
{
    const float b = dot(origin, direction);
    const float c = dot(origin, origin) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0)
        return -1.0;
    const float s = sqrt(discriminant);
    const float nearT = -b - s;
    const float farT = -b + s;
    if (nearT >= 0.0)
        return nearT;
    return farT >= 0.0 ? farT : -1.0;
}

float rayleighPhase(float cosTheta)
{
    return 3.0 / (16.0 * PI) * (1.0 + cosTheta * cosTheta);
}

// Cornette-Shanks: Henyey-Greenstein with the Rayleigh-like (1 + cos^2) term; peaks looking towards the sun for g > 0.
float miePhase(float g, float cosTheta)
{
    const float g2 = g * g;
    const float k = 3.0 / (8.0 * PI) * (1.0 - g2) / (2.0 + g2);
    return k * (1.0 + cosTheta * cosTheta) / pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
}

// Transmittance LUT parametrization (Bruneton 2017, also used by the paper): a height r and the cosine mu of the zenith
// angle <-> uv, spending the resolution near the horizon where transmittance changes fastest.
void transmittanceUvToRMu(float2 uv, out float r, out float mu)
{
    const float H = sqrt(TopRadius * TopRadius - BottomRadius * BottomRadius);
    const float rho = H * uv.y;
    r = sqrt(rho * rho + BottomRadius * BottomRadius);
    const float dMin = TopRadius - r;
    const float dMax = rho + H;
    const float d = dMin + uv.x * (dMax - dMin);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = clamp(mu, -1.0, 1.0);
}

float2 transmittanceRMuToUv(float r, float mu)
{
    const float H = sqrt(TopRadius * TopRadius - BottomRadius * BottomRadius);
    const float rho = sqrt(max(0.0, r * r - BottomRadius * BottomRadius));
    const float discriminant = r * r * (mu * mu - 1.0) + TopRadius * TopRadius;
    const float d = max(0.0, -r * mu + sqrt(max(0.0, discriminant)));
    const float dMin = TopRadius - r;
    const float dMax = rho + H;
    return float2((d - dMin) / (dMax - dMin), rho / H);
}

float3 sampleTransmittance(PushConstantSkyAtmosphere push, float r, float mu)
{
    Texture2D lut = Texture2D.Handle(uint2(push.transmittanceTextureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    return lut.SampleLevel(linearClamp, transmittanceRMuToUv(r, mu), 0.0).rgb;
}

// The multiple-scattering LUT: x = cos(sun zenith) in [-1, 1], y = height in the atmosphere.
float2 multiScatteringUv(float r, float cosSunZenith)
{
    return float2(saturate(cosSunZenith * 0.5 + 0.5), saturate((r - BottomRadius) / (TopRadius - BottomRadius)));
}

float3 sampleMultiScattering(PushConstantSkyAtmosphere push, float r, float cosSunZenith)
{
    Texture2D lut = Texture2D.Handle(uint2(push.multiScatteringTextureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    return lut.SampleLevel(linearClamp, multiScatteringUv(r, cosSunZenith), 0.0).rgb;
}

struct ScatteringResult
{
    float3 luminance;       // in-scattered light along the ray (per unit sun illuminance unless the caller scales it)
    float3 multiScatterAs1; // the paper's f_ms integrand (multiple-scattering LUT pass only)
};

// The paper's IntegrateScatteredLuminance: march the view ray through the atmosphere (to the ground or the top), at each step
// add the sun's light scattered towards the eye (single scattering with the planet's shadow, plus the multiple-scattering
// term) and attenuate by the medium in between. `uniformPhase`: the multiple-scattering LUT pass integrates with an
// isotropic phase and no multiple-scattering input; the sky pass uses the real phase functions and the LUT.
ScatteringResult integrateScatteredLuminance(PushConstantSkyAtmosphere push, float3 origin, float3 direction, float3 sunDirection,
                                             int sampleCount, bool uniformPhase, bool withGround)
{
    ScatteringResult result;
    result.luminance = float3(0.0);
    result.multiScatterAs1 = float3(0.0);

    const float tTop = raySphere(origin, direction, TopRadius);
    const float tBottom = raySphere(origin, direction, BottomRadius);
    if (tTop < 0.0)
        return result; // outside the atmosphere looking away from it (not a case for a camera on the ground)
    const bool hitsGround = tBottom >= 0.0;
    const float tMax = hitsGround ? tBottom : tTop;

    const float cosTheta = dot(direction, sunDirection);
    const float phaseRayleigh = uniformPhase ? 1.0 / (4.0 * PI) : rayleighPhase(cosTheta);
    const float phaseMie = uniformPhase ? 1.0 / (4.0 * PI) : miePhase(push.mieAnisotropy, cosTheta);

    float3 throughput = float3(1.0);
    float segmentStart = 0.0;
    for (int i = 0; i < sampleCount; ++i)
    {
        // Segments growing quadratically with distance (the reference's variable sample distribution): the dense air near
        // the viewer gets most samples, which matters for rays along the horizon (32 steps: within ~6% of 512 steps,
        // uniform steps are off by up to ~20%). One sample 0.3 into each segment, as in the paper.
        const float segmentFraction = (float(i) + 1.0) / float(sampleCount);
        const float segmentEnd = tMax * segmentFraction * segmentFraction;
        const float dt = segmentEnd - segmentStart;
        const float t = segmentStart + 0.3 * dt;
        segmentStart = segmentEnd;

        const float3 position = origin + direction * t;
        const float r = length(position);
        const float3 up = position / r;
        const float cosSunZenith = dot(sunDirection, up);
        const Medium medium = sampleMedium(r - BottomRadius, push);
        const float3 segmentTransmittance = exp(-medium.extinction * dt);

        // Sun light reaching this point: through the atmosphere above it, unless the planet is in the way.
        const float3 sunTransmittance = sampleTransmittance(push, r, cosSunZenith);
        const float planetShadow = raySphere(position, sunDirection, BottomRadius) >= 0.0 ? 0.0 : 1.0;
        const float3 multiScattered = uniformPhase ? float3(0.0) : sampleMultiScattering(push, r, cosSunZenith);

        const float3 phaseTimesScattering = medium.rayleigh * phaseRayleigh + medium.mie * phaseMie;
        const float3 inScattered = planetShadow * sunTransmittance * phaseTimesScattering + multiScattered * medium.scattering;

        // Energy-conserving integration over the segment (the paper's "S - S * T / extinction").
        const float3 extinction = max(medium.extinction, float3(1e-7));
        result.luminance += throughput * (inScattered - inScattered * segmentTransmittance) / extinction;
        result.multiScatterAs1 += throughput * (medium.scattering - medium.scattering * segmentTransmittance) / extinction;
        throughput *= segmentTransmittance;
    }

    // Light bounced off the ground (Lambertian), for rays that end on it.
    if (withGround && hitsGround)
    {
        const float3 groundPosition = origin + direction * tBottom;
        const float r = length(groundPosition);
        const float3 up = groundPosition / r;
        const float cosSunZenith = dot(sunDirection, up);
        const float3 sunTransmittance = sampleTransmittance(push, r, cosSunZenith);
        result.luminance += throughput * sunTransmittance * saturate(cosSunZenith) * push.groundAlbedo / PI;
    }
    return result;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void transmittanceMain(uint3 id : SV_DispatchThreadID, uniform PushConstantSkyAtmosphere push)
{
    if (id.x >= push.outputSize || id.y >= push.outputHeight)
        return;

    float r, mu;
    transmittanceUvToRMu((float2(id.xy) + 0.5) / float2(push.outputSize, push.outputHeight), r, mu);
    const float3 origin = float3(0.0, r, 0.0);
    const float3 direction = float3(sqrt(max(0.0, 1.0 - mu * mu)), mu, 0.0);

    // Optical depth to the top of the atmosphere (to the ground for rays that hit it: the planet shadow test covers those).
    const float tTop = raySphere(origin, direction, TopRadius);
    const float tBottom = raySphere(origin, direction, BottomRadius);
    const float tMax = tBottom >= 0.0 ? tBottom : max(tTop, 0.0);
    const int sampleCount = 40;
    float3 opticalDepth = float3(0.0);
    for (int i = 0; i < sampleCount; ++i)
    {
        const float t = tMax * (float(i) + 0.5) / float(sampleCount);
        const float height = length(origin + direction * t) - BottomRadius;
        opticalDepth += sampleMedium(height, push).extinction * (tMax / float(sampleCount));
    }

    RWTexture2D<float4> output = RWTexture2D<float4>.Handle(uint2(push.outputStorageIndex, 0));
    output[id.xy] = float4(exp(-opticalDepth), 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void multiScatteringMain(uint3 id : SV_DispatchThreadID, uniform PushConstantSkyAtmosphere push)
{
    if (id.x >= push.outputSize || id.y >= push.outputHeight)
        return;

    const float2 uv = (float2(id.xy) + 0.5) / float2(push.outputSize, push.outputHeight);
    const float cosSunZenith = uv.x * 2.0 - 1.0;
    const float r = BottomRadius + PlanetRadiusOffset + uv.y * (TopRadius - BottomRadius - 2.0 * PlanetRadiusOffset);
    const float3 origin = float3(0.0, r, 0.0);
    const float3 sunDirection = normalize(float3(0.0, cosSunZenith, sqrt(max(0.0, 1.0 - cosSunZenith * cosSunZenith))));

    // The paper's eq. 5 and 7: second-order luminance and the transfer f_ms, integrated over the sphere of directions with an
    // isotropic phase (64 directions, a uniform grid in solid angle).
    const int sqrtSamples = 8;
    const float sphereSolidAngle = 4.0 * PI;
    float3 luminance = float3(0.0);
    float3 multiScatterAs1 = float3(0.0);
    for (int i = 0; i < sqrtSamples; ++i)
    {
        for (int j = 0; j < sqrtSamples; ++j)
        {
            const float phi = 2.0 * PI * (float(i) + 0.5) / float(sqrtSamples);
            const float cosTheta = 1.0 - 2.0 * (float(j) + 0.5) / float(sqrtSamples);
            const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
            const float3 direction = float3(cos(phi) * sinTheta, cosTheta, sin(phi) * sinTheta);

            const ScatteringResult result = integrateScatteredLuminance(push, origin, direction, sunDirection, 20, true, true);
            luminance += result.luminance;
            multiScatterAs1 += result.multiScatterAs1;
        }
    }
    const float weight = sphereSolidAngle / float(sqrtSamples * sqrtSamples) / (4.0 * PI); // times the isotropic phase
    luminance *= weight;
    multiScatterAs1 *= weight;

    // Eq. 9 / 10: every further scattering order is f_ms times the one before, a geometric series.
    const float3 psi = luminance / max(float3(1.0) - multiScatterAs1, float3(1e-4));

    RWTexture2D<float4> output = RWTexture2D<float4>.Handle(uint2(push.outputStorageIndex, 0));
    output[id.xy] = float4(psi, 1.0);
}

// Same face layout as EquirectToCubemap.slang (the environment cubemap's convention).
float3 cubemapDirection(uint face, float2 st)
{
    float3 direction = float3(0.0);
    switch (face)
    {
        case 0: direction = float3( 1.0, -st.y, -st.x); break;
        case 1: direction = float3(-1.0, -st.y,  st.x); break;
        case 2: direction = float3( st.x,  1.0,  st.y); break;
        case 3: direction = float3( st.x, -1.0, -st.y); break;
        case 4: direction = float3( st.x, -st.y,  1.0); break;
        case 5: direction = float3(-st.x, -st.y, -1.0); break;
    }
    return normalize(direction);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void skyCubemapMain(uint3 id : SV_DispatchThreadID, uniform PushConstantSkyAtmosphere push)
{
    if (id.x >= push.outputSize || id.y >= push.outputSize)
        return;

    const float2 st = (float2(id.xy) + 0.5) / float(push.outputSize) * 2.0 - 1.0;
    const float3 direction = cubemapDirection(id.z, st);

    // The world's +Y is up; the camera stands at the planet's north pole, cameraAltitudeKm above the ground.
    const float3 origin = float3(0.0, BottomRadius + max(push.cameraAltitudeKm, PlanetRadiusOffset), 0.0);
    const float3 sunDirection = normalize(push.sunDirection);
    const ScatteringResult result = integrateScatteredLuminance(push, origin, direction, sunDirection, 32, false, true);

    RWTexture2DArray<float4> output = RWTexture2DArray<float4>.Handle(uint2(push.outputStorageIndex, 0));
    output[id] = float4(result.luminance * push.sunIlluminance, 1.0);
}
