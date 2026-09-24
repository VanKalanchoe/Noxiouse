#include "ThedmdCanvasBackend.h"

#include <algorithm>
#include <cctype>

#include <imgui.h>
#include <imgui_internal.h> // ImDrawFlags_RoundCornersTop
#include <utilities/widgets.h>

#include "NodeGraphPropertyEditor.h"
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/NodeGraph/NodeType.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    namespace
    {
        // Pin shape + color by declared pin Type, the same "type tells you the socket" convention Blueprint-
        // style editors use: a distinct shape for the domain's own composite type(s), a shared shape (colored
        // differently per type) for scalars. Extend as new pin types show up.
        ax::Drawing::IconType PinIconType(const std::string& pinType)
        {
            if (pinType == "Pose")
                return ax::Drawing::IconType::RoundSquare;
            return ax::Drawing::IconType::Circle;
        }

        ImVec4 PinColor(const std::string& pinType)
        {
            if (pinType == "Pose")     return ImVec4(0.78f, 0.55f, 0.95f, 1.0f); // purple
            if (pinType == "Float")    return ImVec4(0.55f, 0.85f, 0.55f, 1.0f); // green
            if (pinType == "Bool")     return ImVec4(0.85f, 0.45f, 0.45f, 1.0f); // red
            if (pinType == "Vector2")  return ImVec4(0.95f, 0.85f, 0.35f, 1.0f); // yellow
            return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
        }

        // A colored, rounded band across the top of a node, matching the UE4-Blueprint-style category color the
        // real imgui-node-editor demo draws with its BlueprintNodeBuilder -- see this file's header comment for
        // why that class itself isn't used.
        ImU32 CategoryHeaderColor(const std::string& category)
        {
            if (category == "Sources")  return IM_COL32(60, 95, 165, 255);  // blue
            if (category == "Blending") return IM_COL32(170, 110, 40, 255); // orange
            if (category == "Sinks")    return IM_COL32(55, 135, 90, 255);  // green
            if (category == "Inputs")   return IM_COL32(130, 75, 150, 255); // purple
            return IM_COL32(90, 90, 90, 255);
        }

        // A node type that reads a graph parameter is recognized by its default properties containing
        // kParameterReferenceProperty -- not by name, so this works for any domain's such type.
        bool ReadsParameter(const NodeTypeDesc& type)
        {
            return std::any_of(type.DefaultProperties.begin(), type.DefaultProperties.end(),
                [](const auto& property) { return property.first == kParameterReferenceProperty; });
        }
    }

    ThedmdCanvasBackend::ThedmdCanvasBackend()
    {
        ed::Config config;
        // Node positions persist through the .nanimgraph file itself (GraphNode::EditorPosition), not a
        // separate settings file next to the executable.
        config.SettingsFile = nullptr;
        m_Context = ed::CreateEditor(&config);
    }

    ThedmdCanvasBackend::~ThedmdCanvasBackend()
    {
        if (m_Context)
            ed::DestroyEditor(m_Context);
    }

    void ThedmdCanvasBackend::Draw(NodeGraph& graph, const std::function<void()>& onGraphEdited)
    {
        ed::SetCurrentEditor(m_Context);
        ed::Begin("Canvas");

        for (GraphNode& node : graph.Nodes)
        {
            const NodeTypeDesc* type = TypeOf(graph, node);
            if (type)
                DrawNode(graph, node, *type, onGraphEdited);
        }

        for (const GraphLink& link : graph.Links)
        {
            ed::PinId from = MakePinId(link.FromNode, link.FromPin, true);
            ed::PinId to = MakePinId(link.ToNode, link.ToPin, false);
            ed::Link(MakeLinkId(link.ToNode, link.ToPin), from, to);
        }

        HandleLinkCreation(graph, onGraphEdited);
        HandleDeletion(graph, onGraphEdited);

        if (m_FitViewOnNextFrame && !graph.Nodes.empty())
        {
            ed::NavigateToContent(0.0f);
            m_FitViewOnNextFrame = false;
        }

        // Context menus escape the canvas' pan/zoom transform, so they're raised and drawn while suspended --
        // the canonical imgui-node-editor pattern (its own blueprints-example does the same).
        ed::Suspend();

        ed::NodeId contextNodeId;
        if (ed::ShowNodeContextMenu(&contextNodeId))
        {
            m_ContextNodeId = (uint32_t)contextNodeId.Get();
            ImGui::OpenPopup("NodeContext");
        }
        if (ed::ShowBackgroundContextMenu())
        {
            m_CreateNodeScreenPos = ImGui::GetMousePos();
            m_CreateSearch[0] = '\0';
            ImGui::OpenPopup("CreateNode");
        }

        if (ImGui::BeginPopup("NodeContext"))
        {
            if (ImGui::MenuItem("Set as Graph Output"))
            {
                graph.OutputNode = m_ContextNodeId;
                onGraphEdited();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("CreateNode"))
        {
            ShowCreateNodeMenu(graph, onGraphEdited);
            ImGui::EndPopup();
        }

        DrawPendingAssetPicker(graph, onGraphEdited);
        HandleParameterDrop(graph, onGraphEdited);

        ed::Resume();

        ed::End();
        ed::SetCurrentEditor(nullptr);
    }

    void ThedmdCanvasBackend::DrawNode(NodeGraph& graph, GraphNode& node, const NodeTypeDesc& type, const std::function<void()>& onGraphEdited)
    {
        ed::NodeId nodeId(node.Id);

        if (m_SeededPositions.insert(node.Id).second)
            ed::SetNodePosition(nodeId, ImVec2(node.EditorPosition.x, node.EditorPosition.y));

        constexpr ImVec2 kPinIconSize(16.0f, 16.0f);

        ed::BeginNode(nodeId);
        // ImGui identifies widgets by label within the current ID scope, and ed::BeginNode doesn't open one per
        // node -- without this, every node's "Loop"/"Speed"/asset-button shares an ID with every other node's
        // (the reference BlueprintNodeBuilder::Begin does the same PushID for the same reason).
        ImGui::PushID((int)node.Id);
        ImGui::PushItemWidth(120.0f);

        ImGui::TextUnformatted(type.TypeName.c_str());
        // Captured in screen space (matching what GetNodeBackgroundDrawList expects, per the reference
        // BlueprintNodeBuilder::End()) so the header band below lines up with this row regardless of pan/zoom.
        ImVec2 headerMin = ImGui::GetItemRectMin();
        ImVec2 headerMax = ImGui::GetItemRectMax();
        ImGui::Dummy(ImVec2(0.0f, 4.0f)); // room for the header band painted over this node after EndNode()

        const std::vector<GraphLink>& links = graph.Links;

        ImGui::BeginGroup();
        for (uint32_t p = 0; p < (uint32_t)type.Inputs.size(); p++)
        {
            bool connected = std::any_of(links.begin(), links.end(),
                [&](const GraphLink& l) { return l.ToNode == node.Id && l.ToPin == p; });

            ed::BeginPin(MakePinId(node.Id, p, false), ed::PinKind::Input);
            ax::Widgets::Icon(kPinIconSize, PinIconType(type.Inputs[p].Type), connected, PinColor(type.Inputs[p].Type));
            ImGui::SameLine();
            ImGui::TextUnformatted(type.Inputs[p].Name.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", type.Inputs[p].Type.c_str());
            ed::EndPin();
        }
        ImGui::EndGroup();

        if (!type.Outputs.empty())
        {
            ImGui::SameLine();
            ImGui::BeginGroup();
            for (uint32_t p = 0; p < (uint32_t)type.Outputs.size(); p++)
            {
                bool connected = std::any_of(links.begin(), links.end(),
                    [&](const GraphLink& l) { return l.FromNode == node.Id && l.FromPin == p; });

                ed::BeginPin(MakePinId(node.Id, p, true), ed::PinKind::Output);
                ImGui::TextUnformatted(type.Outputs[p].Name.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", type.Outputs[p].Type.c_str());
                ImGui::SameLine();
                ax::Widgets::Icon(kPinIconSize, PinIconType(type.Outputs[p].Type), connected, PinColor(type.Outputs[p].Type));
                ed::EndPin();
            }
            ImGui::EndGroup();
        }

        if (!node.Properties.empty())
        {
            ImGui::Separator();
            for (auto& [name, value] : node.Properties)
            {
                if (uint64_t* handle = std::get_if<uint64_t>(&value))
                {
                    auto hintIt = type.PropertyAssetTypeHints.find(name);
                    DrawAssetPickerButton(node.Id, name, *handle, hintIt != type.PropertyAssetTypeHints.end() ? hintIt->second : std::string());
                }
                else
                {
                    auto rangeIt = type.PropertyRanges.find(name);
                    if (DrawNodeGraphProperty(name, value, rangeIt != type.PropertyRanges.end() ? &rangeIt->second : nullptr))
                        onGraphEdited();
                }
            }
        }

        ImGui::PopItemWidth();
        ImGui::PopID();
        ed::EndNode();

        // A flat category-colored, rounded top band -- see CategoryHeaderColor's comment for why this is
        // painted by hand instead of through the reference BlueprintNodeBuilder.
        if (ImGui::IsItemVisible())
        {
            ImDrawList* backgroundDrawList = ed::GetNodeBackgroundDrawList(nodeId);
            const float rounding = ed::GetStyle().NodeRounding;
            const float pad = 8.0f;
            backgroundDrawList->AddRectFilled(
                ImVec2(headerMin.x - pad, headerMin.y - pad),
                ImVec2(headerMax.x + pad, headerMax.y + pad),
                CategoryHeaderColor(type.Category), rounding, ImDrawFlags_RoundCornersTop);
        }

        ImVec2 currentPos = ed::GetNodePosition(nodeId);
        node.EditorPosition = { currentPos.x, currentPos.y };
    }

    void ThedmdCanvasBackend::DrawAssetPickerButton(uint32_t nodeId, const std::string& propertyName, uint64_t currentHandle, const std::string& assetTypeHint)
    {
        std::string label = "None";
        if (currentHandle != 0 && Project::GetActive())
        {
            const auto& registry = Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry();
            auto it = registry.find(currentHandle);
            label = (it != registry.end()) ? it->second.FilePath.stem().string() : std::to_string(currentHandle);
        }

        ImGui::Text("%s:", propertyName.c_str());
        ImGui::SameLine();
        // Button, not a combo: a plain ImGui::BeginCombo opens its popup at the wrong place and isn't
        // clickable inside an imgui-node-editor node (see this backend's header comment and
        // NodeGraphPropertyEditor.cpp's). This only records the request; DrawPendingAssetPicker draws the
        // actual list later, suspended, once every node this frame is done drawing.
        if (ImGui::Button(label.c_str()))
        {
            m_ShowAssetPicker = true;
            m_AssetPickerNodeId = nodeId;
            m_AssetPickerPropertyName = propertyName;
            m_AssetPickerTypeHint = assetTypeHint;
            m_AssetPickerFilter[0] = '\0';
        }
    }

    void ThedmdCanvasBackend::DrawPendingAssetPicker(NodeGraph& graph, const std::function<void()>& onGraphEdited)
    {
        // OpenPopup fires once, the triggering frame; BeginPopup must still be attempted every frame after that
        // (it tracks ImGui's own popup-open state, not m_ShowAssetPicker) or the popup would vanish the instant
        // the trigger frame ends. Already inside ed::Suspend()/Resume() (called from Draw()) -- correct screen
        // coordinates here.
        if (m_ShowAssetPicker)
        {
            ImGui::OpenPopup("AssetPicker");
            m_ShowAssetPicker = false;
        }

        if (!ImGui::BeginPopup("AssetPicker"))
            return;

        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputTextWithHint("##filter", "Filter...", m_AssetPickerFilter, sizeof(m_AssetPickerFilter));
        std::string filter = m_AssetPickerFilter;
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return (char)std::tolower(c); });

        AssetType wantType = !m_AssetPickerTypeHint.empty() ? AssetTypeFromString(m_AssetPickerTypeHint) : AssetType::None;

        GraphNode* node = FindNodeById(graph, m_AssetPickerNodeId);
        ImGui::BeginChild("AssetPickerList", ImVec2(220.0f, 200.0f), true);

        if (ImGui::Selectable("None"))
        {
            if (node)
            {
                node->Properties[m_AssetPickerPropertyName] = uint64_t(0);
                onGraphEdited();
            }
            ImGui::CloseCurrentPopup();
        }

        if (Project::GetActive())
        {
            const auto& registry = Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry();
            for (const auto& [handle, metadata] : registry)
            {
                if (wantType != AssetType::None && metadata.Type != wantType)
                    continue;

                std::string label = metadata.FilePath.stem().string();
                if (!filter.empty())
                {
                    std::string lowerLabel = label;
                    std::transform(lowerLabel.begin(), lowerLabel.end(), lowerLabel.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    if (lowerLabel.find(filter) == std::string::npos)
                        continue;
                }

                if (ImGui::Selectable(label.c_str()))
                {
                    if (node)
                    {
                        node->Properties[m_AssetPickerPropertyName] = handle;
                        onGraphEdited();
                    }
                    ImGui::CloseCurrentPopup();
                }
            }
        }

        ImGui::EndChild();
        ImGui::EndPopup();
    }

    void ThedmdCanvasBackend::HandleLinkCreation(NodeGraph& graph, const std::function<void()>& onGraphEdited)
    {
        if (ed::BeginCreate())
        {
            ed::PinId startPinId, endPinId;
            if (ed::QueryNewLink(&startPinId, &endPinId))
            {
                uint32_t fromNode, fromPin, toNode, toPin;
                bool fromIsOutput, toIsOutput;
                DecodePinId(startPinId, fromNode, fromPin, fromIsOutput);
                DecodePinId(endPinId, toNode, toPin, toIsOutput);

                if (fromIsOutput == toIsOutput || fromNode == toNode)
                {
                    ed::RejectNewItem();
                }
                else
                {
                    // Normalize so fromNode/fromPin is always the output end regardless of drag direction.
                    if (!fromIsOutput)
                    {
                        std::swap(fromNode, toNode);
                        std::swap(fromPin, toPin);
                    }

                    GraphNode* fromGraphNode = FindNodeById(graph, fromNode);
                    GraphNode* toGraphNode = FindNodeById(graph, toNode);
                    const NodeTypeDesc* fromType = fromGraphNode ? TypeOf(graph, *fromGraphNode) : nullptr;
                    const NodeTypeDesc* toType = toGraphNode ? TypeOf(graph, *toGraphNode) : nullptr;

                    bool valid = fromType && toType &&
                                fromPin < (uint32_t)fromType->Outputs.size() && toPin < (uint32_t)toType->Inputs.size() &&
                                fromType->Outputs[fromPin].Type == toType->Inputs[toPin].Type;

                    if (valid && ed::AcceptNewItem())
                    {
                        // An input pin takes one link; a new one into it replaces whatever was there.
                        graph.Links.erase(std::remove_if(graph.Links.begin(), graph.Links.end(),
                            [&](const GraphLink& l) { return l.ToNode == toNode && l.ToPin == toPin; }), graph.Links.end());
                        graph.Links.push_back({ fromNode, fromPin, toNode, toPin });
                        onGraphEdited();
                    }
                    else if (!valid)
                    {
                        ed::RejectNewItem();
                    }
                }
            }
        }
        ed::EndCreate();
    }

    void ThedmdCanvasBackend::HandleDeletion(NodeGraph& graph, const std::function<void()>& onGraphEdited)
    {
        if (ed::BeginDelete())
        {
            ed::LinkId linkId;
            while (ed::QueryDeletedLink(&linkId))
            {
                if (!ed::AcceptDeletedItem())
                    continue;

                uint32_t toNode, toPin;
                bool isOutput;
                DecodePinId(ed::PinId(linkId.Get()), toNode, toPin, isOutput);

                size_t before = graph.Links.size();
                graph.Links.erase(std::remove_if(graph.Links.begin(), graph.Links.end(),
                    [&](const GraphLink& l) { return l.ToNode == toNode && l.ToPin == toPin; }), graph.Links.end());
                if (graph.Links.size() != before)
                    onGraphEdited();
            }

            ed::NodeId nodeId;
            while (ed::QueryDeletedNode(&nodeId))
            {
                if (!ed::AcceptDeletedItem())
                    continue;

                uint32_t id = (uint32_t)nodeId.Get();
                graph.Nodes.erase(std::remove_if(graph.Nodes.begin(), graph.Nodes.end(),
                    [&](const GraphNode& n) { return n.Id == id; }), graph.Nodes.end());
                graph.Links.erase(std::remove_if(graph.Links.begin(), graph.Links.end(),
                    [&](const GraphLink& l) { return l.FromNode == id || l.ToNode == id; }), graph.Links.end());
                if (graph.OutputNode == id)
                    graph.OutputNode = kInvalidGraphIndex;
                m_SeededPositions.erase(id);

                onGraphEdited();
            }
        }
        ed::EndDelete();
    }

    void ThedmdCanvasBackend::CreateNode(NodeGraph& graph, const NodeTypeDesc& type, const std::string* parameterName,
                                         const ImVec2& screenPos, const std::function<void()>& onGraphEdited)
    {
        GraphNode node;
        node.Id = graph.NextNodeId++;
        node.TypeName = type.TypeName;
        for (const auto& [name, value] : type.DefaultProperties)
            node.Properties[name] = value;
        if (parameterName)
            node.Properties[kParameterReferenceProperty] = *parameterName;

        ImVec2 canvasPos = ed::ScreenToCanvas(screenPos);
        node.EditorPosition = { canvasPos.x, canvasPos.y };

        graph.Nodes.push_back(node);
        m_SeededPositions.insert(node.Id);
        ed::SetNodePosition(ed::NodeId(node.Id), canvasPos);

        onGraphEdited();
    }

    void ThedmdCanvasBackend::HandleParameterDrop(NodeGraph& graph, const std::function<void()>& onGraphEdited)
    {
        // Called suspended, so the current window is the canvas at real screen coordinates. There's no item to
        // hang a drop target on (the canvas is just a window), hence the custom rect over its whole inner area.
        ImGuiWindow* canvasWindow = ImGui::GetCurrentWindow();
        if (!ImGui::BeginDragDropTargetCustom(canvasWindow->InnerRect, ImGui::GetID("NodeGraphParameterDrop")))
            return;

        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kNodeGraphParameterPayload))
        {
            const std::string name(static_cast<const char*>(payload->Data));
            const bool declared = std::any_of(graph.Parameters.begin(), graph.Parameters.end(),
                [&](const GraphParameter& parameter) { return parameter.Name == name; });

            const NodeTypeDesc* reader = nullptr;
            for (const NodeTypeDesc& type : NodeTypeRegistry::All())
            {
                if (type.Domain == graph.Domain && ReadsParameter(type))
                {
                    reader = &type;
                    break;
                }
            }

            if (declared && reader)
                CreateNode(graph, *reader, &name, ImGui::GetMousePos(), onGraphEdited);
        }
        ImGui::EndDragDropTarget();
    }

    void ThedmdCanvasBackend::ShowCreateNodeMenu(NodeGraph& graph, const std::function<void()>& onGraphEdited)
    {
        // Creates a node of `type` where the menu was opened, optionally pointing it at a graph parameter.
        auto createNode = [&](const NodeTypeDesc& type, const std::string* parameterName)
        {
            CreateNode(graph, type, parameterName, m_CreateNodeScreenPos, onGraphEdited);
        };

        // Every node this graph can create, as a flat list: each node type, and one "Get <name>" per declared
        // parameter for every type that reads a parameter (those aren't offered bare -- useless with none chosen).
        struct Entry
        {
            std::string Category;
            std::string Label;
            const NodeTypeDesc* Type = nullptr;
            const std::string* Parameter = nullptr;
        };
        std::vector<Entry> entries;
        for (const NodeTypeDesc& type : NodeTypeRegistry::All())
        {
            if (type.Domain != graph.Domain)
                continue;
            if (!ReadsParameter(type))
            {
                entries.push_back({ type.Category, type.TypeName, &type, nullptr });
                continue;
            }
            for (const GraphParameter& parameter : graph.Parameters)
                entries.push_back({ "Variables", "Get " + parameter.Name, &type, &parameter.Name });
        }

        // Search box first, focused as the menu opens, like Unreal's action palette: type to filter across every
        // category at once, Enter takes the first match -- so it stays usable at hundreds of node types.
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputTextWithHint("##search", "Search nodes...", m_CreateSearch, sizeof(m_CreateSearch));
        ImGui::Separator();

        auto toLower = [](std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return text;
        };
        const std::string filter = toLower(m_CreateSearch);

        if (filter.empty())
        {
            // Browsing: a submenu per category, in registration order.
            std::vector<std::string> categories;
            for (const Entry& entry : entries)
                if (std::find(categories.begin(), categories.end(), entry.Category) == categories.end())
                    categories.push_back(entry.Category);

            for (const std::string& category : categories)
            {
                if (!ImGui::BeginMenu(category.empty() ? "Other" : category.c_str()))
                    continue;
                for (const Entry& entry : entries)
                    if (entry.Category == category && ImGui::MenuItem(entry.Label.c_str()))
                        createNode(*entry.Type, entry.Parameter);
                ImGui::EndMenu();
            }
            return;
        }

        // Searching: flat results, matching the label or its category.
        const Entry* firstMatch = nullptr;
        for (const Entry& entry : entries)
        {
            if (toLower(entry.Label + " " + entry.Category).find(filter) == std::string::npos)
                continue;
            if (!firstMatch)
                firstMatch = &entry;
            if (ImGui::MenuItem((entry.Label + "    (" + entry.Category + ")").c_str()))
                createNode(*entry.Type, entry.Parameter);
        }
        if (!firstMatch)
            ImGui::TextDisabled("No matching nodes");
        else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
        {
            createNode(*firstMatch->Type, firstMatch->Parameter);
            ImGui::CloseCurrentPopup();
        }
    }

    ed::PinId ThedmdCanvasBackend::MakePinId(uint32_t nodeId, uint32_t pinIndex, bool isOutput)
    {
        // nodeId : bits [20, 63], isOutput : bit 19, pinIndex : bits [0, 18].
        uint64_t encoded = (uint64_t(nodeId) << 20) | (isOutput ? (1ull << 19) : 0ull) | (uint64_t(pinIndex) & 0x7FFFFull);
        return ed::PinId(encoded);
    }

    void ThedmdCanvasBackend::DecodePinId(ed::PinId id, uint32_t& nodeId, uint32_t& pinIndex, bool& isOutput)
    {
        uint64_t raw = id.Get();
        nodeId = (uint32_t)(raw >> 20);
        isOutput = (raw & (1ull << 19)) != 0;
        pinIndex = (uint32_t)(raw & 0x7FFFFull);
    }

    ed::LinkId ThedmdCanvasBackend::MakeLinkId(uint32_t toNode, uint32_t toPin)
    {
        // A link is uniquely identified by its input end (an input pin accepts only one incoming link), so this
        // stays valid across frames/erases unlike an array-index-based id would.
        return ed::LinkId(MakePinId(toNode, toPin, false).Get());
    }

    GraphNode* ThedmdCanvasBackend::FindNodeById(NodeGraph& graph, uint32_t nodeId)
    {
        for (GraphNode& n : graph.Nodes)
            if (n.Id == nodeId)
                return &n;
        return nullptr;
    }

    const NodeTypeDesc* ThedmdCanvasBackend::TypeOf(const NodeGraph& graph, const GraphNode& node)
    {
        return NodeTypeRegistry::Find(graph.Domain, node.TypeName);
    }
}
