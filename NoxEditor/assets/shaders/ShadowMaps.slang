// The sun's cascaded shadow maps (RS1, Settings > Sun Shadows: Shadow Maps): the lookup shared by the shadow map resolve
// (ShadowMapResolve.slang, the deferred path's shadow mask) and the passes that shade their own pixels (the forward and particle
// passes, through UniformBufferObject::sunShadowCascadesReference). Renderer::addShadowMapPasses renders the cascades and writes
// GpuShadowCascades (shaderIO.h).
//
// Filtering (GPU Gems 3 ch. 8 / NVIDIA PCSS, as UE5's directional light without virtual shadow maps): every tap is a bilinear 2x2
// depth comparison (what a comparison sampler returns), 16 taps on a Vogel disk turned per pixel and frame, which the temporal
// upscale averages. PCSS first averages the casters around the point, then sizes the kernel by the distance to them times the tan
// of the sun's angular radius -- the same penumbra the RT shadows' cone of rays gives (ShadowMask.slang), hard at contact.
#pragma once

static const uint SHADOW_MAP_TAPS = 16;
static const float SHADOW_MAP_GOLDEN_ANGLE = 2.399963229728653;

// Tap i of SHADOW_MAP_TAPS on the unit disk, turned by `rotation` radians (uniform area density).
float2 shadow_map_tap(uint i, float rotation)
{
    const float r = sqrt((float(i) + 0.5) / float(SHADOW_MAP_TAPS));
    const float theta = float(i) * SHADOW_MAP_GOLDEN_ANGLE + rotation;
    return r * float2(cos(theta), sin(theta));
}

// The 2x2 depth comparisons around uv weighted bilinearly: 1 where nothing nearer the sun than receiverDepth is in the map
// (reverse Z: a caster has the larger depth).
float shadow_map_bilinear(Texture2D<float> map, SamplerState pointClamp, float2 uv, float receiverDepth, float resolution)
{
    const float2 texel = uv * resolution - 0.5;
    const float2 f = frac(texel);
    // GatherRed: x (0, 1), y (1, 1), z (1, 0), w (0, 0) of the 2x2 block whose shared corner is at the sample point.
    const float4 depths = map.GatherRed(pointClamp, (floor(texel) + 1.0) / resolution);
    const float4 lit = step(depths, float4(receiverDepth));
    return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);
}

