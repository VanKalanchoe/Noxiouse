#include "shaderIO.h"
#include "SkyAtmosphereCommon.slang"

// Procedural sky: Sebastien Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020),
// the model behind Unreal's SkyAtmosphere, after the paper's reference implementation (github.com/sebh/UnrealEngineSkyAtmosphere).
// Earth's atmosphere as Rayleigh (air), Mie (aerosols) and ozone layers over a spherical planet, lengths in km. Three passes:
//   1. transmittanceMain:   transmittance from a height towards the top of the atmosphere, for every height and zenith angle.
//   2. multiScatteringMain: the paper's multiple-scattering approximation (Psi_ms, eq. 10), per height and sun angle.
//   3. skyCubemapMain:      the sky radiance seen from the ground in every direction, written into one mip of the
//                           environment cubemap per dispatch (every mip raymarched directly: the sky is smooth).
// The cubemap is the scene's environment like an HDR map would be (skybox, IBL, reflections, DDGI and path tracer misses),
// so raster and path tracer see the same sky. The sun's disc is not in it (the directional light already lights the scene
// with the sun, IBL would count it twice): Skybox.slang draws the disc.

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

    RWTexture2DArray<float4> output = RWTexture2DArray<float4>.Handle(uint2(ap.outputStorageIndex, 0));
    float3 scattered = float3(0.0);
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
            throughput *= segmentTransmittance;
        }
        sliceStart = sliceEnd;
        output[uint3(id.xy, slice)] = float4(scattered * ap.sunIlluminance, dot(throughput, float3(1.0 / 3.0)));
    }
}
