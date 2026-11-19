#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "NoxCore/NodeGraph/NodeGraph.h"
#include "NoxCore/NodeGraph/NodeType.h"
#include "NoxCore/UI/Core/UIElement.h"

namespace Nox
{
    // The node-graph canvas of NoxEditorV2 (animation, audio and material graphs), in Nox::UI -- the look of UI::UINodeGraph
    // (rounded nodes with a category-coloured header and a shadow, bezier wires on a grid) on the engine's NodeGraph itself.
    // It edits the NodeGraph it is given in place and reports each finished operation once (OnEdited), so one drag or one
    // connection is one undo step for the window.
    //
    //  - Pan: right or middle mouse button drag (a right click that does not move opens the context menu). Zoom: wheel, around
    //    the pointer. F: frame the selection (or everything), Home: back to 100 %.
    //  - Nodes: click selects (Ctrl toggles, Shift adds), drag moves the selection, a drag on empty canvas box-selects.
    //    Delete removes the selection (the graph's output node stays), Ctrl+D duplicates, Ctrl+C / Ctrl+V copy and paste,
    //    Ctrl+A selects all. A double-click on a node with states (a state machine) asks the window to open it.
    //  - Wires: drag from a pin to a pin of the same type (an input takes one wire: a new one replaces it); a wire let go on
    //    empty canvas opens the node list with what fits and connects the node picked; Alt+click on a pin cuts its wires;
    //    click a wire to select it (Delete removes it); double-click a wire to put a reroute node on it.
    //  - Right click on empty canvas: the node list (search as you type, Enter takes the first); on a node: its menu.
    class NodeGraphCanvasV2 final : public UI::UIElement
    {
    public:
        NodeGraphCanvasV2();

        // The graph to show and edit (null: none). The view frames it the first time.
        void SetGraph(NodeGraph* graph);
        NodeGraph* GetGraph() const { return m_Graph; }
        // The graph was replaced (undo / redo): drops what is selected that is gone, keeps the view.
        void ReloadGraph();
        void FrameAll();
        void FrameSelection();

        // The parameters the Parameter nodes read (the asset's), for their titles and the drop of a parameter.
        const std::vector<GraphParameter>* Parameters = nullptr;

        // After each finished edit: structural = what the graph computes changed (recompile); false = only where things are.
        std::function<void(bool structural)> OnEdited;
        std::function<void()> OnSelectionChanged;
        // A node with states (a state machine) double-clicked.
        std::function<void(uint32_t nodeId)> OnOpenNode;

        const std::vector<uint32_t>& GetSelectedNodes() const { return m_Selected; }
        void SelectNode(uint32_t nodeId);
        // A node of `typeName` at the canvas point under `screen` (or the middle of the view), selected; its id or 0.
        uint32_t AddNodeAt(const std::string& typeName, const UI::UIVec2& screen, bool atViewCenter = false);
        // Removes the node (and its wires); the output node is refused.
        bool DeleteNodes(const std::vector<uint32_t>& nodeIds);

        bool OnPointerDown(const UI::UIPointerEvent& e) override;
        bool OnPointerMove(const UI::UIPointerEvent& e) override;
        bool OnPointerUp(const UI::UIPointerEvent& e) override;
        bool OnPointerLeave(const UI::UIPointerEvent& e) override;
        bool OnMouseWheel(const UI::UIPointerEvent& e) override;
        bool OnKeyDown(const UI::UIKeyEvent& e) override;

    protected:
        UI::UIVec2 ComputeDesiredSize() override { return { 300.0f, 200.0f }; }
        void OnUpdate(float dt) override;
        void OnPaint(UI::UIDrawList& drawList, const UI::UITheme& theme) override;

    private:
        // Where a node and its pins are, in graph space (zoom 1), worked out from its type each frame.
        struct NodeLayout
        {
            UI::UIVec2 Size{ 0.0f };
            std::vector<NodePinDesc> Inputs, Outputs;
            std::string Title;
            UI::UIColor Header{ 0.35f, 0.35f, 0.35f, 1.0f };
            bool Reroute = false;
            bool Known = true; // the type is registered
        };
        struct PinRef
        {
            uint32_t Node = 0;
            uint32_t Pin = 0;
            bool Output = false;
            bool Valid() const { return Node != 0; }
        };

        void Relayout();
        void SeedLiterals();
        const NodeLayout* LayoutOf(uint32_t nodeId) const;
        GraphNode* FindNode(uint32_t id) const;
        const NodeTypeDesc* TypeOf(const GraphNode& node) const;

        UI::UIVec2 GraphToScreen(const UI::UIVec2& p) const;
        UI::UIVec2 ScreenToGraph(const UI::UIVec2& p) const;
        UI::UIVec2 PinPosition(const GraphNode& node, const NodeLayout& layout, uint32_t pin, bool output) const; // graph space
        std::string PinType(const PinRef& pin) const;

        uint32_t NodeAt(const UI::UIVec2& screen) const;
        PinRef PinAt(const UI::UIVec2& screen) const;
        int LinkAt(const UI::UIVec2& screen) const;

        bool CanConnect(const PinRef& a, const PinRef& b, GraphNode** retype, std::string* retypeTo) const;
        bool Connect(const PinRef& a, const PinRef& b);
        void InsertReroute(int linkIndex, const UI::UIVec2& screen);
        void Duplicate();
        void Copy();
        void Paste(const UI::UIVec2& screen);
        void SetSelection(std::vector<uint32_t> nodes);

        void OpenNodeList(const UI::UIVec2& screen, const PinRef& fromPin);
        void OpenNodeMenu(const UI::UIVec2& screen, uint32_t nodeId);
        void Edited(bool structural);

        NodeGraph* m_Graph = nullptr;
        std::unordered_map<uint32_t, NodeLayout> m_Layouts;
        std::vector<uint32_t> m_Selected;
        int m_SelectedLink = -1;
        int m_HoveredLink = -1;
        uint32_t m_HoveredNode = 0;
        PinRef m_HoveredPin;

        UI::UIVec2 m_Pan{ 0.0f };
        float m_Zoom = 1.0f;
        bool m_NeedsFrame = true;

        enum class Drag : uint8_t { None, Pan, Nodes, Wire, Box };
        Drag m_Drag = Drag::None;
        UI::UIVec2 m_DragStart{ 0.0f };
        UI::UIVec2 m_DragLast{ 0.0f };
        bool m_DragMoved = false;
        int m_PanButton = 0;
        PinRef m_WireFrom;
        std::unordered_map<uint32_t, glm::vec2> m_DragOrigins; // node -> position when the drag started
        std::vector<uint32_t> m_BoxBase;                     // the selection a Ctrl/Shift box adds to

        uint32_t m_LastClickNode = 0;
        int m_LastClickLink = -1;
        std::chrono::steady_clock::time_point m_LastClickTime{};

        // Ctrl+C: the nodes and the wires between them.
        std::vector<GraphNode> m_ClipboardNodes;
        std::vector<GraphLink> m_ClipboardLinks;
    };
}
