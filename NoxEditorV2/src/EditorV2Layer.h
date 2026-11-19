#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "EditorHistory.h"
#include "EditorViewportV2.h"
#include "GamePackager.h"
#include "Panels/ContentBrowserV2Panel.h"
#include "Panels/InspectorV2Panel.h"
#include "Panels/OutputLogV2Panel.h"
#include "Panels/AssetEditorV2.h"
#include "Panels/LevelsV2Panel.h"
#include "Panels/MixerV2Panel.h"
#include "Panels/ProfilerV2Panel.h"
#include "Panels/RenderGraphV2Panel.h"
#include "Panels/WorldPartitionV2Panel.h"
#include "Panels/ProjectSettingsV2Panel.h"
#include "Panels/RendererSettingsV2Panel.h"
#include "Panels/NodeGraph/NodeGraphEditorV2.h"
#include "EditorCommands.h"
#include "EditorSelection.h"
#include "Panels/SceneHierarchyV2Panel.h"
#include "ProjectManager/ProcessLauncher.h"
#include "ScriptBuilder.h"
#include "StreamingTestWorld.h"
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Asset/PrimitiveShapes.h"
#include "NoxCore/Core/Layer.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Core/Ref.h"
#include "NRI/Texture.h"
#include "NoxCore/Network/NetSession.h"
#include "NoxCore/Renderer/EditorCamera.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/SceneLoader.h"
#include "NoxCore/UI/Layout/UIWindow.h"
#include "NoxCore/UI/Widgets/UIMenu.h"

namespace Nox::Mcp
{
    class McpEditor;
}

namespace Nox
{
    class Renderer;
    class Renderer2D;
    class Scene;

    namespace UI
    {
        class UIDockSpace;
        class UIElement;
        class UIImage;
        class UILabel;
        class UIStackBox;
        class UIWindow;
    }

    class EditorV2Layer : public Layer
    {
    public:
        explicit EditorV2Layer(std::filesystem::path projectFile = {}, bool createBasicScene = false);
        ~EditorV2Layer() override;

        void OnUpdate(Timestep ts) override;
        void OnEvent(Event& event) override;

    private:
        enum class SceneState
        {
            Edit = 0,
            Play = 1,
            Simulate = 2
        };

        // Blocking (the start-up loop, before the editor runs); the menus use the Async ones (the dialog does not stop the editor).
        bool NewProject();
        bool OpenProject();
        void NewProjectAsync();
        void OpenProjectAsync();
        bool OpenProject(const std::filesystem::path& path);
        bool OpenStartupProject(const std::filesystem::path& failed = {});
        void OpenProjectManager();

        void NewScene();
        void OpenScene(AssetHandle handle);
        void FinishSceneLoad();
        // `then` runs once the scene is written or not (an untitled scene asks for its name first, without stopping the editor).
        void SaveScene(std::function<void(bool saved)> then = {});
        void SaveSceneAs(std::function<void(bool saved)> then = {});
        bool SaveSceneTo(std::filesystem::path path);
        // Unsaved work (the scene, open graphs, the mixer): asked about before what would drop it -- Save (then go on), Don't Save
        // (the edits are dropped: graphs go back to their files; then go on), Cancel. `action` names what would happen ("opening
        // the project"); `scope` is what the action drops (a new scene drops the scene, not an open graph's edits). Without
        // unsaved work in scope it goes on at once.
        enum UnsavedScope : uint32_t { UnsavedScene = 1, UnsavedAssets = 2, UnsavedMixer = 4, UnsavedAll = 7 }; // (assets: the open asset editors)
        void ConfirmUnsavedChanges(const std::string& action, std::function<void()> proceed, uint32_t scope = UnsavedAll);
        bool HasUnsavedChanges(uint32_t scope = UnsavedAll) const;
        // Save / Don't Save / Cancel: the dialog every unsaved-changes question uses.
        void ShowSaveChangesDialog(const std::string& text, std::function<void()> save, std::function<void()> dontSave);
        void UpdateSceneDirtyState();
        // The scene the editor shows changed (opened, Play / Stop, undo put another copy back): the selection and the panels follow.
        void SetSceneContext(const Ref<Scene>& scene);
        void SerializeScene(const Ref<Scene>& scene, const std::filesystem::path& path);

