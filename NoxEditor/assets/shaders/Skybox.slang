#include "shaderIO.h"

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
        [unroll]
        for (int y = 0; y < 4; y++)
        {
            [unroll]
            for (int x = 0; x < 4; x++)
            {
                float2 offset = (float2(x, y) + 0.5) / 4.0 - 0.5;
                float3 direction = normalize(ray + rayDx * offset.x + rayDy * offset.y);
                envSum += texHandle.SampleLevel(sampHandle, direction, 0.0).rgb;
            }
        }
        float3 envColor = (envSum / 16.0) * ubo->scaleIBLAmbient;

        output.color = float4(envColor, 1.0);
        output.entityID = -1;

        return output;
    }