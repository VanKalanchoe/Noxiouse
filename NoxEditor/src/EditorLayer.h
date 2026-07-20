#pragma once
#include "NoxCore/Core/Layer.h"
#include "NoxCore/Events/InputEvents.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/Renderer.h"

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
        
    private:
        // UI Panels
        void UI_ToolBar();
        
        bool OnKeyPressed(KeyPressedEvent& e);
        bool IsButtonHovered() const;
        bool OnMouseButtonPressed(MouseButtonPressedEvent& event);
        void OnOverlayRender();

        void NewProject();
        bool OpenProject();
        void OpenProject(const std::filesystem::path& path);
        void SaveProject();
        
        void NewScene();
        void OpenScene();
        void OpenScene(AssetHandle handle);
        void SaveScene();
        void SaveSceneAs();
        
        void OnScenePlay();
        void OnSceneSimulate();
        void OnSceneStop();
        void OnScenePause();
        void OnDuplicateEntity();
        
        void SerializeScene(Ref<Scene> scene, const std::filesystem::path& path);

    private:
        Renderer* m_Renderer;
        
        Ref<Scene> m_ActiveScene;
        Ref<Scene> m_EditorScene;
        std::filesystem::path m_EditorScenePath;
        
        Entity m_HoveredEntity;
        
        bool m_PrimaryCamera = true;
        
        EditorCamera m_EditorCamera;
        
        bool m_ViewportFocused = false, m_ViewportHovered = false;
        glm::uvec2 m_ViewportSize = { 0.0f, 0.0f };
        glm::uvec2 lastViewportExtent = {0, 0};
        glm::vec2 m_ViewportBounds[2];//mouse selection
        
        int m_GizmoType = -1;
        
        bool m_ShowPhysicsColliders = false;
        
        enum class SceneState
        {
            Edit = 0, Play = 1, Simulate = 2
        };
        
        SceneState m_SceneState = SceneState::Edit;
        
        // Panels
        SceneHierarchyPanel m_SceneHierarchyPanel;
        Scope<ContentBrowserPanel> m_ContentBrowserPanel;
        
        // Editor resources always static
        Ref<Texture2D> m_IconPlay, m_IconPause, m_IconStep, m_IconStop, m_IconSimulate;
        
        Ref<Font> m_Font; 
    };
}
