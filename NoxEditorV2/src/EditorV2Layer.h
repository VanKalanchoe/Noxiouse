#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>

#include "EditorHistory.h"
#include "GamePackager.h"
#include "ScriptBuilder.h"
#include "StreamingTestWorld.h"
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Asset/PrimitiveShapes.h"
#include "NoxCore/Core/Layer.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/Renderer/EditorCamera.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/SceneLoader.h"

namespace Nox
{
    class Renderer;
    class Renderer2D;
    class Scene;

    namespace UI
    {
        class UIImage;
        class UILabel;
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
        void CreateShape(PrimitiveShape shape);
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
        void RefreshStatusBar();

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
        Ref<UI::UILabel> m_FpsLabel;
        Ref<UI::UILabel> m_StatusLabel;
        bool m_ViewportMouseCaptured = false;
    };
}