        void CreateBasicScene();
        void CreateShape(PrimitiveShape shape, const std::string& folder = {});
        void PlaceAssets(const std::vector<AssetHandle>& handles, const glm::dvec3& point, const std::string& folder);
        void FocusOnEntity(Entity entity);
        void SnapSelectionToFloor();
        void SnapSelectionToGrid();
        AssetHandle GetShapeAsset(PrimitiveShape shape);
        AssetHandle RegisterSceneAsset(const std::filesystem::path& scenePath);
        void AdoptStartSceneIfNone(const std::filesystem::path& scenePath);
        void ApplyBasicSceneView();
        void CaptureProjectThumbnail();

        void SaveEditorCameras();
        bool RestoreEditorCameras();
        void BuildScriptsIfNeverBuilt();
        std::string CustomLitSignature() const;
        void PollCustomLitShaders();
        void WriteMaterialShaderEditorSettings();

        void UpdateHistory();
        void Undo();
        void Redo();
        void RestoreHistoryState(const std::string& text);
        void UnloadUnusedAssets();

        void OnScenePlay();
        void OnSceneSimulate();
        void OnSceneStop();
        void OnScenePause();

        enum class PlayNetMode : uint8_t { Standalone = 0, ListenServer, Client };
        struct PlayNetSettings
        {
            PlayNetMode Mode = PlayNetMode::Standalone;
            int Players = 2;
            int Emulation = 0;
            int Port = Net::DefaultPort;
            bool SaveWithoutAsking = false;
            bool DebugTrace = false;
        };
        std::filesystem::path EditorSceneAssetPath() const;
        AssetHandle EditorSceneHandle() const;
        void BeginMultiplayerPlay();
        void StartPlayScene(Scene::NetMode netMode);
        void EndMultiplayerPlay();
        bool LaunchPlayer(const std::vector<std::string>& arguments, bool console, const char* what);
        void ShowPlaySettingsOverlay(UI::UIElement* anchor);
        void ShowSaveAndPlayOverlay();

        enum class ViewportLayout { One = 0, TwoSideBySide, TwoStacked, Three, Four };
        void ApplyViewportLayout(ViewportLayout layout);
        void OpenViewport(int index);
        void CloseViewport(int index);
        void ActivateViewport(int index);
        void SyncViewportTabBars();
        int HoveredViewport() const;
        int OpenViewportCount() const;
        void SubmitExtraViewports(Timestep ts);
        std::vector<UI::UIMenuItem> BuildViewportOptionsItems(int index);
        void LoadViewportPrefs();
        void SaveViewportPrefs() const;

        void SetViewMode(EditorViewMode mode);
        int GridPlaneAxis() const;
        void SyncOrthoRenderSettings(bool wasOrthographic);
        void OnOverlayRender();
        void DrawWorldGrid();
        void DrawReferenceFigure(const glm::dvec3& worldPosition);
        void DrawPhysicsColliders3D();
        void RefreshStatusBar();
        void RefreshToolBar();
        void RefreshSceneLoadOverlay();

        void BuildUI(bool loadSavedLayout = true);

