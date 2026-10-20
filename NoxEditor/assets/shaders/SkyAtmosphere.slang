#include "shaderIO.h"
#include "SkyAtmosphereCommon.slang"
#include "CloudNoise.slang"

// Procedural sky: Sebastien Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020),
// the model behind Unreal's SkyAtmosphere, after the paper's reference implementation (github.com/sebh/UnrealEngineSkyAtmosphere).
// Earth's atmosphere as Rayleigh (air), Mie (aerosols) and ozone layers over a spherical planet, lengths in km. Three passes:
//   1. transmittanceMain:   transmittance from a height towards the top of the atmosphere, for every height and zenith angle.
//   2. multiScatteringMain: the paper's multiple-scattering approximation (Psi_ms, eq. 10), per height and sun angle.
//   3. skyCubemapMain:      the sky radiance seen from the ground in every direction, written into one mip of the
//                           environment cubemap per dispatch (every mip raymarched directly: the sky is smooth).
// The cubemap is the scene's environment like an HDR map would be (skybox, IBL, reflections, DDGI and path tracer misses),
// so raster and path tracer see the same sky. The sun's disc is not in it (the directional light already lights the scene
// with the sun, IBL would count it twice): Skybox.slang draws the disc, the moon's and the stars. At night the moon lights the
// air and the clouds the same way (a second light, moonIlluminance). The cubemap's alpha is the clouds' transmittance: what
// the skybox's discs and stars show through.

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

float rayleighPhase(float cosTheta)
{
    return 3.0 / (16.0 * SkyPI) * (1.0 + cosTheta * cosTheta);
}

// Cornette-Shanks: Henyey-Greenstein with the Rayleigh-like (1 + cos^2) term; peaks looking towards the sun for g > 0.
float miePhase(float g, float cosTheta)
{
    const float g2 = g * g;
    const float k = 3.0 / (8.0 * SkyPI) * (1.0 - g2) / (2.0 + g2);
    return k * (1.0 + cosTheta * cosTheta) / pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
}

float3 sampleTransmittance(PushConstantSkyAtmosphere push, float r, float mu)
{
    return sampleSkyTransmittance(push.transmittanceTextureIndex, r, mu);
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
    float3 transmittance;   // through the air marched
};