// The sun's visibility at worldPos (render space) on a surface with normal n facing it (n . L > 0): 1 lit, 0 in shadow, 1 past the
// last cascade. noise in [0, 1), per pixel and frame: turns the kernel and dithers the transitions between cascades.
float sun_shadow_map(UniformBufferObject* ubo, GpuShadowCascades* cascades, float3 worldPos, float3 n, float noise)
{
    const uint count = min(cascades->count, MaxShadowCascades);
    if (count == 0)
        return 1.0;

    // The cascade by the depth along the camera's view axis (the CPU split the view the same way).
    const float viewDepth = abs(mul(ubo->view, float4(worldPos, 1.0)).z);
    uint cascade = 0;
    while (cascade < count && viewDepth > cascades->splitFar[cascade])
        cascade++;
    if (cascade >= count)
        return 1.0;

    // The last blendFraction of each cascade dithers into the next one; the last fades into the unshadowed distance.
    const float cascadeNear = cascade > 0 ? cascades->splitFar[cascade - 1] : 0.0;
    const float cascadeFar = cascades->splitFar[cascade];
    const float band = max(cascades->blendFraction * (cascadeFar - cascadeNear), 1e-4);
    const float transition = saturate((viewDepth - (cascadeFar - band)) / band);
    float fade = 1.0;
    if (cascade + 1 < count)
    {
        if (frac(noise * 1.6180339887 + 0.3183098862) < transition)
            cascade++;
    }
    else
    {
        fade = 1.0 - transition;
    }

    const float texelWorld = cascades->texelWorldSize[cascade];
    const float depthRange = max(cascades->depthRangeWorld[cascade], 1e-6);
    const float3 L = cascades->lightDirection.xyz;
    const float NoL = saturate(dot(n, L));
    const float sinTheta = sqrt(saturate(1.0 - NoL * NoL));

    // Normal offset (more at grazing angles, where a texel covers more of the surface's depth), then a constant step towards the sun.
    const float3 receiverPos = worldPos + n * (cascades->normalBias * texelWorld * sinTheta) + L * (cascades->depthBias * texelWorld);
    const float4 clip = mul(cascades->viewProj[cascade], float4(receiverPos, 1.0));
    const float2 uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    if (any(uv <= 0.0) || any(uv >= 1.0))
        return 1.0;
    const float receiverDepth = clip.z;

    Texture2D<float> map = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + cascades->textureIndex[cascade], 0));
    SamplerState pointClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_NEAREST_CLAMP, 0));
    const float resolution = float(cascades->resolution);
    // Receiver plane bias: away from the lookup the surface rises towards the sun by up to tan(theta) per unit; the kernel's taps
    // compare against that height (in depth units per texel of offset), or a sloped surface would shadow itself.
    const float slopeDepthPerTexel = min(sinTheta / max(NoL, 0.05), 4.0) * texelWorld / depthRange;
    const float rotation = noise * 6.283185307;

    float radiusTexels = max(cascades->filterTexels, 0.0);
    const float tanSun = cascades->lightDirection.w;
    if (cascades->filter == SHADOW_FILTER_PCSS && tanSun > 0.0)
    {
        // The casters within the widest penumbra: their average depth.
        const float searchTexels = max(cascades->maxFilterTexels, radiusTexels);
        float blockerDepthSum = 0.0;
        float blockerCount = 0.0;
        for (uint i = 0; i < SHADOW_MAP_TAPS; ++i)
        {
            const float2 offset = shadow_map_tap(i, rotation) * searchTexels;
            const float4 depths = map.GatherRed(pointClamp, uv + offset / resolution);
            const float threshold = receiverDepth + (length(offset) + 1.0) * slopeDepthPerTexel;
            const float4 blocker = step(float4(threshold), depths); // nearer the sun than the receiver (the cleared 0 never is)
            blockerDepthSum += dot(blocker, depths);
            blockerCount += dot(blocker, float4(1.0));
        }
        if (blockerCount <= 0.0)
            return 1.0;
        const float distanceToCaster = max(blockerDepthSum / blockerCount - receiverDepth, 0.0) * depthRange;
        radiusTexels = clamp(distanceToCaster * tanSun / texelWorld, radiusTexels, searchTexels);
    }

    float lit = 0.0;
    for (uint i = 0; i < SHADOW_MAP_TAPS; ++i)
    {
        const float2 offset = shadow_map_tap(i, rotation) * radiusTexels;
        lit += shadow_map_bilinear(map, pointClamp, uv + offset / resolution, receiverDepth + (length(offset) + 1.0) * slopeDepthPerTexel, resolution);
    }
    return lerp(1.0, lit / float(SHADOW_MAP_TAPS), fade);
}

// --- Local lights (RS1b; Renderer::addLocalShadowPasses, GpuLocalShadows in shaderIO.h) ---------------------------------------

// The bilinear comparison within a tile of the atlas: the 2x2 footprint kept inside [lo, hi] (its texel centers), so a kernel at a
// face's edge never reads the neighbouring tile.
float shadow_atlas_bilinear(Texture2D<float> atlas, SamplerState pointClamp, float2 uv, float2 lo, float2 hi, float receiverDepth, float atlasSize)
{
    return shadow_map_bilinear(atlas, pointClamp, clamp(uv, lo, hi), receiverDepth, atlasSize);
}

