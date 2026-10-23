// Screen-probe GI, 5/5: per pixel (ScreenProbeCommon.slang). The four probes around the pixel (bilinear over the probe grid,
// which moves with the frame's jitter), each weighted by how much it lies on the pixel's surface and faces its way, read
// towards the pixel's normal (its normal map included). A pixel no nearby probe speaks for (a thin pole, an inside corner)
// looks a tile further, then falls back to the DDGI cascades / the environment. Then accumulated over frames: last frame's
// result where this surface was (motion vectors, checked against last frame's depth and normal), blended with this frame's
// by 1 / (frames accumulated), up to maxHistory.
// Outputs: irradiance / pi (rgb), frames accumulated (a) -- the texture the deferred lighting reads -- and the distance from the
// camera of the surface each pixel is for (0: sky), with which the forward and ray-hit shading find out whether a point they
// shade is that surface (SceneGI.slang).
#include "ScreenProbeCommon.slang"

struct ScreenProbeIntegrateOutput
{
    float4 irradiance : SV_Target0;
    float distance : SV_Target1;
};

float3 screen_probe_fallback(UniformBufferObject* ubo, float3 position, float3 normal, float3 geometricNormal)
{
    if (ubo->enableDDGI != 0 && ubo->ddgiIrradianceTextureIndex != 0xFFFFFFFF && ubo->ddgiDistanceTextureIndex != 0xFFFFFFFF)
    {
        Texture2D irradianceAtlas = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiIrradianceTextureIndex, 0));
        Texture2D distanceAtlas = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiDistanceTextureIndex, 0));
        SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
        const float3 toCamera = normalize(ubo->cameraWorldPos.xyz - position);
        return sample_ddgi_diffuse_surface(ubo, position, normal, geometricNormal, toCamera, irradianceAtlas, distanceAtlas, linearClamp);
    }
    if (ubo->irradianceMapIndex != 0xFFFFFFFF)
    {
        TextureCube irradiance = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
        SamplerState irradianceSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
        return irradiance.SampleLevel(irradianceSampler, normal, 0).rgb * ubo->scaleIBLAmbient;
    }
    return float3(0.0);
}

