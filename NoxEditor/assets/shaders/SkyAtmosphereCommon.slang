#pragma once

// What the procedural sky's passes (SkyAtmosphere.slang) and the skybox's sun disc (Skybox.slang) share: the planet, and
// how the transmittance LUT is addressed. Lengths in km, the planet's center at the origin, +Y up.

static const float SkyPI = 3.14159265358979;

static const float BottomRadius = 6360.0; // km
static const float TopRadius = 6460.0;    // km: 100 km of atmosphere

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

// Transmittance from height r (km from the planet's center) towards the top of the atmosphere along a direction whose
// zenith angle has cosine mu. Below the horizon the LUT's edge value: callers test the planet's shadow themselves.
float3 sampleSkyTransmittance(uint lutIndex, float r, float mu)
{
    Texture2D lut = Texture2D.Handle(uint2(lutIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    return lut.SampleLevel(linearClamp, transmittanceRMuToUv(r, mu), 0.0).rgb;
}
