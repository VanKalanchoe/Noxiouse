#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "McpProtocol.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/Scene/Entity.h"

namespace Nox
{
    class Scene;
}

namespace Nox::Mcp
{
    // What the tools may ask of the editor -- the only way they reach it (EditorLayer fills these in). All are called on the main thread.
    struct EditorContext
    {
        std::function<Ref<Scene>()> ActiveScene;      // the scene the editor shows now (the running copy in Play / Simulate)
        std::function<std::string()> Mode;            // "edit", "play", "simulate"
        std::function<std::string()> ScenePath;       // the open scene's file under the asset directory (empty: never saved)
        std::function<std::vector<Entity>()> Selection;
        std::function<std::string()> NetStatus;       // the multiplayer session's line, empty when none
        std::function<float()> FramesPerSecond;
        // The main viewport's last image, read back from the GPU a frame or two later: `done` gets RGBA8 / BGRA8 texels (bgra true for
        // the latter), empty when there was none.
        std::function<void(std::function<void(std::vector<uint8_t> pixels, uint32_t width, uint32_t height, bool bgra)> done)> CaptureViewport;
        std::function<ConsoleAccess()> GrantedAccess; // what MCP clients are granted (run_command passes it to the console)
    };

    // MCP1, reading the editor: get_editor_status, read_log, get_hierarchy, find_entities, get_entity, get_selection, list_assets,
    // screenshot; and the console: list_commands, run_command.
    std::vector<Tool> CreateEditorTools(const EditorContext& context);

    // YAML (the scene serializer's) to JSON: maps and sequences as they are, scalars as numbers / booleans when they read as one.
    Json YamlTextToJson(const std::string& yaml);
}
