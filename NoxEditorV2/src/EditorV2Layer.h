#pragma once

#include <filesystem>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Asset/PrimitiveShapes.h"
#include "NoxCore/Core/Layer.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/Renderer/EditorCamera.h"

namespace Nox
{
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
        bool NewProject();
        bool OpenProject();
        bool OpenProject(const std::filesystem::path& path);
        bool OpenStartupProject(const std::filesystem::path& failed = {});
        void OpenProjectManager();

        void NewScene();
        void SaveScene();
        void SaveSceneAs();
        void CreateBasicScene();
        void CreateShape(PrimitiveShape shape);
        AssetHandle GetShapeAsset(PrimitiveShape shape);
        AssetHandle RegisterSceneAsset(const std::filesystem::path& scenePath);
        void AdoptStartSceneIfNone(const std::filesystem::path& scenePath);
        void ApplyBasicSceneView();
        void CaptureProjectThumbnail();

        void BuildUI(bool loadSavedLayout = true);

        Ref<Scene> m_ActiveScene;
        std::filesystem::path m_ActiveScenePath;
        EditorCamera m_EditorCamera;

        bool m_CreateBasicScene = false;
        bool m_ApplyBasicSceneView = false;
        bool m_ThumbnailPending = false;
        float m_ThumbnailTimer = 0.0f;
        bool m_BuildScriptsOnSave = false;

        float m_FpsAccumulator = 0.0f;
        uint32_t m_FpsFrames = 0;

        Ref<UI::UIImage> m_ViewportImage;
        Ref<UI::UILabel> m_FpsLabel;
        Ref<UI::UILabel> m_StatusLabel;
        bool m_ViewportMouseCaptured = false;
    };
}
