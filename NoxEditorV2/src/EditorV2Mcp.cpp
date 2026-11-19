// EditorV2Layer: the MCP server (docs/MCP_Plan_2026-11.md) -- NoxEditor's MCP tools, reached only through Mcp::EditorContext, which
// this file fills from NoxEditorV2 (D1). Tools > MCP Server is the menu.
#include "EditorV2Layer.h"

#if NOX_WITH_MCP

#include <algorithm>
#include <cctype>

#include <SDL3/SDL_clipboard.h>

#include "McpEditor.h"
#include "Panels/AssetPreviewRenderer.h"
#include "Panels/NodeGraph/NodeGraphEditorV2.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/SceneSerializer.h"
#include "NoxCore/Scripting/ScriptEngine.h"
#include "NoxCore/UI/Widgets/UIMenu.h"

namespace Nox
{
    void EditorV2Layer::CreateMcpServer()
    {
        Mcp::EditorContext context;
        context.ActiveScene = [this]() { return m_ActiveScene; };
        context.Mode = [this]() { return std::string(m_SceneState == SceneState::Play ? "play" : m_SceneState == SceneState::Simulate ? "simulate" : "edit"); };
        context.ScenePath = [this]() { return EditorSceneAssetPath().generic_string(); };
        context.Selection = [this]() { return m_Selection.GetEntities(); };
        context.NetStatus = [this]() { return m_PlaySession ? m_PlaySession->Describe() : std::string(); };
        context.FramesPerSecond = []() { const double ms = Profiler::Get().GetFrameTime().Avg; return ms > 0.0 ? static_cast<float>(1000.0 / ms) : 0.0f; };
        context.CaptureViewport = [this](std::function<void(std::vector<uint8_t>, uint32_t, uint32_t, bool)> done)
        {
            Ref<Texture2D> image = m_Renderer ? m_Renderer->GetSceneResourceRef() : nullptr;
            if (!image)
            {
                done({}, 0, 0, false);
                return;
            }
            const bool bgra = m_Renderer->IsSurfaceFormatBGRA();
            m_Renderer->ReadbackTexture(std::move(image), [done, bgra](std::vector<uint8_t> pixels, uint32_t width, uint32_t height)
            {
                done(std::move(pixels), width, height, bgra);
            });
        };
        // Edits: each becomes one undo step (several in a frame: one step together), compared on the whole scene text.
        context.RecordEdit = [this](const std::string& step)
        {
            m_HistoryExternalStep = m_HistoryExternalStep.empty() ? step : std::string("MCP: several edits");
            m_SceneHierarchyPanel.MarkDirty();
        };
        context.Select = [this](const std::vector<Entity>& entities) { m_Selection.Set(entities); };
        context.ShapeMesh = [this](const std::string& shapeName) -> AssetHandle
        {
            for (uint8_t index = 0; index < static_cast<uint8_t>(PrimitiveShape::Count); ++index)
            {
                const std::string name = PrimitiveShapes::GetName(static_cast<PrimitiveShape>(index));
                if (name.size() == shapeName.size() &&
                    std::equal(name.begin(), name.end(), shapeName.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }))
                    return GetShapeAsset(static_cast<PrimitiveShape>(index));
            }
            return 0;
        };
        context.ShapeNames = []()
        {
            std::vector<std::string> names;
            for (uint8_t index = 0; index < static_cast<uint8_t>(PrimitiveShape::Count); ++index)
                names.push_back(PrimitiveShapes::GetName(static_cast<PrimitiveShape>(index)));
            return names;
        };
        context.SaveScene = [this](std::string& error)
        {
            if (m_EditorScenePath.empty())
            {
                error = "the scene was never saved: the user names it once (File > Save Scene As)";
                return false;
            }
            SaveScene();
            return true;
        };
        context.Undo = [this]()
        {
            const size_t before = m_History.GetPosition();
            const std::string name = m_History.CanUndo() ? m_History.GetUndoName() : std::string();
            Undo();
            return m_History.GetPosition() != before ? name : std::string();
        };
        context.Redo = [this]()
        {
            const size_t before = m_History.GetPosition();
            const std::string name = m_History.CanRedo() ? m_History.GetRedoName() : std::string();
            Redo();
            return m_History.GetPosition() != before ? name : std::string();
        };
        context.SnapshotScene = [this]() { return m_EditorScene ? SceneSerializer(m_EditorScene).SerializeToText() : std::string(); };
        context.RestoreScene = [this](const std::string& text) { RestoreHistoryState(text); };
        // Play and verify: what the Play / Simulate / Stop buttons do.
        context.Play = [this](const Mcp::PlayOptions& options, std::string& error)
        {
            if (m_SceneState == SceneState::Play)
            {
                error = "already in Play";
                return false;
            }
            // The call's choices for this Play only: the user's Play settings stay as they were.
            const PlayNetSettings userSettings = m_PlayNet;
            m_PlayNet.Mode = options.NetMode == "listen_server" ? PlayNetMode::ListenServer : options.NetMode == "client" ? PlayNetMode::Client : PlayNetMode::Standalone;
            m_PlayNet.Players = std::clamp(options.Players, 1, 8);
            m_PlayNet.Emulation = options.Emulation == "bad" ? 2 : options.Emulation == "average" ? 1 : 0;
            m_PlayNet.DebugTrace = options.DebugLog;
            if (m_PlayNet.Mode != PlayNetMode::Standalone)
            {
                if (m_EditorScenePath.empty())
                {
                    m_PlayNet = userSettings;
                    error = "multiplayer Play starts the other players on the saved scene, and this scene was never saved: the user names it once "
                            "(File > Save Scene As)";
                    return false;
                }
                m_PlayNet.SaveWithoutAsking = true; // (asking for multiplayer Play is asking for the save it needs)
            }
            OnScenePlay();
            m_PlayNet = userSettings;
            if (m_SceneState != SceneState::Play)
            {
                error = "Play did not start (read_log tells why)";
                return false;
            }
            return true;
        };
        context.Simulate = [this](std::string& error)
        {
            if (m_SceneState == SceneState::Simulate)
            {
                error = "already simulating";
                return false;
            }
            OnSceneSimulate();
            return true;
        };
        context.Stop = [this](std::string& error)
        {
            if (m_SceneState == SceneState::Edit)
            {
                error = "not in Play or Simulate";
                return false;
            }
            OnSceneStop();
            return true;
        };
        context.PlayRunning = [this]() { return m_SceneState == SceneState::Play && !m_PlayWaitingForServer; };
        context.Pause = [this](bool paused, int stepFrames, std::string& error)
        {
            if (m_SceneState == SceneState::Edit || !m_ActiveScene)
            {
                error = "not in Play or Simulate";
                return false;
            }
            m_ActiveScene->SetPaused(paused);
            if (paused && stepFrames > 0)
                m_ActiveScene->Step(stepFrames);
            return true;
        };
        context.Paused = [this]() { return m_SceneState != SceneState::Edit && m_ActiveScene && m_ActiveScene->IsPaused(); };
        context.FocusEntity = [this](Entity entity) { FocusOnEntity(entity); };
        context.GetCamera = [this]()
        {
            Mcp::ViewportCamera camera;
            camera.Position = m_EditorCamera.GetPosition();
            camera.FocalPoint = m_EditorCamera.GetFocalPoint();
            camera.Distance = m_EditorCamera.GetDistance();
            camera.Pitch = m_EditorCamera.GetPitch();
            camera.Yaw = m_EditorCamera.GetYaw();
            camera.Forward = m_EditorCamera.GetForwardDirection();
            camera.Orthographic = m_EditorCamera.IsOrthographic();
            return camera;
        };
        context.SetCamera = [this](const glm::dvec3& focalPoint, double distance, float pitch, float yaw)
        {
            SetViewMode(EditorViewMode::Perspective); // (an orthographic view's direction is fixed)
            m_EditorCamera.SetFocalPoint(focalPoint);
            m_EditorCamera.SetDistance(static_cast<float>(distance));
            m_EditorCamera.SetOrientation(pitch, yaw);
        };
        // Scripts and assets.
        context.ScriptBuild = [this]()
        {
            Mcp::ScriptBuildInfo info;
            info.HasProject = !ScriptEngine::GetProjectScriptsProject().empty();
            info.Building = m_ScriptBuilder.IsBuilding();
            info.Pending = m_ScriptBuilder.IsBuildPending();
            info.Status = m_ScriptBuilder.GetStatus();
            const ScriptBuilder::BuildResult& last = m_ScriptBuilder.GetLastResult();
            info.Number = last.Number;
            info.Succeeded = last.Succeeded;
            info.Reloaded = last.Reloaded;
            info.Seconds = last.Seconds;
            info.Diagnostics = last.Diagnostics;
            info.Log = last.Log.generic_string();
            return info;
        };
        context.RequestScriptBuild = [this]() { m_ScriptBuilder.RequestBuild(); };
        context.MaterialChanged = [](AssetHandle material)
        {
            Renderer::MarkMaterialChanged(material);
            AssetPreviewRenderer::Get().Invalidate(material);
        };
        context.AssetsChanged = [this]() { m_ContentBrowserPanel.Refresh(); };
        // Graphs through their editor window (opened when it is not), packaging as the dialog does.
        context.ApplyGraph = [this](AssetHandle graph, NodeGraph replacement) -> std::string
        {
            OpenAssetV2(graph);
            auto* editor = dynamic_cast<NodeGraphEditorV2*>(m_AssetEditors.Find(graph));
            if (!editor)
                return "the graph editor could not be opened";
            return editor->ApplyExternalEdit(std::move(replacement)) ? std::string() : std::string("the graph could not be saved (read_log)");
        };
        context.Package = [this](const PackageSettings& settings)
        {
            SaveScene(); // the scene as edited is what ships (the Package dialog does the same)
            return PackageGame(settings);
        };
        // (NoxEditorV2's multiplayer Play does not start the players' MCP servers yet: runtime_call finds none.)
        context.Runtimes = []() { return std::vector<Mcp::RuntimeEndpoint>{}; };
        m_Mcp = std::make_unique<Mcp::McpEditor>(std::move(context));
    }

    void EditorV2Layer::UpdateMcpServer()
    {
        if (m_Mcp)
            m_Mcp->Update();
    }

    std::vector<UI::UIMenuItem> EditorV2Layer::BuildMcpMenu()
    {
        if (!m_Mcp)
            return { { "MCP server not running in this build", "", {}, false, false, false, true } };
        const Mcp::McpEditor::MenuState state = m_Mcp->GetMenuState();
        std::vector<UI::UIMenuItem> items;
        items.push_back({ "Enabled", "", [this, enabled = state.Enabled]() { m_Mcp->SetEnabled(!enabled); }, false, true, state.Enabled, false,
                          "Lets AI tools (Claude Code, Codex, Gemini CLI, Cursor ...) read and use this editor over MCP, from this machine only" });
        items.push_back({ state.Running ? "Listening on " + state.Url + " -- " + std::to_string(state.Calls) + " calls"
                                        : (!state.Problem.empty() ? state.Problem : std::string("Off")), "", {}, false, false, false, true });
        items.push_back({ "", "", {}, true });
        items.push_back({ "Clients may:", "", {}, false, false, false, true });
        for (int32_t access = 0; access <= 2; ++access)
            items.push_back({ Mcp::McpEditor::AccessName(access), "", [this, access]() { m_Mcp->SetAccess(access); }, false, true, state.Access == access });
        items.push_back({ "", "", {}, true });
        items.push_back({ "Copy connect command (Claude Code)", "", [this]()
        {
            const std::string text = m_Mcp->ConnectCommand();
            if (!text.empty() && SDL_SetClipboardText(text.c_str()))
                Notify(UI::UINotificationSeverity::Info, "Connect command copied", "Paste it into a terminal in the project folder (it holds the token: do not share it).");
        }, false, state.Ready, false, false, "Paste it into a terminal in the project folder; then start Claude Code there" });
        items.push_back({ "Copy JSON config (other MCP clients)", "", [this]()
        {
            const std::string text = m_Mcp->JsonConfig();
            if (!text.empty() && SDL_SetClipboardText(text.c_str()))
                Notify(UI::UINotificationSeverity::Info, "JSON config copied", "It holds the token: do not share it.");
        }, false, state.Ready });
        items.push_back({ "Rotate token", "", [this]() { m_Mcp->RotateToken(); }, false, state.Initialized, false, false,
                          "A new token: connected clients stop working until they get the new connect command" });
        return items;
    }

    void EditorV2Layer::DestroyMcpServer()
    {
        m_Mcp.reset();
    }
}

#endif
