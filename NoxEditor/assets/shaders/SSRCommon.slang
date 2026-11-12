// The screen-space reflection trace (RS4), shared by the opaque surfaces' SSR (SSR.slang, from the G-buffer) and the transparent
// surfaces' (Material_TransparentLit_Mesh.slang, from their own shading, in raster): the mirror ray traced through a closest-depth
// Hi-Z pyramid (ssr_march). Requires shaderIO.h before it.
#pragma once

float2 ssr_clip_to_uv(float4 clip)
{
    return float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
}

// Towards the screen's edges SSR knows less and less (what is beyond is unknown): it fades out over the outer 20 % smoothly, so
// the environment / probes take over gradually, not at a line (UE's SSR edge fade).
float ssr_edge_fade(float2 uv)
{
    const float2 edge = min(uv, 1.0 - uv);
    return smoothstep(0.0, 0.2, min(edge.x, edge.y));
}

// A level of the Hi-Z (closest depth): level 0 is the depth buffer itself, level k >= 1 the pyramid's mip k - 1 (half the size
// each, HiZBuild.slang keepClosest). `position`: texel coordinates at that level.
float ssr_hiz_load(Texture2D<float> depthTex, Texture2D<float> hizTex, float2 position, int level, float2 viewportSize)
{
    if (level == 0)
        return depthTex.Load(int3(clamp(int2(position), int2(0), int2(viewportSize) - 1), 0)).r;
    uint width, height, levels;
    hizTex.GetDimensions(uint(level - 1), width, height, levels);
    return hizTex.Load(int3(clamp(int2(position), int2(0), int2(int(width), int(height)) - 1), level - 1)).r;
}

