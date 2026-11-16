#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>

#include "EditorHistory.h"
#include "GamePackager.h"
#include "Panels/SceneHierarchyV2Panel.h"
#include "ScriptBuilder.h"
#include "StreamingTestWorld.h"
#include "Viewport/ViewportOverlayV2.h"
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Asset/PrimitiveShapes.h"
#include "NoxCore/Core/Layer.h"
#include "NoxCore/Core/Ref.h"
#include "NRI/Texture.h"
#include "NoxCore/Renderer/EditorCamera.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/SceneLoader.h"

namespace Nox
{
    class Renderer;
    class Renderer2D;
    class Scene;

    namespace UI
    {
        class UIElement;
        class UIImage;
        class UILabel;
        class UIStackBox;
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

        Renderer* m_Renderer = nullptr;
        Renderer2D* m_Renderer2D = nullptr;

        Ref<Scene> m_EditorScene;
        Ref<Scene> m_ActiveScene;
        std::filesystem::path m_EditorScenePath;
        Scope<SceneLoader> m_SceneLoad;
        AssetHandle m_SceneLoadHandle = 0;

        EditorCamera m_EditorCamera;
        SceneState m_SceneState = SceneState::Edit;

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

        Ref<UI::UIImage> m_ViewportImage;
        Ref<ViewportCanvas> m_ViewportCanvas;
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

        Ref<UI::UIElement> m_SceneLoadBanner;
        Ref<UI::UILabel> m_SceneLoadTitleLabel;
        Ref<UI::UILabel> m_SceneLoadStageLabel;
        Ref<UI::UIElement> m_SceneLoadProgressFill;

        Ref<UI::UILabel> m_FpsLabel;
        Ref<UI::UILabel> m_StatusLabel;
        SceneHierarchyV2Panel m_SceneHierarchyPanel;
        bool m_ShowSceneHierarchy = true;
        bool m_ViewportMouseCaptured = false;
        bool m_CursorHidden = false;

        // Tracked features intentionally deferred until their corresponding V2 panels exist:
        // - Properties / Inspector panel (`DrawComponents`, component UIs, AssetPreviewRenderer)
        // - Content Browser panel (drag-drop sources CONTENT_BROWSER_ITEM/ITEMS, CreatePrefab/Variant, ReplaceInScenes)
        // - Settings & Stats windows (render quality / RT / NRD / post-process / physics collider toggle UI)
        // - Output Log, Profiler, Render Graph, World Partition (HLOD), Levels, Audio Mixer panels
        // - Node Graph / Material / Animation Clip / Timeline panels (`OpenAsset` dispatchers)
        // - Modal dialogs (Package Game/Server, Streaming Test World) & Multi-Viewport dockspace
    };
}
