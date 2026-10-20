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
        int32_t DiffuseGI = -1;            // 0 IBL, 1 DDGI, 2 ReSTIR GI, 3 screen probes
        int32_t ShadowedLights = -1;       // shadow rays a pixel: 0 every light, 1..8 the strongest
        int32_t OIT = -1;                  // order-independent transparency: 0 off, 1 on
        int32_t TAA = -1;                  // the engine's TAA where DLSS does not run: 0 off, 1 on
    };

    // The packaged game's window: "Windowed", "Borderless" (fullscreen at the desktop's resolution, the default) or "Fullscreen"
    // (exclusive, the display's current mode). Alt+Enter switches between windowed and the full screen mode.
    inline constexpr const char* kDefaultWindowMode = "Borderless";
}
