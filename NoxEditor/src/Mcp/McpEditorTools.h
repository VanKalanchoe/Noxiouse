#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "McpHost.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/Scene/Entity.h"
#include "GamePackager.h"
#include "NoxCore/NodeGraph/NodeGraph.h"
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
        std::optional<bool> PlayerMcp;      // MCP5: the other players get MCP servers (unset: the Play settings' choice)
    };

    // MCP5: a game multiplayer Play started with its own MCP server (NoxRuntime --mcp), reached with runtime_call.
    struct RuntimeEndpoint
    {
        std::string Name;    // "dedicated server", "player 2" ...
        int Player = 0;      // 2..8 (0: the dedicated server)
        uint16_t Port = 0;   // its MCP server on 127.0.0.1
        bool Running = false; // its process is still running
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
    // The shared part (HostContext: the scene, mode, view capture, access, pause, polls) is what the game tools of both hosts use.
    struct EditorContext : HostContext
    {
        std::function<std::string()> ScenePath;       // the open scene's file under the asset directory (empty: never saved)
        std::function<std::vector<Entity>()> Selection;
        std::function<std::string()> NetStatus;       // the multiplayer session's line, empty when none
        std::function<float()> FramesPerSecond;

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
        std::function<void(Entity entity)> FocusEntity;    // the editor camera frames it (the F key)
        std::function<ViewportCamera()> GetCamera;
        std::function<void(const glm::dvec3& focalPoint, double distance, float pitch, float yaw)> SetCamera;

        // MCP4, scripts and assets:
        std::function<ScriptBuildInfo()> ScriptBuild;
        std::function<void()> RequestScriptBuild;        // as Scripts > Build and Reload
        std::function<void(AssetHandle material)> MaterialChanged; // drawn with its new values from this frame (and its thumbnail)
        std::function<void()> AssetsChanged;              // the Content Browser lists the asset folder again

        // MCP4 part 2: graphs and packaging.
        // A graph replaced through its editor window (opened when it is not): one undo step there, compiled, saved. "" or why not.
        std::function<std::string(AssetHandle graph, NodeGraph replacement)> ApplyGraph;
        // File > Package Game / Package Server: the scene saved first (what ships is the scene as edited), then packaged -- on this
        // thread, the editor waits (as the dialog does).
        std::function<PackageReport(const PackageSettings& settings)> Package;

        // MCP5: the games this Play started with MCP servers (empty: none -- Standalone, or the Play setting is off).
        std::function<std::vector<RuntimeEndpoint>()> Runtimes;
    };

    // MCP1, reading the editor: get_editor_status, get_selection, list_assets (the scene, log, view and console tools of both hosts:
    // CreateGameTools, McpHost.h).
    std::vector<Tool> CreateEditorTools(const EditorContext& context);
    // MCP2, editing the scene (validated, each change an undo step): create_entity, delete_entities, rename_entity, duplicate_entity,
    // set_parent, add_component, remove_component, patch_component, get_component_schema, apply_edits (atomic), select, save_scene,
    // undo, redo.
    std::vector<Tool> CreateEditTools(const EditorContext& context);
    // MCP3, play and verify: play, simulate, stop, focus_entity, get_viewport_camera, set_viewport_camera (pause, wait_for and
    // inject_input: CreateGameTools); MCP5: runtime_call (a tool of a game multiplayer Play started with --mcp).
    std::vector<Tool> CreatePlayTools(const EditorContext& context);
    // MCP4, scripts and assets: build_scripts, get_build_status, get_material, create_material, set_material, create_prefab,
    // import_asset (instantiate_prefab is an edit tool: CreateEditTools).
    std::vector<Tool> CreateAssetTools(const EditorContext& context);
    // MCP4 part 2: list_graph_node_types, get_graph, set_graph, run_tests, get_test_results, package_game, get_package_result.
    std::vector<Tool> CreateWorkflowTools(const EditorContext& context);
}
