#pragma once

#include <filesystem>

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
        explicit EditorV2Layer(std::filesystem::path projectFile = {});
        ~EditorV2Layer() override;

        void OnUpdate(Timestep ts) override;
        void OnEvent(Event& event) override;

    private:
        void OpenProject(const std::filesystem::path& path);
        void BuildUI(bool loadSavedLayout = true);

        Ref<Scene> m_ActiveScene;
        EditorCamera m_EditorCamera;

        Ref<UI::UIImage> m_ViewportImage;
        Ref<UI::UILabel> m_StatusLabel;
        bool m_ViewportMouseCaptured = false;
    };
}
