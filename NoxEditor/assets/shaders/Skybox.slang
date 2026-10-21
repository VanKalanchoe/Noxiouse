#include "shaderIO.h"
#include "SkyAtmosphereCommon.slang"
#include "HeightFog.slang"

    struct SkyboxPayload
    {
        uint dummy;
    };
    groupshared SkyboxPayload gPayload;

    [shader("task")]
    [numthreads(1, 1, 1)]
    void taskMain()
    {
        DispatchMesh(1, 1, 1, gPayload);
    }

    struct MSOutput
    {
        float4 pos : SV_Position;
        float3 worldRay : TEXCOORD0;
    };

    // Fullscreen triangle covering the screen in NDC
    static const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    float3 unprojectPoint(UniformBufferObject* ubo, float2 ndc, float depth)
    {
        const float4 point = mul(ubo->invViewProj, float4(ndc, depth, 1.0));
        return point.xyz / point.w;
    }

    // The direction of the sky seen at a screen position. Unprojected far along the ray (reverse-Z depth 0.001), not at the near
    // plane: minus the camera position, a near-plane point (0.01 units away) loses most of its float digits and the sky direction
    // wobbled by up to a pixel per frame. An orthographic view's rays are all parallel, so its whole sky would be one color: it
    // gets the sky a 60 degree perspective camera looking the same way would see.
    float3 skyRay(UniformBufferObject* ubo, float2 ndc)
    {
        const float3 centreForward = normalize(unprojectPoint(ubo, float2(0.0), 0.001) - unprojectPoint(ubo, float2(0.0), 0.5));
        const float3 cornerForward = normalize(unprojectPoint(ubo, float2(1.0), 0.001) - unprojectPoint(ubo, float2(1.0), 0.5));
        if (dot(centreForward, cornerForward) > 0.999999)
        {
            const float3 centre = unprojectPoint(ubo, float2(0.0), 0.001);
            const float3 right = unprojectPoint(ubo, float2(1.0, 0.0), 0.001) - centre; // half the view's width
            const float3 up = unprojectPoint(ubo, float2(0.0, 1.0), 0.001) - centre;    // half its height
            const float tanHalfFov = 0.57735; // tan(30 degrees)
            return centreForward + (right * ndc.x + up * ndc.y) * (tanHalfFov / max(length(up), 1e-6));
        }
        return unprojectPoint(ubo, ndc, 0.001) - ubo->cameraWorldPos.xyz;
    }

    [shader("mesh")]
    [outputtopology("triangle")]
    [numthreads(3, 1, 1)]
    void meshMain(
        out indices uint3 triangles[1],
        out vertices MSOutput vertices[3],
        uint threadID : SV_GroupIndex,
        uniform PushConstantSkybox pushConstant
    )
    {
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

        SetMeshOutputCounts(3, 1);

        if (threadID < 3)
        {
            float2 screenPos = positions[threadID];

            // REVERSE-Z: Output depth at 0.0 (Far Plane), w = 1.0 (never clipped!)
            vertices[threadID].pos = float4(screenPos, 0.0, 1.0);

            // Linear in the screen position either way: interpolated across the triangle like the position.
            vertices[threadID].worldRay = skyRay(ubo, screenPos);
        }

        if (threadID == 0)
        {
            triangles[0] = uint3(0, 1, 2);
        }
    }

    // The sun's disc over a procedural sky: the directional light's sun, dimmed and reddened by the air in front of it, with
    // limb darkening (the real sun's edge is darker than its centre). None for an HDR environment or behind the planet.
    float3 sunDisc(PushConstantSkybox pushConstant, float3 direction)
    {
        if (pushConstant.skyTransmittanceIndex == 0xFFFFFFFF)
            return float3(0.0);
        const float cosAngle = dot(direction, pushConstant.sunDirection);
        if (cosAngle < pushConstant.sunCosAngularRadius)
            return float3(0.0);
        const float r = BottomRadius + max(pushConstant.skyAltitudeKm, 0.01);
        if (raySphere(float3(0.0, r, 0.0), direction, BottomRadius) >= 0.0)
            return float3(0.0);

        const float sinAngle = sqrt(max(0.0, 1.0 - cosAngle * cosAngle));
        const float sinRadius = sqrt(max(1e-8, 1.0 - pushConstant.sunCosAngularRadius * pushConstant.sunCosAngularRadius));
        const float fromCentre = saturate(sinAngle / sinRadius);
        const float limb = 1.0 - 0.6 * (1.0 - sqrt(1.0 - fromCentre * fromCentre));
        // The camera stands at the planet's pole (+Y up): the direction's y is the cosine of its zenith angle.
        return pushConstant.sunDiscRadiance * limb * sampleSkyTransmittance(pushConstant.skyTransmittanceIndex, r, direction.y);
    }

    // The air's transmittance from the viewer towards a direction of the sky; 0 below the horizon.
    float3 skyTransmittanceTowards(PushConstantSkybox pushConstant, float3 direction)
    {
        const float r = BottomRadius + max(pushConstant.skyAltitudeKm, 0.01);
        if (raySphere(float3(0.0, r, 0.0), direction, BottomRadius) >= 0.0)
            return float3(0.0);
        // The camera stands at the planet's pole (+Y up): the direction's y is the cosine of its zenith angle.
        return sampleSkyTransmittance(pushConstant.skyTransmittanceIndex, r, direction.y);
    }

    float moonHash(float3 p)
    {
        p = frac(p * 0.3183099 + 0.1) * 17.0;
        return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
    }

    float moonValueNoise(float3 x)
    {
        const float3 cell = floor(x);
        const float3 f = x - cell;
        const float3 u = f * f * (3.0 - 2.0 * f);
        return lerp(lerp(lerp(moonHash(cell), moonHash(cell + float3(1, 0, 0)), u.x),
                         lerp(moonHash(cell + float3(0, 1, 0)), moonHash(cell + float3(1, 1, 0)), u.x), u.y),
                    lerp(lerp(moonHash(cell + float3(0, 0, 1)), moonHash(cell + float3(1, 0, 1)), u.x),
                         lerp(moonHash(cell + float3(0, 1, 1)), moonHash(cell + float3(1, 1, 1)), u.x), u.y), u.z);
    }

    // The moon's disc: a sphere lit by the sun (so it has phases; full when opposite the sun), its dark seas (maria) a noise on
    // the sphere. Lommel-Seeliger shading: the dusty surface looks flat when full, not darkening towards its edge like a ball.
    float3 moonDisc(PushConstantSkybox pushConstant, float3 direction)
    {
        if (pushConstant.skyTransmittanceIndex == 0xFFFFFFFF || all(pushConstant.moonDiscRadiance <= 0.0))
            return float3(0.0);
        const float3 moon = pushConstant.moonDirection;
        const float cosAngle = dot(direction, moon);
        if (cosAngle < pushConstant.moonCosAngularRadius)
            return float3(0.0);

        // The point of the sphere seen in this direction: its offset from the disc's centre in disc radii, on the side facing us.
        const float sinRadius = sqrt(max(1e-8, 1.0 - pushConstant.moonCosAngularRadius * pushConstant.moonCosAngularRadius));
        const float3 offset = (direction - moon * cosAngle) / sinRadius;
        const float3 normal = normalize(offset - moon * sqrt(saturate(1.0 - dot(offset, offset))));
        const float lightCos = saturate(dot(normal, pushConstant.sunDirection));
        const float viewCos = saturate(dot(normal, -moon));
        const float lit = 2.0 * lightCos / max(lightCos + viewCos, 1e-4); // 1 all over a full moon
        const float maria = 0.6 * moonValueNoise(normal * 3.0) + 0.3 * moonValueNoise(normal * 7.0) + 0.1 * moonValueNoise(normal * 17.0);
        const float albedo = lerp(0.55, 1.1, smoothstep(0.35, 0.65, maria));
        return pushConstant.moonDiscRadiance * lit * albedo * skyTransmittanceTowards(pushConstant, direction);
    }

    uint starHash(uint3 v)
    {
        v = v * 1664525u + 1013904223u;
        v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
        v ^= v >> 16u;
        v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
        return v.x ^ v.y ^ v.z;
    }

    float starRandom(inout uint state)
    {
        state = state * 747796405u + 2891336453u;
        const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        return float((word >> 22u) ^ word) * (1.0 / 4294967296.0);
    }

    // A star field fixed to the world: one star in most cells of a grid on the cube's faces (256 x 256 each), somewhere inside
    // its cell, brightness from a power law (many faint, few bright, like magnitudes) and a color from blue-white to orange.
    // Each is a small gaussian at least a pixel wide (pixelAngle, radians), its light the same whatever its size: no aliasing.
    float3 starField(PushConstantSkybox pushConstant, float3 direction, float pixelAngle)
    {
        if (pushConstant.skyTransmittanceIndex == 0xFFFFFFFF || pushConstant.starIntensity <= 0.0)
            return float3(0.0);
        const float3 a = abs(direction);
        uint face;
        float2 uv;
        if (a.x >= a.y && a.x >= a.z) { face = direction.x > 0.0 ? 0u : 1u; uv = direction.yz / a.x; }
        else if (a.y >= a.z)         { face = direction.y > 0.0 ? 2u : 3u; uv = direction.xz / a.y; }
        else                         { face = direction.z > 0.0 ? 4u : 5u; uv = direction.xy / a.z; }

        const float cells = 256.0;
        const uint2 cell = min(uint2((uv * 0.5 + 0.5) * cells), uint2(cells - 1.0));
        uint state = starHash(uint3(cell, face));
        if (starRandom(state) > 0.7)
            return float3(0.0);
        // Inside the cell with a margin: its gaussian stays inside it, so only this cell needs looking at.
        const float2 starUv = (float2(cell) + 0.2 + 0.6 * float2(starRandom(state), starRandom(state))) / cells * 2.0 - 1.0;
        float3 starDirection;
        const float side = (face & 1u) == 0u ? 1.0 : -1.0;
        if (face < 2u)      starDirection = float3(side, starUv.x, starUv.y);
        else if (face < 4u) starDirection = float3(starUv.x, side, starUv.y);
        else                starDirection = float3(starUv.x, starUv.y, side);
        starDirection = normalize(starDirection);

        // Flux: a Pareto tail over ten magnitudes (count ~ flux^-1.2); the brightest about a full moon's / 25000.
        const float brightest = 1.5e-6;
        const float flux = min(brightest * 1e-4 * pow(max(starRandom(state), 1e-6), -1.0 / 1.2), brightest);
        const float temperature = starRandom(state);
        float3 color = lerp(float3(1.0, 0.72, 0.5), float3(0.72, 0.82, 1.0), temperature);
        color /= dot(color, float3(0.2126, 0.7152, 0.0722));

        const float sigma = clamp(pixelAngle * 0.6, 2e-4, 0.2 / cells);
        const float angleSq = max(2.0 * (1.0 - dot(direction, starDirection)), 0.0);
        const float spread = exp(-0.5 * angleSq / (sigma * sigma)) / (2.0 * 3.14159265 * sigma * sigma);
        return color * flux * spread * pushConstant.starIntensity * skyTransmittanceTowards(pushConstant, direction);
    }

    struct FSOutput
    {
        float4 color : SV_TARGET0;
        int entityID : SV_TARGET1;
    };

    [shader("fragment")]
    FSOutput fragMain(MSOutput input, uniform PushConstantSkybox pushConstant)
    {
        FSOutput output;

        // Every other shader in this codebase offsets bindless texture indices by
        // ubo->imageHeapIndexOffset before constructing a Handle - this one never did, so it was
        // reading whatever happens to sit at the raw, un-offset slot in the descriptor heap
        // instead of the actual environment cubemap.
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
        TextureCube texHandle = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.cubemapIndex, 0));
        SamplerState sampHandle = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));

        // The environment cubemap is a 4k BC6H DDS with NO mip chain: sampled once at level 0 it is minified 3-4x on screen
        // with no filtering, so its texels crawl and its bright HDR details flicker whenever the camera turns (jitter that
        // only shows while moving). Average a 4x4 grid across the pixel's footprint in direction space instead.
        float3 ray = input.worldRay;
        float3 rayDx = ddx(ray);
        float3 rayDy = ddy(ray);
        float4 envSum = float4(0.0, 0.0, 0.0, 0.0);
        float3 discSum = float3(0.0, 0.0, 0.0); // the sun's and moon's edges are antialiased by the same 16 directions
        [unroll]
        for (int y = 0; y < 4; y++)
        {
            [unroll]
            for (int x = 0; x < 4; x++)
            {
                float2 offset = (float2(x, y) + 0.5) / 4.0 - 0.5;
                float3 direction = normalize(ray + rayDx * offset.x + rayDy * offset.y);
                float4 environment = texHandle.SampleLevel(sampHandle, direction, 0.0);
                // Near the horizon a procedural sky's band (its far clouds at the screen's resolution), fading into the cubemap at
                // its top. Its bottom is under the horizon (the ground), where both agree.
                if (pushConstant.horizonBandIndex != 0xFFFFFFFF)
                {
                    const float2 bandUv = horizonBandUv(direction);
                    const float bandWeight = saturate((1.0 - bandUv.y) * 6.0) * step(0.0, bandUv.y);
                    if (bandWeight > 0.0)
                    {
                        Texture2D band = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.horizonBandIndex, 0));
                        // Repeat across the azimuth's seam, clamped in elevation (the half texel at the bottom edge).
                        const float2 uv = float2(bandUv.x, clamp(bandUv.y, 0.5 / float(SkyHorizonBandHeight), 1.0 - 0.5 / float(SkyHorizonBandHeight)));
                        const float4 bandSample = band.SampleLevel(SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0)), uv, 0.0);
                        environment = lerp(environment, bandSample, bandWeight);
                    }
                }
                envSum += environment;
                discSum += sunDisc(pushConstant, direction) + moonDisc(pushConstant, direction);
            }
        }
        // A procedural sky's alpha is its clouds' transmittance: the discs and the stars show through it (1 for an HDR map).
        const float cloudTransmittance = saturate(envSum.a / 16.0);
        const float pixelAngle = max(length(rayDx), length(rayDy)) / max(length(ray), 1e-6);
        const float3 lights = discSum / 16.0 + starField(pushConstant, normalize(ray), pixelAngle);
        // Radiance Scale (scaleIBLAmbient) scales the environment, not the sun: that is the directional light's intensity.
        float3 envColor = (envSum.rgb / 16.0) * ubo->scaleIBLAmbient + lights * cloudTransmittance;
        // Height fog over the sky (and the sun): seen to "infinity", thick at the horizon, thin overhead.
        envColor = applyHeightFog(ubo, envColor, normalize(ray), 1e30);

        output.color = float4(envColor, 1.0);
        output.entityID = -1;

        return output;
    }