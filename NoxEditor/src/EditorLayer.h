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
        // Writes a prefab from the dragged hierarchy entity (or the whole selection when it is part of it) into the Content
        // Browser folder (relative to the asset directory) and registers it. The scene is not changed. 0 on failure.
        AssetHandle CreatePrefab(UUID dragged, const std::filesystem::path& relativeFolder);
        // A new variant of a prefab in the Content Browser folder (relative to the asset directory), registered; 0 on failure.
        AssetHandle CreateVariant(AssetHandle base, const std::filesystem::path& relativeFolder);
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

        // Prefab Mode (docs/Prefab_Architecture_Plan_2026.md P3): the prefab's entities as a scene of their own, edited in
        // isolation. The level is kept aside and comes back on exit; Save writes the .nprefab, reloads the asset and spawns every
        // instance in the level again.
        void OpenPrefabMode(AssetHandle handle);
        // Snapping (like Unreal's viewport toolbar): one switch, and a step each for location (in cm), rotation (degrees) and scale.
        // Ctrl inverts it while held. The location step is in cm whatever the world unit is (WorldUnits converts).
        void UI_ViewportOverlay();
        // The world grid on the ground plane (y = 0): cells of the snap step (sparser far from the ground), every tenth line stronger, the
        // axes coloured, fading out with distance. Thin lines of the 2D overlay pass, so it is depth tested.
        void DrawWorldGrid();
        // Drops every selected top entity straight down until its bounds' bottom rests on the surface below (the top of the highest
        // other mesh whose bounds overlap it from above), or on the ground plane when there is none.
        void SnapSelectionToFloor();
        // While a translation snaps: tick marks at every step along the axis being dragged (the current one lit), or a checkerboard of
        // snap-sized squares on the plane being dragged (the current cell lit), so the steps can be seen. ImGui draw list, inside the
        // viewport window.
        void DrawTranslationSnapFeedback(const glm::mat4& view, const glm::mat4& projection, const glm::mat4& current);
        float LocationSnapWorldUnits() const;
        glm::vec3 SnapLocation(const glm::vec3& point) const; // the point itself while snapping is off

        void SavePrefabMode();
        void ExitPrefabMode();
        // Leaves Prefab Mode; with unsaved changes asks first (Save / Don't Save / Cancel) and then runs `afterwards` (opening a scene).
        void RequestExitPrefabMode(std::function<void()> afterwards = {});
        std::string CurrentPrefabText();
        void UpdatePrefabModeDirty();
        void UI_PrefabExitPrompt();

        // Any open node-graph editor window focused or hovered: the scene's shortcuts and picking must ignore input.
        bool AnyNodeGraphEditorWantsInput() const;
        bool AnyNodeGraphEditorHovered() const; // mouse only: focus alone must not swallow a click elsewhere

    private:
        Renderer* m_Renderer;
        Renderer2D* m_Renderer2D;
        
        Ref<Scene> m_ActiveScene;
        Ref<Scene> m_EditorScene;
        std::filesystem::path m_EditorScenePath;

        struct PrefabModeState
        {
            bool Active = false;
            AssetHandle Prefab = 0;
            std::string Name;
            Ref<Scene> ReturnScene;                // the level being edited before Prefab Mode
            std::filesystem::path ReturnScenePath;
            UUID Root = 0;                         // the prefab's root entity in the prefab scene
            bool Variant = false;                  // a variant: the scene holds an instance of its base carrying the variant's changes
            // Unsaved changes: the prefab's text once everything in the scene had spawned, against the text now.
            std::string Snapshot;
            bool SnapshotTaken = false;
            bool Dirty = false;
            int FramesSinceCheck = 0;
        };
        PrefabModeState m_PrefabMode;

        bool m_SnapEnabled = false;
        int m_LocationSnapIndex = 2; // 10 cm
        int m_RotationSnapIndex = 2; // 15 degrees
        int m_ScaleSnapIndex = 1;    // 0.25
        bool m_ShowGrid = true;
        bool m_GizmoWasUsing = false;
        glm::mat4 m_GizmoDragStart{ 1.0f }; // the dragged entity's world transform when the drag began
        bool m_ShowPrefabExitPrompt = false;
        std::function<void()> m_AfterPrefabExit;
        
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
