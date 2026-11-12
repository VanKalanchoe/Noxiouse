#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui_node_editor.h>

#include "INodeGraphCanvasBackend.h"
#include "WireRouter.h"
#include "NoxCore/NodeGraph/NodeType.h"

namespace Nox
{
    namespace ed = ax::NodeEditor;

    // Canvas backend on thedmd/imgui-node-editor (NoxCore/vendors/imgui-node-editor, fetched fresh from master
    // -- includes its "fixing for modern imgui" commits). Pin icons come from the vendored blueprints-example
    // utilities (ax::Widgets::Icon: shape + color by declared pin Type). Node headers are a flat category color
    // painted by hand: BlueprintNodeBuilder's real gradient-textured header needs Spring()/BeginHorizontal()
    // layout helpers only available in thedmd's own patched ImGui fork, not this vendored copy or this
    // project's ImGui -- see docs/Animation_Graph_Architecture_Plan_2026.md's decisions log for the
    // investigation. One instance per open graph editor window (owns its own ed::EditorContext).
    class ThedmdCanvasBackend : public INodeGraphCanvasBackend
    {
    public:
        ThedmdCanvasBackend();
        ~ThedmdCanvasBackend() override;

        ThedmdCanvasBackend(const ThedmdCanvasBackend&) = delete;
        ThedmdCanvasBackend& operator=(const ThedmdCanvasBackend&) = delete;

        void Draw(NodeGraph& graph, const std::vector<GraphParameter>& parameters, const std::function<void()>& onGraphEdited,
                  uint32_t* outOpenNode = nullptr) override;
        void DrawStateMachine(GraphNode& machine, const NodeGraph& rootGraph, const std::function<void()>& onGraphEdited,
                              uint32_t& outOpenState, StateMachineSelection& selection) override;
        void ResetView() override;
        void SetBoneList(std::vector<BoneEntry> bones, std::string skeletonName) override;
        bool ConsumeLayoutEdited() override;

    private:
        // Where a state box's two pins are this frame (canvas space).
        struct StatePins
        {
            ImVec2 InputMin, InputMax, OutputMin, OutputMax;
        };
        // The state view's transitions as orthogonally routed wires between the pins (see the .cpp).
        void DrawTransitionWires(const GraphNode& machine, const std::unordered_map<uint32_t, StatePins>& statePins);
        void DrawNode(NodeGraph& graph, GraphNode& node, const NodeTypeDesc& type, const std::function<void()>& onGraphEdited);
        // Draws just the button for a uint64_t (AssetHandle-shaped) property; a plain combo doesn't work inside
        // a node here (see the .cpp), so this only queues the request -- DrawPendingAssetPicker renders the
        // actual popup later, suspended, after every node this frame is done drawing.
        void DrawAssetPickerButton(uint32_t nodeId, const std::string& propertyName, uint64_t currentHandle, const std::string& assetTypeHint);
        void DrawPendingAssetPicker(NodeGraph& graph, const std::function<void()>& onGraphEdited);
        // A bone-name property: a button with the bone's name (orange when the preview skeleton has no such bone) that queues the
        // bone picker, drawn later like the asset picker (popups do not work inside a node).
        void DrawBonePickerButton(uint32_t nodeId, const std::string& propertyName, const std::string& current);
        void DrawPendingBonePicker(NodeGraph& graph, const std::function<void()>& onGraphEdited);
        void HandleLinkCreation(NodeGraph& graph, const std::function<void()>& onGraphEdited);
        // The graph's wires, routed orthogonally around the nodes (WireRouter) and drawn under them; see the .cpp.
        void DrawWires(NodeGraph& graph);
        // Hovering, clicking (select), Delete, double-click (reroute), right-click (menu) and dragging a leg of a wire; Alt+click on
        // a pin breaks its links. Called suspended (screen space).
        void HandleWireInput(NodeGraph& graph, const std::function<void()>& onGraphEdited);
        void DrawWireContextMenu(NodeGraph& graph, const std::function<void()>& onGraphEdited);
        // Connects a node just made from the create menu opened by dropping a wire on empty canvas to that wire's pin.
        void ConnectNewNode(NodeGraph& graph, uint32_t newNodeId);
        // Unreal's reroute node: a small dot with an input and an output of its wire's type. Double-clicking a wire puts one there.
        void DrawRerouteNode(NodeGraph& graph, GraphNode& node);
        void InsertReroute(NodeGraph& graph, uint64_t linkId, const ImVec2& screenPos, const std::function<void()>& onGraphEdited);
        uint64_t m_DoubleClickedLink = 0;
        void HandleDeletion(NodeGraph& graph, const std::function<void()>& onGraphEdited);
        void ShowCreateNodeMenu(NodeGraph& graph, const std::vector<GraphParameter>& parameters, const std::function<void()>& onGraphEdited);
        // Adds a node of `type` (seeded with its default properties) at a screen position, optionally pointing
        // it at a graph parameter -- shared by the create-node menu and the parameter drag-and-drop.
        uint32_t CreateNode(NodeGraph& graph, const NodeTypeDesc& type, const std::string* parameterName, const ImVec2& screenPos,
                            const std::function<void()>& onGraphEdited);
        // Accepts a parameter dragged from the panel's Parameters list (kNodeGraphParameterPayload) anywhere over
        // the canvas and creates a node reading it at the drop position.
        void HandleParameterDrop(NodeGraph& graph, const std::vector<GraphParameter>& parameters, const std::function<void()>& onGraphEdited);