        // The layout files live with the project (<Project>/Saved/Layout, like Unreal's Saved/Config): every project keeps its own,
        // nothing in the engine's folder is written. Reset Layout runs at the start of the next frame (not inside the menu's click).
        std::filesystem::path LayoutDirectory() const;
        void SaveLayoutFiles();
        void ResetLayout();
        // The Content Browser and the Output Log live in the drawer (Unreal's Content Drawer: slides up over the viewport from the
        // status bar, Ctrl+Space / `) until "Dock in Layout" docks them as tabs under the viewport.
        enum class DrawerPage : uint8_t { None = 0, Content, Log };
        void ShowPage(DrawerPage page);   // the drawer opened on it, or its docked tab brought to the front
        void ToggleDrawer(DrawerPage page);
        void OpenDrawer(DrawerPage page);
        void CloseDrawer();
        bool IsDrawerOpen() const;
        void DockPage(DrawerPage page);   // "Dock in Layout"
        void UndockPage(DrawerPage page); // back into the drawer
        void ShowOutputLog() { ShowPage(DrawerPage::Log); }
        bool IsPanelDocked(const std::string& title) const;

        // Entities from the hierarchy as a .nprefab in `relativeFolder` (the dragged one, or the selection it is part of).
        AssetHandle CreatePrefab(UUID dragged, const std::filesystem::path& relativeFolder);
        // A toast at the bottom right (above the status bar).
        void Notify(UI::UINotificationSeverity severity, std::string title, std::string message = {});

        Renderer* m_Renderer = nullptr;
        Renderer2D* m_Renderer2D = nullptr;

        Ref<Scene> m_EditorScene;
        Ref<Scene> m_ActiveScene;
        std::filesystem::path m_EditorScenePath;
        Scope<SceneLoader> m_SceneLoad;
        AssetHandle m_SceneLoadHandle = 0;

        EditorCamera m_EditorCamera;
        SceneState m_SceneState = SceneState::Edit;

        static constexpr int kMaxViewports = 4;
        struct ViewportSlot
        {
            const char* Title = "Viewport";
            bool Open = false;
            bool CameraSet = false;
            EditorCamera Camera;
            glm::uvec2 Size = { 0, 0 };
            bool Hovered = false;
            bool Submitted = false;

            uint32_t ShowFlags = Renderer::ExtraViewOutline;
            bool ShowGrid = true;
            int ResolutionIndex = 0;
            bool Realtime = true;
            bool ShowCameraPosition = false;

            glm::mat4 DrawnViewProjection{ 0.0f };
            glm::uvec2 DrawnSize{ 0, 0 };
            uint32_t DrawnFlags = 0;
            size_t DrawnHistory = SIZE_MAX;
            size_t DrawnSelection = 0;
            float SinceDrawn = 0.0f;
        };
        std::array<ViewportSlot, kMaxViewports> m_Viewports;
        int m_ActiveViewport = 0;

        PlayNetSettings m_PlayNet;
        std::unique_ptr<Net::NetSession> m_PlaySession;
        std::vector<std::unique_ptr<ChildProcess>> m_PlayProcesses;
        bool m_PlayWaitingForServer = false;
        bool m_PlaySessionEnded = false;
        bool m_OpenSaveAndPlay = false;

        struct OrthoSavedSettings
        {
            bool Valid = false;
            bool Dlss = false;
            bool RayTracing = false;
            bool PathTracing = false;
        } m_OrthoSaved;

        EditorHistory m_History;
        bool m_HistoryCheckRequested = false;
        // The scene changed since it was opened or saved: an undo step was taken (or one undone / redone) since then.
        bool m_SceneDirty = false;
        bool m_SceneCleanPending = true; // set on open / new / save: the history position then is the clean one
        size_t m_HistoryPositionSeen = 0;
        std::string m_WindowTitleShown;