// The ray from `start` along R (world space), at most `maxDistance` long, through this frame's depth (reverse Z): whether it hits what
// the depth buffer shows, and where (hitUV, the depth there). Hierarchical (AMD FidelityFX SSSR's / UE's Hi-Z tracing): in screen
// space (uv, depth -- straight lines stay straight there), the ray steps from tile edge to tile edge of the closest-depth pyramid,
// a level coarser while it passes in front of whole tiles, a level finer where a tile's closest surface stops it -- empty space
// crossed in a few large steps, the surface found to the pixel. (A march of equal steps stepped over what it hit by up to a dozen
// pixels on long rays: the same spots were found and lost as the camera moved.) rayConfidence: the hit's (the ray within the
// thickness behind the surface where it stopped, squared falloff) times the fade of rays towards the camera, which soon go behind
// what the screen shows.
bool ssr_march(UniformBufferObject* ubo, Texture2D<float> depthTex, Texture2D<float> hizTex, uint hizLevels, float3 start, float3 R,
               float maxDistance, float thickness, uint maxIterations, float2 viewportSize, out float2 hitUV, out float hitDepth,
               out float rayConfidence)
{
    hitUV = float2(0.0);
    hitDepth = 0.0;
    rayConfidence = 0.0;
    // Towards the camera the ray soon goes behind what the screen shows: faded out (the view's +z is forward).
    const float3 viewR = mul(ubo->view, float4(R, 0.0)).xyz;
    const float facingFade = saturate((viewR.z + 0.25) * 4.0);
    if (facingFade <= 0.0)
        return false;

    const float nearPlane = mul(ubo->proj, float4(0.0, 0.0, 1.0, 1.0)).z; // depth x view distance (infinite reverse Z)
    const float4x4 viewProj = mul(ubo->proj, ubo->view);
    const float startZ = mul(ubo->view, float4(start, 1.0)).z;
    float rayLength = maxDistance;
    if (viewR.z < 0.0) // ends in front of the near plane otherwise
        rayLength = min(rayLength, max(startZ - 2.0 * nearPlane, 0.0) / -viewR.z);
    if (rayLength <= 0.0)
        return false;
    const float4 clip0 = mul(viewProj, float4(start, 1.0));
    const float4 clip1 = mul(viewProj, float4(start + R * rayLength, 1.0));
    if (clip0.w <= 1e-5 || clip1.w <= 1e-5)
        return false;

    // The segment in screen space, t 0..1 from start to its end; no further than where it leaves the screen.
    const float3 origin = float3(ssr_clip_to_uv(clip0), clip0.z / clip0.w);
    const float3 direction = float3(ssr_clip_to_uv(clip1), clip1.z / clip1.w) - origin;
    float tEnd = 1.0;
    if (direction.x > 1e-9) tEnd = min(tEnd, (1.0 - origin.x) / direction.x);
    if (direction.x < -1e-9) tEnd = min(tEnd, -origin.x / direction.x);
    if (direction.y > 1e-9) tEnd = min(tEnd, (1.0 - origin.y) / direction.y);
    if (direction.y < -1e-9) tEnd = min(tEnd, -origin.y / direction.y);
    const float3 invDirection = float3(abs(direction.x) > 1e-12 ? 1.0 / direction.x : 1e30, abs(direction.y) > 1e-12 ? 1.0 / direction.y : 1e30,
                                       abs(direction.z) > 1e-20 ? 1.0 / direction.z : 1e30);

    int level = 0;
    const int maxLevel = int(hizLevels); // the pyramid's mips are levels 1..hizLevels
    float2 levelResolution = viewportSize;
    float2 levelResolutionInv = 1.0 / viewportSize;
    // The tile edge the ray crosses next (towards its direction), a hair beyond it so the next step starts inside the next tile.
    const float2 floorOffset = float2(direction.x < 0.0 ? 0.0 : 1.0, direction.y < 0.0 ? 0.0 : 1.0);
    const float2 uvOffset = float2(direction.x < 0.0 ? -0.005 : 0.005, direction.y < 0.0 ? -0.005 : 0.005) / viewportSize;

    // First out of the start pixel (no hit on the surface itself).
    float t;
    {
        const float2 plane = (floor(levelResolution * origin.xy) + floorOffset) * levelResolutionInv + uvOffset;
        const float2 tPlane = plane * invDirection.xy - origin.xy * invDirection.xy;
        t = min(tPlane.x, tPlane.y);
    }
    float3 position = origin + t * direction;

    uint iteration = 0;
    while (iteration < maxIterations && level >= 0 && t <= tEnd)
    {
        const float2 levelPosition = levelResolution * position.xy;
        const float surfaceZ = ssr_hiz_load(depthTex, hizTex, levelPosition, level, viewportSize);
        // The tile's edges and its closest surface: how far the ray gets before it crosses one.
        const float2 plane = (floor(levelPosition) + floorOffset) * levelResolutionInv + uvOffset;
        float3 tBoundary = float3(plane, surfaceZ) * invDirection - origin * invDirection;
        // Within 2 pixels of the start the ray leaves the surface it starts from: at a grazing angle it is behind that surface's
        // depth at the next pixels' centres, and stopping there lost the reflection (it was rejected as a self-hit).
        const bool nearOrigin = level == 0 && all(abs(position.xy - origin.xy) < 2.0 / viewportSize);
        tBoundary.z = direction.z < 0.0 && !nearOrigin ? tBoundary.z : 1e30; // (reverse Z: only a ray going away reaches a surface's depth)
        const float tMin = min(min(tBoundary.x, tBoundary.y), tBoundary.z);
        // The ray in front of the tile's closest surface (reverse Z: larger is closer).
        const bool aboveSurface = surfaceZ < position.z || nearOrigin;
        // Out of the tile without touching its surface: a coarser level next; stopped by the surface (or behind it): a finer one.
        const bool skippedTile = asuint(tMin) != asuint(tBoundary.z) && aboveSurface;
        t = aboveSurface ? tMin : t;
        position = origin + t * direction;
        if (!(skippedTile && level >= maxLevel))
        {
            level += skippedTile ? 1 : -1;
            levelResolution *= skippedTile ? 0.5 : 2.0;
            levelResolutionInv *= skippedTile ? 2.0 : 0.5;
        }
        ++iteration;
    }
    // Found when it went finer than the finest level (the depth buffer's pixel stopped it), within the ray and the screen.
    if (level >= 0 || t > tEnd)
        return false;
    hitUV = position.xy;
    if (any(hitUV <= 0.0) || any(hitUV >= 1.0))
        return false;
    if (all(abs(hitUV - origin.xy) < 2.0 / viewportSize))
        return false; // (still on the surface it started from)
    hitDepth = depthTex.Load(int3(clamp(int2(hitUV * viewportSize), int2(0), int2(viewportSize) - 1), 0)).r;
    if (hitDepth <= 0.0)
        return false; // the sky
    // How far behind the surface the ray is where it stopped (view distance): a ray passing behind a thin object is not a hit on it.
    const float behind = abs(nearPlane / max(position.z, 1e-12) - nearPlane / hitDepth);
    float hitConfidence = 1.0 - smoothstep(0.0, max(thickness, 1e-4), behind);
    hitConfidence *= hitConfidence;
    rayConfidence = facingFade * hitConfidence;
    return rayConfidence > 0.0;
}
