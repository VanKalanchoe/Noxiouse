#pragma once

#include <unordered_set>

#include <imgui_node_editor.h>

#include "INodeGraphCanvasBackend.h"
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

        void Draw(NodeGraph& graph, const std::function<void()>& onGraphEdited) override;

    private:
        void DrawNode(NodeGraph& graph, GraphNode& node, const NodeTypeDesc& type, const std::function<void()>& onGraphEdited);
        // Draws just the button for a uint64_t (AssetHandle-shaped) property; a plain combo doesn't work inside
        // a node here (see the .cpp), so this only queues the request -- DrawPendingAssetPicker renders the
        // actual popup later, suspended, after every node this frame is done drawing.
        void DrawAssetPickerButton(uint32_t nodeId, const std::string& propertyName, uint64_t currentHandle, const std::string& assetTypeHint);
        void DrawPendingAssetPicker(NodeGraph& graph, const std::function<void()>& onGraphEdited);
        void HandleLinkCreation(NodeGraph& graph, const std::function<void()>& onGraphEdited);
        void HandleDeletion(NodeGraph& graph, const std::function<void()>& onGraphEdited);
        void ShowCreateNodeMenu(NodeGraph& graph, const std::function<void()>& onGraphEdited);
        // Adds a node of `type` (seeded with its default properties) at a screen position, optionally pointing
        // it at a graph parameter -- shared by the create-node menu and the parameter drag-and-drop.
        void CreateNode(NodeGraph& graph, const NodeTypeDesc& type, const std::string* parameterName, const ImVec2& screenPos,
                        const std::function<void()>& onGraphEdited);
        // Accepts a parameter dragged from the panel's Parameters list (kNodeGraphParameterPayload) anywhere over
        // the canvas and creates a node reading it at the drop position.
        void HandleParameterDrop(NodeGraph& graph, const std::function<void()>& onGraphEdited);

        static ed::PinId MakePinId(uint32_t nodeId, uint32_t pinIndex, bool isOutput);
        static void DecodePinId(ed::PinId id, uint32_t& nodeId, uint32_t& pinIndex, bool& isOutput);
        static ed::LinkId MakeLinkId(uint32_t toNode, uint32_t toPin); // a link is identified by its (unique) input pin

        static GraphNode* FindNodeById(NodeGraph& graph, uint32_t nodeId);
        static const NodeTypeDesc* TypeOf(const NodeGraph& graph, const GraphNode& node);

    private:
        ed::EditorContext* m_Context = nullptr;
        std::unordered_set<uint32_t> m_SeededPositions; // node ids whose ed:: position has been seeded from GraphNode::EditorPosition
        ImVec2 m_CreateNodeScreenPos{0.0f, 0.0f};
        uint32_t m_ContextNodeId = 0;
        bool m_FitViewOnNextFrame = true; // NavigateToContent() once all node positions are seeded, so nothing opens off-screen

        // Pending asset-picker request (github.com/thedmd/imgui-node-editor issues #48/#154's documented
        // workaround: a node-body button just requests the popup, the popup itself is drawn later, suspended).
        bool m_ShowAssetPicker = false;
        uint32_t m_AssetPickerNodeId = 0;
        std::string m_AssetPickerPropertyName;
        std::string m_AssetPickerTypeHint;
        char m_AssetPickerFilter[64] = "";
        char m_CreateSearch[64] = ""; // the create-node menu's search box
    };
}
