#pragma once

#include <filesystem>

#include "NoxCore/Core/Layer.h"
#include "NoxCore/Events/InputEvents.h"
#include "Panels/SceneHierarchyPanel.h"
#include "NoxCore/Asset/TextureImporter.h"

namespace Nox
{
    class EditorLayer : public Layer
    {
    public:
        EditorLayer();
        ~EditorLayer() override;
        
        void OnEvent(Event& event) override;
        void OnUpdate(Timestep ts) override;
        void OnRender() override;
        void OnImGuiRender() override;
        bool OnKeyPressed(KeyPressedEvent& e);
        bool IsButtonHovered() const;
        bool OnMouseButtonPressed(MouseButtonPressedEvent& event);

    private:
        Ref<Scene> m_ActiveScene;
        Ref<Scene> m_EditorScene;
        /*std::filesystem::path m_EditorScenePath;*/
        
        EditorCamera m_EditorCamera;
        
        enum class SceneState
        {
            Edit = 0, Play = 1, Simulate = 2
        };
    
        Ref<Texture2D> texture1;
        
        SceneState m_SceneState = SceneState::Edit;
        
        // Panels
        SceneHierarchyPanel m_SceneHierarchyPanel;
    };
}
