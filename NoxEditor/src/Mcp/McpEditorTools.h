#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "McpProtocol.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/Scene/Entity.h"
#include "ScriptDiagnostics.h"

namespace Nox
{
    class Scene;
}

namespace Nox::Mcp
{
    // MCP3: how Play starts (the Play settings' choices).
    struct PlayOptions
    {
        std::string NetMode = "standalone"; // "standalone", "listen_server", "client"
        int Players = 2;                    // multiplayer: the editor's player included
        std::string Emulation = "off";      // "off", "average", "bad"
        bool DebugLog = false;              // the network debug trace (--net-debug)
    };

    // MCP4: the script build as the tools see it (ScriptBuilder).
    struct ScriptBuildInfo
    {
        bool HasProject = false; // the project has a C# project to build
        bool Building = false;
        bool Pending = false;    // asked for or due, not started yet
        std::string Status;      // the editor's status line ("Scripts built and reloaded" ...)
        uint64_t Number = 0;     // the last finished build (0: none since the editor started)
        bool Succeeded = false;
        bool Reloaded = false;
        double Seconds = 0.0;
        std::vector<ScriptDiagnostic> Diagnostics;
        std::string Log;         // the full output's file
    };

    // The editor camera of the active viewport (an orbit camera: it looks at FocalPoint from Distance away).
    struct ViewportCamera
    {
        glm::dvec3 Position{ 0.0 };
        glm::dvec3 FocalPoint{ 0.0 };
        double Distance = 0.0;
        float Pitch = 0.0f; // radians, positive looks down
        float Yaw = 0.0f;   // radians, 0 looks along -Z
        glm::vec3 Forward{ 0.0f, 0.0f, -1.0f };
        bool Orthographic = false;
    };

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

        // MCP3, play and verify:
        std::function<bool(const PlayOptions& options, std::string& error)> Play; // from Edit (multiplayer saves the scene first)
        std::function<bool(std::string& error)> Simulate;
        std::function<bool(std::string& error)> Stop;      // back to Edit
        std::function<bool()> PlayRunning;                 // Play's scene runs (a client waiting for its server: not yet)
        std::function<bool(bool paused, int stepFrames, std::string& error)> Pause; // Play / Simulate
        std::function<bool()> Paused;
        std::function<void(Entity entity)> FocusEntity;    // the editor camera frames it (the F key)
        std::function<ViewportCamera()> GetCamera;
        std::function<void(const glm::dvec3& focalPoint, double distance, float pitch, float yaw)> SetCamera;
        // Runs `poll` at the start of every frame (main thread) until it returns true -- tools that answer later (wait_for,
        // inject_input). McpEditor fills it in; polls still going when the server stops are dropped (their calls were answered).
        std::function<void(std::function<bool()> poll)> EveryFrame;

        // MCP4, scripts and assets:
        std::function<ScriptBuildInfo()> ScriptBuild;
        std::function<void()> RequestScriptBuild;        // as Scripts > Build and Reload
        std::function<void(AssetHandle material)> MaterialChanged; // drawn with its new values from this frame (and its thumbnail)
        std::function<void()> AssetsChanged;              // the Content Browser lists the asset folder again
    };

    // MCP1, reading the editor: get_editor_status, read_log, get_hierarchy, find_entities, get_entity, get_selection, list_assets,
    // screenshot; and the console: list_commands, run_command.
    std::vector<Tool> CreateEditorTools(const EditorContext& context);
    // MCP2, editing the scene (validated, each change an undo step): create_entity, delete_entities, rename_entity, duplicate_entity,
    // set_parent, add_component, remove_component, patch_component, get_component_schema, apply_edits (atomic), select, save_scene,
    // undo, redo.
    std::vector<Tool> CreateEditTools(const EditorContext& context);
    // MCP3, play and verify: play, simulate, stop, pause, wait_for, inject_input, focus_entity, get_viewport_camera,
    // set_viewport_camera.
    std::vector<Tool> CreatePlayTools(const EditorContext& context);
    // MCP4, scripts and assets: build_scripts, get_build_status, get_material, create_material, set_material, create_prefab,
    // import_asset (instantiate_prefab is an edit tool: CreateEditTools).
    std::vector<Tool> CreateAssetTools(const EditorContext& context);

    // inject_input's key names: the C# KeyCode names (W, LeftShift, Space, Alpha1, F5 ...) and SDL's ("Left Shift"), case ignored.
    // 0 when unknown.
    int KeyScancode(const std::string& name);
    std::vector<std::string> KeyNames(); // the KeyCode names

    // YAML (the scene serializer's) to JSON: maps and sequences as they are, scalars as numbers / booleans when they read as one.
    Json YamlTextToJson(const std::string& yaml);
}