[shader("fragment")]
ScreenProbeIntegrateOutput fragMain(ScreenProbeVertex input, uniform PushConstantScreenProbe pushConstant)
{
    ScreenProbeIntegrateOutput output;
    output.irradiance = float4(0.0);
    output.distance = 0.0;

    const int2 pixel = int2(input.pos.xy);
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    const float depth = depthTex.Load(int3(pixel, 0));
    if (depth <= 0.0)
        return output;

    const float2 pixelCenter = float2(pixel) + 0.5;
    const float3 position = screen_probe_world_position(pushConstant.invViewProj, pixelCenter, pushConstant.viewportSize, depth);
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.normalTextureIndex, 0));
    const float3 rawNormal = normalTex.Load(int3(pixel, 0)).xyz;
    const float normalLengthSq = dot(rawNormal, rawNormal);
    const float3 normal = normalLengthSq > 1e-8 ? rawNormal * rsqrt(normalLengthSq) : float3(0.0, 1.0, 0.0);
    const float3 geometricNormal = ddgi_geometric_normal(depthTex, pixel, pushConstant.viewportSize, pushConstant.invViewProj, position, normal);
    const float viewDistance = length(position - ubo->cameraWorldPos.xyz);
    // Foliage and cloth (double-sided): leaf cards a few pixels wide, whose normal from the depth around (geometricNormal) is
    // whichever leaf or gap the neighbours are -- a different one every frame with the jitter.
    bool doubleSided = false;
    if (pushConstant.materialTextureIndex != 0xFFFFFFFF)
    {
        Texture2D materialTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.materialTextureIndex, 0));
        doubleSided = gbuffer_double_sided(materialTex.Load(int3(pixel, 0)).a);
    }

    Texture2D positionTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.probePositionTextureIndex, 0));
    Texture2D probeNormalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.probeNormalTextureIndex, 0));
    Texture2D atlas = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.inputTextureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    const float2 atlasSize = float2(pushConstant.probeCountX, pushConstant.probeCountY) * float(ScreenProbeIrradianceRes);
    const int2 probeMax = int2(pushConstant.probeCountX, pushConstant.probeCountY) - 1;

    // The probe grid in probe units: probe i stands at (i + jitter) * tileSize this frame.
    const float2 grid = pixelCenter / float(pushConstant.tileSize) - screen_probe_jitter(pushConstant.frameIndex);
    const int2 base = int2(floor(grid));
    const float2 f = grid - float2(base);

    float3 sum = float3(0.0);
    float weightSum = 0.0;
    for (uint corner = 0; corner < 4; ++corner)
    {
        const int2 offset = int2(corner & 1, corner >> 1);
        const int2 probe = base + offset;
        if (any(probe < 0) || any(probe > probeMax))
            continue;
        const float4 probePosition = positionTex.Load(int3(probe, 0));
        if (probePosition.w <= 0.0)
            continue;
        const float bilinear = (offset.x != 0 ? f.x : 1.0 - f.x) * (offset.y != 0 ? f.y : 1.0 - f.y);
        const float3 probeNormal = probeNormalTex.Load(int3(probe, 0)).xyz;
        const float weight = bilinear * screen_probe_surface_weight(probePosition.xyz, probeNormal, position, geometricNormal, viewDistance);
        if (weight <= 1e-4)
            continue;
        sum += screen_probe_irradiance(atlas, linearClamp, uint2(probe), normal, atlasSize) * weight;
        weightSum += weight;
    }

    if (weightSum < 0.05)
    {
        // None of the four is on this surface: the 4 x 4 around, nearer ones first.
        sum = float3(0.0);
        weightSum = 0.0;
        for (int dy = -1; dy <= 2; ++dy)
        {
            for (int dx = -1; dx <= 2; ++dx)
            {
                const int2 probe = base + int2(dx, dy);
                if (any(probe < 0) || any(probe > probeMax))
                    continue;
                const float4 probePosition = positionTex.Load(int3(probe, 0));
                if (probePosition.w <= 0.0)
                    continue;
                const float2 away = float2(probe) + 0.5 - grid;
                const float3 probeNormal = probeNormalTex.Load(int3(probe, 0)).xyz;
                const float weight = screen_probe_surface_weight(probePosition.xyz, probeNormal, position, geometricNormal, viewDistance) /
                                     (1.0 + dot(away, away));
                if (weight <= 1e-4)
                    continue;
                sum += screen_probe_irradiance(atlas, linearClamp, uint2(probe), normal, atlasSize) * weight;
                weightSum += weight;
            }
        }
    }
    float3 current = weightSum >= 0.01 ? sum / weightSum : screen_probe_fallback(ubo, position, normal, geometricNormal);
    if (!all(isfinite(current)))
        current = screen_probe_fallback(ubo, position, normal, geometricNormal);

    // Last frame's result for this surface: bilinear over the four pixels around where it was, each only if it was this surface.
    float4 history = float4(0.0);
    if (pushConstant.historyValid != 0)
    {
        float2 previousUV;
        if (pushConstant.velocityTextureIndex != 0xFFFFFFFF)
        {
            Texture2D velocityTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.velocityTextureIndex, 0));
            // This (jittered) pixel's unjittered position, moved by the jitter-free motion, then to last frame's jittered pixels.
            previousUV = pixelCenter / pushConstant.viewportSize - pushConstant.currJitterUv + velocityTex.Load(int3(pixel, 0)).xy +
                         pushConstant.prevJitterUv;
        }
        else
        {
            const float4 previousClip = mul(ubo->prevProj, mul(ubo->prevView, float4(position, 1.0)));
            previousUV = previousClip.w > 0.0 ? (previousClip.xy / previousClip.w) * float2(0.5, -0.5) + 0.5 + pushConstant.prevJitterUv
                                              : float2(-1.0);
        }

        if (all(previousUV > 0.0) && all(previousUV < 1.0))
        {
            Texture2D historyTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.historyTextureIndex, 0));
            Texture2D<float> previousDepthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.prevDepthTextureIndex, 0));
            Texture2D previousNormalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.prevNormalTextureIndex, 0));
            const float2 previousPixel = previousUV * pushConstant.viewportSize - 0.5;
            const int2 previousBase = int2(floor(previousPixel));
            const float2 previousF = previousPixel - float2(previousBase);
            const int2 pixelMax = int2(pushConstant.viewportSize) - 1;
            float historyWeight = 0.0;
            for (uint corner = 0; corner < 4; ++corner)
            {
                const int2 offset = int2(corner & 1, corner >> 1);
                const int2 tap = previousBase + offset;
                if (any(tap < 0) || any(tap > pixelMax))
                    continue;
                const float previousDepth = previousDepthTex.Load(int3(tap, 0));
                if (previousDepth <= 0.0)
                    continue;
                const float3 previousPosition = screen_probe_world_position(pushConstant.prevInvViewProj, float2(tap) + 0.5, pushConstant.viewportSize,
                                                                            previousDepth);
                // A leaf: the same point (within 2% of its distance) is enough; its plane and normal from the depth are noise, and
                // testing them threw the history of most foliage away every frame while moving -- fresh probe light each frame, in
                // spots that flashed.
                if (doubleSided)
                {
                    const float3 previousNormal = previousNormalTex.Load(int3(tap, 0)).xyz;
                    if (length(previousPosition - position) > 0.02 * viewDistance || dot(previousNormal, normal) < -0.2)
                        continue;
                    const float4 tapValue = historyTex.Load(int3(tap, 0));
                    if (!all(isfinite(tapValue)))
                        continue;
                    const float weight = (offset.x != 0 ? previousF.x : 1.0 - previousF.x) * (offset.y != 0 ? previousF.y : 1.0 - previousF.y);
                    history += tapValue * weight;
                    historyWeight += weight;
                    continue;
                }
                const float planeDistance = abs(dot(previousPosition - position, geometricNormal)) / max(viewDistance, 1e-4);
                if (planeDistance > 0.02)
                    continue;
                // Against the flat surface's normal, not the normal-mapped one: with the camera jitter every frame lands on another
                // texel of the normal map, and a bumpy surface threw its history away nearly every frame (and boiled).
                const float3 previousRawNormal = previousNormalTex.Load(int3(tap, 0)).xyz;
                const float3 previousNormal = previousRawNormal * rsqrt(max(dot(previousRawNormal, previousRawNormal), 1e-8));
                if (dot(previousNormal, geometricNormal) < 0.5)
                    continue;
                const float4 tapValue = historyTex.Load(int3(tap, 0));
                if (!all(isfinite(tapValue)))
                    continue;
                const float weight = (offset.x != 0 ? previousF.x : 1.0 - previousF.x) * (offset.y != 0 ? previousF.y : 1.0 - previousF.y);
                history += tapValue * weight;
                historyWeight += weight;
            }
            history = historyWeight > 0.05 ? history / historyWeight : float4(0.0);
        }
    }

    // No shortcut for large changes: a probe catching a bright spot for one frame (a lamp's fixture at night) made it flash.
    float frames = min(floor(history.a + 0.5) + 1.0, float(max(pushConstant.maxHistory, 1u)));
    // Outliers: with a few frames of history, this frame may be at most 3x brighter than it. At night a lamp is a tiny target for
    // a probe's 64 rays: the probe that happens to hit it gets all its light for one frame (the next frame another one does),
    // which boiled in tile-sized blotches no denoiser removes (REBLUR never sees this signal, DLSS RR takes the blotches for
    // detail). Light that really comes on still arrives within a few frames (x3 per frame); darkening is not held back.
    float3 currentClamped = current;
    const float currentLuma = dot(current, float3(0.2126, 0.7152, 0.0722));
    if (history.a >= 4.0)
    {
        const float historyLuma = dot(history.rgb, float3(0.2126, 0.7152, 0.0722));
        const float limit = historyLuma * 3.0 + 1e-4;
        if (currentLuma > limit && isfinite(currentLuma))
        {
            currentClamped *= limit / currentLuma;
            // Held back again: no longer a one-frame spike but light the history has not caught up with. Weighed in at
            // 1 / (frames accumulated), up to 3x a frame let it grow ~10% a frame: the light of a lamp at night reached a wall only
            // where it had stayed on screen for a second or more, in a trail of the camera's path (a swirl, and the same shape
            // in the reflections that read this light), never where the camera had just turned. A short history follows it in a
            // few frames; one bright frame still adds at most 3x a quarter.
            frames = min(frames, 4.0);
        }
    }
    else if (doubleSided && currentLuma > 0.0 && isfinite(currentLuma))
    {
        // Foliage with little or no history (a leaf's edge just revealed): the probes' one frame of light, held to 3x the world-space
        // GI here (the DDGI cascades / the sky's irradiance), stable from frame to frame. One probe that caught the sun through a gap
        // lit the newly revealed leaves for a frame: the spots flashing on foliage while moving. Not on other surfaces: the
        // world-space GI holds no lamp light, so at night every newly seen wall near a lamp started black.
        const float fallbackLuma = dot(screen_probe_fallback(ubo, position, normal, geometricNormal), float3(0.2126, 0.7152, 0.0722));
        const float limit = max(fallbackLuma, history.a > 0.0 ? dot(history.rgb, float3(0.2126, 0.7152, 0.0722)) : 0.0) * 3.0 + 1e-4;
        if (currentLuma > limit && isfinite(fallbackLuma))
            currentClamped *= limit / currentLuma;
    }
    float3 result = history.a > 0.0 ? lerp(history.rgb, currentClamped, 1.0 / frames) : currentClamped;
    result = all(isfinite(result)) ? clamp(result, 0.0, 30000.0) : (all(isfinite(current)) ? clamp(current, 0.0, 30000.0) : float3(0.0));
    output.irradiance = float4(result, frames);
    output.distance = viewDistance;
    return output;
}
