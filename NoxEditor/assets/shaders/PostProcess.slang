import Tonemapping;
    #include "shaderIO.h"


    struct PostProcessPayload
    {
        uint dummy;
    };
    groupshared PostProcessPayload gPayload;

    [shader("task")]
    [numthreads(1, 1, 1)]
    void taskMain()
    {
        DispatchMesh(1, 1, 1, gPayload);
    }

    struct MSOutput
    {
        float4 pos : SV_Position;
        float2 texCoord : TEXCOORD0;
    };

    static const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    static const float2 uvs[3] =
    {
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
        uint threadID : SV_GroupIndex
    )
    {
        SetMeshOutputCounts(3, 1);

        if (threadID < 3)
        {
            vertices[threadID].pos = float4(positions[threadID], 0.0, 1.0);
            vertices[threadID].texCoord = uvs[threadID];
        }

        if (threadID == 0)
        {
            triangles[0] = uint3(0, 1, 2);
        }
    }

    [shader("fragment")]
    float4 fragMain(
        MSOutput input,
        uniform PushConstantPostProcess pushConstant
    ) : SV_Target
    {
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
        int2 pixelCoord = int2(input.pos.xy);

        Texture2D hdrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.hdrTextureIndex, 0));
        float4 hdrColor = hdrTex.Load(int3(pixelCoord, 0));
 
        // When inspecting debug views (Albedo, Normals, Roughness, EntityID, DepthBuffer), pass through directly!
        if (pushConstant.debugMode != 0)
        {
            return hdrColor;
        }

        // Post effects (PP1; each off at 0 / 0xFFFFFFFF): chromatic aberration, bloom and lens flares, colour grading before the
        // tonemapper; sharpening, the grading LUT, vignette and film grain after it.
        uint width, height, levels;
        hdrTex.GetDimensions(0, width, height, levels);
        const float2 size = float2(width, height);
        const float2 uv = (float2(pixelCoord) + 0.5) / size;
        SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
        const float3 unfiltered = hdrColor.rgb;
        if (pushConstant.chromaticAberration > 0.0)
        {
            // Red and blue pulled apart along the line to the centre, more towards the edges (a lens' lateral aberration).
            const float2 shift = (uv - 0.5) * (0.01 * pushConstant.chromaticAberration);
            hdrColor.r = hdrTex.SampleLevel(linearClamp, uv - shift, 0.0).r;
            hdrColor.b = hdrTex.SampleLevel(linearClamp, uv + shift, 0.0).b;
        }
        if (pushConstant.bloomTextureIndex != 0xFFFFFFFF)
        {
            Texture2D bloomTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.bloomTextureIndex, 0));
            hdrColor.rgb = lerp(hdrColor.rgb, bloomTex.SampleLevel(linearClamp, uv, 0.0).rgb, saturate(pushConstant.bloomIntensity));
            if (pushConstant.lensFlareIntensity > 0.0)
            {
                // Image-based flares (UE's): the blurred image mirrored through the centre at a few scales (ghosts, tinted), and a halo.
                const float2 flipped = 1.0 - uv;
                const float2 ghostStep = (0.5 - flipped) * 0.35;
                const float3 tints[4] = { float3(1.0, 0.8, 0.4), float3(0.5, 1.0, 0.6), float3(0.4, 0.6, 1.0), float3(1.0, 0.5, 0.8) };
                float3 flare = float3(0.0);
                for (uint ghost = 0; ghost < 4; ++ghost)
                {
                    const float2 g = flipped + ghostStep * float(ghost);
                    const float fade = pow(saturate(1.0 - length(0.5 - g) / 0.7071), 8.0);
                    flare += bloomTex.SampleLevel(linearClamp, g, 3.0).rgb * fade * tints[ghost];
                }
                const float2 haloUV = flipped + normalize(ghostStep + float2(1e-6, 0.0)) * 0.45;
                flare += bloomTex.SampleLevel(linearClamp, haloUV, 4.0).rgb * pow(saturate(1.0 - length(0.5 - haloUV) / 0.7071), 4.0) * 0.5;
                hdrColor.rgb += flare * pushConstant.lensFlareIntensity;
            }
        }
        // Grading in the scene's light (before the tonemapper): saturation around the luminance, contrast around mid grey in log space.
        const float luma = dot(max(hdrColor.rgb, float3(0.0)), float3(0.2126, 0.7152, 0.0722));
        hdrColor.rgb = max(lerp(float3(luma), hdrColor.rgb, pushConstant.saturation), float3(0.0));
        if (abs(pushConstant.contrast - 1.0) > 1e-3)
            hdrColor.rgb = 0.18 * pow(hdrColor.rgb / 0.18, pushConstant.contrast);

        // Mode 0 (Full Lit PBR): Apply Tonemapping (Exposure + Khronos PBR Neutral + Gamma)
        TonemapParameters tonemapParams;
        // The adapted average (one texel), or a luminance pyramid's last mip (extra views), to the key (scene_exposure: brightening
        // is only partly made up, as the eye does it -- a night still looks like night instead of a grey day).
        tonemapParams.exposure = scene_exposure(ubo, pushConstant.autoExposureEnabled != 0 ? pushConstant.autoExposureTextureIndex : 0xFFFFFFFF,
                                                pushConstant.exposureCompensation, pushConstant.autoExposureMinEV, pushConstant.autoExposureMaxEV);
        tonemapParams.gamma = ubo->gamma;
        tonemapParams.mode = (int)pushConstant.tonemapMode;;


        float3 ldrColor = toneMap(hdrColor.rgb, tonemapParams);

        // Sharpening (AMD FidelityFX CAS) on the tonemapped image: the pixel against its cross of neighbours, less where they are already
        // contrasty; its change is added to the graded image.
        if (pushConstant.sharpen > 0.0)
        {
            const int2 last = int2(size) - 1;
            const float3 c = toneMap(unfiltered, tonemapParams);
            const float3 n = toneMap(hdrTex.Load(int3(clamp(pixelCoord + int2(0, -1), int2(0), last), 0)).rgb, tonemapParams);
            const float3 so = toneMap(hdrTex.Load(int3(clamp(pixelCoord + int2(0, 1), int2(0), last), 0)).rgb, tonemapParams);
            const float3 w = toneMap(hdrTex.Load(int3(clamp(pixelCoord + int2(-1, 0), int2(0), last), 0)).rgb, tonemapParams);
            const float3 e = toneMap(hdrTex.Load(int3(clamp(pixelCoord + int2(1, 0), int2(0), last), 0)).rgb, tonemapParams);
            const float3 mn = min(c, min(min(n, so), min(w, e)));
            const float3 mx = max(c, max(max(n, so), max(w, e)));
            const float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-5)));
            const float3 weight = -amp / lerp(8.0, 5.0, saturate(pushConstant.sharpen));
            const float3 sharpened = (c + weight * (n + so + w + e)) / (1.0 + 4.0 * weight);
            ldrColor = max(ldrColor + (sharpened - c), float3(0.0));
        }
        // The grading LUT (a strip of size slices of size x size, blue across the slices), in display space.
        if (pushConstant.lutTextureIndex != 0xFFFFFFFF && pushConstant.lutStrength > 0.0)
        {
            Texture2D lut = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.lutTextureIndex, 0));
            uint lutWidth, lutHeight, lutLevels;
            lut.GetDimensions(0, lutWidth, lutHeight, lutLevels);
            const float n = float(lutHeight);
            const float3 c = saturate(ldrColor) * (n - 1.0);
            const float slice0 = floor(c.b);
            const float slice1 = min(slice0 + 1.0, n - 1.0);
            const float v = (c.g + 0.5) / n;
            const float3 a = lut.SampleLevel(linearClamp, float2((slice0 * n + c.r + 0.5) / (n * n), v), 0.0).rgb;
            const float3 b = lut.SampleLevel(linearClamp, float2((slice1 * n + c.r + 0.5) / (n * n), v), 0.0).rgb;
            ldrColor = lerp(ldrColor, lerp(a, b, c.b - slice0), saturate(pushConstant.lutStrength));
        }
        // Vignette: darker towards the corners (the frame's aspect kept round).
        if (pushConstant.vignetteIntensity > 0.0)
        {
            const float2 d = (uv - 0.5) * float2(size.x / size.y, 1.0);
            ldrColor *= 1.0 - saturate(pushConstant.vignetteIntensity) * smoothstep(0.25, 1.0, length(d) * 1.2);
        }
        // Film grain: per pixel and frame, strongest in the mid tones.
        if (pushConstant.filmGrainIntensity > 0.0)
        {
            const float2 p = float2(pixelCoord) + float(pushConstant.frameIndex % 1024u) * float2(17.17, 11.31);
            const float grain = frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
            const float midTones = 1.0 - abs(2.0 * saturate(dot(ldrColor, float3(0.2126, 0.7152, 0.0722))) - 1.0);
            ldrColor = max(ldrColor + grain * pushConstant.filmGrainIntensity * 0.2 * midTones, float3(0.0));
        }
        return float4(ldrColor, 1.0);
    }
