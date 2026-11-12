#pragma once
#include <cstdint>
#include <string>

namespace Nox
{
    // Renderer settings a project keeps on top of its quality preset (ProjectConfig::Renderer, the .nproj's "Renderer" map), so
    // the game starts as the editor was set up. -1 everywhere: what the preset says.
    struct ProjectRendererSettings
    {
        int32_t RayTracingResolution = -1; // 1 every pixel, 2 one of each 2 x 2 block
        int32_t DiffuseGI = -1;            // 0 IBL, 1 DDGI, 2 ReSTIR GI, 3 screen probes, 4 raster probe volume
        int32_t ShadowedLights = -1;       // shadow rays a pixel: 0 every light, 1..8 the strongest
        int32_t OIT = -1;                  // order-independent transparency: 0 off, 1 weighted blended, 2 AVBOIT
        int32_t TAA = -1;                  // the engine's TAA where DLSS does not run: 0 off, 1 on
        int32_t Shadows = -1;              // every shadow: 0 off, 1 on
        int32_t SunShadows = -1;           // 0 ray traced (the RT shadow mask; shadow maps without ray traced shadows), 1 cascaded shadow maps
        int32_t LocalShadows = -1;         // local lights: 0 RT shadow rays, 1 shadow maps (atlas)
        int32_t ContactShadows = -1;       // screen-space contact shadows on shadow-mapped lights: 0 off, 1 on
        int32_t SSAOMethod = -1;           // screen-space AO: 0 classic SSAO, 1 GTAO
        int32_t SSR = -1;                  // screen-space reflections (where RT reflections are off): 0 off, 1 on
        int32_t SSGI = -1;                 // screen-space GI (with the environment / raster probe volume GI): 0 off, 1 on
        int32_t VolumetricFog = -1;        // the height fog in froxels with shadows: 0 off, 1 on
        int32_t Bloom = -1;                // post effects: 0 off, 1 on (their amounts are the renderer's defaults)
        int32_t DepthOfField = -1;
        int32_t MotionBlur = -1;
    };

    // The packaged game's window: "Windowed", "Borderless" (fullscreen at the desktop's resolution, the default) or "Fullscreen"
    // (exclusive, the display's current mode). Alt+Enter switches between windowed and the full screen mode.
    inline constexpr const char* kDefaultWindowMode = "Borderless";
}
