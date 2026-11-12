#include "ThedmdCanvasBackend.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <array>
#include <unordered_map>

#include <imgui.h>
#include <imgui_internal.h> // ImDrawFlags_RoundCornersTop
#include <utilities/widgets.h>

#include "NodeGraphPropertyEditor.h"
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Animation/AnimationGraphNodes.h"
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
        enum class PinShape { Circle, Square, RoundSquare, Flow };
        PinShape PinShapeOf(const std::string& pinType)
        {
            if (pinType == "Pose")
                return PinShape::RoundSquare;
            if (pinType == "Audio")
                return PinShape::Square; // a signal (MetaSounds' audio pins)
            if (pinType == "Trigger")
                return PinShape::Flow;   // an event, like an execution pin
            return PinShape::Circle;
        }

        // A pin's icon: filled when connected, outlined when not. Drawn here rather than with the blueprints example's icon widget,
        // whose drawing.cpp passes AddRect / PathStroke's thickness and flags in the order ImGui had before 1.92.8 (they were
        // swapped): "Incorrect parameter. Did you swap 'thickness' and 'flags'?" for every outlined pin.
        void DrawPinIcon(const ImVec2& size, PinShape shape, bool filled, const ImVec4& colour)
        {
            if (ImGui::IsRectVisible(size))
            {
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                const ImVec2 centre(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
                const float thickness = 2.0f * size.x / 24.0f;
                const ImU32 colour32 = ImGui::ColorConvertFloat4ToU32(colour);
                const ImU32 inner = IM_COL32(32, 32, 32, static_cast<int>(colour.w * 255.0f));
                const float r = filled ? size.x * 0.25f : size.x * 0.25f - 0.5f;
                switch (shape)
                {
                case PinShape::Circle:
                    if (filled)
                        drawList->AddCircleFilled(centre, r, colour32, 16);
                    else
                    {
                        drawList->AddCircleFilled(centre, r, inner, 16);
                        drawList->AddCircle(centre, r, colour32, 16, thickness);
                    }
                    break;
                case PinShape::Square:
                case PinShape::RoundSquare:
                {
                    const float rounding = shape == PinShape::RoundSquare ? r * 0.5f : 0.0f;
                    const ImVec2 p0(centre.x - r, centre.y - r), p1(centre.x + r, centre.y + r);
                    if (filled)
                        drawList->AddRectFilled(p0, p1, colour32, rounding);
                    else
                    {
                        drawList->AddRectFilled(p0, p1, inner, rounding);
                        drawList->AddRect(p0, p1, colour32, rounding, thickness);
                    }
                    break;
                }
                case PinShape::Flow:
                {
                    // An arrow pointing right.
                    const float w = size.x * 0.32f, h = size.y * 0.3f;
                    const ImVec2 points[5] = { ImVec2(centre.x - w, centre.y - h), ImVec2(centre.x + w * 0.2f, centre.y - h), ImVec2(centre.x + w, centre.y),
                                               ImVec2(centre.x + w * 0.2f, centre.y + h), ImVec2(centre.x - w, centre.y + h) };
                    if (filled)
                        drawList->AddConvexPolyFilled(points, 5, colour32);
                    else
                    {
                        drawList->AddConvexPolyFilled(points, 5, inner);
                        drawList->AddPolyline(points, 5, colour32, thickness, ImDrawFlags_Closed);
                    }
                    break;
                }
                }
            }
            ImGui::Dummy(size);
        }

        ImVec4 PinColor(const std::string& pinType)
        {
            if (pinType == "Pose")     return ImVec4(0.78f, 0.55f, 0.95f, 1.0f); // purple
            if (pinType == "Float")    return ImVec4(0.55f, 0.85f, 0.55f, 1.0f); // green
            if (pinType == "Bool")     return ImVec4(0.85f, 0.45f, 0.45f, 1.0f); // red
            if (pinType == "Int")      return ImVec4(0.40f, 0.85f, 0.85f, 1.0f); // cyan
            if (pinType == "Vector2")  return ImVec4(0.95f, 0.85f, 0.35f, 1.0f); // yellow
            if (pinType == "Audio")    return ImVec4(0.40f, 0.60f, 1.00f, 1.0f); // blue
            if (pinType == "Trigger")  return ImVec4(0.95f, 0.95f, 0.95f, 1.0f); // white
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
            if (category == "State Machines") return IM_COL32(150, 60, 60, 255); // red
            // Audio graph categories.
            if (category == "Triggers")  return IM_COL32(150, 150, 150, 255); // grey-white
            if (category == "Math")      return IM_COL32(60, 130, 70, 255);   // green
            if (category == "Envelopes") return IM_COL32(150, 120, 40, 255);  // amber
            if (category == "Mixing")    return IM_COL32(40, 120, 150, 255);  // teal
            if (category == "Filters")   return IM_COL32(120, 70, 140, 255);  // violet
            return IM_COL32(90, 90, 90, 255);
        }

        // A node type that reads a graph parameter is recognized by its default properties containing
        // kParameterReferenceProperty -- not by name, so this works for any domain's such type.
        bool ReadsParameter(const NodeTypeDesc& type)
        {
            return std::any_of(type.DefaultProperties.begin(), type.DefaultProperties.end(),
                [](const auto& property) { return property.first == kParameterReferenceProperty; });
        }

        // The domain's node reading `parameter`: of its type (an audio graph's Get Float / Get Trigger), or one reading any type.
        const NodeTypeDesc* ParameterReader(const std::string& domain, const GraphParameter& parameter)
        {
            const NodeTypeDesc* any = nullptr;
            for (const NodeTypeDesc& type : NodeTypeRegistry::All())
            {
                if (type.Domain != domain || !ReadsParameter(type))
                    continue;
                if (type.ParameterType == GraphParameterTypeName(parameter))
                    return &type;
                if (type.ParameterType.empty() && !any)
                    any = &type;
            }
            return any;
        }

        // The layer the graph's wires are drawn in: an empty node kept under the first node and below every node (z), whose
        // background draw list sits under all the nodes (the node editor offers per-node layers only). Its id is above every pin id
        // (node id << 20 | pin), so it never meets one.
        constexpr uint64_t kGraphWireLayerNode = 1ull << 60;
        bool IsGraphWireLayer(ed::NodeId node) { return node.Get() == kGraphWireLayerNode; }

        constexpr float kWireCornerRadius = 8.0f;
        constexpr float kWireLaneSpacing = 6.0f;  // between wires sharing a line
        constexpr float kWireHitDistance = 6.0f;  // screen pixels around a wire that pick it
        WireRouting::RouteSettings GraphRouteSettings() { return WireRouting::RouteSettings{}; }

        ImU32 ToU32(const ImVec4& color) { return ImGui::ColorConvertFloat4ToU32(color); }
    }

    ThedmdCanvasBackend::ThedmdCanvasBackend()
    {
        ed::Config config;
        // Node positions persist through the .nanimgraph file itself (GraphNode::EditorPosition), not a
        // separate settings file next to the executable.
        config.SettingsFile = nullptr;
        m_Context = ed::CreateEditor(&config);
        m_StateContext = ed::CreateEditor(&config);
    }

    ThedmdCanvasBackend::~ThedmdCanvasBackend()
    {
        if (m_Context)
            ed::DestroyEditor(m_Context);
        if (m_StateContext)
            ed::DestroyEditor(m_StateContext);
    }

    void ThedmdCanvasBackend::Draw(NodeGraph& graph, const std::vector<GraphParameter>& parameters, const std::function<void()>& onGraphEdited,
                                   uint32_t* outOpenNode)
    {
        ed::SetCurrentEditor(m_Context);
        m_CanvasMin = ImGui::GetCursorScreenPos();
        ed::Begin("Canvas");
        m_CanvasMax = ImVec2(m_CanvasMin.x + ed::GetScreenSize().x, m_CanvasMin.y + ed::GetScreenSize().y);

        // A node made from an input pin's dropped wire: its right edge where the wire was dropped, once its width is known.
        if (m_AlignNewNodeId != 0)
        {
            const ed::NodeId alignId(m_AlignNewNodeId);
            const ImVec2 size = ed::GetNodeSize(alignId);
            if (!FindNodeById(graph, m_AlignNewNodeId))
                m_AlignNewNodeId = 0;
            else if (size.x > 0.0f)
            {
                const ImVec2 position = ed::GetNodePosition(alignId);
                ed::SetNodePosition(alignId, ImVec2(m_AlignNewNodeRight - size.x, position.y));
                m_AlignNewNodeId = 0;
            }
        }

        m_PinIcons.clear();
        for (GraphNode& node : graph.Nodes)
        {
            if (node.TypeName == kRerouteNodeType)
            {
                DrawRerouteNode(graph, node);
                continue;
            }
            const NodeTypeDesc* type = TypeOf(graph, node);
            if (type)
                DrawNode(graph, node, *type, onGraphEdited);
        }

        // Nodes the user moved leave the asset unsaved; a route shaped by hand whose two nodes moved together (a box selection
        // dragged) moves with them, while moving only one end stretches it.
        std::unordered_map<uint32_t, ImVec2> moved;
        for (const GraphNode& node : graph.Nodes)
        {
            const ImVec2 now(node.EditorPosition.x, node.EditorPosition.y);
            const auto previous = m_PreviousNodePositions.find(node.Id);
            if (previous != m_PreviousNodePositions.end() && (previous->second.x != now.x || previous->second.y != now.y))
                moved[node.Id] = ImVec2(now.x - previous->second.x, now.y - previous->second.y);
            m_PreviousNodePositions[node.Id] = now;
        }
        if (!moved.empty())
        {
            m_LayoutEdited = true;
            for (GraphLink& link : graph.Links)
            {
                const auto from = moved.find(link.FromNode);
                const auto to = moved.find(link.ToNode);
                if (link.Route.empty() || from == moved.end() || to == moved.end() || from->second.x != to->second.x || from->second.y != to->second.y)
                    continue;
                for (size_t i = 0; i < link.Route.size(); ++i)
                    link.Route[i] += i % 2 == 0 ? from->second.x : from->second.y;
            }
        }

        // Wires in their type's color (Unreal's), so a wire through reroutes is followed by its color; routed with right angles
        // around the nodes, under them.
        DrawWires(graph);

        HandleLinkCreation(graph, onGraphEdited);
        HandleDeletion(graph, onGraphEdited);

        // Double-clicking a node that owns sub graphs (a state machine) asks the panel to open its state view.
        if (outOpenNode)
        {
            const ed::NodeId doubleClicked = ed::GetDoubleClickedNode();
            if (doubleClicked && !IsGraphWireLayer(doubleClicked))
            {
                GraphNode* clickedNode = FindNodeById(graph, static_cast<uint32_t>(doubleClicked.Get()));
                const NodeTypeDesc* clickedType = clickedNode ? TypeOf(graph, *clickedNode) : nullptr;
                if (clickedType && clickedType->OwnsSubGraphs)
                    *outOpenNode = clickedNode->Id;
            }
        }

        if (m_FitViewOnNextFrame && !graph.Nodes.empty())
        {
            ed::NavigateToContent(0.0f);
            m_FitViewOnNextFrame = false;
        }

        // Context menus escape the canvas' pan/zoom transform, so they're raised and drawn while suspended --
        // the canonical imgui-node-editor pattern (its own blueprints-example does the same).
        ed::Suspend();

        HandleWireInput(graph, onGraphEdited);
        if (m_DoubleClickedLink != 0)
        {
            InsertReroute(graph, m_DoubleClickedLink, ImGui::GetMousePos(), onGraphEdited);
            m_DoubleClickedLink = 0;
        }

        ed::NodeId contextNodeId;
        if (ed::ShowNodeContextMenu(&contextNodeId) && !IsGraphWireLayer(contextNodeId))
        {
            m_ContextNodeId = (uint32_t)contextNodeId.Get();
            ImGui::OpenPopup("NodeContext");
        }
        if (ed::ShowBackgroundContextMenu())
        {
            m_CreateNodeScreenPos = ImGui::GetMousePos();
            m_CreateSearch[0] = '\0';
            m_NewNodeFromPin = 0;
            ImGui::OpenPopup("CreateNode");
        }
        if (m_OpenCreateFromPin)
        {
            m_OpenCreateFromPin = false;
            m_CreateNodeScreenPos = ImGui::GetMousePos();
            m_CreateSearch[0] = '\0';
            ImGui::OpenPopup("CreateNode");
        }
        if (m_OpenWireContext)
        {
            m_OpenWireContext = false;
            ImGui::OpenPopup("WireContext");
        }
        DrawWireContextMenu(graph, onGraphEdited);

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
            ShowCreateNodeMenu(graph, parameters, onGraphEdited);
            ImGui::EndPopup();
        }
        else
            m_NewNodeFromPin = 0; // closed without picking one: the dropped wire is gone, nothing changed (Unreal's Escape)

        DrawPendingAssetPicker(graph, onGraphEdited);
        DrawPendingBonePicker(graph, onGraphEdited);
        HandleParameterDrop(graph, parameters, onGraphEdited);

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

        // Everything but the output pins is laid out first, in one group, to know the node's width: the outputs then go at its
        // right edge (each row ending flush with it) as the inputs start at its left edge, so every wire ends at the node's border.
        ImGui::BeginGroup();
        ImGui::TextUnformatted(type.TypeName.c_str());
        // Captured in screen space (matching what GetNodeBackgroundDrawList expects, per the reference
        // BlueprintNodeBuilder::End()) so the header band below lines up with this row regardless of pan/zoom.
        const ImVec2 headerMax = ImGui::GetItemRectMax();
        ImGui::Dummy(ImVec2(0.0f, 4.0f)); // room for the header band painted over this node after EndNode()

        const std::vector<GraphLink>& links = graph.Links;
        const float pinsTop = ImGui::GetCursorScreenPos().y;
        const ImGuiStyle& style = ImGui::GetStyle();

        // The node's pins (a graph node's come from the graph it plays); its value inputs' literals seeded the first time.
        const NodePinList pins = ResolveNodePins(type, node.Properties);
        for (const auto& [name, literal] : pins.Literals)
            node.Properties.try_emplace(name, literal);
        const uint32_t inputCount = static_cast<uint32_t>(pins.Inputs.size());
        const bool hasInputColumn = inputCount > 0 || !type.VariableInputs.empty();
        if (hasInputColumn)
            ImGui::BeginGroup();
        for (uint32_t p = 0; p < inputCount; p++)
        {
            bool connected = std::any_of(links.begin(), links.end(),
                [&](const GraphLink& l) { return l.ToNode == node.Id && l.ToPin == p; });

            const NodePinDesc& pin = pins.Inputs[p];
            const ed::PinId pinId = MakePinId(node.Id, p, false);
            ed::BeginPin(pinId, ed::PinKind::Input);
            DrawPinIcon(kPinIconSize, PinShapeOf(pin.Type), connected, PinColor(pin.Type));
            m_PinIcons[pinId.Get()] = { ImGui::GetItemRectMin(), ImGui::GetItemRectMax() };
            ed::PinPivotRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); // a wire being dragged starts at the icon
            ImGui::SameLine();
            ImGui::TextUnformatted(pin.Name.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", pin.Type.c_str());
            ed::EndPin();
        }
        // Unreal's Add Pin: one more of the type's variable input group (its literals seeded), or the last group away (its links
        // and literals with it).
        if (!type.VariableInputs.empty())
        {
            const uint32_t variable = VariableInputCount(type, node.Properties);
            ImGui::PushID(static_cast<int>(node.Id));
            if (ImGui::SmallButton("+ Pin"))
            {
                for (const NodePinDesc& pin : type.VariableInputs)
                    if (const auto literal = type.VariableInputLiterals.find(pin.Name); literal != type.VariableInputLiterals.end())
                        node.Properties.try_emplace(pin.Name + " " + std::to_string(variable), literal->second);
                node.Properties[kVariableInputCountProperty] = static_cast<int32_t>(variable + 1);
                onGraphEdited();
            }
            if (variable > type.MinVariableInputs)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("- Pin"))
                {
                    const uint32_t firstOfLast = inputCount - static_cast<uint32_t>(type.VariableInputs.size());
                    graph.Links.erase(std::remove_if(graph.Links.begin(), graph.Links.end(),
                        [&](const GraphLink& l) { return l.ToNode == node.Id && l.ToPin >= firstOfLast; }), graph.Links.end());
                    for (const NodePinDesc& pin : type.VariableInputs)
                        node.Properties.erase(pin.Name + " " + std::to_string(variable - 1));
                    node.Properties[kVariableInputCountProperty] = static_cast<int32_t>(variable - 1);
                    onGraphEdited();
                }
            }
            ImGui::PopID();
        }
        if (hasInputColumn)
            ImGui::EndGroup();

        // The output column's room, beside the inputs (the pins themselves are drawn at the end, once the width is known).
        const float rowHeight = std::max(kPinIconSize.y, ImGui::GetTextLineHeight());
        std::vector<float> outputWidths;
        float outputsWidth = 0.0f;
        for (const NodePinDesc& pin : pins.Outputs)
        {
            outputWidths.push_back(ImGui::CalcTextSize(pin.Name.c_str()).x + style.ItemSpacing.x + kPinIconSize.x);
            outputsWidth = std::max(outputsWidth, outputWidths.back());
        }
        if (!pins.Outputs.empty())
        {
            if (hasInputColumn)
                ImGui::SameLine(0.0f, 16.0f);
            const float count = static_cast<float>(pins.Outputs.size());
            ImGui::Dummy(ImVec2(outputsWidth, count * rowHeight + (count - 1.0f) * style.ItemSpacing.y));
        }

        if (type.OwnsSubGraphs)
        {
            ImGui::TextDisabled("%zu states", node.SubGraphs.size());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Double-click to edit the states and transitions");
        }

        // Properties a state machine sets from its own view are not drawn; a separator with nothing under it would stretch
        // the node to the canvas width.
        const bool hasVisibleProperties = std::any_of(node.Properties.begin(), node.Properties.end(),
            [&](const auto& property) { return !(type.OwnsSubGraphs && property.first == "EntryState"); });
        if (hasVisibleProperties)
        {
            ImGui::Separator();
            // In the order the node type declares them (Clip0, Position0, Clip1, ... for a blend space), not the map's hash
            // order; properties the type does not declare (older files) after them, by name.
            std::vector<std::string> order;
            order.reserve(node.Properties.size());
            for (const auto& [name, defaultValue] : type.DefaultProperties)
            {
                // A property the type gained after the node was saved (a blend space's Smoothing Time) shows with its default --
                // the value evaluation already falls back to -- and is saved with the graph from then on.
                if (!node.Properties.contains(name) && name != kVariableInputCountProperty && !(type.OwnsSubGraphs && name == "EntryState"))
                    node.Properties[name] = defaultValue;
                if (node.Properties.contains(name))
                    order.push_back(name);
            }
            std::vector<std::string> extra;
            for (const auto& [name, value] : node.Properties)
            {
                if (std::none_of(order.begin(), order.end(), [&](const std::string& declared) { return declared == name; }))
                    extra.push_back(name);
            }
            std::sort(extra.begin(), extra.end());
            order.insert(order.end(), extra.begin(), extra.end());

            for (const std::string& name : order)
            {
                if (name == kVariableInputCountProperty)
                    continue; // the + / - pin buttons
                if (type.OwnsSubGraphs && name == "EntryState")
                    continue; // set from the state view
                NodeGraphValue& value = node.Properties[name];
                // One ID scope per property: widgets whose label is their value (asset buttons all reading "None") would
                // otherwise collide inside the node.
                ImGui::PushID(name.c_str());
                // A pin's literal does nothing while a link feeds the pin: shown, but greyed out.
                uint32_t pinIndex = UINT32_MAX;
                for (uint32_t p = 0; p < inputCount && pinIndex == UINT32_MAX; ++p)
                    if (pins.Inputs[p].Name == name)
                        pinIndex = p;
                const bool pinConnected = pinIndex != UINT32_MAX &&
                    std::any_of(links.begin(), links.end(), [&](const GraphLink& l) { return l.ToNode == node.Id && l.ToPin == pinIndex; });
                ImGui::BeginDisabled(pinConnected);
                const auto boneHint = type.PropertyAssetTypeHints.find(name);
                std::string* text = std::get_if<std::string>(&value);
                if (text && boneHint != type.PropertyAssetTypeHints.end() && boneHint->second == "Bone")
                {
                    DrawBonePickerButton(node.Id, name, *text);
                }
                else if (uint64_t* handle = std::get_if<uint64_t>(&value))
                {
                    auto hintIt = type.PropertyAssetTypeHints.find(name);
                    DrawAssetPickerButton(node.Id, name, *handle, hintIt != type.PropertyAssetTypeHints.end() ? hintIt->second : std::string());
                }
                else
                {
                    auto rangeIt = type.PropertyRanges.find(name);
                    auto optionsIt = type.PropertyEnums.find(name);
                    if (DrawNodeGraphProperty(name, value, rangeIt != type.PropertyRanges.end() ? &rangeIt->second : nullptr,
                                              optionsIt != type.PropertyEnums.end() ? &optionsIt->second : nullptr))
                        onGraphEdited();
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }

        ImGui::EndGroup();
        const float rightEdge = ImGui::GetItemRectMax().x;

        // The output pins, each row ending at the right edge.
        for (uint32_t p = 0; p < static_cast<uint32_t>(pins.Outputs.size()); p++)
        {
            const bool connected = std::any_of(links.begin(), links.end(),
                [&](const GraphLink& l) { return l.FromNode == node.Id && l.FromPin == p; });
            ImGui::SetCursorScreenPos(ImVec2(rightEdge - outputWidths[p], pinsTop + static_cast<float>(p) * (rowHeight + style.ItemSpacing.y)));
            const ed::PinId pinId = MakePinId(node.Id, p, true);
            ed::BeginPin(pinId, ed::PinKind::Output);
            ImGui::TextUnformatted(pins.Outputs[p].Name.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", pins.Outputs[p].Type.c_str());
            ImGui::SameLine();
            DrawPinIcon(kPinIconSize, PinShapeOf(pins.Outputs[p].Type), connected, PinColor(pins.Outputs[p].Type));
            m_PinIcons[pinId.Get()] = { ImGui::GetItemRectMin(), ImGui::GetItemRectMax() };
            ed::PinPivotRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ed::EndPin();
        }

        ImGui::PopItemWidth();
        ImGui::PopID();
        ed::EndNode();

        // A flat category-colored, rounded top band across the whole node -- see CategoryHeaderColor's comment for why this is
        // painted by hand instead of through the reference BlueprintNodeBuilder.
        if (ImGui::IsItemVisible())
        {
            ImDrawList* backgroundDrawList = ed::GetNodeBackgroundDrawList(nodeId);
            const float rounding = ed::GetStyle().NodeRounding;
            const float pad = 8.0f;
            const ImVec2 nodeMin = ed::GetNodePosition(nodeId);
            const ImVec2 nodeSize = ed::GetNodeSize(nodeId);
            backgroundDrawList->AddRectFilled(
                ImVec2(nodeMin.x, nodeMin.y),
                ImVec2(nodeMin.x + nodeSize.x, headerMax.y + pad),
                CategoryHeaderColor(type.Category), rounding, ImDrawFlags_RoundCornersTop);
        }

        ImVec2 currentPos = ed::GetNodePosition(nodeId);
        node.EditorPosition = { currentPos.x, currentPos.y };
    }

    void ThedmdCanvasBackend::SetBoneList(std::vector<BoneEntry> bones, std::string skeletonName)
    {
        m_Bones = std::move(bones);
        m_BoneSkeletonName = std::move(skeletonName);
    }

    void ThedmdCanvasBackend::DrawBonePickerButton(uint32_t nodeId, const std::string& propertyName, const std::string& current)
    {
        ImGui::Text("%s:", propertyName.c_str());
        ImGui::SameLine();
        const bool known = m_Bones.empty() || current.empty() ||
                           std::any_of(m_Bones.begin(), m_Bones.end(), [&](const BoneEntry& bone) { return bone.Name == current; });
        if (!known)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
        if (ImGui::Button(((current.empty() ? std::string("(none)") : current) + "##bone").c_str()))
        {
            m_ShowBonePicker = true;
            m_BonePickerNodeId = nodeId;
            m_BonePickerPropertyName = propertyName;
            m_BonePickerFilter[0] = '\0';
            strncpy_s(m_BonePickerTyped, current.c_str(), sizeof(m_BonePickerTyped) - 1);
        }
        if (!known)
        {
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s has no bone of this name", m_BoneSkeletonName.c_str());
        }
    }

    void ThedmdCanvasBackend::DrawPendingBonePicker(NodeGraph& graph, const std::function<void()>& onGraphEdited)
    {
        // Like the asset picker: opened the frame the button was pressed, drawn suspended (screen coordinates) every frame it is open.
        if (m_ShowBonePicker)
        {
            ImGui::OpenPopup("BonePicker");
            m_ShowBonePicker = false;
        }
        if (!ImGui::BeginPopup("BonePicker"))
            return;

        GraphNode* node = FindNodeById(graph, m_BonePickerNodeId);
        auto choose = [&](const std::string& bone)
        {
            if (node)
            {
                node->Properties[m_BonePickerPropertyName] = bone;
                onGraphEdited();
            }
            ImGui::CloseCurrentPopup();
        };

        if (m_Bones.empty())
        {
            ImGui::TextDisabled("No skeleton to list bones from:");
            ImGui::TextDisabled("pick a Mesh in the Preview section of this window.");
        }
        else
        {
            ImGui::TextDisabled("Bones of %s", m_BoneSkeletonName.c_str());
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(240.0f);
            ImGui::InputTextWithHint("##filter", "Search bones...", m_BonePickerFilter, sizeof(m_BonePickerFilter));
            auto lower = [](std::string text)
            {
                std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                return text;
            };
            const std::string filter = lower(m_BonePickerFilter);
            const std::string current = node && node->Properties.contains(m_BonePickerPropertyName) &&
                                                std::holds_alternative<std::string>(node->Properties[m_BonePickerPropertyName])
                                            ? std::get<std::string>(node->Properties[m_BonePickerPropertyName]) : std::string();
            // The skeleton's tree, indented by depth (a flat list of matches while searching).
            ImGui::BeginChild("BonePickerList", ImVec2(260.0f, 280.0f), true);
            for (const BoneEntry& bone : m_Bones)
            {
                if (!filter.empty() && lower(bone.Name).find(filter) == std::string::npos)
                    continue;
                if (filter.empty() && bone.Depth > 0)
                    ImGui::Indent(12.0f * bone.Depth);
                if (ImGui::Selectable(bone.Name.c_str(), bone.Name == current))
                    choose(bone.Name);
                if (filter.empty() && bone.Depth > 0)
                    ImGui::Unindent(12.0f * bone.Depth);
            }
            ImGui::EndChild();
        }

        // Any name, typed (a skeleton not previewed, a bone added later).
        ImGui::SetNextItemWidth(180.0f);
        const bool entered = ImGui::InputText("##typed", m_BonePickerTyped, sizeof(m_BonePickerTyped), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Set") || entered)
            choose(m_BonePickerTyped);
        ImGui::EndPopup();
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
        if (ImGui::Button((label + "##asset").c_str()))
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

                // Clips of different models often share a name ("Walk"): the handle keeps their IDs apart, the path tells them apart.
                ImGui::PushID(static_cast<int>(static_cast<uint64_t>(handle) ^ (static_cast<uint64_t>(handle) >> 32)));
                if (ImGui::Selectable(label.c_str()))
                {
                    if (node)
                    {
                        node->Properties[m_AssetPickerPropertyName] = handle;
                        onGraphEdited();
                    }
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", metadata.FilePath.generic_string().c_str());
                ImGui::PopID();
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

                    bool valid = false;
                    GraphNode* retype = nullptr; // an untyped (or unconnected) reroute takes the other end's type
                    std::string retypeTo;
                    if (fromType && toType)
                    {
                        const NodePinList fromPins = ResolveNodePins(*fromType, fromGraphNode->Properties);
                        const NodePinList toPins = ResolveNodePins(*toType, toGraphNode->Properties);
                        if (fromPin < (uint32_t)fromPins.Outputs.size() && toPin < (uint32_t)toPins.Inputs.size())
                        {
                            const std::string& a = fromPins.Outputs[fromPin].Type;
                            const std::string& b = toPins.Inputs[toPin].Type;
                            auto retypable = [&](const GraphNode& node, const std::string& type)
                            {
                                if (node.TypeName != kRerouteNodeType)
                                    return false;
                                const bool linked = std::any_of(graph.Links.begin(), graph.Links.end(),
                                    [&](const GraphLink& l) { return l.FromNode == node.Id || l.ToNode == node.Id; });
                                return type.empty() || !linked;
                            };
                            if (!a.empty() && a == b)
                                valid = true;
                            else if (!a.empty() && retypable(*toGraphNode, b))
                            {
                                retype = toGraphNode;
                                retypeTo = a;
                                valid = true;
                            }
                            else if (!b.empty() && retypable(*fromGraphNode, a))
                            {
                                retype = fromGraphNode;
                                retypeTo = b;
                                valid = true;
                            }
                        }
                    }

                    if (valid && ed::AcceptNewItem())
                    {
                        if (retype)
                            retype->Properties[kRerouteTypeProperty] = retypeTo;
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

            // Unreal: a wire let go over empty canvas opens the create menu with the nodes that fit it; the one picked is connected.
            ed::PinId newNodePin;
            if (ed::QueryNewNode(&newNodePin))
            {
                ImGui::SetTooltip("+ Create Node");
                if (ed::AcceptNewItem())
                {
                    m_NewNodeFromPin = newNodePin.Get();
                    m_OpenCreateFromPin = true;
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
                if (IsGraphWireLayer(nodeId))
                {
                    ed::RejectDeletedItem();
                    continue;
                }
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

    void ThedmdCanvasBackend::DrawRerouteNode(NodeGraph& graph, GraphNode& node)
    {
        const ed::NodeId nodeId(node.Id);
        if (m_SeededPositions.insert(node.Id).second)
            ed::SetNodePosition(nodeId, ImVec2(node.EditorPosition.x, node.EditorPosition.y));
        const auto found = node.Properties.find(kRerouteTypeProperty);
        const std::string* typeName = found != node.Properties.end() ? std::get_if<std::string>(&found->second) : nullptr;
        const std::string pinType = typeName ? *typeName : std::string();
        const ImVec4 color = pinType.empty() ? ImVec4(0.7f, 0.7f, 0.7f, 1.0f) : PinColor(pinType);
        const bool inLinked = std::any_of(graph.Links.begin(), graph.Links.end(), [&](const GraphLink& l) { return l.ToNode == node.Id; });
        const bool outLinked = std::any_of(graph.Links.begin(), graph.Links.end(), [&](const GraphLink& l) { return l.FromNode == node.Id; });

        // A compact node (no header, no properties): the two pins side by side, the wires meeting at its middle.
        ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(3.0f, 3.0f, 3.0f, 3.0f));
        ed::PushStyleVar(ed::StyleVar_NodeRounding, 8.0f);
        ed::BeginNode(nodeId);
        ImGui::PushID(static_cast<int>(node.Id));
        constexpr ImVec2 kKnot(10.0f, 12.0f);
        const ed::PinId inputPin = MakePinId(node.Id, 0, false);
        ed::BeginPin(inputPin, ed::PinKind::Input);
        ed::PinPivotAlignment(ImVec2(1.0f, 0.5f));
        DrawPinIcon(kKnot, PinShapeOf(pinType), inLinked, color);
        m_PinIcons[inputPin.Get()] = { ImGui::GetItemRectMin(), ImGui::GetItemRectMax() };
        ed::EndPin();
        ImGui::SameLine(0.0f, 0.0f);
        const ed::PinId outputPin = MakePinId(node.Id, 0, true);
        ed::BeginPin(outputPin, ed::PinKind::Output);
        ed::PinPivotAlignment(ImVec2(0.0f, 0.5f));
        DrawPinIcon(kKnot, PinShapeOf(pinType), outLinked, color);
        m_PinIcons[outputPin.Get()] = { ImGui::GetItemRectMin(), ImGui::GetItemRectMax() };
        ed::EndPin();
        ImGui::PopID();
        ed::EndNode();
        ed::PopStyleVar(2);
        const ImVec2 position = ed::GetNodePosition(nodeId); // (saved with the graph, as every node's)
        node.EditorPosition = { position.x, position.y };
        if (ed::GetHoveredNode() == nodeId)
        {
            ed::Suspend();
            ImGui::SetTooltip(pinType.empty() ? "Reroute (connect a wire: it takes that wire's type)" : "Reroute (%s)", pinType.c_str());
            ed::Resume();
        }
    }

    void ThedmdCanvasBackend::InsertReroute(NodeGraph& graph, uint64_t linkId, const ImVec2& screenPos, const std::function<void()>& onGraphEdited)
    {
        uint32_t toNode = 0, toPin = 0;
        bool isOutput = false;
        DecodePinId(ed::PinId(linkId), toNode, toPin, isOutput);
        const auto linkIt = std::find_if(graph.Links.begin(), graph.Links.end(), [&](const GraphLink& l) { return l.ToNode == toNode && l.ToPin == toPin; });
        if (linkIt == graph.Links.end())
            return;
        const GraphLink link = *linkIt;
        // The wire's type: what its output end carries.
        std::string pinType;
        if (const GraphNode* fromNode = FindNodeById(graph, link.FromNode))
            if (const NodeTypeDesc* fromType = TypeOf(graph, *fromNode))
            {
                const NodePinList pins = ResolveNodePins(*fromType, fromNode->Properties);
                if (link.FromPin < pins.Outputs.size())
                    pinType = pins.Outputs[link.FromPin].Type;
            }
        GraphNode reroute;
        reroute.Id = graph.NextNodeId++;
        reroute.TypeName = kRerouteNodeType;
        reroute.Properties[kRerouteTypeProperty] = pinType;
        const ImVec2 canvas = ed::ScreenToCanvas(screenPos);
        const ImVec2 position(canvas.x - 13.0f, canvas.y - 9.0f); // (centred on the click)
        reroute.EditorPosition = { position.x, position.y };
        graph.Nodes.push_back(reroute);
        m_SeededPositions.insert(reroute.Id);
        ed::SetNodePosition(ed::NodeId(reroute.Id), position);
        graph.Links.erase(linkIt);
        graph.Links.push_back({ link.FromNode, link.FromPin, reroute.Id, 0 });
        graph.Links.push_back({ reroute.Id, 0, link.ToNode, link.ToPin });
        onGraphEdited();
    }

    void ThedmdCanvasBackend::DrawWires(NodeGraph& graph)
    {
        using namespace WireRouting;
        const RouteSettings settings = GraphRouteSettings();

        // Every node is a box to route around.
        std::vector<Rect> boxes;
        std::vector<uint32_t> boxNodes;
        std::unordered_map<uint32_t, int> boxOf;
        for (const GraphNode& node : graph.Nodes)
        {
            const ImVec2 position = ed::GetNodePosition(ed::NodeId(node.Id));
            const ImVec2 size = ed::GetNodeSize(ed::NodeId(node.Id));
            if (size.x <= 0.0f || size.y <= 0.0f)
                continue;
            boxOf[node.Id] = static_cast<int>(boxes.size());
            boxes.push_back({ position, ImVec2(position.x + size.x, position.y + size.y) });
            boxNodes.push_back(node.Id);
        }
        m_Wires.clear();
        if (boxes.empty())
            return;

        // The layer node, tucked under the first node (so it is never clicked) and below every node.
        {
            const ed::NodeId layer(kGraphWireLayerNode);
            ed::SetNodePosition(layer, ImVec2(boxes.front().Min.x + 4.0f, boxes.front().Min.y + 4.0f));
            ed::SetNodeZPosition(layer, -1.0e6f);
            ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ed::BeginNode(layer);
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
            ed::EndNode();
            ed::PopStyleColor(2);
            ed::PopStyleVar();
        }
        ImDrawList* wireLayer = ed::GetNodeBackgroundDrawList(ed::NodeId(kGraphWireLayerNode));

        // A route's key: its ends, its hand-shaped coordinates, and every node overlapping the area it was routed in (a node moved
        // anywhere else cannot change it).
        auto makeKey = [&](ImVec2 start, ImVec2 end, const std::vector<float>& manual, const Rect& area)
        {
            std::vector<float> key{ start.x, start.y, end.x, end.y, static_cast<float>(manual.size()) };
            key.insert(key.end(), manual.begin(), manual.end());
            if (manual.empty())
                for (size_t k = 0; k < boxes.size(); ++k)
                    if (boxes[k].Max.x > area.Min.x && boxes[k].Min.x < area.Max.x && boxes[k].Max.y > area.Min.y && boxes[k].Min.y < area.Max.y)
                        key.insert(key.end(), { static_cast<float>(boxNodes[k]), boxes[k].Min.x, boxes[k].Min.y, boxes[k].Max.x, boxes[k].Max.y });
            return key;
        };

        std::vector<std::vector<ImVec2>> routes;
        std::vector<uint64_t> nets;
        std::unordered_set<uint64_t> live;
        for (const GraphLink& link : graph.Links)
        {
            const ed::PinId fromPin = MakePinId(link.FromNode, link.FromPin, true);
            const ed::PinId toPin = MakePinId(link.ToNode, link.ToPin, false);
            const auto fromIcon = m_PinIcons.find(fromPin.Get());
            const auto toIcon = m_PinIcons.find(toPin.Get());
            const auto fromBox = boxOf.find(link.FromNode);
            const auto toBox = boxOf.find(link.ToNode);
            if (fromIcon == m_PinIcons.end() || toIcon == m_PinIcons.end() || fromBox == boxOf.end() || toBox == boxOf.end())
                continue;
            const ImVec2 fromCentre(0.5f * (fromIcon->second.first.x + fromIcon->second.second.x), 0.5f * (fromIcon->second.first.y + fromIcon->second.second.y));
            const ImVec2 toCentre(0.5f * (toIcon->second.first.x + toIcon->second.second.x), 0.5f * (toIcon->second.first.y + toIcon->second.second.y));
            // The wire runs from the source node's right border to the target node's left border; inside the nodes it is drawn on
            // them, from the border to the pin.
            const ImVec2 start(boxes[fromBox->second].Max.x, fromCentre.y);
            const ImVec2 end(boxes[toBox->second].Min.x, toCentre.y);

            // A hand-shaped route keeps its legs, but its first / last vertical leg never goes back into the node it leaves / enters
            // (an end moved past it). One vertical leg that cannot do both (the wire now runs backwards) gives way to the router.
            std::vector<float> manual = link.Route;
            if (manual.size() % 2 == 0 && !manual.empty())
                manual.pop_back();
            if (!manual.empty())
            {
                if (manual.size() == 1 && start.x + settings.Stub > end.x - settings.Stub)
                    manual.clear();
                else
                {
                    manual.front() = std::max(manual.front(), start.x + settings.Stub);
                    manual.back() = std::min(manual.back(), end.x - settings.Stub);
                }
            }

            const uint64_t linkId = MakeLinkId(link.ToNode, link.ToPin).Get();
            live.insert(linkId);
            WireCache& cache = m_WireCache[linkId];
            if (cache.Route.empty() || makeKey(start, end, manual, cache.Area) != cache.Key)
            {
                cache.Route = manual.empty() ? RouteWire(start, end, boxes, settings) : RouteFromCoordinates(start, end, manual);
                Rect area{ start, start };
                for (const ImVec2& p : cache.Route)
                {
                    area.Min = ImVec2(std::min(area.Min.x, p.x), std::min(area.Min.y, p.y));
                    area.Max = ImVec2(std::max(area.Max.x, p.x), std::max(area.Max.y, p.y));
                }
                const float grow = settings.Margin + settings.Stub;
                cache.Area = { ImVec2(area.Min.x - grow, area.Min.y - grow), ImVec2(area.Max.x + grow, area.Max.y + grow) };
                cache.Key = makeKey(start, end, manual, cache.Area);
            }

            ImVec4 color(1.0f, 1.0f, 1.0f, 1.0f);
            if (const GraphNode* fromNode = FindNodeById(graph, link.FromNode))
                if (const NodeTypeDesc* fromType = TypeOf(graph, *fromNode))
                {
                    const NodePinList pins = ResolveNodePins(*fromType, fromNode->Properties);
                    if (link.FromPin < pins.Outputs.size() && !pins.Outputs[link.FromPin].Type.empty())
                        color = PinColor(pins.Outputs[link.FromPin].Type);
                }

            routes.push_back(cache.Route);
            nets.push_back(fromPin.Get());
            DrawnWire wire;
            wire.LinkId = linkId;
            wire.Color = ToU32(color);
            wire.FromNode = link.FromNode;
            wire.ToNode = link.ToNode;
            wire.FromPin = fromCentre;
            wire.ToPin = toCentre;
            m_Wires.push_back(std::move(wire));
        }
        for (auto it = m_WireCache.begin(); it != m_WireCache.end();)
            it = live.contains(it->first) ? std::next(it) : m_WireCache.erase(it);
        for (auto it = m_SelectedLinks.begin(); it != m_SelectedLinks.end();)
            it = live.contains(*it) ? std::next(it) : m_SelectedLinks.erase(it);

        // Wires of different pins running along one line are spread into lanes.
        SeparateOverlaps(routes, nets, kWireLaneSpacing);
        for (size_t i = 0; i < routes.size(); ++i)
        {
            Simplify(routes[i]);
            m_Wires[i].Points = std::move(routes[i]);
        }

        // Plain wires first, then the ones standing out (hovered, selected, or of a selected node) on top of them.
        auto drawWire = [&](const DrawnWire& wire, bool emphasized)
        {
            const bool selected = m_SelectedLinks.contains(wire.LinkId);
            const bool hovered = wire.LinkId == m_HoveredLink;
            const bool ofSelectedNode = ed::IsNodeSelected(ed::NodeId(wire.FromNode)) || ed::IsNodeSelected(ed::NodeId(wire.ToNode));
            if ((selected || hovered || ofSelectedNode) != emphasized || wire.Points.size() < 2)
                return;
            const float thickness = hovered ? 3.5f : (selected || ofSelectedNode ? 3.0f : 2.0f);
            ImDrawList* fromLayer = ed::GetNodeBackgroundDrawList(ed::NodeId(wire.FromNode));
            ImDrawList* toLayer = ed::GetNodeBackgroundDrawList(ed::NodeId(wire.ToNode));
            const ImVec2 start = wire.Points.front(), end = wire.Points.back();
            if (selected)
            {
                // An orange outline, like a selected node's.
                const ImU32 outline = IM_COL32(255, 176, 50, 255);
                StrokeRoundedPolyline(wireLayer, wire.Points, outline, thickness + 3.0f, kWireCornerRadius);
                fromLayer->AddLine(wire.FromPin, start, outline, thickness + 3.0f);
                toLayer->AddLine(end, wire.ToPin, outline, thickness + 3.0f);
            }
            StrokeRoundedPolyline(wireLayer, wire.Points, wire.Color, thickness, kWireCornerRadius);
            // From the pin to the node's border, on the node itself (the wire layer is under the node's background).
            fromLayer->AddLine(wire.FromPin, start, wire.Color, thickness);
            toLayer->AddLine(end, wire.ToPin, wire.Color, thickness);
        };
        for (const DrawnWire& wire : m_Wires)
            drawWire(wire, false);
        for (const DrawnWire& wire : m_Wires)
            drawWire(wire, true);
    }

    void ThedmdCanvasBackend::HandleWireInput(NodeGraph& graph, const std::function<void()>& onGraphEdited)
    {
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 mouseScreen = ImGui::GetMousePos();
        const ImVec2 mouse = ed::ScreenToCanvas(mouseScreen);
        const float zoom = std::max(ed::GetCurrentZoom(), 0.01f);
        const ImGuiID buttonId = ImGui::GetID("##WireHit");
        const bool buttonActive = ImGui::GetActiveID() == buttonId;
        const bool overCanvas = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(m_CanvasMin, m_CanvasMax, false);
        auto findLink = [&](uint64_t linkId) -> GraphLink*
        {
            uint32_t toNode = 0, toPin = 0;
            bool isOutput = false;
            DecodePinId(ed::PinId(linkId), toNode, toPin, isOutput);
            const auto it = std::find_if(graph.Links.begin(), graph.Links.end(), [&](const GraphLink& l) { return l.ToNode == toNode && l.ToPin == toPin; });
            return it != graph.Links.end() ? &*it : nullptr;
        };
        auto findWire = [&](uint64_t linkId) -> const DrawnWire*
        {
            const auto it = std::find_if(m_Wires.begin(), m_Wires.end(), [&](const DrawnWire& w) { return w.LinkId == linkId; });
            return it != m_Wires.end() ? &*it : nullptr;
        };
        auto breakLinks = [&](const std::function<bool(const GraphLink&)>& which)
        {
            const size_t before = graph.Links.size();
            graph.Links.erase(std::remove_if(graph.Links.begin(), graph.Links.end(), which), graph.Links.end());
            if (graph.Links.size() != before)
                onGraphEdited();
        };

        // Unreal's Alt+click on a pin: break every link of that pin.
        const ed::PinId hoveredPin = ed::GetHoveredPin();
        if (hoveredPin && io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && overCanvas)
        {
            uint32_t pinNode = 0, pinIndex = 0;
            bool isOutput = false;
            DecodePinId(hoveredPin, pinNode, pinIndex, isOutput);
            breakLinks([&](const GraphLink& l)
            {
                return isOutput ? (l.FromNode == pinNode && l.FromPin == pinIndex) : (l.ToNode == pinNode && l.ToPin == pinIndex);
            });
            return;
        }

        // The wire under the mouse (within a few pixels at any zoom), when no node or pin is.
        m_HoveredLink = 0;
        const ed::NodeId hoveredNode = ed::GetHoveredNode();
        if (buttonActive || (!ImGui::IsAnyItemActive() && overCanvas && !hoveredPin &&
                             (!hoveredNode || IsGraphWireLayer(hoveredNode))))
        {
            float nearest = kWireHitDistance / zoom;
            for (const DrawnWire& wire : m_Wires)
            {
                float distance = 0.0f;
                const int segment = WireRouting::NearestSegment(wire.Points, mouse, &distance);
                if (segment >= 0 && distance < nearest)
                {
                    nearest = distance;
                    m_HoveredLink = wire.LinkId;
                }
            }
        }
        if (buttonActive && m_PressedLink != 0)
            m_HoveredLink = m_PressedLink;

        // A click on a node or the background (not adding with Ctrl / Shift) drops the wire selection.
        const bool additive = io.KeyCtrl || io.KeyShift;
        if (!additive && !buttonActive && m_HoveredLink == 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && overCanvas)
            m_SelectedLinks.clear();

        // Pressing on a wire: an invisible button under the mouse takes the press, so the node editor starts no selection rectangle,
        // and stays active while the mouse is held (a leg being dragged). Only submitted then: otherwise it would also take the mouse
        // wheel's zoom away from the canvas wherever a wire is.
        const bool pressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        if (buttonActive || (m_HoveredLink != 0 && pressed))
        {
            ImGui::SetCursorScreenPos(ImVec2(mouseScreen.x - 4.0f, mouseScreen.y - 4.0f));
            ImGui::InvisibleButton("##WireHit", ImVec2(8.0f, 8.0f), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            if (ImGui::IsItemActivated())
            {
                m_PressedLink = m_HoveredLink;
                m_PressCanvasPos = mouse;
                m_PressButton = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? ImGuiMouseButton_Right : ImGuiMouseButton_Left;
                m_DraggingLeg = false;
                if (m_PressButton == ImGuiMouseButton_Right)
                {
                    m_ContextLink = m_PressedLink;
                    m_ContextScreenPos = mouseScreen;
                    m_OpenWireContext = true;
                    if (!m_SelectedLinks.contains(m_PressedLink))
                    {
                        ed::ClearSelection();
                        m_SelectedLinks = { m_PressedLink };
                    }
                    m_PressedLink = 0;
                }
                else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    m_DoubleClickedLink = m_PressedLink; // a reroute node there
                    m_PressedLink = 0;
                }
                else if (io.KeyAlt)
                {
                    const uint64_t linkId = m_PressedLink;
                    m_PressedLink = 0;
                    breakLinks([&](const GraphLink& l) { return MakeLinkId(l.ToNode, l.ToPin).Get() == linkId; });
                }
            }

            // Dragging a leg sideways shapes the route by hand (kept with the graph; right-click > Reset Route undoes it).
            if (ImGui::IsItemActive() && m_PressedLink != 0 && m_PressButton == ImGuiMouseButton_Left &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f))
            {
                GraphLink* link = findLink(m_PressedLink);
                const DrawnWire* wire = findWire(m_PressedLink);
                const WireRouting::RouteSettings settings = GraphRouteSettings();
                if (link && wire && !m_DraggingLeg)
                {
                    // The coordinates of the route as drawn (lanes included), so nothing jumps as the drag starts.
                    m_DragCoordinates = WireRouting::RouteCoordinates(wire->Points);
                    const int segment = WireRouting::NearestSegment(wire->Points, m_PressCanvasPos);
                    const ImVec2 start = wire->Points.front(), end = wire->Points.back();
                    if (wire->Points.size() == 2 && end.x - start.x > 2.0f * settings.Stub + 16.0f)
                    {
                        // A straight wire: dragging it up or down makes a step around the point it was grabbed at.
                        const float left = std::clamp(m_PressCanvasPos.x - 24.0f, start.x + settings.Stub, end.x - settings.Stub - 16.0f);
                        const float right = std::clamp(m_PressCanvasPos.x + 24.0f, left + 16.0f, end.x - settings.Stub);
                        m_DragCoordinates = { left, start.y, right };
                        m_DragIndex = 1;
                        m_DragVertical = false;
                        m_DraggingLeg = true;
                    }
                    else if (!m_DragCoordinates.empty() && segment >= 1 && segment + 2 < static_cast<int>(wire->Points.size()))
                    {
                        // Leg k (0 = the one out of the source pin) is coordinate k - 1: odd legs are vertical.
                        m_DragIndex = segment - 1;
                        m_DragVertical = segment % 2 == 1;
                        m_DraggingLeg = true;
                    }
                    else
                        m_PressedLink = 0; // the legs on the pins' rows stay where the pins are
                }
                if (link && wire && m_DraggingLeg && m_DragIndex >= 0 && m_DragIndex < static_cast<int>(m_DragCoordinates.size()))
                {
                    const ImVec2 start = wire->Points.front(), end = wire->Points.back();
                    float value = m_DragVertical ? mouse.x : mouse.y;
                    if (m_DragVertical && m_DragIndex == 0)
                        value = std::max(value, start.x + settings.Stub);
                    if (m_DragVertical && m_DragIndex + 1 == static_cast<int>(m_DragCoordinates.size()))
                        value = std::min(value, end.x - settings.Stub);
                    m_DragCoordinates[m_DragIndex] = value;
                    link->Route = m_DragCoordinates;
                }
            }

            if (ImGui::IsItemDeactivated())
            {
                if (m_DraggingLeg)
                    m_LayoutEdited = true;
                else if (m_PressedLink != 0 && m_PressButton == ImGuiMouseButton_Left)
                {
                    // A click: select the wire (Ctrl / Shift adds or removes it).
                    if (!additive)
                    {
                        ed::ClearSelection();
                        m_SelectedLinks = { m_PressedLink };
                    }
                    else if (!m_SelectedLinks.erase(m_PressedLink))
                        m_SelectedLinks.insert(m_PressedLink);
                }
                m_PressedLink = 0;
                m_DraggingLeg = false;
            }
        }

        // Delete removes the selected wires (the node editor deletes the selected nodes).
        if (!m_SelectedLinks.empty() && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            const std::unordered_set<uint64_t> doomed = std::move(m_SelectedLinks);
            m_SelectedLinks.clear();
            breakLinks([&](const GraphLink& l) { return doomed.contains(MakeLinkId(l.ToNode, l.ToPin).Get()); });
        }
    }

    void ThedmdCanvasBackend::DrawWireContextMenu(NodeGraph& graph, const std::function<void()>& onGraphEdited)
    {
        if (!ImGui::BeginPopup("WireContext"))
            return;
        uint32_t toNode = 0, toPin = 0;
        bool isOutput = false;
        DecodePinId(ed::PinId(m_ContextLink), toNode, toPin, isOutput);
        const auto link = std::find_if(graph.Links.begin(), graph.Links.end(), [&](const GraphLink& l) { return l.ToNode == toNode && l.ToPin == toPin; });
        if (link == graph.Links.end())
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        if (ImGui::MenuItem("Break Link"))
        {
            graph.Links.erase(link);
            m_SelectedLinks.erase(m_ContextLink);
            onGraphEdited();
        }
        else if (ImGui::MenuItem("Add Reroute Node"))
            InsertReroute(graph, m_ContextLink, m_ContextScreenPos, onGraphEdited);
        else
        {
            ImGui::BeginDisabled(link->Route.empty());
            if (ImGui::MenuItem("Reset Route"))
            {
                link->Route.clear();
                m_LayoutEdited = true;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(link->Route.empty() ? "Routed automatically (drag a leg of the wire to shape it by hand)"
                                                      : "Route this wire automatically again, around the nodes");
        }
        ImGui::EndPopup();
    }

    void ThedmdCanvasBackend::ConnectNewNode(NodeGraph& graph, uint32_t newNodeId)
    {
        uint32_t pinNode = 0, pinIndex = 0;
        bool fromOutput = false;
        DecodePinId(ed::PinId(m_NewNodeFromPin), pinNode, pinIndex, fromOutput);
        GraphNode* anchor = FindNodeById(graph, pinNode);
        GraphNode* created = FindNodeById(graph, newNodeId);
        const NodeTypeDesc* anchorType = anchor ? TypeOf(graph, *anchor) : nullptr;
        const NodeTypeDesc* createdType = created ? TypeOf(graph, *created) : nullptr;
        if (!anchorType || !createdType)
            return;
        const NodePinList anchorPins = ResolveNodePins(*anchorType, anchor->Properties);
        const std::vector<NodePinDesc>& anchorList = fromOutput ? anchorPins.Outputs : anchorPins.Inputs;
        if (pinIndex >= anchorList.size())
            return;
        const std::string wireType = anchorList[pinIndex].Type;

        // The new node's first pin of the wire's type on the other side (a reroute's one pin, typed after the wire).
        uint32_t createdPin = 0;
        std::string createdPinType = wireType;
        if (created->TypeName == kRerouteNodeType)
        {
            if (!wireType.empty())
                created->Properties[kRerouteTypeProperty] = wireType;
        }
        else
        {
            const NodePinList pins = ResolveNodePins(*createdType, created->Properties);
            const std::vector<NodePinDesc>& list = fromOutput ? pins.Inputs : pins.Outputs;
            const auto found = std::find_if(list.begin(), list.end(), [&](const NodePinDesc& pin) { return wireType.empty() || pin.Type == wireType; });
            if (found == list.end())
                return;
            createdPin = static_cast<uint32_t>(found - list.begin());
            createdPinType = found->Type;
        }
        // An untyped reroute the wire came from takes the type of what it now connects to.
        if (anchor->TypeName == kRerouteNodeType && wireType.empty() && !createdPinType.empty())
            anchor->Properties[kRerouteTypeProperty] = createdPinType;

        // An input takes one link: the new one replaces what was there.
        if (fromOutput)
        {
            graph.Links.erase(std::remove_if(graph.Links.begin(), graph.Links.end(),
                [&](const GraphLink& l) { return l.ToNode == newNodeId && l.ToPin == createdPin; }), graph.Links.end());
            graph.Links.push_back({ pinNode, pinIndex, newNodeId, createdPin });
        }
        else
        {
            graph.Links.erase(std::remove_if(graph.Links.begin(), graph.Links.end(),
                [&](const GraphLink& l) { return l.ToNode == pinNode && l.ToPin == pinIndex; }), graph.Links.end());
            graph.Links.push_back({ newNodeId, createdPin, pinNode, pinIndex });
        }
    }

    uint32_t ThedmdCanvasBackend::CreateNode(NodeGraph& graph, const NodeTypeDesc& type, const std::string* parameterName,
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
        return node.Id;
    }

    void ThedmdCanvasBackend::HandleParameterDrop(NodeGraph& graph, const std::vector<GraphParameter>& parameters, const std::function<void()>& onGraphEdited)
    {
        // Called suspended, so the current window is the canvas at real screen coordinates. There's no item to
        // hang a drop target on (the canvas is just a window), hence the custom rect over its whole inner area.
        ImGuiWindow* canvasWindow = ImGui::GetCurrentWindow();
        if (!ImGui::BeginDragDropTargetCustom(canvasWindow->InnerRect, ImGui::GetID("NodeGraphParameterDrop")))
            return;

        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kNodeGraphParameterPayload))
        {
            const std::string name(static_cast<const char*>(payload->Data));
            const auto declared = std::find_if(parameters.begin(), parameters.end(),
                [&](const GraphParameter& parameter) { return parameter.Name == name; });
            const NodeTypeDesc* reader = declared != parameters.end() ? ParameterReader(graph.Domain, *declared) : nullptr;
            if (reader)
                CreateNode(graph, *reader, &name, ImGui::GetMousePos(), onGraphEdited);
        }
        ImGui::EndDragDropTarget();
    }

    void ThedmdCanvasBackend::ShowCreateNodeMenu(NodeGraph& graph, const std::vector<GraphParameter>& parameters, const std::function<void()>& onGraphEdited)
    {
        // Opened by a wire dropped on empty canvas (Unreal's context-sensitive menu): the pin it came from and the type it carries.
        bool fromWire = false, fromOutput = false;
        std::string wireType;
        if (m_NewNodeFromPin != 0)
        {
            uint32_t pinNode = 0, pinIndex = 0;
            DecodePinId(ed::PinId(m_NewNodeFromPin), pinNode, pinIndex, fromOutput);
            const GraphNode* anchor = FindNodeById(graph, pinNode);
            const NodeTypeDesc* anchorType = anchor ? TypeOf(graph, *anchor) : nullptr;
            if (anchorType)
            {
                const NodePinList anchorPins = ResolveNodePins(*anchorType, anchor->Properties);
                const std::vector<NodePinDesc>& list = fromOutput ? anchorPins.Outputs : anchorPins.Inputs;
                if (pinIndex < list.size())
                {
                    fromWire = true;
                    wireType = list[pinIndex].Type;
                }
            }
        }

        // Creates a node of `type` where the menu was opened, optionally pointing it at a graph parameter; from a dropped wire, next to
        // where it was dropped (left of it for a wire from an input) and connected to it.
        auto createNode = [&](const NodeTypeDesc& type, const std::string* parameterName)
        {
            const uint32_t id = CreateNode(graph, type, parameterName, m_CreateNodeScreenPos, onGraphEdited);
            if (!fromWire)
                return;
            const ImVec2 drop = ed::ScreenToCanvas(m_CreateNodeScreenPos);
            ImVec2 position(drop.x + 10.0f, drop.y - 40.0f); // the first pin row about level with the drop point
            if (type.TypeName == kRerouteNodeType)
                position = ImVec2(drop.x - 13.0f, drop.y - 9.0f);
            else if (!fromOutput)
            {
                position.x = drop.x - 200.0f;
                m_AlignNewNodeId = id;
                m_AlignNewNodeRight = drop.x - 10.0f;
            }
            ed::SetNodePosition(ed::NodeId(id), position);
            if (GraphNode* created = FindNodeById(graph, id))
                created->EditorPosition = { position.x, position.y };
            ConnectNewNode(graph, id);
            m_NewNodeFromPin = 0;
            onGraphEdited();
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
            for (const GraphParameter& parameter : parameters)
                if (ParameterReader(graph.Domain, parameter) == &type)
                    entries.push_back({ "Variables", "Get " + parameter.Name, &type, &parameter.Name });
        }

        // From a dropped wire only the nodes with a pin that takes it (any reroute: it takes the wire's type).
        if (fromWire)
        {
            entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& entry)
            {
                if (entry.Type->TypeName == kRerouteNodeType)
                    return false;
                std::unordered_map<std::string, NodeGraphValue> properties(entry.Type->DefaultProperties.begin(), entry.Type->DefaultProperties.end());
                if (entry.Parameter)
                    properties[kParameterReferenceProperty] = *entry.Parameter;
                const NodePinList pins = ResolveNodePins(*entry.Type, properties);
                const std::vector<NodePinDesc>& list = fromOutput ? pins.Inputs : pins.Outputs;
                return std::none_of(list.begin(), list.end(), [&](const NodePinDesc& pin) { return wireType.empty() || pin.Type == wireType; });
            }), entries.end());
            ImGui::TextDisabled(fromOutput ? "Nodes taking %s" : "Nodes giving %s", wireType.empty() ? "any value" : wireType.c_str());
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

    bool ThedmdCanvasBackend::ConsumeLayoutEdited()
    {
        const bool edited = m_LayoutEdited;
        m_LayoutEdited = false;
        return edited;
    }

    void ThedmdCanvasBackend::ResetView()
    {
        // Another graph on the canvas (node ids are per graph): nothing of the last one's wires or positions carries over.
        m_WireCache.clear();
        m_Wires.clear();
        m_PreviousNodePositions.clear();
        m_SelectedLinks.clear();
        m_HoveredLink = 0;
        m_PressedLink = 0;
        m_DraggingLeg = false;
        m_NewNodeFromPin = 0;
        m_AlignNewNodeId = 0;
        m_SeededPositions.clear();
        m_SeededStatePositions.clear();
        m_FitViewOnNextFrame = true;
        m_FitStatesOnNextFrame = true;
    }

    namespace
    {
        // A state's pins: id = state id * 2 (+1 for the output side), so an id decodes back to (state, side).
        // Nodes and pins share one numeric id space in the node editor: states use their own ids, pins 2^32 + .., and a transition's
        // icon (a node of its own, see DrawStateMachine) 2^34 + its index.
        constexpr uint64_t kStatePinBase = 1ull << 32;
        // The layer the transition wires are drawn in: an empty node kept under the first state and below every state (z), whose
        // background draw list sits under all the states (the node editor offers per-node layers only).
        constexpr uint64_t kWireLayerNode = 1ull << 34;
        bool IsWireLayer(ed::NodeId node) { return node.Get() == kWireLayerNode; }

        ImVec2 Add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

        struct Box
        {
            ImVec2 Min, Max;
        };

        // Does the axis-aligned segment a-b pass through the box (grown by `margin`)?
        bool SegmentHitsBox(ImVec2 a, ImVec2 b, const Box& box, float margin)
        {
            const float minX = std::min(a.x, b.x), maxX = std::max(a.x, b.x);
            const float minY = std::min(a.y, b.y), maxY = std::max(a.y, b.y);
            return maxX > box.Min.x - margin && minX < box.Max.x + margin && maxY > box.Min.y - margin && minY < box.Max.y + margin;
        }

        // How much of the axis-aligned segment a-b runs along (on top of, within 3 px) any segment of the other wires.
        float OverlapLength(ImVec2 a, ImVec2 b, const std::vector<std::vector<ImVec2>>& others)
        {
            const bool horizontal = std::abs(a.y - b.y) < 0.01f;
            float overlap = 0.0f;
            for (const std::vector<ImVec2>& wire : others)
                for (size_t i = 0; i + 1 < wire.size(); ++i)
                {
                    const ImVec2 c = wire[i], d = wire[i + 1];
                    if (horizontal != (std::abs(c.y - d.y) < 0.01f))
                        continue;
                    if (horizontal ? std::abs(c.y - a.y) > 3.0f : std::abs(c.x - a.x) > 3.0f)
                        continue;
                    const float lo = horizontal ? std::max(std::min(a.x, b.x), std::min(c.x, d.x)) : std::max(std::min(a.y, b.y), std::min(c.y, d.y));
                    const float hi = horizontal ? std::min(std::max(a.x, b.x), std::max(c.x, d.x)) : std::min(std::max(a.y, b.y), std::max(c.y, d.y));
                    overlap += std::max(0.0f, hi - lo);
                }
            return overlap;
        }

        // An orthogonal route from an output pin (leaving to the right) to an input pin (entering from the left): out by `stub`,
        // along a column, across a row, along another column, in by `stub`. Every combination of candidate columns and rows (the
        // stubs, the midpoint, and two lanes beside, above and below each box) is scored: crossing a box costs far more than any
        // length, running on top of an already routed wire (`others`) costs its length several times over, each corner a little.
        // The cheapest is kept. The source box is ignored for the first leg and the target box for the last one.
        std::vector<ImVec2> RouteWire(ImVec2 start, ImVec2 end, const std::vector<Box>& boxes, int sourceBox, int targetBox, float stub,
                                      float laneShift, const std::vector<std::vector<ImVec2>>& others)
        {
            constexpr float kBoxCost = 100000.0f;
            constexpr float kOverlapCost = 6.0f;
            constexpr float kCornerCost = 40.0f;
            const float clearance = 6.0f;
            std::vector<float> columnsOut{ start.x + stub }, columnsIn{ end.x - stub }, rows{ start.y, end.y };
            const float middle = 0.5f * (start.x + end.x) + laneShift;
            if (middle >= start.x + stub && middle <= end.x - stub)
            {
                columnsOut.push_back(middle);
                columnsIn.push_back(middle);
            }
            for (const Box& box : boxes)
                for (float lane : { stub, 1.75f * stub })
                {
                    if (box.Max.x + lane > start.x + stub)
                        columnsOut.push_back(box.Max.x + lane);
                    if (box.Min.x - lane < end.x - stub)
                        columnsIn.push_back(box.Min.x - lane);
                    rows.push_back(box.Min.y - lane);
                    rows.push_back(box.Max.y + lane);
                }

            // Points where the direction changes. A route that doubles back on itself (out to the right, then back left along the
            // same row) is refused: it would retrace its own line.
            auto build = [&](float x1, float y, float x2, std::vector<ImVec2>& points)
            {
                const ImVec2 raw[] = { start, ImVec2(x1, start.y), ImVec2(x1, y), ImVec2(x2, y), ImVec2(x2, end.y), end };
                points.clear();
                for (const ImVec2& p : raw)
                {
                    if (!points.empty() && std::abs(points.back().x - p.x) < 0.01f && std::abs(points.back().y - p.y) < 0.01f)
                        continue;
                    if (points.size() >= 2)
                    {
                        const ImVec2 a = points[points.size() - 2], b = points.back();
                        const bool sameColumn = std::abs(a.x - b.x) < 0.01f && std::abs(b.x - p.x) < 0.01f;
                        const bool sameRow = std::abs(a.y - b.y) < 0.01f && std::abs(b.y - p.y) < 0.01f;
                        if (sameColumn || sameRow)
                        {
                            const float ab = sameRow ? b.x - a.x : b.y - a.y;
                            const float bp = sameRow ? p.x - b.x : p.y - b.y;
                            if (ab * bp < 0.0f)
                                return false; // reverses
                            points.pop_back(); // b is in a straight run
                        }
                    }
                    points.push_back(p);
                }
                return points.size() >= 2;
            };
            auto score = [&](const std::vector<ImVec2>& points, bool& clear)
            {
                float cost = kCornerCost * static_cast<float>(points.size() - 2);
                clear = true;
                for (size_t i = 0; i + 1 < points.size(); ++i)
                {
                    const ImVec2 a = points[i], b = points[i + 1];
                    cost += std::abs(b.x - a.x) + std::abs(b.y - a.y);
                    for (int k = 0; k < static_cast<int>(boxes.size()); ++k)
                    {
                        if ((i == 0 && k == sourceBox) || (i + 2 == points.size() && k == targetBox))
                            continue;
                        if (SegmentHitsBox(a, b, boxes[k], clearance))
                        {
                            cost += kBoxCost;
                            clear = false;
                        }
                    }
                    // The short legs at the pins may share their pin's line with wires of the same pin; the rest should not.
                    if (i != 0 && i + 2 != points.size())
                    {
                        const float overlap = OverlapLength(a, b, others);
                        cost += kOverlapCost * overlap;
                        clear &= overlap <= 0.0f;
                    }
                }
                return cost;
            };

            std::vector<ImVec2> points, best;
            bool clear = false;
            // The usual case first: a forward wire with two corners at the midpoint, when it crosses no box and no other wire.
            if (middle >= start.x + stub && middle <= end.x - stub && build(middle, end.y, middle, points))
            {
                score(points, clear);
                if (clear)
                    return points;
            }

            float bestCost = FLT_MAX;
            for (float x1 : columnsOut)
                for (float x2 : columnsIn)
                    for (float y : rows)
                    {
                        if (!build(x1, y, x2, points))
                            continue;
                        const float cost = score(points, clear);
                        if (cost < bestCost)
                        {
                            bestCost = cost;
                            best = points;
                        }
                    }
            return best;
        }

        using WireRouting::DistanceToSegment;
        using WireRouting::StrokeRoundedPolyline;

        ed::PinId StateInputPin(uint32_t stateId) { return ed::PinId(kStatePinBase + static_cast<uint64_t>(stateId) * 2); }
        ed::PinId StateOutputPin(uint32_t stateId) { return ed::PinId(kStatePinBase + static_cast<uint64_t>(stateId) * 2 + 1); }
        uint32_t PinState(ed::PinId pin) { return static_cast<uint32_t>((pin.Get() - kStatePinBase) >> 1); }
        bool PinIsOutput(ed::PinId pin) { return ((pin.Get() - kStatePinBase) & 1) != 0; }

        NodeGraph MakeEmptyStateGraph(const std::string& domain)
        {
            if (domain == kAnimationGraphDomain)
                return CreateEmptyAnimationGraph();
            NodeGraph graph;
            graph.Domain = domain;
            return graph;
        }
    }

    void ThedmdCanvasBackend::DrawTransitionWires(const GraphNode& machine, const std::unordered_map<uint32_t, StatePins>& statePins)
    {
        std::vector<Box> boxes;
        std::unordered_map<uint32_t, int> boxOf;
        for (const NodeSubGraph& state : machine.SubGraphs)
        {
            const ImVec2 position = ed::GetNodePosition(ed::NodeId(state.Id));
            const ImVec2 size = ed::GetNodeSize(ed::NodeId(state.Id));
            if (size.x <= 0.0f || size.y <= 0.0f || !statePins.contains(state.Id))
                continue;
            boxOf[state.Id] = static_cast<int>(boxes.size());
            boxes.push_back({ position, Add(position, size) });
        }

        // The layer node, tucked under the first state (so it is never clicked) and below every state.
        if (!machine.SubGraphs.empty())
        {
            const ed::NodeId layer(kWireLayerNode);
            const ImVec2 anchor = Add(ed::GetNodePosition(ed::NodeId(machine.SubGraphs.front().Id)), ImVec2(4.0f, 4.0f));
            ed::SetNodePosition(layer, anchor);
            ed::SetNodeZPosition(layer, -1.0e6f);
            ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ed::BeginNode(layer);
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
            ed::EndNode();
            ed::PopStyleColor(2);
            ed::PopStyleVar();
        }
        ImDrawList* wireLayer = machine.SubGraphs.empty() ? nullptr : ed::GetNodeBackgroundDrawList(ed::NodeId(kWireLayerNode));

        constexpr float kStub = 20.0f;      // straight out of / into a pin before the first / after the last corner
        constexpr float kLaneSpacing = 9.0f; // between wires joining the same two states
        constexpr float kCornerRadius = 8.0f;
        constexpr float kArrowLength = 9.0f;
        constexpr float kArrowHalfWidth = 4.5f;
        // Every wire's ends, then the routes: searched again only when a box, a pin or the transitions changed (moving a box
        // re-routes live), all together and in order, each avoiding the wires routed before it.
        struct WireEnds
        {
            ImVec2 Start, End;
            int SourceBox = -1, TargetBox = -1, Lane = 0;
        };
        std::vector<WireEnds> ends(machine.Transitions.size());
        std::vector<float> routeKey;
        routeKey.reserve(boxes.size() * 4 + machine.Transitions.size() * 7);
        for (const Box& box : boxes)
            routeKey.insert(routeKey.end(), { box.Min.x, box.Min.y, box.Max.x, box.Max.y });
        for (size_t i = 0; i < machine.Transitions.size(); ++i)
        {
            const NodeTransition& transition = machine.Transitions[i];
            const auto fromBox = boxOf.find(transition.FromState);
            const auto toBox = boxOf.find(transition.ToState);
            if (fromBox != boxOf.end() && toBox != boxOf.end())
            {
                const StatePins& from = statePins.at(transition.FromState);
                const StatePins& to = statePins.at(transition.ToState);
                WireEnds& wire = ends[i];
                wire.Start = ImVec2(from.OutputMax.x, 0.5f * (from.OutputMin.y + from.OutputMax.y));
                wire.End = ImVec2(to.InputMin.x, 0.5f * (to.InputMin.y + to.InputMax.y));
                wire.SourceBox = fromBox->second;
                wire.TargetBox = toBox->second;
                // Its lane among the transitions joining the same two states in the same direction.
                for (size_t j = 0; j < i; ++j)
                    if (machine.Transitions[j].FromState == transition.FromState && machine.Transitions[j].ToState == transition.ToState)
                        ++wire.Lane;
            }
            routeKey.insert(routeKey.end(), { ends[i].Start.x, ends[i].Start.y, ends[i].End.x, ends[i].End.y,
                                              static_cast<float>(ends[i].SourceBox), static_cast<float>(ends[i].TargetBox),
                                              static_cast<float>(ends[i].Lane) });
        }
        if (routeKey != m_RouteKey)
        {
            m_RouteKey = std::move(routeKey);
            m_TransitionWires.assign(machine.Transitions.size(), {});
            for (size_t i = 0; i < ends.size(); ++i)
            {
                if (ends[i].SourceBox < 0)
                    continue;
                const float lane = static_cast<float>(ends[i].Lane);
                m_TransitionWires[i] = RouteWire(ends[i].Start, ends[i].End, boxes, ends[i].SourceBox, ends[i].TargetBox,
                                                 kStub + kLaneSpacing * lane, kLaneSpacing * lane, m_TransitionWires);
            }
        }

        for (size_t i = 0; i < machine.Transitions.size(); ++i)
        {
            const NodeTransition& transition = machine.Transitions[i];
            const std::vector<ImVec2>& points = m_TransitionWires[i];
            if (points.size() < 2)
                continue;
            const ImVec2 start = ends[i].Start;
            const ImVec2 end = ends[i].End;
            const auto fromBox = boxOf.find(transition.FromState);
            const auto toBox = boxOf.find(transition.ToState);

            const bool selected = static_cast<int>(i) == m_SelectedTransition;
            const bool hovered = static_cast<int>(i) == m_HoveredTransition;
            const ImU32 color = selected ? IM_COL32(255, 176, 50, 255) : (hovered ? IM_COL32(50, 176, 255, 255) : IM_COL32(200, 200, 200, 255));
            const float thickness = selected || hovered ? 3.0f : 2.0f;

            // The wire, ending where the arrowhead starts; the arrowhead points into the input pin.
            std::vector<ImVec2> wire = points;
            wire.back().x -= kArrowLength;
            if (wireLayer)
                StrokeRoundedPolyline(wireLayer, wire, color, thickness, kCornerRadius);

            // Inside the two boxes (under them in the wire layer) the first and last legs are drawn again on the boxes themselves.
            const Box& source = boxes[fromBox->second];
            const Box& target = boxes[toBox->second];
            if (ImDrawList* sourceLayer = ed::GetNodeBackgroundDrawList(ed::NodeId(transition.FromState)))
                sourceLayer->AddLine(start, ImVec2(std::min(points[1].x, source.Max.x), start.y), color, thickness);
            if (ImDrawList* targetLayer = ed::GetNodeBackgroundDrawList(ed::NodeId(transition.ToState)))
            {
                const ImVec2 arrowBase(end.x - kArrowLength, end.y);
                const float legStart = std::max(points[points.size() - 2].x, target.Min.x);
                if (legStart < arrowBase.x)
                    targetLayer->AddLine(ImVec2(legStart, end.y), arrowBase, color, thickness);
                targetLayer->AddTriangleFilled(end, ImVec2(arrowBase.x, end.y - kArrowHalfWidth), ImVec2(arrowBase.x, end.y + kArrowHalfWidth), color);
            }
        }
    }

    void ThedmdCanvasBackend::DrawStateMachine(GraphNode& machine, const NodeGraph& rootGraph, const std::function<void()>& onGraphEdited,
                                               uint32_t& outOpenState, StateMachineSelection& selection)
    {
        constexpr ImVec2 kPinIconSize(16.0f, 16.0f);
        const uint32_t entryState = static_cast<uint32_t>([&]
        {
            auto entry = machine.Properties.find("EntryState");
            const int32_t* value = entry != machine.Properties.end() ? std::get_if<int32_t>(&entry->second) : nullptr;
            return value ? *value : 0;
        }());

        ed::SetCurrentEditor(m_StateContext);
        ed::Begin("States");

        std::unordered_map<uint32_t, StatePins> statePins;

        for (NodeSubGraph& state : machine.SubGraphs)
        {
            const ed::NodeId nodeId(state.Id);
            if (m_SeededStatePositions.insert(state.Id).second)
                ed::SetNodePosition(nodeId, ImVec2(state.EditorPosition.x, state.EditorPosition.y));

            ed::BeginNode(nodeId);
            ImGui::PushID("state");
            ImGui::PushID(static_cast<int>(state.Id));

            const bool isEntry = state.Id == entryState;
            ImGui::TextUnformatted(state.Name.empty() ? "State" : state.Name.c_str());
            const ImVec2 headerMin = ImGui::GetItemRectMin();
            const ImVec2 headerMax = ImGui::GetItemRectMax();
            ImGui::Dummy(ImVec2(0.0f, 4.0f));

            StatePins& pins = statePins[state.Id];
            ed::BeginPin(StateInputPin(state.Id), ed::PinKind::Input);
            DrawPinIcon(kPinIconSize, PinShape::Circle, false, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
            pins.InputMin = ImGui::GetItemRectMin();
            pins.InputMax = ImGui::GetItemRectMax();
            ed::EndPin();
            ImGui::SameLine();
            ImGui::TextDisabled(isEntry ? "entry" : "     ");
            // The output pin at the right edge (under a long name too), so its wire leaves the box where the pin is.
            ImGui::SameLine();
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(std::max(cursor.x, headerMax.x - kPinIconSize.x), cursor.y));
            ed::BeginPin(StateOutputPin(state.Id), ed::PinKind::Output);
            DrawPinIcon(kPinIconSize, PinShape::Circle, false, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
            pins.OutputMin = ImGui::GetItemRectMin();
            pins.OutputMax = ImGui::GetItemRectMax();
            ed::EndPin();

            ImGui::PopID();
            ImGui::PopID();
            ed::EndNode();

            if (ImGui::IsItemVisible())
            {
                ImDrawList* backgroundDrawList = ed::GetNodeBackgroundDrawList(nodeId);
                const float pad = 8.0f;
                backgroundDrawList->AddRectFilled(
                    ImVec2(headerMin.x - pad, headerMin.y - pad), ImVec2(headerMax.x + pad, headerMax.y + pad),
                    isEntry ? IM_COL32(55, 135, 90, 255) : IM_COL32(150, 60, 60, 255), ed::GetStyle().NodeRounding, ImDrawFlags_RoundCornersTop);
            }

            const ImVec2 position = ed::GetNodePosition(nodeId);
            state.EditorPosition = { position.x, position.y };
        }

        // Transitions: wires from a state's output pin (right) into the other state's input pin (left), routed with right-angle
        // corners around the other state boxes (rounded a little), like a Blueprint editor's orthogonal wire routing -- a wire
        // going back (JumpLand -> Locomotion) goes over or under the boxes instead of curling through them. Several between the
        // same two states run in their own lanes. Click a wire to select it (Delete, or right-click > Delete Transition, removes
        // it). Wires lie under the states (DrawTransitionWires); the bits inside the two boxes, from the box edge to the pin, are
        // drawn again on those boxes so a wire visibly ends in its pin.
        DrawTransitionWires(machine, statePins);

        // Connecting an output pin to another state's input makes a transition (rules are edited in the panel below).
        if (ed::BeginCreate())
        {
            ed::PinId startPin, endPin;
            if (ed::QueryNewLink(&startPin, &endPin))
            {
                uint32_t fromState = PinState(startPin);
                uint32_t toState = PinState(endPin);
                if (!PinIsOutput(startPin))
                {
                    std::swap(fromState, toState);
                    std::swap(startPin, endPin);
                }

                if (PinIsOutput(startPin) == PinIsOutput(endPin) || fromState == toState)
                {
                    ed::RejectNewItem();
                }
                else if (ed::AcceptNewItem())
                {
                    NodeTransition transition;
                    transition.FromState = fromState;
                    transition.ToState = toState;
                    // A transition with no rule fires the moment its state is active: start with the first parameter.
                    if (!rootGraph.Parameters.empty())
                        transition.Rules.push_back({ rootGraph.Parameters.front().Name, TransitionCompare::Greater, 0.0f });
                    machine.Transitions.push_back(std::move(transition));
                    onGraphEdited();
                }
            }
        }
        ed::EndCreate();

        if (ed::BeginDelete())
        {
            ed::NodeId nodeId;
            std::vector<uint32_t> deletedStates;
            while (ed::QueryDeletedNode(&nodeId))
            {
                if (IsWireLayer(nodeId))
                {
                    ed::RejectDeletedItem();
                    continue;
                }
                if (ed::AcceptDeletedItem())
                    deletedStates.push_back(static_cast<uint32_t>(nodeId.Get()));
            }

            if (!deletedStates.empty())
            {
                m_SelectedTransition = -1; // indices shift
                for (uint32_t stateId : deletedStates)
                {
                    machine.SubGraphs.erase(std::remove_if(machine.SubGraphs.begin(), machine.SubGraphs.end(),
                        [&](const NodeSubGraph& s) { return s.Id == stateId; }), machine.SubGraphs.end());
                    machine.Transitions.erase(std::remove_if(machine.Transitions.begin(), machine.Transitions.end(),
                        [&](const NodeTransition& t) { return t.FromState == stateId || t.ToState == stateId; }), machine.Transitions.end());
                    m_SeededStatePositions.erase(stateId);
                    if (stateId == entryState && !machine.SubGraphs.empty())
                        machine.Properties["EntryState"] = static_cast<int32_t>(machine.SubGraphs.front().Id);
                }
                onGraphEdited();
            }
        }
        ed::EndDelete();

        // Selection and double-click, for the panel.
        selection = StateMachineSelection{};
        std::array<ed::NodeId, 2> selectedNodes;
        const int selectedCount = ed::GetSelectedNodes(selectedNodes.data(), static_cast<int>(selectedNodes.size()));
        for (int i = 0; i < selectedCount; ++i)
        {
            if (!IsWireLayer(selectedNodes[i]))
            {
                selection.State = static_cast<uint32_t>(selectedNodes[i].Get());
                m_SelectedTransition = -1; // a state was picked
                break;
            }
        }
        if (m_SelectedTransition >= static_cast<int>(machine.Transitions.size()))
            m_SelectedTransition = -1;
        selection.Transition = m_SelectedTransition;

        const ed::NodeId doubleClicked = ed::GetDoubleClickedNode();
        if (doubleClicked && !IsWireLayer(doubleClicked))
            outOpenState = static_cast<uint32_t>(doubleClicked.Get());

        if (m_FitStatesOnNextFrame && !machine.SubGraphs.empty())
        {
            ed::NavigateToContent(0.0f);
            m_FitStatesOnNextFrame = false;
        }

        ed::Suspend();

        // The wire under the mouse (within a few pixels at any zoom), when no state or pin is: hovered, clicked to select, Delete
        // or right-click to remove.
        m_HoveredTransition = -1;
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ed::GetHoveredPin() &&
            (!ed::GetHoveredNode() || IsWireLayer(ed::GetHoveredNode())))
        {
            const ImVec2 mouse = ed::ScreenToCanvas(ImGui::GetMousePos());
            const float reach = 6.0f / std::max(ed::GetCurrentZoom(), 0.01f);
            float nearest = reach;
            for (size_t i = 0; i < m_TransitionWires.size(); ++i)
                for (size_t k = 0; k + 1 < m_TransitionWires[i].size(); ++k)
                {
                    const float distance = DistanceToSegment(mouse, m_TransitionWires[i][k], m_TransitionWires[i][k + 1]);
                    if (distance < nearest)
                    {
                        nearest = distance;
                        m_HoveredTransition = static_cast<int>(i);
                    }
                }
        }
        if (ed::IsBackgroundClicked())
        {
            m_SelectedTransition = m_HoveredTransition;
            selection.Transition = m_SelectedTransition;
        }
        auto deleteTransition = [&](int index)
        {
            if (index < 0 || index >= static_cast<int>(machine.Transitions.size()))
                return;
            machine.Transitions.erase(machine.Transitions.begin() + index);
            m_SelectedTransition = -1;
            m_HoveredTransition = -1;
            selection.Transition = -1;
            onGraphEdited();
        };
        if (m_SelectedTransition >= 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            deleteTransition(m_SelectedTransition);

        ed::NodeId contextNode;
        if (ed::ShowNodeContextMenu(&contextNode) && !IsWireLayer(contextNode))
        {
            m_ContextStateId = static_cast<uint32_t>(contextNode.Get());
            ImGui::OpenPopup("StateContext");
        }
        if (ed::ShowBackgroundContextMenu())
        {
            if (m_HoveredTransition >= 0)
            {
                m_ContextTransition = m_HoveredTransition;
                m_SelectedTransition = m_HoveredTransition;
                selection.Transition = m_SelectedTransition;
                ImGui::OpenPopup("TransitionContext");
            }
            else
            {
                m_AddStateScreenPos = ImGui::GetMousePos();
                ImGui::OpenPopup("AddState");
            }
        }
        if (ImGui::BeginPopup("TransitionContext"))
        {
            if (ImGui::MenuItem("Delete Transition"))
                deleteTransition(m_ContextTransition);
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("StateContext"))
        {
            if (ImGui::MenuItem("Set as Entry State"))
            {
                machine.Properties["EntryState"] = static_cast<int32_t>(m_ContextStateId);
                onGraphEdited();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("AddState"))
        {
            if (ImGui::MenuItem("Add State"))
            {
                NodeSubGraph state;
                for (const NodeSubGraph& existing : machine.SubGraphs)
                    state.Id = std::max(state.Id, existing.Id);
                state.Id += 1;
                state.Name = "State " + std::to_string(state.Id);
                state.Graph = MakeEmptyStateGraph(rootGraph.Domain);
                const ImVec2 canvasPos = ed::ScreenToCanvas(m_AddStateScreenPos);
                state.EditorPosition = { canvasPos.x, canvasPos.y };
                if (machine.SubGraphs.empty())
                    machine.Properties["EntryState"] = static_cast<int32_t>(state.Id);
                machine.SubGraphs.push_back(std::move(state));
                onGraphEdited();
            }
            ImGui::EndPopup();
        }

        ed::Resume();

        ed::End();
        ed::SetCurrentEditor(nullptr);
    }
}