        static ed::PinId MakePinId(uint32_t nodeId, uint32_t pinIndex, bool isOutput);
        static void DecodePinId(ed::PinId id, uint32_t& nodeId, uint32_t& pinIndex, bool& isOutput);
        static ed::LinkId MakeLinkId(uint32_t toNode, uint32_t toPin); // a link is identified by its (unique) input pin

        static GraphNode* FindNodeById(NodeGraph& graph, uint32_t nodeId);
        static const NodeTypeDesc* TypeOf(const NodeGraph& graph, const GraphNode& node);

    private:
        ed::EditorContext* m_Context = nullptr;
        ed::EditorContext* m_StateContext = nullptr; // the state-machine view has its own canvas (own pan / zoom)
        std::unordered_set<uint32_t> m_SeededStatePositions;
        bool m_FitStatesOnNextFrame = true;
        uint32_t m_ContextStateId = 0;
        int m_ContextTransition = -1;  // the transition whose wire was right-clicked (its index)
        int m_SelectedTransition = -1; // the state view's selected transition (its wire, clicked)
        int m_HoveredTransition = -1;  // the wire under the mouse, last frame
        std::vector<std::vector<ImVec2>> m_TransitionWires; // each transition's route this frame (canvas space), for picking
        std::vector<float> m_RouteKey; // the boxes and wire ends m_TransitionWires was routed for
        ImVec2 m_AddStateScreenPos{0.0f, 0.0f};
        std::unordered_set<uint32_t> m_SeededPositions; // node ids whose ed:: position has been seeded from GraphNode::EditorPosition
        ImVec2 m_CreateNodeScreenPos{0.0f, 0.0f};
        uint32_t m_ContextNodeId = 0;
        bool m_FitViewOnNextFrame = true; // NavigateToContent() once all node positions are seeded, so nothing opens off-screen

        // Pending asset-picker request (github.com/thedmd/imgui-node-editor issues #48/#154's documented
        // workaround: a node-body button just requests the popup, the popup itself is drawn later, suspended).
        std::vector<BoneEntry> m_Bones;
        std::string m_BoneSkeletonName;
        bool m_ShowBonePicker = false;
        uint32_t m_BonePickerNodeId = 0;
        std::string m_BonePickerPropertyName;
        char m_BonePickerFilter[64] = "";
        char m_BonePickerTyped[128] = "";
        bool m_ShowAssetPicker = false;
        uint32_t m_AssetPickerNodeId = 0;
        std::string m_AssetPickerPropertyName;
        std::string m_AssetPickerTypeHint;
        char m_AssetPickerFilter[64] = "";
        char m_CreateSearch[64] = ""; // the create-node menu's search box

        // The graph's wires (DrawWires). A route is searched again only when its ends, its hand-shaped route or a node near it change.
        struct WireCache
        {
            std::vector<float> Key;
            std::vector<ImVec2> Route;
            WireRouting::Rect Area{}; // the nodes overlapping this area are part of the key
        };
        struct DrawnWire
        {
            uint64_t LinkId = 0;
            std::vector<ImVec2> Points; // this frame's route (canvas space), after the lanes are spread
            ImU32 Color = 0;
            uint32_t FromNode = 0, ToNode = 0;
            ImVec2 FromPin{}, ToPin{}; // the pin icons' centres: the short legs inside the two nodes end there
        };
        std::unordered_map<uint64_t, WireCache> m_WireCache; // by link id
        std::vector<DrawnWire> m_Wires;
        std::unordered_map<uint64_t, std::pair<ImVec2, ImVec2>> m_PinIcons; // pin id -> its icon's rect this frame (canvas space)
        std::unordered_map<uint32_t, ImVec2> m_PreviousNodePositions;      // to see nodes move (layout edits, routes moved along)
        uint64_t m_HoveredLink = 0;
        std::unordered_set<uint64_t> m_SelectedLinks;
        uint64_t m_PressedLink = 0;   // the wire the mouse went down on (0 once the press is used up)
        ImGuiMouseButton m_PressButton = ImGuiMouseButton_Left;
        ImVec2 m_PressCanvasPos{};
        bool m_DraggingLeg = false;   // a leg of m_PressedLink is being dragged
        int m_DragIndex = -1;         // which of its route coordinates
        bool m_DragVertical = false;
        std::vector<float> m_DragCoordinates;
        uint64_t m_ContextLink = 0;   // the wire whose menu is open
        ImVec2 m_ContextScreenPos{};
        bool m_OpenWireContext = false;
        // Unreal: a wire dropped on empty canvas opens the create menu showing only nodes that fit it; the node picked is connected.
        uint64_t m_NewNodeFromPin = 0;
        bool m_OpenCreateFromPin = false;
        uint32_t m_AlignNewNodeId = 0;  // a node made from an input pin's wire: put with its right edge at m_AlignNewNodeRight
        float m_AlignNewNodeRight = 0.0f;
        bool m_LayoutEdited = false;
        ImVec2 m_CanvasMin{}, m_CanvasMax{}; // the canvas on screen (it shares its window with the panel's other sections)
    };
}