// Whether the local light has a shadow map this frame, and its visibility at worldPos on a surface with normal n facing it (L: unit
// vector towards the light). false: none -- the caller keeps the light's RT shadow ray. Perspective views (shaderIO.h): the depth is
// nearPlane / view depth, so depth differences are turned into distances along the view through it.
bool local_light_shadow_map(UniformBufferObject* ubo, uint lightIndex, LightData light, float3 worldPos, float3 n, float3 L, float noise, out float visibility)
{
    visibility = 1.0;
    if (ubo->localShadowsReference == 0 || ubo->localShadowLightViewsReference == 0)
        return false;
    const uint entry = ((uint*)ubo->localShadowLightViewsReference)[lightIndex];
    if (entry == LOCAL_SHADOW_NONE)
        return false;
    GpuLocalShadows* shadows = (GpuLocalShadows*)ubo->localShadowsReference;

    // A cube's face by the direction from the light (+X, -X, +Y, -Y, +Z, -Z).
    uint viewIndex = entry & 0xFFFFu;
    if ((entry >> 16) == 6u)
    {
        const float3 d = worldPos - light.position.xyz;
        const float3 a = abs(d);
        viewIndex += a.x >= a.y && a.x >= a.z ? (d.x >= 0.0 ? 0u : 1u) : (a.y >= a.z ? (d.y >= 0.0 ? 2u : 3u) : (d.z >= 0.0 ? 4u : 5u));
    }
    const GpuShadowView view = shadows->views[min(viewIndex, MaxLocalShadowViews - 1u)];
    if (view.atlasRect.z <= 0.0) // a face behind a rect / disc light
        return true;

    const float viewDepth = mul(view.viewProj, float4(worldPos, 1.0)).w;
    if (viewDepth <= view.nearPlane)
        return true;
    const float texelWorld = view.texelPerDepth * viewDepth;
    const float NoL = saturate(dot(n, L));
    const float sinTheta = sqrt(saturate(1.0 - NoL * NoL));
    const float3 receiverPos = worldPos + n * (shadows->normalBias * texelWorld * sinTheta) + L * (shadows->depthBias * texelWorld);
    const float4 clip = mul(view.viewProj, float4(receiverPos, 1.0));
    if (clip.w <= view.nearPlane)
        return true;
    const float3 ndc = clip.xyz / clip.w;
    const float2 tileUV = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(tileUV <= 0.0) || any(tileUV >= 1.0)) // outside a spot light's cone
        return true;
    const float receiverDepth = ndc.z;
    const float receiverZ = clip.w;

    Texture2D<float> atlas = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + shadows->atlasTextureIndex, 0));
    SamplerState pointClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_NEAREST_CLAMP, 0));
    const float atlasSize = float(shadows->atlasSize);
    const float2 uv = view.atlasRect.xy + tileUV * view.atlasRect.zw;
    const float2 lo = view.atlasRect.xy + 1.0 / atlasSize;
    const float2 hi = view.atlasRect.xy + view.atlasRect.zw - 1.0 / atlasSize;
    // The receiver plane's rise per texel of offset, in depth (d depth = nearPlane d z / z^2).
    const float slopeDepthPerTexel = view.nearPlane * min(sinTheta / max(NoL, 0.05), 4.0) * texelWorld / (receiverZ * receiverZ);
    const float rotation = noise * 6.283185307;
    const float maxRadius = max(min(shadows->maxFilterTexels, float(view.borderTexels) - 1.0), 0.0);
    float radiusTexels = min(max(shadows->filterTexels, 0.0), maxRadius);

    // PCSS: the light's size (a sphere's radius; a rect / disc's mean half extent) over the distance to the casters.
    const uint lightType = (uint)light.position.w;
    const float sourceRadius = is_area_light(lightType) ? 0.5 * (length(light.spotParams.xyz) + light.spotParams.w) : light.spotParams.z;
    if (shadows->filter == SHADOW_FILTER_PCSS && sourceRadius > 0.0)
    {
        float blockerDepthSum = 0.0;
        float blockerCount = 0.0;
        for (uint i = 0; i < SHADOW_MAP_TAPS; ++i)
        {
            const float2 offset = shadow_map_tap(i, rotation) * maxRadius;
            const float4 depths = atlas.GatherRed(pointClamp, clamp(uv + offset / atlasSize, lo, hi));
            const float4 blocker = step(float4(receiverDepth + (length(offset) + 1.0) * slopeDepthPerTexel), depths);
            blockerDepthSum += dot(blocker, depths);
            blockerCount += dot(blocker, float4(1.0));
        }
        if (blockerCount <= 0.0)
            return true;
        const float blockerZ = view.nearPlane / max(blockerDepthSum / blockerCount, 1e-6);
        const float penumbraWorld = sourceRadius * max(receiverZ - blockerZ, 0.0) / max(blockerZ, 1e-4);
        radiusTexels = clamp(penumbraWorld / texelWorld, radiusTexels, maxRadius);
    }

    float lit = 0.0;
    for (uint i = 0; i < SHADOW_MAP_TAPS; ++i)
    {
        const float2 offset = shadow_map_tap(i, rotation) * radiusTexels;
        lit += shadow_atlas_bilinear(atlas, pointClamp, uv + offset / atlasSize, lo, hi, receiverDepth + (length(offset) + 1.0) * slopeDepthPerTexel, atlasSize);
    }
    visibility = lit / float(SHADOW_MAP_TAPS);
    return true;
}

