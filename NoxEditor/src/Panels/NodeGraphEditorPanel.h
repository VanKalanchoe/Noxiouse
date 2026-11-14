#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Core.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/NodeGraph/NodeGraphAsset.h"

#include <glm/glm.hpp>

namespace Nox
{
    class INodeGraphCanvasBackend;

    // Window shell for editing one open .nanimgraph (or any future domain reusing NodeGraph): owns the asset,
    // title, dirty state and Save(), and delegates all actual canvas drawing to a swappable
    // INodeGraphCanvasBackend (NoxEditor/src/Panels/NodeGraph/) -- see
    // docs/Animation_Graph_Architecture_Plan_2026.md Step 3 for why that split exists. Edits the loaded
    // NodeGraphAsset in place (the backend recompiles it after every edit, so a running scene picks changes up
    // immediately); Save() writes it to disk.
    //
    // One instance per open graph asset; EditorLayer owns the open set, keyed by AssetHandle.
    class NodeGraphEditorPanel
    {
    public:
        explicit NodeGraphEditorPanel(AssetHandle graphAsset);
        ~NodeGraphEditorPanel();

        NodeGraphEditorPanel(const NodeGraphEditorPanel&) = delete;
        NodeGraphEditorPanel& operator=(const NodeGraphEditorPanel&) = delete;

        void OnImGuiRender();
        bool IsOpen() const { return m_Open; }
        void SetOpen(bool open) { m_Open = open; }
        AssetHandle GetGraphAsset() const { return m_GraphHandle; }
        // An edit from outside the window (MCP set_graph): the graph replaced as one undo step of this window (Ctrl+Z here takes it
        // back), compiled, shown and saved. False when the window has no graph or the file could not be written.
        bool ApplyExternalEdit(NodeGraph graph);

        // True while this window is focused or under the mouse (as of its last render). EditorLayer's global
        // shortcuts and viewport picking stand down then -- otherwise Delete on a selected node also deleted the
        // scene entity selected in the hierarchy, since those keys are handled from raw SDL events, not ImGui.
        bool WantsInput() const { return m_WantsInput; }
        // Hover only: for mouse clicks, which must not be swallowed just because the window was used a moment ago.
        bool IsHovered() const { return m_Hovered; }

    private:
        void Save();
        // History (Ctrl+Z / Ctrl+Y): the whole graph as it was after each finished edit. An edit is finished when no mouse button
        // is held and no widget is active, so dragging a node or a slider is one step, not one per frame.
        void MarkEdited() { m_Dirty = true; m_EditPending = true; }
        void CommitHistory();
        void Undo();
        void Redo();
        void RestoreGraph(NodeGraph graph);
        // The file changed on disk (EditorAssetManager reimported the graph in place): the window shows the new graph -- an undo step,
        // so Ctrl+Z goes back to what it showed -- or, with unsaved edits of its own, asks which to keep (DrawDiskChangeBar).
        void FollowDiskChanges();
        void DrawDiskChangeBar();
        // The graph's declared parameters (add / rename / retype / default / delete), above the canvas. Backend-
        // independent: it edits NodeGraph::Parameters, which every canvas backend draws its own nodes from.
        void DrawParametersPanel();
        // Below a state machine's state view: the selected state (name, entry) or transition (blend, rules).
        void DrawStateMachineInspector(GraphNode& machine);
        // An animation graph's preview (A7): the graph played on a skeletal mesh, its parameters set here.
        void DrawPreview();
        // An audio graph's preview (MetaSounds' Play in the graph window): the graph heard as it is edited (an edit restarts it with
        // the new graph), its parameters as sliders / checkboxes / Fire buttons, the output's level.
        void DrawAudioPreview();
        void StopAudioPreview();

    private:
        AssetHandle m_GraphHandle;
        // Audio preview.
        uint64_t m_AudioPreviewVoice = 0; // AudioVoiceHandle
        bool m_AudioPreviewWanted = false;
        std::shared_ptr<const class AudioGraphProgram> m_AudioPreviewProgram; // what the playing preview runs
        std::unordered_map<std::string, NodeGraphValue> m_AudioPreviewValues;
        Ref<NodeGraphAsset> m_Asset;
        std::string m_Title;
        bool m_Open = true;
        bool m_Dirty = false;
        bool m_WantsInput = false;
        bool m_Hovered = false;

        // Which graph is on the canvas: ids alternating node (a state machine in the current graph) / state (one of its
        // states), so [] = the root graph, [n] = n's state view, [n, s] = state s's own graph, and so on.
        std::vector<uint32_t> m_Path;
        std::vector<uint32_t> m_ShownPath; // what the backend was last reset for
        int m_SelectedTransition = -1;
        uint32_t m_SelectedState = 0;

        Scope<INodeGraphCanvasBackend> m_Backend;

        NodeGraph m_Committed;               // the graph after the last finished edit
        std::vector<NodeGraph> m_UndoStack;  // older states, newest last
        std::vector<NodeGraph> m_RedoStack;
        bool m_EditPending = false;          // edited since m_Committed
        uint32_t m_DiskVersion = 0;          // the asset's DiskVersion this window last showed

        // The preview: its own graph instance (playheads, parameters), the mesh and skeleton it plays on.
        Scope<class AnimationGraphInstance> m_PreviewInstance;
        AssetHandle m_PreviewMesh = 0;
        AssetHandle m_PreviewSkeleton = 0;
        bool m_PreviewPlaying = true;
        std::vector<glm::mat4> m_PreviewBones;
        // The preview skeleton's bones handed to the canvas for bone-name properties (UpdateBoneList), and which skeleton they are.
        AssetHandle m_BoneListSkeleton = 0;
        bool m_BoneListLoaded = false;
        void UpdateBoneList();
    };
}
