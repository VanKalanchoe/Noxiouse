#pragma once

#include <string>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Core.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/NodeGraph/NodeGraphAsset.h"

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

        // True while this window is focused or under the mouse (as of its last render). EditorLayer's global
        // shortcuts and viewport picking stand down then -- otherwise Delete on a selected node also deleted the
        // scene entity selected in the hierarchy, since those keys are handled from raw SDL events, not ImGui.
        bool WantsInput() const { return m_WantsInput; }
        // Hover only: for mouse clicks, which must not be swallowed just because the window was used a moment ago.
        bool IsHovered() const { return m_Hovered; }

    private:
        void Save();
        // The graph's declared parameters (add / rename / retype / default / delete), above the canvas. Backend-
        // independent: it edits NodeGraph::Parameters, which every canvas backend draws its own nodes from.
        void DrawParametersPanel();
        // Below a state machine's state view: the selected state (name, entry) or transition (blend, rules).
        void DrawStateMachineInspector(GraphNode& machine);

    private:
        AssetHandle m_GraphHandle;
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
    };
}
