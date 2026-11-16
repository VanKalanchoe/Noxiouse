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
#include "Panels/SceneHierarchyV2Panel.h"
#include "ProjectManager/ProcessLauncher.h"
#include "ScriptBuilder.h"
#include "StreamingTestWorld.h"
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Asset/PrimitiveShapes.h"
#include "NoxCore/Core/Layer.h"
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

        bool NewProject();
        bool OpenProject();
        bool OpenProject(const std::filesystem::path& path);
        bool OpenStartupProject(const std::filesystem::path& failed = {});
        void OpenProjectManager();

        void NewScene();
        void OpenScene(AssetHandle handle);
        void FinishSceneLoad();
        void SaveScene();
        void SaveSceneAs();
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
        void ShowOutputLog();
        bool IsPanelDocked(const std::string& title) const;
        void TogglePanel(const std::string& title);

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

        ScriptBuilder m_ScriptBuilder;
        Scope<StreamingTestWorldGenerator> m_TestWorldGeneration;
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
        void UpdateMaterialEditors();

        struct MaterialEditorState
        {
            AssetHandle Handle = 0;
            std::string Title;
            Ref<UI::UIWindow> Window;
            Ref<UI::UIStackBox> Body;
            std::unordered_map<std::string, bool> ExpandedSections;
            std::vector<std::function<void()>> Syncs;
            bool Dirty = true;
        };

        SceneHierarchyV2Panel m_SceneHierarchyPanel;
        InspectorV2Panel m_InspectorPanel;
        ContentBrowserV2Panel m_ContentBrowserPanel;
        OutputLogV2Panel m_OutputLogPanel;
        Ref<UI::UIElement> m_ContentBrowserRoot;
        Ref<UI::UIElement> m_OutputLogRoot;
        std::vector<MaterialEditorState> m_MaterialEditors;
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
