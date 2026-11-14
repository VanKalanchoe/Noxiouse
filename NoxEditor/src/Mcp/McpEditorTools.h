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

        // MCP2, editing (Edit mode only):
        std::function<void(const std::string& step)> RecordEdit;       // the scene changed: one undo step with this name
        std::function<void(const std::vector<Entity>& entities)> Select; // replaces the selection (empty: none)
        std::function<AssetHandle(const std::string& shape)> ShapeMesh;  // a basic shape's mesh ("Cube" ...), 0 when unknown
        std::function<std::vector<std::string>()> ShapeNames;
        std::function<bool(std::string& error)> SaveScene;             // the open scene (and its sub-levels) to their files
        std::function<std::string()> Undo;                              // the step undone / redone, empty when there was none
        std::function<std::string()> Redo;
        std::function<std::string()> SnapshotScene;                     // the scene as text (an atomic batch's way back)
        std::function<void(const std::string& text)> RestoreScene;
    };

    // MCP1, reading the editor: get_editor_status, read_log, get_hierarchy, find_entities, get_entity, get_selection, list_assets,
    // screenshot; and the console: list_commands, run_command.
    std::vector<Tool> CreateEditorTools(const EditorContext& context);
    // MCP2, editing the scene (validated, each change an undo step): create_entity, delete_entities, rename_entity, duplicate_entity,
    // set_parent, add_component, remove_component, patch_component, get_component_schema, apply_edits (atomic), select, save_scene,
    // undo, redo.
    std::vector<Tool> CreateEditTools(const EditorContext& context);

    // YAML (the scene serializer's) to JSON: maps and sequences as they are, scalars as numbers / booleans when they read as one.
    Json YamlTextToJson(const std::string& yaml);
}
