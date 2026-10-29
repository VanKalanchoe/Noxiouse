#pragma once
#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <unordered_map>

#include "NoxCore/Core/Layer.h"
#include "NoxCore/Events/InputEvents.h"
#include "EditorHistory.h"
#include "ScriptBuilder.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/ProfilerPanel.h"
#include "Panels/WorldPartitionPanel.h"
#include "NoxCore/Scene/HLODBuilder.h"
#include "StreamingTestWorld.h"
#include "Panels/RenderGraphPanel.h"
#include "Panels/NodeGraphEditorPanel.h"
#include "Panels/MaterialEditorPanel.h"
#include "Panels/AnimationClipPanel.h"
#include "Panels/TimelinePanel.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/SceneLoader.h"

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

        // A new project: its .nproj picked in a save dialog, its folders made beside it. False when cancelled or it failed.
        bool NewProject();
        // A .nproj picked in a dialog. False when cancelled or it did not load.
        bool OpenProject();
        bool OpenProject(const std::filesystem::path& path);
        // Without a project on the command line: the most recent one, else the sample project above the editor's folder, else
        // a choice of opening one, making one or quitting (asked again until one opens). False: the user chose to quit.
        bool OpenStartupProject();
        void SaveProject();
        
        // Each viewport's camera per scene, in <project>/Cache/EditorCameras.txt: a scene opens where it was looked at last.
        void SaveEditorCameras();
        void RestoreEditorCameras();
        // Moves the selection's positions to the nearest point of the location step's grid.
        void SnapSelectionToGrid();
        void NewScene();
        void OpenScene();
        void OpenScene(AssetHandle handle);
        // The scene being opened (SceneLoader: read and parsed on a thread, its entities created a slice a frame); the one shown
        // until then stays. FinishSceneLoad swaps it in.
        void FinishSceneLoad();
        void UI_SceneLoadProgress();
        void PlaceAssets(const std::vector<AssetHandle>& handles, const glm::vec3& point, const std::string& folder);
        // Basic shapes (docs/Future.md item 10): the static mesh asset of a shape, written into the project's Shapes folder and
        // imported the first time it is asked for (0 on failure); CreateShape places it where the editor camera looks.
        AssetHandle GetShapeAsset(PrimitiveShape shape);
        void CreateShape(PrimitiveShape shape, const std::string& folder);
        // Writes a prefab from the dragged hierarchy entity (or the whole selection when it is part of it) into the Content
        // Browser folder (relative to the asset directory) and registers it. The scene is not changed. 0 on failure.
        AssetHandle CreatePrefab(UUID dragged, const std::filesystem::path& relativeFolder);
        // A new variant of a prefab in the Content Browser folder (relative to the asset directory), registered; 0 on failure.
        AssetHandle CreateVariant(AssetHandle base, const std::filesystem::path& relativeFolder);
        void SaveScene();
        void SaveSceneAs();
        // File > Package Game (GamePackager): the output folder, every scene or the start scene, and the last result.
        void UI_PackageGame();
        // The project's C# scripts: built (dotnet) and hot-reloaded on request or after a source changed.
        ScriptBuilder m_ScriptBuilder;
        bool m_ShowPackageGame = false;
        char m_PackageOutput[512] = {};
        bool m_PackageAllScenes = false;
        bool m_PackagePackFiles = true;
        bool m_PackagePrecompiledShaders = true;
        bool m_PackageSelfContainedDotNet = false;
        std::string m_PackageResult;

        // Undo / redo (EditorHistory): UpdateHistory runs at the end of every ImGui frame, after the panels made their changes.
        void UpdateHistory();
        void Undo();
        void Redo();
        // Makes a history state (scene text) the edited scene; the selection is kept by UUID.
        void RestoreHistoryState(const std::string& text);
        
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
        void OpenMaterialEditor(AssetHandle handle);

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
        // UE5's "Frame Selected" (F) / double-clicking an entity in the Outliner: re-centres the editor camera on it, sized to
        // its world bounds (its meshes and everything below it), falling back to its Position alone when it has no mesh.
        void FocusOnEntity(Entity entity);
        // 3D physics collider wireframes (thin lines, depth tested), toolbar "Show physics collider". One block per shape kind, so a
        // future collider kind (a terrain/heightfield) is just another block reading its own component the same way.
        void DrawPhysicsColliders3D();
        // Ortho views (Top / Bottom / Front / Back / Left / Right, numpad 7 / 1 / 3, Ctrl for the opposite side, numpad 5 = perspective).
        // They are raster only: entering one turns DLSS, ray tracing and path tracing off (what they were is put back when leaving).
        void SetViewMode(EditorViewMode mode);
        int GridPlaneAxis() const; // the normal of the plane the grid lies on: 1 (the ground) in perspective / Top / Bottom, 2 Front / Back, 0 Left / Right
        // A 180 cm blocky figure (a size reference, like UE's mannequin) standing at `position`; toolbar "Figure": at the world origin,
        // or on the surface under the cursor.
        void DrawReferenceFigure(const glm::vec3& position);
        // Drops every selected top entity straight down until its bounds' bottom rests on the surface below (the top of the highest
        // other mesh whose bounds overlap it from above), or on the ground plane when there is none.
        void SnapSelectionToFloor();
        // While a translation snaps: tick marks at every step along the axis being dragged (the current one lit), or a checkerboard of
        // snap-sized squares on the plane being dragged (the current cell lit), so the steps can be seen. ImGui draw list, inside the
        // viewport window.
        void DrawTranslationSnapFeedback(const glm::mat4& view, const glm::mat4& projection, const glm::mat4& current);

        // Measure tool (UE / Godot's ruler): M, click a first point, a line follows the cursor, click the second; it stays until the next click.
        // Points come from the depth under the cursor (perspective), the ground plane where nothing is drawn; they snap like everything else.
        bool PickWorldPoint(glm::vec3& out) const;
        void MeasureClick();
        void UI_MeasureOverlay();
        // A measuring line in the viewport window: a bar across at both ends, the distance in the middle.
        void DrawMeasureLine(const glm::mat4& view, const glm::mat4& projection, const glm::vec3& start, const glm::vec3& end, float minimumLength);
        float LocationSnapWorldUnits() const;
        glm::vec3 SnapLocation(const glm::vec3& point) const; // the point itself while snapping is off

        void SavePrefabMode();
        void ExitPrefabMode();
        // Leaves Prefab Mode; with unsaved changes asks first (Save / Don't Save / Cancel) and then runs `afterwards` (opening a scene).
        void RequestExitPrefabMode(std::function<void()> afterwards = {});
        std::string CurrentPrefabText();
        void UpdatePrefabModeDirty();
        void UI_PrefabExitPrompt();

        // Multi-viewport (docs/Future.md item 5). The viewports dock inside a dockspace of their own ("Viewports", in the middle of the
        // editor), so a layout preset can rebuild just that area: 1, 2 side by side, 2 stacked, 3, or 4 (2x2). They can still be dragged
        // anywhere and resized. The active viewport (the last one clicked or scrolled in) is the full render and takes every tool;
        // the others draw their own camera raster only (Renderer::SubmitExtraView).
        enum class ViewportLayout : int { One, TwoSideBySide, TwoStacked, Three, Four };
        void UI_Viewports();
        void UI_ActiveViewport();
        void UI_ExtraViewport(int index);
        void ApplyViewportLayout(ViewportLayout layout, ImGuiID dockSpace, ImVec2 size);
        // Makes a viewport the active one: its camera becomes m_EditorCamera (the old one goes back into its slot) and the main render
        // follows it (size, raster-only settings of an ortho view).
        void ActivateViewport(int index);
        void OpenViewport(int index);
        int HoveredViewport() const;
        int OpenViewportCount() const;
        // The extra views' renders for this frame (Renderer::SubmitExtraView), with their own grid.
        void SubmitExtraViewports(Timestep ts);
        // The options of a viewport as an extra view (Window > Viewports).
        void UI_ViewportOptionsMenu(int index);
        // Ortho views are raster only: entering one turns DLSS, ray tracing and path tracing off, leaving it puts them back.
        void SyncOrthoRenderSettings(bool wasOrthographic);

        // Any open node-graph editor window focused or hovered: the scene's shortcuts and picking must ignore input.
        bool AnyNodeGraphEditorWantsInput() const;
        bool AnyNodeGraphEditorHovered() const; // mouse only: focus alone must not swallow a click elsewhere

    private:
        Renderer* m_Renderer;
        Renderer2D* m_Renderer2D;
        
        Ref<Scene> m_ActiveScene;
        Scope<SceneLoader> m_SceneLoad;
        AssetHandle m_SceneLoadHandle = 0;
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
        struct OrthoSavedSettings
        {
            bool Valid = false;
            bool Dlss = false;
            bool RayTracing = false;
            bool PathTracing = false;
        } m_OrthoSaved;
        bool m_ShowReferenceFigure = false;
        bool m_ReferenceFigureAtCursor = false;
        glm::vec3 m_ReferenceFigurePosition{ 0.0f };
        bool m_GizmoWasUsing = false;
        bool m_MeasureTool = false;
        int m_MeasureStage = 0; // 0 nothing, 1 first point set (line follows the cursor), 2 both set (kept until the next click)
        glm::vec3 m_MeasureA{ 0.0f };
        glm::vec3 m_MeasureB{ 0.0f };
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
        
        EditorCamera m_EditorCamera; // the active viewport's camera
        
        static constexpr int kMaxViewports = 4;
        struct ViewportSlot
        {
            const char* Title = "Viewport";
            bool Open = false;
            bool CameraSet = false;  // the camera was given its starting view (a copy of the active one, in the slot's default view)
            EditorCamera Camera;     // while the slot is not the active one
            glm::vec2 Bounds[2] = { glm::vec2(0.0f), glm::vec2(0.0f) };
            glm::uvec2 Size = { 0, 0 };
            bool Hovered = false;
            bool Submitted = false;  // its image was submitted to the renderer this frame (an extra view)

            // How it draws while it is not the active viewport (Window > Viewports > its options; kept in imgui.ini).
            uint32_t ShowFlags = Renderer::ExtraViewOutline; // Renderer::ExtraViewFlags
            bool ShowGrid = true;
            int ResolutionIndex = 0;  // full, half, quarter
            bool Realtime = true;     // off: drawn again only when its camera, its size, the scene or the selection change
            // What it was last drawn with (realtime off).
            glm::mat4 DrawnViewProjection{ 0.0f };
            glm::uvec2 DrawnSize{ 0, 0 };
            uint32_t DrawnFlags = 0;
            size_t DrawnHistory = SIZE_MAX;
            size_t DrawnSelection = 0;
            float SinceDrawn = 0.0f;
        };
        std::array<ViewportSlot, kMaxViewports> m_Viewports;
        int m_ActiveViewport = 0;
        int m_MouseCaptureViewport = -1; // the viewport a mouse button went down in, until every button is up again
        int m_PendingViewportLayout = -1; // a Window > Viewports preset, applied before the viewport dockspace is drawn

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

        EditorHistory m_History;
        EditorHistory m_LevelHistory; // the level's history, put aside while Prefab Mode edits the prefab
        bool m_HistoryItemWasActive = false;
        bool m_HistoryGizmoWasUsing = false;
        bool m_HistoryCheckRequested = false; // an editor shortcut ran (Delete, Ctrl+D, End, ...)
        bool m_UnloadUnusedAssetsRequested = false;
        std::chrono::steady_clock::time_point m_LastUnloadSweep{}; // the sweep runs at most once a second
        
        // Panels
        SceneHierarchyPanel m_SceneHierarchyPanel;
        Scope<ContentBrowserPanel> m_ContentBrowserPanel;
        Scope<RenderGraphPanel> m_RenderGraphPanel;
        ProfilerPanel m_ProfilerPanel;
        WorldPartitionPanel m_WorldPartitionPanel;
        // Build HLODs, while it runs (stepped every frame; Play, another scene, undo cancel it).
        Scope<HLODBuilder> m_HLODBuild;
        // Create > Streaming Test World (§5.13.4): its settings dialog, and the generation while it runs (stepped every frame; the
        // generated scene opens once it is done).
        StreamingTestWorldGenerator::Settings m_TestWorldSettings;
        bool m_OpenTestWorldDialog = false;
        Scope<StreamingTestWorldGenerator> m_TestWorldGeneration;
        // Where the editor camera looks once the scene being opened is in (the generated world: its old view, if any, is another
        // scene's, maybe under the new terrain).
        std::optional<glm::vec3> m_FocusOnOpen;
        void UI_StreamingTestWorld();
        void StartHLODBuild();
        void CancelHLODBuild(const char* reason);
        // A world's cells were loading or unloading last frame: once done, the undo history starts again (loading is not a step).
        bool m_WorldCellsBusy = false;
        // Open node-graph editors (docs/Animation_Graph_Architecture_Plan_2026.md Step 3), one per opened graph
        // asset; OpenNodeGraphEditor focuses an already-open one instead of duplicating it.
        std::vector<Scope<NodeGraphEditorPanel>> m_NodeGraphEditors;
        std::vector<Scope<MaterialEditorPanel>> m_MaterialEditors;
        std::vector<Scope<AnimationClipPanel>> m_AnimationClipPanels;
        // Sequencer timelines (Future.md item 3, A8), one window per open .ntimeline.
        std::vector<Scope<TimelinePanel>> m_TimelinePanels;
        // Puts back what the timeline previews posed (before saving, playing, or another scene).
        void EndTimelinePreviews();
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