        ScriptBuilder m_ScriptBuilder;
        Scope<StreamingTestWorldGenerator> m_TestWorldGeneration;
        // File > Package Game / Server, Create > Streaming Test World (EditorV2Dialogs.cpp).
        void ShowPackageDialog(bool server);
        void ShowStreamingTestWorldDialog();
        void CancelStreamingTestWorld();
        // The MCP server (EditorV2Mcp.cpp; NoxEditor's tools through Mcp::EditorContext). An MCP edit names the next undo step.
        std::string m_HistoryExternalStep;
#if NOX_WITH_MCP
        void CreateMcpServer();
        void UpdateMcpServer();
        void DestroyMcpServer();
        std::vector<UI::UIMenuItem> BuildMcpMenu();
        std::unique_ptr<Mcp::McpEditor> m_Mcp;
#endif
        PackageSettings m_PackageSettings;
        std::string m_PackageOutputText;
        std::vector<std::function<void()>> m_PackageSyncs;
        StreamingTestWorldGenerator::Settings m_TestWorldSettings = [] { StreamingTestWorldGenerator::Settings s; s.TargetGigabytes = 2.0f; return s; }();
        std::optional<glm::dvec3> m_FocusOnOpen;

        bool m_CreateBasicScene = false;
        bool m_ApplyBasicSceneView = false;
        bool m_ThumbnailPending = false;
        float m_ThumbnailTimer = 0.0f;

        float m_CustomLitPollTimer = 0.0f;
        std::string m_CustomLitSignature;

        bool m_UnloadUnusedAssetsRequested = false;
        std::chrono::steady_clock::time_point m_LastUnloadSweep{};
        std::unordered_set<AssetHandle> m_EditorSceneReferences;
        bool m_EditorSceneReferencesValid = false;

        float m_StatusRefreshTimer = 0.0f;
        float m_FpsAccumulator = 0.0f;
        uint32_t m_FpsFrames = 0;

        std::array<Ref<UI::UIElement>, kMaxViewports> m_ViewportSlotRoots;
        Ref<UI::UIDockSpace> m_ViewportDockSpace;
        Ref<UI::UIElement> m_ActiveViewportHost;
        Ref<UI::UIImage> m_ViewportImage;
        Ref<ViewportCanvas> m_ViewportCanvas;
        std::array<Ref<UI::UIElement>, kMaxViewports> m_ExtraViewportHosts;
        std::array<Ref<UI::UIImage>, kMaxViewports> m_ExtraViewportImages;
        std::array<Ref<ExtraViewportCanvas>, kMaxViewports> m_ExtraViewportCanvases;
        ViewportToolState m_ViewportTools;
        ViewportOverlayToolbar m_ViewportToolbar;
        Entity m_HoveredEntity;

        Ref<Texture2D> m_IconPlay;
        Ref<Texture2D> m_IconStop;
        Ref<Texture2D> m_IconPause;
        Ref<Texture2D> m_IconSimulate;
        Ref<Texture2D> m_IconStep;

        Ref<UI::UIStackBox> m_ToolBar;
        Ref<UI::UIElement> m_PlayBtnWrap;
        Ref<UI::UIElement> m_SimulateBtnWrap;
        Ref<UI::UIElement> m_PauseBtnWrap;
        Ref<UI::UIElement> m_StepBtnWrap;
        Ref<UI::UIImage> m_PlayBtnIcon;
        Ref<UI::UIImage> m_SimulateBtnIcon;
        Ref<UIVectorIconButton> m_PlaySettingsCogBtn;
        Ref<UI::UILabel> m_PlayNetStatusLabel;

        Ref<UI::UIElement> m_SceneLoadBanner;
        Ref<UI::UILabel> m_SceneLoadTitleLabel;
        Ref<UI::UILabel> m_SceneLoadStageLabel;
        Ref<UI::UIElement> m_SceneLoadProgressFill;

        Ref<UI::UILabel> m_FpsLabel;
        Ref<UI::UIElement> m_StatusBar;
        Ref<UI::UIElement> m_StatusActivity; // left: Ready / loading / streaming / importing / building
        Ref<UI::UIElement> m_StatusCounts;   // right: warnings and errors in the log (click: the Output Log)
        bool m_ResetLayoutRequested = false;
        bool m_LayoutBuilt = false;
        uint64_t m_LastScriptBuildNotified = 0;
        void OpenAssetV2(AssetHandle handle);
        // Asset editors (E6): which editor opens which asset type is registered once (RegisterAssetEditors); the manager owns
        // the open windows, asks before one with unsaved edits closes and ends an editor whose window is gone.
        void RegisterAssetEditors();
        AssetEditorManager m_AssetEditors;

