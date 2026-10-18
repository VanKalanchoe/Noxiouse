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

            // Unproject near-plane point (depth = 1.0 in Reverse-Z) to world space
            // Unproject far along the ray (reverse-Z depth 0.001), not at the near plane: minus the camera position, a near-plane
            // point (0.01 units away) loses most of its float digits and the sky direction wobbled by up to a pixel per frame.
            float4 unprojected = mul(ubo->invViewProj, float4(screenPos, 0.001, 1.0));
            vertices[threadID].worldRay = unprojected.xyz / unprojected.w - ubo->cameraWorldPos.xyz;
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
        float3 envSum = float3(0.0, 0.0, 0.0);
        float3 sunSum = float3(0.0, 0.0, 0.0); // the disc's edge is antialiased by the same 16 directions
        [unroll]
        for (int y = 0; y < 4; y++)
        {
            [unroll]
            for (int x = 0; x < 4; x++)
            {
                float2 offset = (float2(x, y) + 0.5) / 4.0 - 0.5;
                float3 direction = normalize(ray + rayDx * offset.x + rayDy * offset.y);
                envSum += texHandle.SampleLevel(sampHandle, direction, 0.0).rgb;
                sunSum += sunDisc(pushConstant, direction);
            }
        }
        // Radiance Scale (scaleIBLAmbient) scales the environment, not the sun: that is the directional light's intensity.
        float3 envColor = (envSum / 16.0) * ubo->scaleIBLAmbient + sunSum / 16.0;
        // Height fog over the sky (and the sun): seen to "infinity", thick at the horizon, thin overhead.
        envColor = applyHeightFog(ubo, envColor, normalize(ray), 1e30);

        output.color = float4(envColor, 1.0);
        output.entityID = -1;

        return output;
    }