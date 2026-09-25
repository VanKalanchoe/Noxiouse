#pragma once
#include <functional>
#include <unordered_map>

#include "NoxCore/Core/Layer.h"
#include "NoxCore/Events/InputEvents.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/RenderGraphPanel.h"
#include "Panels/NodeGraphEditorPanel.h"
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
        void UI_StatusBar();

        void NewProject();
        bool OpenProject();
        void OpenProject(const std::filesystem::path& path);
        void SaveProject();
        
        void NewScene();
        void OpenScene();
        void OpenScene(AssetHandle handle);
        void PlaceAssets(const std::vector<AssetHandle>& handles, const glm::vec3& point, const std::string& folder);
        void SaveScene();
        void SaveSceneAs();
        
        void OnScenePlay();
        void OnSceneSimulate();
        void OnSceneStop();

        // Unloads assets no live scene references anymore (GPU memory freed after in-flight frames).
        void UnloadUnusedAssets();
        void OnScenePause();
        void OnDuplicateEntity();
        
        void SerializeScene(Ref<Scene> scene, const std::filesystem::path& path);

        // Focuses an already-open editor for handle, or opens a new one (docs/Animation_Graph_Architecture_Plan_2026.md Step 3).
        void OpenNodeGraphEditor(AssetHandle handle);

        // Opens whatever editor window is registered for handle's AssetType (m_AssetOpeners); does nothing for
        // types with none. What double-clicking an asset in the Content Browser or an inspector reference calls.
        void OpenAsset(AssetHandle handle);

        // Any open node-graph editor window focused or hovered: the scene's shortcuts and picking must ignore input.
        bool AnyNodeGraphEditorWantsInput() const;
        bool AnyNodeGraphEditorHovered() const; // mouse only: focus alone must not swallow a click elsewhere

    private:
        Renderer* m_Renderer;
        Renderer2D* m_Renderer2D;
        
        Ref<Scene> m_ActiveScene;
        Ref<Scene> m_EditorScene;
        std::filesystem::path m_EditorScenePath;
        
        Entity m_HoveredEntity;

        struct PlacementPreview
        {
            AssetHandle Handle = 0;
            Entity Root;
            glm::vec3 InitialTranslation = { 0.0f, 0.0f, 0.0f };
            bool Active = false;
        };

        PlacementPreview m_PlacementPreview;
        
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
        bool m_UnloadUnusedAssetsRequested = false;
        
        // Panels
        SceneHierarchyPanel m_SceneHierarchyPanel;
        Scope<ContentBrowserPanel> m_ContentBrowserPanel;
        Scope<RenderGraphPanel> m_RenderGraphPanel;
        // Open node-graph editors (docs/Animation_Graph_Architecture_Plan_2026.md Step 3), one per opened graph
        // asset; OpenNodeGraphEditor focuses an already-open one instead of duplicating it.
        std::vector<Scope<NodeGraphEditorPanel>> m_NodeGraphEditors;
        // AssetType -> "open its editor window", so a new asset editor is one entry here rather than new
        // double-click handling in every panel that shows asset references.
        std::unordered_map<AssetType, std::function<void(AssetHandle)>> m_AssetOpeners;

        // Editor resources always static
        Ref<Texture2D> m_IconPlay, m_IconPause, m_IconStep, m_IconStop, m_IconSimulate;
        
        Ref<Font> m_Font; 
        
        // Multi Select Viewport
        bool m_IsBoxSelecting = false;
        glm::vec2 m_BoxSelectStart = { 0.0f, 0.0f };
        glm::vec2 m_BoxSelectEnd = { 0.0f, 0.0f };

        // What the status bar shows, taken once per StatusBarRefreshSeconds so the counts stay readable.
        struct StatusBarCounts
        {
            size_t Loading = 0;
            size_t Streaming = 0;
            double PendingMB = 0.0;
            size_t BlasBuilds = 0;
        };
        static constexpr double StatusBarRefreshSeconds = 1.0;
        StatusBarCounts m_StatusBarCounts;
        double m_StatusBarRefreshTime = -StatusBarRefreshSeconds;
    };
}