        // Window > Renderer Settings (this session's rendering) and Window > Project Settings (what the project keeps); floating
        // windows, updated while open (R1).
        RendererSettingsV2Panel m_RendererSettings;
        ProjectSettingsV2Panel m_ProjectSettings;
        LevelsV2Panel m_LevelsPanel;          // Window > Levels (L1)
        WorldPartitionV2Panel m_WorldPartitionPanel; // Window > World Partition (L1)
        MixerV2Panel m_MixerPanel;                   // Window > Mixer (A1)
        ProfilerV2Panel m_ProfilerPanel;             // Window > Profiler (P1)
        RenderGraphV2Panel m_RenderGraphPanel;       // Window > Render Graph (P1)
        Scope<HLODBuilder> m_HLODBuild;              // Build HLODs: a step a frame while it runs
        void StartHLODBuild();
        void CancelHLODBuild(const char* reason); // (the scene changes, Play / Simulate start)
        void OpenToolWindow(const std::string& title, const Ref<UI::UIElement>& root, UI::UIVec2 size);
        void UpdateToolWindows();
        void ApplyQualityPreset(int quality);

        // What is selected: the editor's, read and changed by every panel, the viewports and the shortcuts (E3).
        EditorSelection m_Selection;
        // What the menus and keys run (E4; EditorV2Commands.cpp).
        EditorCommandList m_Commands;
        void RegisterCommands();
        // A key press: the focused widget first, then the command bound to it. True: dispatched here (the UI layer must not
        // dispatch it again).
        bool DispatchCommandKey(KeyPressedEvent& key);
        bool IsLevelFocus() const;
        std::filesystem::path KeyBindingsPath() const;
        void OpenKeyboardShortcuts();
        void RebuildKeyboardShortcuts();
        Ref<UI::UIStackBox> m_ShortcutsList;
        std::string m_CaptureCommand; // the command whose key the Keyboard Shortcuts window waits for
        bool m_CapturePrimary = true;
        SceneHierarchyV2Panel m_SceneHierarchyPanel;
        InspectorV2Panel m_InspectorPanel;
        ContentBrowserV2Panel m_ContentBrowserPanel;
        OutputLogV2Panel m_OutputLogPanel;
        Ref<UI::UIElement> m_ContentBrowserRoot;
        Ref<UI::UIElement> m_OutputLogRoot;
        Ref<UI::UIElement> m_CenterArea;      // the dock space with the drawer over it
        Ref<UI::UIElement> m_DrawerHost;      // where the drawer's page goes
        Ref<UI::UIElement> m_ContentDockButton, m_LogDockButton;
        Ref<UI::UIElement> m_ContentDrawerTab, m_LogDrawerTab; // in the status bar
        DrawerPage m_DrawerPage = DrawerPage::None;
        float m_DrawerFraction = 0.42f;
        bool m_ShowSceneHierarchy = true;
        bool m_ShowInspector = true;
        bool m_ShowToolBar = true;
        bool m_ViewportMouseCaptured = false;
        bool m_CursorHidden = false;

        // Tracked features intentionally deferred until their corresponding V2 panels exist:
        // - Content Browser: prefab variants, Replace References, the asset preview panel (thumbnails, browser, drag and drop,
        //   Create Prefab are in)
        // - Settings & Stats windows (render quality / RT / NRD / post-process / physics collider toggle UI)
        // - Output Log, Profiler, Render Graph, World Partition (HLOD), Levels, Audio Mixer panels
        // - Node Graph / Animation Clip / Timeline panels
        // - Modal dialogs (Package Game/Server, Streaming Test World)
    };
}