// The paper's IntegrateScatteredLuminance: march the view ray through the atmosphere (to the ground or the top), at each step
// add the sun's light scattered towards the eye (single scattering with the planet's shadow, plus the multiple-scattering
// term) and attenuate by the medium in between. `uniformPhase`: the multiple-scattering LUT pass integrates with an
// isotropic phase and no multiple-scattering input; the sky pass uses the real phase functions and the LUT.
// `maxDistance` stops the march early (the air in front of a cloud); the ground's light is added only when the ray reaches it.
ScatteringResult integrateScatteredLuminance(PushConstantSkyAtmosphere push, float3 origin, float3 direction, float3 sunDirection,
                                             int sampleCount, bool uniformPhase, bool withGround, float maxDistance = 1e30)
{
    ScatteringResult result;
    result.luminance = float3(0.0);
    result.multiScatterAs1 = float3(0.0);
    result.transmittance = float3(1.0);

    const float tTop = raySphere(origin, direction, TopRadius);
    const float tBottom = raySphere(origin, direction, BottomRadius);
    if (tTop < 0.0)
        return result; // outside the atmosphere looking away from it (not a case for a camera on the ground)
    const bool hitsGround = tBottom >= 0.0 && tBottom <= maxDistance;
    const float tMax = min(tBottom >= 0.0 ? tBottom : tTop, maxDistance);

    const float cosTheta = dot(direction, sunDirection);
    const float phaseRayleigh = uniformPhase ? 1.0 / (4.0 * SkyPI) : rayleighPhase(cosTheta);
    const float phaseMie = uniformPhase ? 1.0 / (4.0 * SkyPI) : miePhase(push.mieAnisotropy, cosTheta);

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
        // Not a ?: : both sides of one may run, and the multiple-scattering LUT pass has that LUT bound for writing.
        float3 multiScattered = float3(0.0);
        if (!uniformPhase)
            multiScattered = sampleMultiScattering(push, r, cosSunZenith);

        const float3 phaseTimesScattering = medium.rayleigh * phaseRayleigh + medium.mie * phaseMie;
        const float3 inScattered = planetShadow * sunTransmittance * phaseTimesScattering + multiScattered * medium.scattering;

        // Energy-conserving integration over the segment (the paper's "S - S * T / extinction").
        const float3 extinction = max(medium.extinction, float3(1e-7));
        result.luminance += throughput * (inScattered - inScattered * segmentTransmittance) / extinction;
        result.multiScatterAs1 += throughput * (medium.scattering - medium.scattering * segmentTransmittance) / extinction;
        throughput *= segmentTransmittance;
    }
    result.transmittance = throughput;

    // Light bounced off the ground (Lambertian), for rays that end on it.
    if (withGround && hitsGround)
    {
        const float3 groundPosition = origin + direction * tBottom;
        const float r = length(groundPosition);
        const float3 up = groundPosition / r;
        const float cosSunZenith = dot(sunDirection, up);
        const float3 sunTransmittance = sampleTransmittance(push, r, cosSunZenith);
        result.luminance += throughput * sunTransmittance * saturate(cosSunZenith) * push.groundAlbedo / SkyPI;
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

    // A storage slot from registerStorageTextureMip: a one-layer 2D array view.
    RWTexture2DArray<float4> output = RWTexture2DArray<float4>.Handle(uint2(push.outputStorageIndex, 0));
    output[uint3(id.xy, 0)] = float4(exp(-opticalDepth), 1.0);
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
    const float sphereSolidAngle = 4.0 * SkyPI;
    float3 luminance = float3(0.0);
    float3 multiScatterAs1 = float3(0.0);
    for (int i = 0; i < sqrtSamples; ++i)
    {
        for (int j = 0; j < sqrtSamples; ++j)
        {
            const float phi = 2.0 * SkyPI * (float(i) + 0.5) / float(sqrtSamples);
            const float cosTheta = 1.0 - 2.0 * (float(j) + 0.5) / float(sqrtSamples);
            const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
            const float3 direction = float3(cos(phi) * sinTheta, cosTheta, sin(phi) * sinTheta);

            const ScatteringResult result = integrateScatteredLuminance(push, origin, direction, sunDirection, 20, true, true);
            luminance += result.luminance;
            multiScatterAs1 += result.multiScatterAs1;
        }
    }
    const float weight = sphereSolidAngle / float(sqrtSamples * sqrtSamples) / (4.0 * SkyPI); // times the isotropic phase
    luminance *= weight;
    multiScatterAs1 *= weight;

    // Eq. 9 / 10: every further scattering order is f_ms times the one before, a geometric series.
    const float3 psi = luminance / max(float3(1.0) - multiScatterAs1, float3(1e-4));

    RWTexture2DArray<float4> output = RWTexture2DArray<float4>.Handle(uint2(push.outputStorageIndex, 0));
    output[uint3(id.xy, 0)] = float4(psi, 1.0);
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

// ---------------------------------------------------------------------------------------------------------------------
// Volumetric clouds (Schneider 2015 / Hillaire 2016 "Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite"):
// a layer of cumulus cloudBottomKm above the ground, raymarched into the environment cubemap together with the sky, so the
// skybox, the IBL and the path tracer's misses all see them. Lengths in km, like the atmosphere.
// ---------------------------------------------------------------------------------------------------------------------

static const uint CloudShapeSize = 64;      // the 3D noise textures, stored as 2D arrays of that many layers
static const uint CloudDetailSize = 32;
static const uint CloudWeatherSize = 128;
static const float CloudWeatherTileKm = 40.0;
static const float CloudDetailTileFraction = 0.15; // the detail noise repeats this much faster than the shape noise
static const float CloudExtinctionPerKm = 40.0;    // cumulus: ~0.04 per meter
static const float CloudMaxMarchKm = 80.0;
static const float CloudAmbientScale = 1.5;        // the sky's light on the clouds (the LUT holds its multiple-scattered part)

// Shape noise texel: billowy Perlin remapped by Worley (Perlin-Worley) in r, Worley FBMs at three frequencies in gba.
[shader("compute")]
[numthreads(4, 4, 4)]
void cloudShapeNoiseMain(uint3 id : SV_DispatchThreadID, uniform PushConstantSkyAtmosphere push)
{
    if (any(id >= uint3(CloudShapeSize)))
        return;
    const float3 uvw = (float3(id) + 0.5) / float(CloudShapeSize);
    const float billow = abs(cloudPerlinFbm(uvw, 4, 5, 11u));
    const float worley0 = cloudWorleyFbm(uvw, 4, 23u);
    const float worley1 = cloudWorleyFbm(uvw, 8, 37u);
    const float worley2 = cloudWorleyFbm(uvw, 16, 53u);
    RWTexture2DArray<float4> output = RWTexture2DArray<float4>.Handle(uint2(push.outputStorageIndex, 0));
    output[id] = float4(cloudRemap(billow, 0.0, 1.0, worley0, 1.0), worley0, worley1, worley2);
}

// Detail noise texel: Worley FBMs at three frequencies (rgb).
[shader("compute")]
[numthreads(4, 4, 4)]
void cloudDetailNoiseMain(uint3 id : SV_DispatchThreadID, uniform PushConstantSkyAtmosphere push)
{
    if (any(id >= uint3(CloudDetailSize)))
        return;
    const float3 uvw = (float3(id) + 0.5) / float(CloudDetailSize);
    RWTexture2DArray<float4> output = RWTexture2DArray<float4>.Handle(uint2(push.outputStorageIndex, 0));
    output[id] = float4(cloudWorleyFbm(uvw, 4, 71u), cloudWorleyFbm(uvw, 8, 83u), cloudWorleyFbm(uvw, 16, 97u), 1.0);
}

// Weather texel: where clouds gather (Perlin FBM, more contrast), r.
[shader("compute")]
[numthreads(8, 8, 1)]
void cloudWeatherMain(uint3 id : SV_DispatchThreadID, uniform PushConstantSkyAtmosphere push)
{
    if (id.x >= CloudWeatherSize || id.y >= CloudWeatherSize)
        return;
    const float2 uv = (float2(id.xy) + 0.5) / float(CloudWeatherSize);
    const float value = cloudPerlinFbm(float3(uv, 0.5), 4, 4, 131u) * 0.5 + 0.5;
    RWTexture2DArray<float4> output = RWTexture2DArray<float4>.Handle(uint2(push.outputStorageIndex, 0));
    output[uint3(id.xy, 0)] = float4(saturate((value - 0.5) * 1.8 + 0.5), 0.0, 0.0, 1.0);
}

// A tiling 3D texture stored as a 2D array: linear in x and y by the sampler, between layers by hand (wrapping).
float4 sampleCloudVolume(uint textureIndex, float3 uvw, uint layers)
{
    Texture2DArray volume = Texture2DArray.Handle(uint2(textureIndex, 0));
    SamplerState linearRepeat = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
    const float layer = frac(uvw.z) * float(layers) - 0.5;
    const float lower = floor(layer);
    const float below = lower < 0.0 ? float(layers - 1) : lower;
    const float above = lower + 1.0 >= float(layers) ? 0.0 : lower + 1.0;
    return lerp(volume.SampleLevel(linearRepeat, float3(uvw.xy, below), 0.0),
                volume.SampleLevel(linearRepeat, float3(uvw.xy, above), 0.0), layer - lower);
}

// Cloud extinction (per km) at p (planet-centered km); heightFraction: where in the layer, 0 bottom to 1 top.
float cloudDensity(PushConstantSkyAtmosphere push, float3 p, bool withDetail, out float heightFraction)
{
    const float r = length(p);
    heightFraction = (r - (BottomRadius + push.cloudBottomKm)) / max(push.cloudThicknessKm, 1e-3);
    if (heightFraction <= 0.0 || heightFraction >= 1.0)
        return 0.0;

    // Horizontal position (around the pole x and z are the ground plane) moved by the wind offset; height in the layer.
    const float3 q = float3(p.x + push.cloudOffsetXKm, r - BottomRadius - push.cloudBottomKm, p.z + push.cloudOffsetZKm);

    Texture2D weatherTexture = Texture2D.Handle(uint2(push.cloudWeatherTextureIndex, 0));
    SamplerState linearRepeat = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
    const float weather = weatherTexture.SampleLevel(linearRepeat, q.xz / CloudWeatherTileKm, 0.0).r;
    const float coverage = saturate(push.cloudCoverage + (weather - 0.5) * 0.7);
    if (coverage <= 0.0)
        return 0.0;

    // Cumulus profile: a rounded bottom, thinning towards the top.
    const float profile = saturate(heightFraction / 0.12) * saturate((1.0 - heightFraction) / 0.55);

    const float4 shape = sampleCloudVolume(push.cloudShapeTextureIndex, q / push.cloudScaleKm, CloudShapeSize);
    const float lowFrequency = shape.g * 0.625 + shape.b * 0.25 + shape.a * 0.125;
    float cloud = saturate(cloudRemap(shape.r, -(1.0 - lowFrequency), 1.0, 0.0, 1.0)) * profile;
    // Coverage: only the densest part of the shape where coverage is low, all of it under an overcast sky.
    cloud = saturate(cloudRemap(cloud, 1.0 - coverage, 1.0, 0.0, 1.0)) * coverage;
    if (cloud <= 0.0)
        return 0.0;

    if (withDetail)
    {
        // Wispy edges at the bottom, billowy towards the top (Schneider's detail erosion).
        const float3 detail = sampleCloudVolume(push.cloudDetailTextureIndex, q / (push.cloudScaleKm * CloudDetailTileFraction), CloudDetailSize).rgb;
        const float highFrequency = detail.r * 0.625 + detail.g * 0.25 + detail.b * 0.125;
        const float erosion = lerp(highFrequency, 1.0 - highFrequency, saturate(heightFraction * 4.0));
        cloud = saturate(cloudRemap(cloud, erosion * 0.3, 1.0, 0.0, 1.0));
    }
    return cloud * push.cloudDensity * CloudExtinctionPerKm;
}

float henyeyGreenstein(float g, float cosTheta)
{
    const float g2 = g * g;
    return (1.0 - g2) / (4.0 * SkyPI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

// Clouds scatter strongly forward (the silver lining towards the sun) and a little backward.
float cloudPhase(float cosTheta, float eccentricityScale)
{
    return lerp(henyeyGreenstein(-0.3 * eccentricityScale, cosTheta), henyeyGreenstein(0.8 * eccentricityScale, cosTheta), 0.7);
}

bool raySphereRange(float3 origin, float3 direction, float radius, out float tNear, out float tFar)
{
    const float b = dot(origin, direction);
    const float c = dot(origin, origin) - radius * radius;
    const float discriminant = b * b - c;
    tNear = -1.0;
    tFar = -1.0;
    if (discriminant < 0.0)
        return false;
    const float s = sqrt(discriminant);
    tNear = -b - s;
    tFar = -b + s;
    return true;
}

// The stretch of the ray inside the cloud layer, for a camera below, inside or above it.
bool cloudLayerSegment(float3 origin, float3 direction, float inner, float outer, out float tStart, out float tEnd)
{
    tStart = 0.0;
    tEnd = 0.0;
    float outerNear, outerFar, innerNear, innerFar;
    if (!raySphereRange(origin, direction, outer, outerNear, outerFar) || outerFar <= 0.0)
        return false;
    const bool hitsInner = raySphereRange(origin, direction, inner, innerNear, innerFar);
    const float r = length(origin);
    if (r < inner)
    {
        tStart = innerFar;
        tEnd = outerFar;
    }
    else if (r < outer)
    {
        tStart = 0.0;
        tEnd = (hitsInner && innerNear > 0.0) ? innerNear : outerFar;
    }
    else
    {
        tStart = max(outerNear, 0.0);
        tEnd = (hitsInner && innerNear > 0.0) ? innerNear : outerFar;
    }
    return tEnd > tStart;
}

struct CloudResult
{
    float3 luminance;     // per unit sun illuminance, like the sky
    float transmittance;  // through the clouds
    float distance;       // where along the ray the clouds are (their light-weighted mean), for the air in front of them
};

CloudResult marchClouds(PushConstantSkyAtmosphere push, float3 origin, float3 direction, float3 sunDirection, float jitter)
{
    CloudResult result;
    result.luminance = float3(0.0);
    result.transmittance = 1.0;
    result.distance = 0.0;
    if (push.cloudShapeTextureIndex == 0xFFFFFFFF || push.cloudCoverage <= 0.0 || push.cloudDensity <= 0.0)
        return result;

    const float inner = BottomRadius + push.cloudBottomKm;
    const float outer = inner + max(push.cloudThicknessKm, 1e-3);
    float tStart, tEnd;
    if (!cloudLayerSegment(origin, direction, inner, outer, tStart, tEnd))
        return result;
    // The ground in front of the layer (looking down from below it).
    const float tGround = raySphere(origin, direction, BottomRadius);
    if (tGround >= 0.0 && tGround < tStart)
        return result;
    tEnd = min(tEnd, tStart + CloudMaxMarchKm);

    // A step of at most ~0.6 km (64 to 128 steps): a ray grazing the horizon crosses up to 80 km of the layer, and 64 steps there
    // were 1.25 km each against ~2 km clouds -- every texel a different random sample of them, blocky noise far away (worst at
    // night, moonlit clouds against a dark sky).
    const int steps = clamp(int(ceil((tEnd - tStart) / 0.6)), 64, 128);
    const float dt = (tEnd - tStart) / float(steps);
    const float cosTheta = dot(direction, sunDirection);
    float weightedDistance = 0.0;
    float weightSum = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        const float t = tStart + (float(i) + jitter) * dt;
        const float3 p = origin + direction * t;
        float heightFraction;
        // The detail noise (tens of meters) only near: farther than ~20 km it is smaller than a texel and only aliases.
        const float density = cloudDensity(push, p, t < 20.0, heightFraction);
        if (density <= 0.0)
            continue;

        // Optical depth towards the sun through the clouds: six growing steps, ~1.3 km.
        float sunOpticalDepth = 0.0;
        float travelled = 0.0;
        for (int j = 0; j < 6; ++j)
        {
            const float lightStep = 0.06 * float(j + 1);
            float unused;
            sunOpticalDepth += cloudDensity(push, p + sunDirection * (travelled + 0.5 * lightStep), false, unused) * lightStep;
            travelled += lightStep;
        }

        const float r = length(p);
        const float3 up = p / r;
        const float cosSunZenith = dot(sunDirection, up);
        const float planetShadow = raySphere(p, sunDirection, BottomRadius) >= 0.0 ? 0.0 : 1.0;
        const float3 sunLight = planetShadow * sampleTransmittance(push, r, cosSunZenith);
        const float3 ambient = sampleMultiScattering(push, r, cosSunZenith) * CloudAmbientScale * lerp(0.5, 1.0, heightFraction);

        // Hillaire 2016: multiple scattering as octaves of less extinction, less energy and a wider phase.
        float scattering = 0.0;
        float extinctionScale = 1.0;
        float contribution = 1.0;
        float eccentricity = 1.0;
        for (int octave = 0; octave < 3; ++octave)
        {
            scattering += contribution * cloudPhase(cosTheta, eccentricity) * exp(-sunOpticalDepth * extinctionScale);
            extinctionScale *= 0.5;
            contribution *= 0.5;
            eccentricity *= 0.5;
        }

        // Albedo ~1: scattering = extinction. Energy-conserving step, as for the air.
        const float3 inScattered = density * (sunLight * scattering + ambient);
        const float stepTransmittance = exp(-density * dt);
        result.luminance += result.transmittance * (inScattered - inScattered * stepTransmittance) / density;
        const float absorbed = result.transmittance * (1.0 - stepTransmittance);
        weightedDistance += t * absorbed;
        weightSum += absorbed;
        result.transmittance *= stepTransmittance;
        if (result.transmittance < 0.01)
            break;
    }
    result.distance = weightSum > 0.0 ? weightedDistance / weightSum : tStart;
    return result;
}

// The sky and its clouds seen in one direction, lit by one light (the sun or the moon), per unit of that light's illuminance.
float3 skyLuminance(PushConstantSkyAtmosphere push, float3 origin, float3 direction, float3 lightDirection, float jitter,
                    out float cloudTransmittance)
{
    const ScatteringResult sky = integrateScatteredLuminance(push, origin, direction, lightDirection, 32, false, true);
    float3 luminance = sky.luminance;

    // Clouds: in front of the sky behind them, behind the air in front of them.
    const CloudResult clouds = marchClouds(push, origin, direction, lightDirection, jitter);
    cloudTransmittance = clouds.transmittance;
    if (clouds.transmittance < 0.999)
    {
        const ScatteringResult front = integrateScatteredLuminance(push, origin, direction, lightDirection, 16, false, false, clouds.distance);
        // sky = front + T_air * behind, so behind the clouds: (sky - front) dimmed by them.
        luminance = front.luminance + front.transmittance * clouds.luminance + clouds.transmittance * max(sky.luminance - front.luminance, 0.0);
    }
    return luminance;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void skyCubemapMain(uint3 id : SV_DispatchThreadID, uniform PushConstantSkyAtmosphere push)
{
    if (id.x >= push.outputSize || id.y >= push.outputSize)
        return;

    const uint3 texel = uint3(id.xy, id.z + push.firstFace);
    const float2 centerSt = (float2(id.xy) + 0.5) / float(push.outputSize) * 2.0 - 1.0;

    // The world's +Y is up; the camera stands at the planet's north pole, cameraAltitudeKm above the ground.
    const float3 origin = float3(0.0, BottomRadius + max(push.cameraAltitudeKm, PlanetRadiusOffset), 0.0);
    const uint hash = cloudHashPcg3d(texel).x;

    // Near the horizon the clouds are tens of km away and smaller than a texel: two rays a texel (diagonally apart, each with its
    // own jitter) average them instead of one ray picking a random part.
    const bool horizon = abs(cubemapDirection(texel.z, centerSt).y) < 0.15;
    const uint sampleCount = horizon ? 2u : 1u;
    float3 luminance = float3(0.0);
    float cloudTransmittance = 0.0;
    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        const float2 subPixel = horizon ? (sampleIndex == 0 ? float2(0.25) : float2(0.75)) : float2(0.5);
        const float2 st = (float2(id.xy) + subPixel) / float(push.outputSize) * 2.0 - 1.0;
        const float3 direction = cubemapDirection(texel.z, st);
        const float jitter = float(((hash >> (sampleIndex * 16u)) & 0xFFFFu)) / 65536.0;
        float sampleTransmittance;
        luminance += skyLuminance(push, origin, direction, normalize(push.sunDirection), jitter, sampleTransmittance) * push.sunIlluminance;
        // At night the moon's light too (the same air and clouds, from another direction).
        if (any(push.moonIlluminance > 0.0))
        {
            float moonCloudTransmittance;
            luminance += skyLuminance(push, origin, direction, normalize(push.moonDirection), jitter, moonCloudTransmittance) * push.moonIlluminance;
        }
        cloudTransmittance += sampleTransmittance;
    }
    luminance /= float(sampleCount);
    cloudTransmittance /= float(sampleCount);

    RWTexture2DArray<float4> output = RWTexture2DArray<float4>.Handle(uint2(push.outputStorageIndex, 0));
    output[texel] = float4(luminance, cloudTransmittance);
}

// The clouds' shadow on the scene: for each point of a square of the ground around the camera (a texel of the map), the cloud
// layer's transmittance towards the light (the sun, or the moon at night) -- 16 steps of the shape noise, without the detail
// erosion (soft shadows do not show it). Every frame (the wind moves them); cloud_shadow() in shaderIO.h reads it.
[shader("compute")]
[numthreads(8, 8, 1)]
void cloudShadowMain(uint3 id : SV_DispatchThreadID, uniform PushConstantSkyAtmosphere push)
{
    if (id.x >= push.outputSize || id.y >= push.outputSize)
        return;

    const float2 uv = (float2(id.xy) + 0.5) / float(push.outputSize);
    const float2 groundKm = push.cloudShadowCenterKm + (uv * 2.0 - 1.0) * push.cloudShadowExtentKm;
    // The planet's pole is the world origin (as for the sky): a point of the ground here is that far beside it.
    const float3 origin = float3(groundKm.x, BottomRadius + max(push.cameraAltitudeKm, PlanetRadiusOffset), groundKm.y);
    const float3 lightDirection = normalize(push.sunDirection);

    float transmittance = 1.0;
    const float inner = BottomRadius + push.cloudBottomKm;
    const float outer = inner + max(push.cloudThicknessKm, 1e-3);
    float tStart, tEnd;
    if (push.cloudShapeTextureIndex != 0xFFFFFFFF && lightDirection.y > 0.0 && cloudLayerSegment(origin, lightDirection, inner, outer, tStart, tEnd))
    {
        tEnd = min(tEnd, tStart + 20.0); // a low light crosses the layer for long: the first 20 km of it
        const int steps = 16;
        const float dt = (tEnd - tStart) / float(steps);
        float opticalDepth = 0.0;
        for (int i = 0; i < steps; ++i)
        {
            float heightFraction;
            opticalDepth += cloudDensity(push, origin + lightDirection * (tStart + (float(i) + 0.5) * dt), false, heightFraction) * dt;
        }
        transmittance = exp(-opticalDepth);
    }
    RWTexture2DArray<float> output = RWTexture2DArray<float>.Handle(uint2(push.outputStorageIndex, 0));
    output[uint3(id.xy, 0)] = transmittance;
}

// A mip of the environment cubemap from the one above: the centre of a texel here is the shared corner of four texels
// there, so one bilinear read averages them (the clouds need filtered mips; raymarching each mip would alias them).
[shader("compute")]
[numthreads(16, 16, 1)]
void downsampleCubeMain(uint3 id : SV_DispatchThreadID, uniform PushConstantSkyAtmosphere push)
{
    if (id.x >= push.outputSize || id.y >= push.outputSize)
        return;
    const float2 st = (float2(id.xy) + 0.5) / float(push.outputSize) * 2.0 - 1.0;
    TextureCube source = TextureCube.Handle(uint2(push.sourceTextureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    // Alpha too: the clouds' transmittance (skyCubemapMain).
    const float4 color = source.SampleLevel(linearClamp, cubemapDirection(id.z, st), push.sourceLod);
    RWTexture2DArray<float4> output = RWTexture2DArray<float4>.Handle(uint2(push.outputStorageIndex, 0));
    output[id] = color;
}

// Aerial perspective (the paper's camera volume, UE's "aerial perspective LUT"): for each froxel column the camera's view
// ray is marched through the atmosphere once, writing the light scattered towards the camera and the transmittance at the
// end of every depth slice. Deferred lighting looks it up by the pixel's distance: color * transmittance + scattered.
[shader("compute")]
[numthreads(8, 8, 1)]
void aerialPerspectiveMain(uint3 id : SV_DispatchThreadID, uniform PushConstantAerialPerspective ap)
{
    if (id.x >= AerialPerspectiveSize || id.y >= AerialPerspectiveSize)
        return;

    UniformBufferObject* ubo = (UniformBufferObject*)ap.matrixReference;

    // The sky's functions take the sky's push constants: the same atmosphere.
    PushConstantSkyAtmosphere atmosphere = (PushConstantSkyAtmosphere)0;
    atmosphere.transmittanceTextureIndex = ap.transmittanceTextureIndex;
    atmosphere.multiScatteringTextureIndex = ap.multiScatteringTextureIndex;
    atmosphere.rayleighScale = ap.rayleighScale;
    atmosphere.mieScale = ap.mieScale;
    atmosphere.ozoneScale = ap.ozoneScale;
    atmosphere.mieAnisotropy = ap.mieAnisotropy;

    // The froxel column's view direction, with the same screen convention as DeferredLighting.slang's reconstruction. A
    // point well in front of the camera, not at the near plane (precision, see Skybox.slang).
    const float2 uv = (float2(id.xy) + 0.5) / float(AerialPerspectiveSize);
    const float4 farPoint = mul(ubo->invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.001, 1.0));
    const float3 direction = normalize(farPoint.xyz / farPoint.w - ubo->cameraWorldPos.xyz);

    // The camera in the atmosphere: the world origin AltitudeKm above the ground, +Y up, kept above the ground.
    float3 cameraKm = ubo->cameraWorldPos.xyz * ap.kmPerWorldUnit;
    cameraKm.y = max(cameraKm.y + BottomRadius + ap.altitudeKm, BottomRadius + PlanetRadiusOffset);

    const float3 sunDirection = normalize(ap.sunDirection);
    const float cosTheta = dot(direction, sunDirection);
    const float phaseRayleigh = rayleighPhase(cosTheta);
    const float phaseMie = miePhase(ap.mieAnisotropy, cosTheta);
    // The moon at night: the same air scattering its light from its own direction.
    const bool moon = any(ap.moonIlluminance > 0.0);
    const float3 moonDirection = normalize(ap.moonDirection);
    const float moonCosTheta = dot(direction, moonDirection);
    const float moonPhaseRayleigh = rayleighPhase(moonCosTheta);
    const float moonPhaseMie = miePhase(ap.mieAnisotropy, moonCosTheta);

    RWTexture2DArray<float4> output = RWTexture2DArray<float4>.Handle(uint2(ap.outputStorageIndex, 0));
    float3 scattered = float3(0.0);
    float3 moonScattered = float3(0.0);
    float3 throughput = float3(1.0);
    float sliceStart = 0.0;
    for (uint slice = 0; slice < AerialPerspectiveSlices; ++slice)
    {
        const float sliceFraction = float(slice + 1) / float(AerialPerspectiveSlices);
        const float sliceEnd = AerialPerspectiveMaxDistanceKm * sliceFraction * sliceFraction;
        const int samplesPerSlice = 2;
        for (int s = 0; s < samplesPerSlice; ++s)
        {
            const float dt = (sliceEnd - sliceStart) / float(samplesPerSlice);
            const float t = sliceStart + (float(s) + 0.5) * dt;
            const float3 position = cameraKm + direction * t;
            // Below the ground (a view ray going down past where geometry would be): the air at ground level.
            const float r = max(length(position), BottomRadius + PlanetRadiusOffset);
            const float3 up = normalize(position);
            const float cosSunZenith = dot(sunDirection, up);
            const Medium medium = sampleMedium(r - BottomRadius, atmosphere);

            const float3 sunTransmittance = sampleTransmittance(atmosphere, r, cosSunZenith);
            const float planetShadow = raySphere(up * r, sunDirection, BottomRadius) >= 0.0 ? 0.0 : 1.0;
            const float3 multiScattered = sampleMultiScattering(atmosphere, r, cosSunZenith);
            const float3 inScattered = planetShadow * sunTransmittance * (medium.rayleigh * phaseRayleigh + medium.mie * phaseMie) +
                                       multiScattered * medium.scattering;

            const float3 segmentTransmittance = exp(-medium.extinction * dt);
            const float3 extinction = max(medium.extinction, float3(1e-7));
            scattered += throughput * (inScattered - inScattered * segmentTransmittance) / extinction;
            if (moon)
            {
                const float cosMoonZenith = dot(moonDirection, up);
                const float moonShadow = raySphere(up * r, moonDirection, BottomRadius) >= 0.0 ? 0.0 : 1.0;
                const float3 moonInScattered =
                    moonShadow * sampleTransmittance(atmosphere, r, cosMoonZenith) * (medium.rayleigh * moonPhaseRayleigh + medium.mie * moonPhaseMie) +
                    sampleMultiScattering(atmosphere, r, cosMoonZenith) * medium.scattering;
                moonScattered += throughput * (moonInScattered - moonInScattered * segmentTransmittance) / extinction;
            }
            throughput *= segmentTransmittance;
        }
        sliceStart = sliceEnd;
        output[uint3(id.xy, slice)] = float4(scattered * ap.sunIlluminance + moonScattered * ap.moonIlluminance, dot(throughput, float3(1.0 / 3.0)));
    }
}
