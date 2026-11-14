#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "McpProtocol.h"
#include "NoxCore/Core/Ref.h"

namespace Nox
{
    class Scene;
}

namespace Nox::Mcp
{
    // What the tools both MCP hosts offer -- NoxEditor's server and NoxRuntime's (--mcp) -- may ask of the program they run in (the
    // host fills these in; all are called on its main thread). The editor's EditorContext extends it.
    struct HostContext
    {
        std::function<Ref<Scene>()> ActiveScene;      // the scene shown now (the editor: the running copy in Play / Simulate)
        std::function<std::string()> Mode;            // "edit", "play", "simulate" (NoxRuntime: always "play")
        std::function<bool()> PlayRunning;            // the game's scene runs (a client waiting for its server: not yet)
        // The main view's last image, read back from the GPU a frame or two later: `done` gets RGBA8 / BGRA8 texels (bgra true for the
        // latter), empty when there was none. Unset (a dedicated server draws nothing): screenshot answers that there is no view.
        std::function<void(std::function<void(std::vector<uint8_t> pixels, uint32_t width, uint32_t height, bool bgra)> done)> CaptureViewport;
        std::function<ConsoleAccess()> GrantedAccess; // what MCP clients are granted (run_command passes it to the console)
        // Pause / step (the editor's Play and Simulate; NoxRuntime's game); unset: pause answers that it cannot.
        std::function<bool(bool paused, int stepFrames, std::string& error)> Pause;
        std::function<bool()> Paused;
        // Runs `poll` at the start of every frame (main thread) until it returns true -- tools that answer later (wait_for,
        // inject_input). The host fills it in; polls still going when its server stops are dropped (their calls were answered).
        std::function<void(std::function<bool()> poll)> EveryFrame;
        // A local player reads input here (false: a dedicated server -- inject_input is left out, as screenshot is without
        // CaptureViewport).
        bool HasLocalPlayer = true;
    };

    // The tools of both hosts: read_log, get_hierarchy, find_entities, get_entity, screenshot (with CaptureViewport), list_commands,
    // run_command, pause, wait_for, inject_input (with HasLocalPlayer).
    std::vector<Tool> CreateGameTools(const HostContext& context);

    // inject_input's key names: the C# KeyCode names (W, LeftShift, Space, Alpha1, F5 ...) and SDL's ("Left Shift"), case ignored.
    // 0 when unknown.
    int KeyScancode(const std::string& name);
    std::vector<std::string> KeyNames(); // the KeyCode names

}