// --- Contact shadows (RS2; UE5's screen space contact shadows) ------------------------------------------------------------------

// A short ray from the surface towards the light (L, at most maxLength) marched through the camera's depth buffer: 0 where something
// on screen covers it within a thickness, else 1. The detail a shadow map's texels miss (a hand on a table, grass blades, creases);
// what is off screen or behind the visible surface casts nothing here. Perspective views only (the depth's view distance:
// near / depth for the cameras' infinite reverse-Z projection). noise in [0, 1): the steps' offset, averaged by the temporal upscale.
float contact_shadow(UniformBufferObject* ubo, Texture2D<float> depthTex, float2 viewportSize, float3 worldPos, float3 n, float3 L, float maxLength, float noise)
{
    const float rayLength = min(ubo->contactShadowLength, maxLength);
    if (rayLength <= 0.0 || mul(ubo->proj, float4(0.0, 0.0, 2.0, 1.0)).w < 1.5) // off, or an orthographic camera (w = 1)
        return 1.0;
    const float nearPlane = mul(ubo->proj, float4(0.0, 0.0, 1.0, 1.0)).z; // depth x view distance (reverse-Z infinite projection)
    const float4x4 viewProj = mul(ubo->proj, ubo->view); // jittered, as the depth buffer
    const uint steps = clamp(ubo->contactShadowSamples, 1u, 64u);
    // A covering surface counts within this distance behind it (thinner than the ray: no shadow from a far wall behind an edge).
    const float thickness = max(0.5 * rayLength, 0.02 * ubo->unitsPerMeter);
    const float3 start = worldPos + n * raster_surface_bias(ubo, worldPos, 0.01 * ubo->unitsPerMeter);
    for (uint i = 0; i < steps; ++i)
    {
        const float t = (float(i) + noise) / float(steps);
        const float4 clip = mul(viewProj, float4(start + L * (rayLength * t), 1.0));
        if (clip.w <= nearPlane)
            break;
        const float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
        if (any(uv <= 0.0) || any(uv >= 1.0))
            break;
        const float sceneDepth = depthTex.Load(int3(int2(uv * viewportSize), 0)).r;
        if (sceneDepth <= 0.0) // the sky
            continue;
        const float behind = clip.w - nearPlane / sceneDepth; // how far the ray is behind what the pixel shows
        if (behind > 0.002 * clip.w && behind < thickness)
            return 0.0;
    }
    return 1.0;
}
