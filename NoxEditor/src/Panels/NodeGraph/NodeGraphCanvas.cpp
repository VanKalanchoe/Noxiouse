#include "NodeGraphCanvas.h"

#include <algorithm>
#include <cmath>
#include <format>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include "NoxCore/NodeGraph/GraphEvalContext.h"
#include "NoxCore/Renderer/Font.h"
#include <NoxUI/Core/UIContext.h>
#include <NoxUI/Layout/UILayout.h>
#include <NoxUI/Rendering/UIDrawList.h>
#include <NoxUI/Rendering/UIIcons.h>
#include <NoxUI/Widgets/UIButton.h>
#include <NoxUI/Widgets/UILabel.h>
#include <NoxUI/Widgets/UIMenu.h>
#include <NoxUI/Widgets/UIPopup.h>
#include <NoxUI/Widgets/UIScrollView.h>
#include <NoxUI/Widgets/UITextInput.h>

namespace Nox
{
    namespace
    {
        using NoxUI::UIColor;
        using NoxUI::UIVec2;

        constexpr float kHeader = 28.0f;   // node header height (graph space)
        constexpr float kRow = 24.0f;      // pin row height
        constexpr float kBottom = 8.0f;
        constexpr float kPinRadius = 5.5f;
        constexpr float kMinWidth = 150.0f;
        constexpr float kTitleFont = 13.5f;
        constexpr float kPinFont = 12.5f;

        // Pin colours by type (Unreal's convention: the type tells the socket), the same as NoxEditor's canvas.
        UIColor PinColor(const std::string& type)
        {
            if (type == "Pose")    return NoxUI::ThemeColor("PinPose", { 0.78f, 0.55f, 0.95f, 1.0f });
            if (type == "Float")   return NoxUI::ThemeColor("PinFloat", { 0.55f, 0.85f, 0.55f, 1.0f });
            if (type == "Bool")    return NoxUI::ThemeColor("PinBool", { 0.85f, 0.45f, 0.45f, 1.0f });
            if (type == "Int")     return NoxUI::ThemeColor("PinInt", { 0.40f, 0.85f, 0.85f, 1.0f });
            if (type == "Vector2") return NoxUI::ThemeColor("PinVector2", { 0.95f, 0.85f, 0.35f, 1.0f });
            if (type == "Audio")   return NoxUI::ThemeColor("PinAudio", { 0.40f, 0.60f, 1.00f, 1.0f });
            if (type == "Trigger") return NoxUI::ThemeColor("PinTrigger", { 0.95f, 0.95f, 0.95f, 1.0f });
            if (type == "Value")   return NoxUI::ThemeColor("PinValue", { 0.45f, 0.82f, 0.74f, 1.0f }); // a material graph's values
            return NoxUI::ThemeColor("PinDefault", { 0.75f, 0.75f, 0.78f, 1.0f });
        }

        // A node's header by its category; categories without a colour of their own get a steady one from their name.
        UIColor CategoryColor(const std::string& category)
        {
            auto rgb = [](int r, int g, int b) { return UIColor{ r / 255.0f, g / 255.0f, b / 255.0f, 1.0f }; };
            if (category == "Sources")        return rgb(60, 95, 165);
            if (category == "Blending")       return rgb(170, 110, 40);
            if (category == "Sinks")          return rgb(55, 135, 90);
            if (category == "Inputs")         return rgb(130, 75, 150);
            if (category == "State Machines") return rgb(150, 60, 60);
            if (category == "Triggers")       return rgb(120, 120, 128);
            if (category == "Math")           return rgb(60, 130, 70);
            if (category == "Envelopes")      return rgb(150, 120, 40);
            if (category == "Mixing")         return rgb(40, 120, 150);
            if (category == "Filters")        return rgb(120, 70, 140);
            if (category == "Constants")      return rgb(70, 110, 70);
            if (category == "Parameters")     return rgb(130, 75, 150);
            if (category == "Textures")       return rgb(150, 80, 60);
            if (category == "Output")         return rgb(55, 135, 90);
            const size_t hash = std::hash<std::string>{}(category);
            const float hue = static_cast<float>(hash % 360) / 360.0f;
            const glm::vec3 k = glm::abs(glm::fract(glm::vec3(hue) + glm::vec3(1.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f);
            const glm::vec3 c = glm::mix(glm::vec3(1.0f), glm::clamp(k - 1.0f, 0.0f, 1.0f), 0.55f) * 0.55f;
            return { c.r, c.g, c.b, 1.0f };
        }

        bool ReadsParameter(const NodeTypeDesc& type)
        {
            return std::any_of(type.DefaultProperties.begin(), type.DefaultProperties.end(),
                               [](const auto& property) { return property.first == kParameterReferenceProperty; });
        }

        // A short text of a value, shown beside an input pin that has no wire.
        std::string ShortValue(const NodeGraphValue& value)
        {
            return std::visit([](const auto& v) -> std::string
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
                else if constexpr (std::is_same_v<T, int32_t>) return std::to_string(v);
                else if constexpr (std::is_same_v<T, float>)
                {
                    std::string text = std::format("{:.3f}", v);
                    while (text.size() > 1 && text.back() == '0' && text[text.size() - 2] != '.')
                        text.pop_back();
                    return text;
                }
                else if constexpr (std::is_same_v<T, glm::vec2>) return std::format("{:.2g}, {:.2g}", v.x, v.y);
                else if constexpr (std::is_same_v<T, glm::vec4>) return std::format("{:.2g}, {:.2g}, {:.2g}", v.x, v.y, v.z);
                else if constexpr (std::is_same_v<T, std::string>) return v.size() > 14 ? v.substr(0, 13) + "..." : v;
                else return std::string();
            }, value);
        }

        // The node type reading a parameter of its type in this domain (Get Float, Get Trigger), else one that reads any.
        const NodeTypeDesc* FindParameterReader(const std::string& domain, const GraphParameter& parameter)
        {
            const NodeTypeDesc* reader = nullptr;
            for (const NodeTypeDesc& type : NodeTypeRegistry::All())
            {
                if (type.Domain != domain || !ReadsParameter(type))
                    continue;
                if (type.ParameterType == GraphParameterTypeName(parameter))
                    return &type;
                if (type.ParameterType.empty() && !reader)
                    reader = &type;
            }
            return reader;
        }

        bool Ctrl() { return (NoxUI::UIContext::GetCurrentModifiers() & NoxUI::UIMod::Ctrl) != 0; }
        bool Shift() { return (NoxUI::UIContext::GetCurrentModifiers() & NoxUI::UIMod::Shift) != 0; }
        bool Alt() { return (NoxUI::UIContext::GetCurrentModifiers() & NoxUI::UIMod::Alt) != 0; }

        UIVec2 BezierPoint(const UIVec2& p0, const UIVec2& p1, const UIVec2& p2, const UIVec2& p3, float t)
        {
            const float u = 1.0f - t;
            return u * u * u * p0 + 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t * p3;
        }

        float SegmentDistance(const UIVec2& p, const UIVec2& a, const UIVec2& b)
        {
            const UIVec2 ab = b - a;
            const float t = glm::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-6f), 0.0f, 1.0f);
            return glm::length(p - (a + ab * t));
        }
    }

    NodeGraphCanvas::NodeGraphCanvas()
    {
        HitTestMode = NoxUI::UIHitTestMode::Self;
        Focusable = true;
        ClipChildren = true;
        WidthMode = NoxUI::UISizeMode::Fill;
        HeightMode = NoxUI::UISizeMode::Fill;
        AcceptedDropType = kParameterDragType;
        OnDrop = [this](const NoxUI::UIDragPayload& payload)
        {
            NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
            return ctx && AddParameterReader(payload.Text, ctx->GetLastMousePos()) != 0;
        };
    }

    uint32_t NodeGraphCanvas::AddParameterReader(const std::string& parameterName, const UIVec2& screen)
    {
        if (!m_Graph || !Parameters)
            return 0;
        const auto parameter = std::find_if(Parameters->begin(), Parameters->end(), [&](const GraphParameter& p) { return p.Name == parameterName; });
        if (parameter == Parameters->end())
            return 0;
        const NodeTypeDesc* reader = FindParameterReader(m_Graph->Domain, *parameter);
        if (!reader)
            return 0;
        const uint32_t id = AddNodeAt(reader->TypeName, screen);
        if (GraphNode* node = FindNode(id))
        {
            node->Properties[kParameterReferenceProperty] = parameterName;
            Relayout();
            Edited(true);
        }
        return id;
    }

    const NodeGraphCanvas::ValueHit* NodeGraphCanvas::ValueAt(const UIVec2& screen) const
    {
        for (auto it = m_ValueHits.rbegin(); it != m_ValueHits.rend(); ++it)
            if (it->Rect.Contains(screen))
                return &*it;
        return nullptr;
    }

    void NodeGraphCanvas::SetGraph(NodeGraph* graph)
    {
        if (graph == m_Graph)
            return;
        m_Graph = graph;
        m_Selected.clear();
        m_SelectedLink = -1;
        m_HoveredLink = -1;
        m_HoveredNode = 0;
        m_Drag = Drag::None;
        m_NeedsFrame = true;
        SeedLiterals();
        Relayout();
        if (OnSelectionChanged)
            OnSelectionChanged();
    }

    void NodeGraphCanvas::SeedLiterals()
    {
        // A node saved before its type had a value input has no literal for it: filled in once, when the graph is opened (the old
        // canvas did it while drawing -- the asset changed with nothing knowing). Reported as an edit when anything was added.
        if (!m_Graph)
            return;
        bool added = false;
        for (GraphNode& node : m_Graph->Nodes)
        {
            if (const NodeTypeDesc* type = TypeOf(node))
                for (const auto& [name, literal] : ResolveNodePins(*type, node.Properties).Literals)
                    added = node.Properties.try_emplace(name, literal).second || added;
        }
        if (added)
            Edited(true);
    }

    void NodeGraphCanvas::ReloadGraph()
    {
        Relayout();
        std::erase_if(m_Selected, [this](uint32_t id) { return !FindNode(id); });
        m_SelectedLink = -1;
        if (OnSelectionChanged)
            OnSelectionChanged();
    }

    GraphNode* NodeGraphCanvas::FindNode(uint32_t id) const
    {
        if (!m_Graph)
            return nullptr;
        for (GraphNode& node : m_Graph->Nodes)
        {
            if (node.Id == id)
                return &node;
        }
        return nullptr;
    }

    const NodeTypeDesc* NodeGraphCanvas::TypeOf(const GraphNode& node) const
    {
        return m_Graph ? NodeTypeRegistry::Find(m_Graph->Domain, node.TypeName) : nullptr;
    }

    const NodeGraphCanvas::NodeLayout* NodeGraphCanvas::LayoutOf(uint32_t nodeId) const
    {
        const auto found = m_Layouts.find(nodeId);
        return found != m_Layouts.end() ? &found->second : nullptr;
    }

    void NodeGraphCanvas::Relayout()
    {
        m_Layouts.clear();
        if (!m_Graph)
            return;
        for (GraphNode& node : m_Graph->Nodes)
        {
            NodeLayout layout;
            const NodeTypeDesc* type = TypeOf(node);
            if (node.TypeName == kRerouteNodeType)
            {
                // A reroute: one pin each way, the wire passing through its middle.
                std::string pinType;
                if (const auto found = node.Properties.find(kRerouteTypeProperty); found != node.Properties.end())
                    if (const auto* text = std::get_if<std::string>(&found->second))
                        pinType = *text;
                layout.Reroute = true;
                layout.Inputs = { { "", pinType } };
                layout.Outputs = { { "", pinType } };
                layout.Size = { 24.0f, 16.0f };
                m_Layouts.emplace(node.Id, std::move(layout));
                continue;
            }
            if (!type)
            {
                layout.Known = false;
                layout.Title = node.TypeName + " (unknown)";
                layout.Header = NoxUI::ThemeColor("NodeHeaderError", { 0.5f, 0.2f, 0.2f, 1.0f });
                layout.Size = { std::max(kMinWidth, NoxUI::UILabel::MeasureText(layout.Title, nullptr, kTitleFont).x + 24.0f), kHeader + kRow + kBottom };
                m_Layouts.emplace(node.Id, std::move(layout));
                continue;
            }
            const NodePinList pins = ResolveNodePins(*type, node.Properties);
            layout.Inputs = pins.Inputs;
            layout.Outputs = pins.Outputs;
            layout.Title = node.TypeName;
            if (const auto found = node.Properties.find(kParameterReferenceProperty); found != node.Properties.end())
                if (const auto* name = std::get_if<std::string>(&found->second); name && !name->empty())
                    layout.Title = node.TypeName + ": " + *name;
            layout.Header = node.Id == m_Graph->OutputNode ? NoxUI::ThemeColor("NodeHeaderOutput", { 0.22f, 0.53f, 0.35f, 1.0f }) : CategoryColor(type->Category);

            float inputWidth = 0.0f, outputWidth = 0.0f;
            for (const NodePinDesc& pin : layout.Inputs)
            {
                float width = NoxUI::UILabel::MeasureText(pin.Name, nullptr, kPinFont).x;
                if (const auto literal = node.Properties.find(pin.Name); literal != node.Properties.end())
                    width += NoxUI::UILabel::MeasureText(ShortValue(literal->second), nullptr, kPinFont).x + 10.0f;
                inputWidth = std::max(inputWidth, width);
            }
            for (const NodePinDesc& pin : layout.Outputs)
                outputWidth = std::max(outputWidth, NoxUI::UILabel::MeasureText(pin.Name, nullptr, kPinFont).x);
            const float titleWidth = NoxUI::UILabel::MeasureText(layout.Title, nullptr, kTitleFont).x + 28.0f;
            const float rows = static_cast<float>(std::max<size_t>(std::max(layout.Inputs.size(), layout.Outputs.size()), 1)) +
                               (type->OwnsSubGraphs ? 1.0f : 0.0f);
            layout.Size = { std::max({ kMinWidth, titleWidth, inputWidth + outputWidth + 56.0f }), kHeader + rows * kRow + kBottom };
            m_Layouts.emplace(node.Id, std::move(layout));
        }
    }

    UIVec2 NodeGraphCanvas::GraphToScreen(const UIVec2& p) const { return m_View.ToScreen(GetBounds(), p); }
    UIVec2 NodeGraphCanvas::ScreenToGraph(const UIVec2& p) const { return m_View.ToGraph(GetBounds(), p); }

    UIVec2 NodeGraphCanvas::PinPosition(const GraphNode& node, const NodeLayout& layout, uint32_t pin, bool output) const
    {
        if (layout.Reroute)
            return node.EditorPosition + UIVec2{ output ? layout.Size.x : 0.0f, layout.Size.y * 0.5f };
        return node.EditorPosition + UIVec2{ output ? layout.Size.x : 0.0f, kHeader + (static_cast<float>(pin) + 0.5f) * kRow + 2.0f };
    }

    std::string NodeGraphCanvas::PinType(const PinRef& pin) const
    {
        const NodeLayout* layout = LayoutOf(pin.Node);
        if (!layout)
            return {};
        const auto& list = pin.Output ? layout->Outputs : layout->Inputs;
        return pin.Pin < list.size() ? list[pin.Pin].Type : std::string();
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    uint32_t NodeGraphCanvas::NodeAt(const UIVec2& screen) const
    {
        if (!m_Graph)
            return 0;
        // Selected nodes are drawn last (on top): they are hit first.
        for (int pass = 0; pass < 2; ++pass)
        {
            for (auto it = m_Graph->Nodes.rbegin(); it != m_Graph->Nodes.rend(); ++it)
            {
                const bool selected = std::find(m_Selected.begin(), m_Selected.end(), it->Id) != m_Selected.end();
                if ((pass == 0) != selected)
                    continue;
                const NodeLayout* layout = LayoutOf(it->Id);
                if (!layout)
                    continue;
                const UIVec2 min = GraphToScreen(it->EditorPosition), max = GraphToScreen(it->EditorPosition + layout->Size);
                if (NoxUI::UIRect{ min, max }.Contains(screen))
                    return it->Id;
            }
        }
        return 0;
    }

    NodeGraphCanvas::PinRef NodeGraphCanvas::PinAt(const UIVec2& screen) const
    {
        if (!m_Graph)
            return {};
        const float reach = std::max(8.0f, kPinRadius * m_View.Zoom + 4.0f);
        PinRef best;
        float bestDistance = reach;
        for (const GraphNode& node : m_Graph->Nodes)
        {
            const NodeLayout* layout = LayoutOf(node.Id);
            if (!layout)
                continue;
            for (int output = 0; output < 2; ++output)
            {
                const auto& pins = output ? layout->Outputs : layout->Inputs;
                for (uint32_t i = 0; i < pins.size(); ++i)
                {
                    const float distance = glm::length(GraphToScreen(PinPosition(node, *layout, i, output != 0)) - screen);
                    if (distance < bestDistance)
                    {
                        bestDistance = distance;
                        best = { node.Id, i, output != 0 };
                    }
                }
            }
        }
        return best;
    }

    int NodeGraphCanvas::LinkAt(const UIVec2& screen) const
    {
        if (!m_Graph)
            return -1;
        for (int i = static_cast<int>(m_Graph->Links.size()) - 1; i >= 0; --i)
        {
            const GraphLink& link = m_Graph->Links[static_cast<size_t>(i)];
            const GraphNode* from = FindNode(link.FromNode);
            const GraphNode* to = FindNode(link.ToNode);
            const NodeLayout* fromLayout = LayoutOf(link.FromNode);
            const NodeLayout* toLayout = LayoutOf(link.ToNode);
            if (!from || !to || !fromLayout || !toLayout)
                continue;
            const UIVec2 p0 = GraphToScreen(PinPosition(*from, *fromLayout, link.FromPin, true));
            const UIVec2 p3 = GraphToScreen(PinPosition(*to, *toLayout, link.ToPin, false));
            const float tangent = std::max(40.0f * m_View.Zoom, std::abs(p3.x - p0.x) * 0.5f);
            const UIVec2 p1 = p0 + UIVec2{ tangent, 0.0f }, p2 = p3 - UIVec2{ tangent, 0.0f };
            UIVec2 previous = p0;
            for (int s = 1; s <= 24; ++s)
            {
                const UIVec2 point = BezierPoint(p0, p1, p2, p3, static_cast<float>(s) / 24.0f);
                if (SegmentDistance(screen, previous, point) < 6.0f)
                    return i;
                previous = point;
            }
        }
        return -1;
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    bool NodeGraphCanvas::CanConnect(const PinRef& a, const PinRef& b, GraphNode** retype, std::string* retypeTo) const
    {
        if (!m_Graph || !a.Valid() || !b.Valid() || a.Output == b.Output || a.Node == b.Node)
            return false;
        const PinRef& from = a.Output ? a : b;
        const PinRef& to = a.Output ? b : a;
        GraphNode* fromNode = FindNode(from.Node);
        GraphNode* toNode = FindNode(to.Node);
        if (!fromNode || !toNode)
            return false;
        const std::string fromType = PinType(from), toType = PinType(to);
        // An untyped (or unconnected) reroute takes the other end's type.
        auto retypable = [this](const GraphNode& node, const std::string& type)
        {
            if (node.TypeName != kRerouteNodeType)
                return false;
            const bool linked = std::any_of(m_Graph->Links.begin(), m_Graph->Links.end(),
                                            [&](const GraphLink& l) { return l.FromNode == node.Id || l.ToNode == node.Id; });
            return type.empty() || !linked;
        };
        if (!fromType.empty() && fromType == toType)
            return true;
        if (!fromType.empty() && retypable(*toNode, toType))
        {
            if (retype) *retype = toNode;
            if (retypeTo) *retypeTo = fromType;
            return true;
        }
        if (!toType.empty() && retypable(*fromNode, fromType))
        {
            if (retype) *retype = fromNode;
            if (retypeTo) *retypeTo = toType;
            return true;
        }
        return false;
    }

    bool NodeGraphCanvas::Connect(const PinRef& a, const PinRef& b)
    {
        GraphNode* retype = nullptr;
        std::string retypeTo;
        if (!CanConnect(a, b, &retype, &retypeTo))
            return false;
        const PinRef& from = a.Output ? a : b;
        const PinRef& to = a.Output ? b : a;
        if (retype)
            retype->Properties[kRerouteTypeProperty] = retypeTo;
        // An input pin takes one wire: a new one replaces it.
        std::erase_if(m_Graph->Links, [&](const GraphLink& l) { return l.ToNode == to.Node && l.ToPin == to.Pin; });
        m_Graph->Links.push_back({ from.Node, from.Pin, to.Node, to.Pin });
        Relayout();
        Edited(true);
        return true;
    }

    void NodeGraphCanvas::InsertReroute(int linkIndex, const UIVec2& screen)
    {
        if (!m_Graph || linkIndex < 0 || linkIndex >= static_cast<int>(m_Graph->Links.size()))
            return;
        const GraphLink link = m_Graph->Links[static_cast<size_t>(linkIndex)];
        const std::string type = PinType({ link.FromNode, link.FromPin, true });
        GraphNode reroute;
        reroute.Id = m_Graph->NextNodeId++;
        reroute.TypeName = kRerouteNodeType;
        reroute.EditorPosition = ScreenToGraph(screen) - UIVec2{ 12.0f, 8.0f };
        reroute.Properties[kRerouteTypeProperty] = type;
        m_Graph->Nodes.push_back(reroute);
        m_Graph->Links.erase(m_Graph->Links.begin() + linkIndex);
        m_Graph->Links.push_back({ link.FromNode, link.FromPin, reroute.Id, 0 });
        m_Graph->Links.push_back({ reroute.Id, 0, link.ToNode, link.ToPin });
        m_SelectedLink = -1;
        Relayout();
        SetSelection({ reroute.Id });
        Edited(true);
    }

    uint32_t NodeGraphCanvas::AddNodeAt(const std::string& typeName, const UIVec2& screen, bool atViewCenter)
    {
        if (!m_Graph)
            return 0;
        const NodeTypeDesc* type = NodeTypeRegistry::Find(m_Graph->Domain, typeName);
        if (!type)
            return 0;
        GraphNode node;
        node.Id = m_Graph->NextNodeId++;
        node.TypeName = typeName;
        node.EditorPosition = atViewCenter ? ScreenToGraph(GetBounds().Center()) - UIVec2{ 80.0f, 30.0f } : ScreenToGraph(screen);
        for (const auto& [name, value] : type->DefaultProperties)
            node.Properties[name] = value;
        // Its value inputs' literals (a variable input group's too, for the pins it starts with).
        for (const auto& [name, literal] : ResolveNodePins(*type, node.Properties).Literals)
            node.Properties.try_emplace(name, literal);
        const uint32_t variable = VariableInputCount(*type, node.Properties);
        for (uint32_t index = 0; index < variable; ++index)
        {
            for (const NodePinDesc& pin : type->VariableInputs)
                if (const auto literal = type->VariableInputLiterals.find(pin.Name); literal != type->VariableInputLiterals.end())
                    node.Properties.try_emplace(pin.Name + " " + std::to_string(index), literal->second);
        }
        m_Graph->Nodes.push_back(std::move(node));
        const uint32_t id = m_Graph->Nodes.back().Id;
        Relayout();
        SetSelection({ id });
        return id;
    }

    bool NodeGraphCanvas::DeleteNodes(const std::vector<uint32_t>& nodeIds)
    {
        if (!m_Graph)
            return false;
        std::unordered_set<uint32_t> doomed;
        for (uint32_t id : nodeIds)
        {
            // The graph's output (Output Pose, Material Output, the audio output) stays: a graph without one computes nothing.
            if (id != m_Graph->OutputNode)
                doomed.insert(id);
        }
        if (doomed.empty())
            return false;
        std::erase_if(m_Graph->Nodes, [&](const GraphNode& n) { return doomed.contains(n.Id); });
        std::erase_if(m_Graph->Links, [&](const GraphLink& l) { return doomed.contains(l.FromNode) || doomed.contains(l.ToNode); });
        std::erase_if(m_Selected, [&](uint32_t id) { return doomed.contains(id); });
        m_SelectedLink = -1;
        Relayout();
        if (OnSelectionChanged)
            OnSelectionChanged();
        Edited(true);
        return true;
    }

    void NodeGraphCanvas::Copy()
    {
        if (!m_Graph || m_Selected.empty())
            return;
        std::unordered_set<uint32_t> picked(m_Selected.begin(), m_Selected.end());
        picked.erase(m_Graph->OutputNode); // one output per graph
        m_ClipboardNodes.clear();
        m_ClipboardLinks.clear();
        for (const GraphNode& node : m_Graph->Nodes)
        {
            if (picked.contains(node.Id))
                m_ClipboardNodes.push_back(node);
        }
        for (const GraphLink& link : m_Graph->Links)
        {
            if (picked.contains(link.FromNode) && picked.contains(link.ToNode))
                m_ClipboardLinks.push_back(link);
        }
    }

    void NodeGraphCanvas::Paste(const UIVec2& screen)
    {
        if (!m_Graph || m_ClipboardNodes.empty())
            return;
        // Placed with their top-left at the pointer, their layout kept; new ids, the wires between them remapped.
        UIVec2 origin = m_ClipboardNodes.front().EditorPosition;
        for (const GraphNode& node : m_ClipboardNodes)
            origin = glm::min(origin, node.EditorPosition);
        const UIVec2 target = GetBounds().Contains(screen) ? ScreenToGraph(screen) : origin + UIVec2{ 40.0f };
        std::unordered_map<uint32_t, uint32_t> remap;
        std::vector<uint32_t> added;
        for (GraphNode node : m_ClipboardNodes)
        {
            const uint32_t id = m_Graph->NextNodeId++;
            remap[node.Id] = id;
            node.Id = id;
            node.EditorPosition = target + (node.EditorPosition - origin);
            added.push_back(id);
            m_Graph->Nodes.push_back(std::move(node));
        }
        for (GraphLink link : m_ClipboardLinks)
        {
            link.FromNode = remap[link.FromNode];
            link.ToNode = remap[link.ToNode];
            link.Route.clear();
            m_Graph->Links.push_back(std::move(link));
        }
        Relayout();
        SetSelection(added);
        Edited(true);
    }

    void NodeGraphCanvas::Duplicate()
    {
        const auto keepNodes = m_ClipboardNodes;
        const auto keepLinks = m_ClipboardLinks;
        Copy();
        if (!m_ClipboardNodes.empty())
        {
            UIVec2 origin = m_ClipboardNodes.front().EditorPosition;
            for (const GraphNode& node : m_ClipboardNodes)
                origin = glm::min(origin, node.EditorPosition);
            Paste(GraphToScreen(origin + UIVec2{ 30.0f, 30.0f }));
        }
        m_ClipboardNodes = keepNodes; // Ctrl+D leaves what Ctrl+C took
        m_ClipboardLinks = keepLinks;
    }

    void NodeGraphCanvas::SetSelection(std::vector<uint32_t> nodes)
    {
        m_Selected = std::move(nodes);
        m_SelectedLink = -1;
        if (OnSelectionChanged)
            OnSelectionChanged();
    }

    void NodeGraphCanvas::SelectNode(uint32_t nodeId) { SetSelection({ nodeId }); }

    void NodeGraphCanvas::Edited(bool structural)
    {
        if (OnEdited)
            OnEdited(structural);
    }

    void NodeGraphCanvas::FrameAll()
    {
        if (!m_Graph)
            return;
        std::vector<uint32_t> all;
        for (const GraphNode& node : m_Graph->Nodes)
            all.push_back(node.Id);
        const std::vector<uint32_t> keep = m_Selected;
        m_Selected = all;
        FrameSelection();
        m_Selected = keep;
    }

    void NodeGraphCanvas::FrameSelection()
    {
        const NoxUI::UIRect& b = GetBounds();
        if (!m_Graph || b.Width() < 10.0f || b.Height() < 10.0f)
            return;
        const std::vector<uint32_t>& nodes = m_Selected;
        if (nodes.empty())
        {
            FrameAll();
            return;
        }
        UIVec2 min{ 1e9f }, max{ -1e9f };
        for (uint32_t id : nodes)
        {
            const GraphNode* node = FindNode(id);
            const NodeLayout* layout = LayoutOf(id);
            if (!node || !layout)
                continue;
            min = glm::min(min, UIVec2(node->EditorPosition));
            max = glm::max(max, UIVec2(node->EditorPosition) + layout->Size);
        }
        if (min.x > max.x)
            return;
        m_View.Fit(b, min, max, 0.3f, 1.25f, 0.0f, 60.0f);
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    void NodeGraphCanvas::OnUpdate(float)
    {
        Relayout();
        if (m_NeedsFrame && m_Graph && GetBounds().Width() > 10.0f)
        {
            m_NeedsFrame = false;
            FrameAll();
        }
    }

    bool NodeGraphCanvas::OnPointerDown(const NoxUI::UIPointerEvent& e)
    {
        if (!m_Graph)
            return true;
        if (NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent())
            ctx->SetFocus(this);
        m_DragStart = m_DragLast = e.Position;
        m_DragMoved = false;

        if (e.Button == 2 || e.Button == 3)
        {
            m_Drag = Drag::Pan;
            m_PanButton = e.Button;
            return true;
        }
        if (e.Button != 1)
            return false;

        const auto now = std::chrono::steady_clock::now();
        const bool quick = now - m_LastClickTime < std::chrono::milliseconds(350);
        m_LastClickTime = now;

        if (const PinRef pin = PinAt(e.Position); pin.Valid())
        {
            if (Alt())
            {
                // Alt+click: the pin's wires cut (Unreal's).
                const size_t before = m_Graph->Links.size();
                std::erase_if(m_Graph->Links, [&](const GraphLink& l)
                {
                    return pin.Output ? (l.FromNode == pin.Node && l.FromPin == pin.Pin) : (l.ToNode == pin.Node && l.ToPin == pin.Pin);
                });
                if (m_Graph->Links.size() != before)
                    Edited(true);
                return true;
            }
            m_Drag = Drag::Wire;
            m_WireFrom = pin;
            return true;
        }

        // A value on a node: a bool switches now, a number waits to see whether it is dragged, anything else opens on release.
        if (const ValueHit* hit = !Ctrl() && !Shift() && !Alt() ? ValueAt(e.Position) : nullptr)
        {
            GraphNode* node = FindNode(hit->Node);
            const auto value = node ? node->Properties.find(hit->Property) : decltype(node->Properties.end()){};
            if (node && value != node->Properties.end())
            {
                if (std::find(m_Selected.begin(), m_Selected.end(), hit->Node) == m_Selected.end())
                    SetSelection({ hit->Node });
                if (bool* flag = std::get_if<bool>(&value->second))
                {
                    *flag = !*flag;
                    Relayout();
                    Edited(true);
                    return true;
                }
                m_Drag = Drag::Value;
                m_ValueDrag = *hit;
                m_ValueStart = value->second;
                m_ValueChanged = false;
                if (NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent())
                    ctx->CancelPendingDrag();
                return true;
            }
        }

        if (const uint32_t node = NodeAt(e.Position); node != 0)
        {
            if (quick && node == m_LastClickNode)
            {
                m_LastClickNode = 0;
                const GraphNode* graphNode = FindNode(node);
                const NodeTypeDesc* type = graphNode ? TypeOf(*graphNode) : nullptr;
                if (type && type->OwnsSubGraphs && OnOpenNode)
                    OnOpenNode(node);
                return true;
            }
            m_LastClickNode = node;
            const bool selected = std::find(m_Selected.begin(), m_Selected.end(), node) != m_Selected.end();
            if (Ctrl())
            {
                std::vector<uint32_t> next = m_Selected;
                if (selected)
                    std::erase(next, node);
                else
                    next.push_back(node);
                SetSelection(next);
            }
            else if (Shift())
            {
                if (!selected)
                {
                    std::vector<uint32_t> next = m_Selected;
                    next.push_back(node);
                    SetSelection(next);
                }
            }
            else if (!selected)
            {
                SetSelection({ node });
            }
            m_Drag = Drag::Nodes;
            m_DragOrigins.clear();
            for (uint32_t id : m_Selected)
                if (const GraphNode* graphNode = FindNode(id))
                    m_DragOrigins[id] = graphNode->EditorPosition;
            return true;
        }
        m_LastClickNode = 0;

        if (const int link = LinkAt(e.Position); link >= 0)
        {
            if (quick && link == m_LastClickLink)
            {
                m_LastClickLink = -1;
                InsertReroute(link, e.Position);
                return true;
            }
            m_LastClickLink = link;
            m_Selected.clear();
            m_SelectedLink = link;
            if (OnSelectionChanged)
                OnSelectionChanged();
            return true;
        }
        m_LastClickLink = -1;

        // Empty canvas: a box selection (Ctrl / Shift add to what is selected).
        m_Drag = Drag::Box;
        m_BoxBase = (Ctrl() || Shift()) ? m_Selected : std::vector<uint32_t>{};
        if (!Ctrl() && !Shift())
            SetSelection({});
        return true;
    }

    bool NodeGraphCanvas::OnPointerMove(const NoxUI::UIPointerEvent& e)
    {
        if (!m_Graph)
            return true;
        if (glm::length(e.Position - m_DragStart) > 3.0f)
            m_DragMoved = true;
        switch (m_Drag)
        {
        case Drag::Pan:
            m_View.Pan += e.Position - m_DragLast;
            break;
        case Drag::Nodes:
        {
            const UIVec2 delta = (e.Position - m_DragStart) / m_View.Zoom;
            for (const auto& [id, origin] : m_DragOrigins)
                if (GraphNode* node = FindNode(id))
                    node->EditorPosition = origin + glm::vec2(delta);
            break;
        }
        case Drag::Value:
        {
            // A number dragged sideways: its type's range over 300 px (no range: 0.01 a pixel; an int: one per 8 px); Shift finer.
            GraphNode* node = m_DragMoved ? FindNode(m_ValueDrag.Node) : nullptr;
            const NodeTypeDesc* type = node ? TypeOf(*node) : nullptr;
            const auto value = node ? node->Properties.find(m_ValueDrag.Property) : decltype(node->Properties.end()){};
            if (!node || value == node->Properties.end())
                break;
            const float dx = (e.Position.x - m_DragStart.x) * (Shift() ? 0.1f : 1.0f);
            const auto range = type ? type->PropertyRanges.find(m_ValueDrag.Property) : decltype(type->PropertyRanges.end()){};
            const bool hasRange = type && range != type->PropertyRanges.end();
            if (const float* start = std::get_if<float>(&m_ValueStart); start && std::holds_alternative<float>(value->second))
            {
                float next = *start + dx * (hasRange ? (range->second.second - range->second.first) / 300.0f : 0.01f);
                if (hasRange)
                    next = std::clamp(next, range->second.first, range->second.second);
                value->second = next;
                m_ValueChanged = true;
            }
            else if (const int32_t* startInt = std::get_if<int32_t>(&m_ValueStart); startInt && std::holds_alternative<int32_t>(value->second))
            {
                int32_t next = *startInt + static_cast<int32_t>(dx / 8.0f);
                if (hasRange)
                    next = std::clamp(next, static_cast<int32_t>(range->second.first), static_cast<int32_t>(range->second.second));
                value->second = next;
                m_ValueChanged = true;
            }
            Relayout();
            break;
        }
        case Drag::Box:
        {
            const UIVec2 a = ScreenToGraph(m_DragStart), b = ScreenToGraph(e.Position);
            const UIVec2 min = glm::min(a, b), max = glm::max(a, b);
            std::vector<uint32_t> next = m_BoxBase;
            for (const GraphNode& node : m_Graph->Nodes)
            {
                const NodeLayout* layout = LayoutOf(node.Id);
                if (!layout)
                    continue;
                const UIVec2 nodeMin = node.EditorPosition, nodeMax = UIVec2(node.EditorPosition) + layout->Size;
                const bool overlaps = nodeMin.x < max.x && nodeMax.x > min.x && nodeMin.y < max.y && nodeMax.y > min.y;
                if (overlaps && std::find(next.begin(), next.end(), node.Id) == next.end())
                    next.push_back(node.Id);
            }
            if (next != m_Selected)
                SetSelection(next);
            break;
        }
        default:
        {
            m_HoveredPin = PinAt(e.Position);
            m_HoveredNode = m_HoveredPin.Valid() ? 0 : NodeAt(e.Position);
            m_HoveredLink = (m_HoveredPin.Valid() || m_HoveredNode) ? -1 : LinkAt(e.Position);
            // What a pin or value on a node does, while the pointer is on it.
            Tooltip.clear();
            if (m_HoveredPin.Valid())
            {
                if (const NodeLayout* layout = LayoutOf(m_HoveredPin.Node))
                {
                    const auto& pins = m_HoveredPin.Output ? layout->Outputs : layout->Inputs;
                    if (m_HoveredPin.Pin < pins.size())
                    {
                        const NodePinDesc& pin = pins[m_HoveredPin.Pin];
                        Tooltip = pin.Tooltip.empty() ? pin.Type : (pin.Tooltip + "\n(" + pin.Type + ")");
                    }
                }
            }
            else if (const ValueHit* hit = ValueAt(e.Position))
                if (const GraphNode* node = FindNode(hit->Node))
                    if (const auto value = node->Properties.find(hit->Property); value != node->Properties.end())
                        Tooltip = std::holds_alternative<bool>(value->second) ? hit->Property + ": click to switch"
                                : (std::holds_alternative<float>(value->second) || std::holds_alternative<int32_t>(value->second))
                                    ? hit->Property + ": drag sideways to change (Shift: finer), click to type"
                                    : hit->Property + ": click to edit";
            break;
        }
        }
        if (m_Drag == Drag::Wire)
            m_HoveredPin = PinAt(e.Position);
        m_DragLast = e.Position;
        return true;
    }

    bool NodeGraphCanvas::OnPointerUp(const NoxUI::UIPointerEvent& e)
    {
        if (!m_Graph)
            return true;
        const Drag drag = m_Drag;
        m_Drag = Drag::None;
        switch (drag)
        {
        case Drag::Pan:
            if (!m_DragMoved && m_PanButton == 3)
            {
                if (const uint32_t node = NodeAt(e.Position); node != 0)
                    OpenNodeMenu(e.Position, node);
                else
                    OpenNodeList(e.Position, {});
            }
            break;
        case Drag::Nodes:
            if (m_DragMoved)
                Edited(false);
            break;
        case Drag::Value:
            if (m_DragMoved)
            {
                if (m_ValueChanged)
                    Edited(true); // (one step for the whole drag; compiled once, when let go)
            }
            else if (OnEditValue)
                OnEditValue(m_ValueDrag.Node, m_ValueDrag.Property, m_ValueDrag.Rect);
            break;
        case Drag::Wire:
            if (const PinRef target = PinAt(e.Position); target.Valid())
                Connect(m_WireFrom, target);
            else if (m_DragMoved && NodeAt(e.Position) == 0)
                OpenNodeList(e.Position, m_WireFrom); // let go on empty canvas: the nodes that fit, connected when picked
            break;
        default:
            break;
        }
        return true;
    }

    bool NodeGraphCanvas::OnPointerLeave(const NoxUI::UIPointerEvent&)
    {
        if (m_Drag == Drag::None)
        {
            m_HoveredPin = {};
            m_HoveredNode = 0;
            m_HoveredLink = -1;
        }
        return false;
    }

    bool NodeGraphCanvas::OnMouseWheel(const NoxUI::UIPointerEvent& e)
    {
        // Zoom around the pointer: the point under it stays under it.
        const NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
        const UIVec2 pointer = ctx ? ctx->GetLastMousePos() : GetBounds().Center();
        m_View.ZoomAt(GetBounds(), pointer, e.ScrollDelta.y);
        return true;
    }

    bool NodeGraphCanvas::OnKeyDown(const NoxUI::UIKeyEvent& e)
    {
        if (!m_Graph)
            return false;
        const NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
        const UIVec2 pointer = ctx ? ctx->GetLastMousePos() : GetBounds().Center();
        switch (e.KeyCode)
        {
        case SDL_SCANCODE_DELETE:
        case SDL_SCANCODE_BACKSPACE:
            if (m_SelectedLink >= 0 && m_SelectedLink < static_cast<int>(m_Graph->Links.size()))
            {
                m_Graph->Links.erase(m_Graph->Links.begin() + m_SelectedLink);
                m_SelectedLink = -1;
                Edited(true);
            }
            else
            {
                DeleteNodes(m_Selected);
            }
            return true;
        case SDL_SCANCODE_D:
            if (Ctrl()) { Duplicate(); return true; }
            return false;
        case SDL_SCANCODE_C:
            if (Ctrl()) { Copy(); return true; }
            return false;
        case SDL_SCANCODE_V:
            if (Ctrl()) { Paste(pointer); return true; }
            return false;
        case SDL_SCANCODE_A:
            if (Ctrl())
            {
                std::vector<uint32_t> all;
                for (const GraphNode& node : m_Graph->Nodes)
                    all.push_back(node.Id);
                SetSelection(all);
                return true;
            }
            return false;
        case SDL_SCANCODE_F:
            FrameSelection();
            return true;
        case SDL_SCANCODE_HOME:
        {
            m_View.ResetZoom(GetBounds());
            return true;
        }
        case SDL_SCANCODE_ESCAPE:
            SetSelection({});
            return true;
        default:
            return false;
        }
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    void NodeGraphCanvas::OpenNodeList(const UIVec2& screen, const PinRef& fromPin)
    {
        NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
        if (!ctx || !m_Graph)
            return;

        // What can be created: the domain's node types (with the wire's type, when made from a wire), and a "Get" per parameter.
        struct Entry
        {
            std::string Label, Category, TypeName, Parameter;
            uint32_t ConnectPin = 0;
        };
        const std::string wireType = fromPin.Valid() ? PinType(fromPin) : std::string();
        std::vector<Entry> entries;
        for (const NodeTypeDesc& type : NodeTypeRegistry::All())
        {
            if (type.Domain != m_Graph->Domain || ReadsParameter(type))
                continue;
            std::unordered_map<std::string, NodeGraphValue> defaults(type.DefaultProperties.begin(), type.DefaultProperties.end());
            const NodePinList pins = ResolveNodePins(type, defaults);
            uint32_t connectPin = 0;
            if (fromPin.Valid())
            {
                // A wire from an output wants an input of its type (and the other way round); a reroute takes any.
                const auto& candidates = fromPin.Output ? pins.Inputs : pins.Outputs;
                bool fits = type.TypeName == kRerouteNodeType;
                for (uint32_t i = 0; i < candidates.size() && !fits; ++i)
                {
                    if (candidates[i].Type == wireType)
                    {
                        fits = true;
                        connectPin = i;
                    }
                }
                if (!fits)
                    continue;
            }
            entries.push_back({ type.TypeName, type.Category.empty() ? std::string("Other") : type.Category, type.TypeName, {}, connectPin });
        }
        if (Parameters && !fromPin.Output)
        {
            for (const GraphParameter& parameter : *Parameters)
            {
                const NodeTypeDesc* reader = FindParameterReader(m_Graph->Domain, parameter);
                if (!reader)
                    continue;
                if (fromPin.Valid())
                {
                    std::unordered_map<std::string, NodeGraphValue> defaults(reader->DefaultProperties.begin(), reader->DefaultProperties.end());
                    const NodePinList pins = ResolveNodePins(*reader, defaults);
                    if (std::none_of(pins.Outputs.begin(), pins.Outputs.end(), [&](const NodePinDesc& pin) { return pin.Type == wireType; }))
                        continue;
                }
                entries.push_back({ "Get " + parameter.Name, "Parameters", reader->TypeName, parameter.Name, 0 });
            }
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
        {
            return a.Category != b.Category ? a.Category < b.Category : a.Label < b.Label;
        });

        Ref<NoxUI::UIStackBox> content(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 6.0f));
        content->StyleClass = "MenuPopup";
        content->DrawBackground = true;
        content->Padding = NoxUI::UIThickness::All(8.0f);
        content->WidthMode = NoxUI::UISizeMode::Fixed;
        content->FixedSize = { 300.0f, 0.0f };
        Ref<NoxUI::UILabel> heading(new NoxUI::UILabel(fromPin.Valid() ? "Nodes for a " + (wireType.empty() ? std::string("wire") : wireType + " wire") : "Add Node", 13.0f));
        heading->UseColorOverride = true;
        heading->ColorOverride = NoxUI::ThemeColor("TextMuted", { 0.62f, 0.66f, 0.74f, 1.0f });
        content->AddChild(heading);
        Ref<NoxUI::UITextInput> search(new NoxUI::UITextInput("", "Search nodes..."));
        search->WidthMode = NoxUI::UISizeMode::Fill;
        content->AddChild(search);
        Ref<NoxUI::UIScrollView> list(new NoxUI::UIScrollView());
        list->WidthMode = NoxUI::UISizeMode::Fill;
        list->HeightMode = NoxUI::UISizeMode::Auto;
        list->MaxSize.y = 380.0f;
        list->ChildSpacing = 1.0f;
        content->AddChild(list);

        const UIVec2 graphPoint = ScreenToGraph(screen);
        auto entriesRef = std::make_shared<std::vector<Entry>>(std::move(entries));
        auto first = std::make_shared<std::function<void()>>();
        NoxUI::UIScrollView* listRaw = list.get();
        auto pick = [this, graphPoint, fromPin, ctx](const Entry& entry)
        {
            // Copied first: closing the popup destroys the button that called this.
            const Entry chosen = entry;
            const PinRef wire = fromPin;
            NoxUI::UIContext* context = ctx;
            context->ClearOverlay();
            if (!m_Graph)
                return;
            const uint32_t id = AddNodeAt(chosen.TypeName, GraphToScreen(graphPoint));
            GraphNode* node = FindNode(id);
            if (!node)
                return;
            if (!chosen.Parameter.empty())
                node->Properties[kParameterReferenceProperty] = chosen.Parameter;
            Relayout();
            // Made from a wire: placed so the wire meets it, connected.
            if (wire.Valid())
            {
                if (const NodeLayout* layout = LayoutOf(id); layout && !wire.Output)
                    node->EditorPosition.x -= layout->Size.x;
                const PinRef created{ id, chosen.ConnectPin, !wire.Output };
                if (!Connect(wire, created))
                    Edited(true);
            }
            else
            {
                Edited(true);
            }
        };
        auto rebuild = [listRaw, entriesRef, first, pick](const std::string& query)
        {
            listRaw->ClearChildren();
            *first = {};
            std::string lowered = query;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string category;
            for (const Entry& entry : *entriesRef)
            {
                std::string label = entry.Label + " " + entry.Category;
                std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!lowered.empty() && label.find(lowered) == std::string::npos)
                    continue;
                if (entry.Category != category)
                {
                    category = entry.Category;
                    Ref<NoxUI::UILabel> header(new NoxUI::UILabel(category, 12.0f));
                    header->UseColorOverride = true;
                    header->ColorOverride = NoxUI::ThemeColor("TextMuted", { 0.55f, 0.60f, 0.70f, 1.0f });
                    header->Margin = NoxUI::UIThickness{ 4.0f, 6.0f, 0.0f, 2.0f };
                    listRaw->AddChild(header);
                }
                Ref<NoxUI::UIButton> button(new NoxUI::UIButton(entry.Label, [pick, entry]() { pick(entry); }));
                button->StyleClass = "MenuItem";
                button->WidthMode = NoxUI::UISizeMode::Fill;
                listRaw->AddChild(button);
                if (!*first)
                    *first = [pick, entry]() { pick(entry); };
            }
            if (listRaw->GetChildren().empty())
            {
                Ref<NoxUI::UILabel> none(new NoxUI::UILabel("No node matches", 13.0f));
                none->UseColorOverride = true;
                none->ColorOverride = NoxUI::ThemeColor("TextMuted", { 0.55f, 0.60f, 0.70f, 1.0f });
                listRaw->AddChild(none);
            }
        };
        rebuild("");
        search->OnChanged = [rebuild](const std::string& text) { rebuild(text); };
        search->OnSubmit = [first](const std::string&) { if (*first) (*first)(); };
        NoxUI::UIPopup::Show(*ctx, { screen, screen + UIVec2{ 1.0f } }, content);
        ctx->SetFocus(search.get());
    }

    void NodeGraphCanvas::OpenNodeMenu(const UIVec2& screen, uint32_t nodeId)
    {
        NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
        const GraphNode* node = FindNode(nodeId);
        if (!ctx || !node)
            return;
        if (std::find(m_Selected.begin(), m_Selected.end(), nodeId) == m_Selected.end())
            SetSelection({ nodeId });
        const NodeTypeDesc* type = TypeOf(*node);
        const bool isOutput = m_Graph && nodeId == m_Graph->OutputNode;
        std::vector<NoxUI::UIMenuItem> items;
        if (type && type->OwnsSubGraphs)
            items.push_back({ "Open States", "Double-click", [this, nodeId]() { if (OnOpenNode) OnOpenNode(nodeId); } });
        items.push_back({ "Duplicate", "Ctrl+D", [this]() { Duplicate(); } });
        items.push_back({ "Copy", "Ctrl+C", [this]() { Copy(); } });
        items.push_back({ "Break All Links", "", [this, nodeId]()
        {
            if (!m_Graph)
                return;
            const size_t before = m_Graph->Links.size();
            std::erase_if(m_Graph->Links, [nodeId](const GraphLink& l) { return l.FromNode == nodeId || l.ToNode == nodeId; });
            if (m_Graph->Links.size() != before)
                Edited(true);
        } });
        items.push_back({ "", "", {}, true });
        items.push_back({ "Delete", "Del", [this]() { DeleteNodes(m_Selected); }, false, !isOutput || m_Selected.size() > 1, false, false,
                          isOutput ? "The graph's output stays: without it the graph computes nothing." : "" });
        NoxUI::UIMenuPopup::ShowContextMenu(*ctx, screen, std::move(items));
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    void NodeGraphCanvas::OnPaint(NoxUI::UIDrawList& d, const NoxUI::UITheme& theme)
    {
        const NoxUI::UIRect& b = GetBounds();
        const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        const UIColor border = theme.GetToken("Border", { 0.20f, 0.235f, 0.32f, 1.0f });
        const UIColor muted = theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f });
        d.AddRoundedRect(b.Min, b.Max, theme.GetToken("Background", { 0.063f, 0.075f, 0.106f, 1.0f }));
        d.PushClipRect(b.Min, b.Max);
        m_ValueHits.clear();

        // Grid: fine lines every 16 units, stronger every 128.
        m_View.PaintGrid(d, b);

        if (!m_Graph)
        {
            d.AddText("No graph", nullptr, b.Center() - UIVec2{ 30.0f, 8.0f }, 14.0f, muted);
            d.PopClipRect();
            return;
        }

        auto wire = [&](const UIVec2& p0, const UIVec2& p3, const UIColor& color, float width)
        {
            const float tangent = std::max(40.0f * m_View.Zoom, std::abs(p3.x - p0.x) * 0.5f);
            d.AddBezier(p0, p0 + UIVec2{ tangent, 0.0f }, p3 - UIVec2{ tangent, 0.0f }, p3, width, color);
        };

        // Wires under the nodes, in their output's colour (a wire through reroutes keeps it).
        const std::unordered_set<uint32_t> selected(m_Selected.begin(), m_Selected.end());
        for (size_t i = 0; i < m_Graph->Links.size(); ++i)
        {
            const GraphLink& link = m_Graph->Links[i];
            const GraphNode* from = FindNode(link.FromNode);
            const GraphNode* to = FindNode(link.ToNode);
            const NodeLayout* fromLayout = LayoutOf(link.FromNode);
            const NodeLayout* toLayout = LayoutOf(link.ToNode);
            if (!from || !to || !fromLayout || !toLayout)
                continue;
            UIColor color = PinColor(link.FromPin < fromLayout->Outputs.size() ? fromLayout->Outputs[link.FromPin].Type : std::string());
            const bool lit = static_cast<int>(i) == m_SelectedLink || static_cast<int>(i) == m_HoveredLink ||
                             selected.contains(link.FromNode) || selected.contains(link.ToNode);
            if (!lit)
                color.a = 0.8f;
            wire(GraphToScreen(PinPosition(*from, *fromLayout, link.FromPin, true)), GraphToScreen(PinPosition(*to, *toLayout, link.ToPin, false)),
                 static_cast<int>(i) == m_SelectedLink ? NoxUI::ThemeColor("WireSelected", { 1.0f, 1.0f, 1.0f, 1.0f }) : color, (lit ? 3.0f : 2.2f) * std::max(m_View.Zoom, 0.6f));
        }

        const bool showText = m_View.Zoom >= 0.45f;
        auto drawPin = [&](const UIVec2& center, const std::string& type, bool connected, bool hovered)
        {
            const UIColor color = PinColor(type);
            const float r = kPinRadius * m_View.Zoom * (hovered ? 1.3f : 1.0f);
            const UIColor fill = connected ? color : NoxUI::ThemeColor("PinUnconnected", { 0.08f, 0.10f, 0.14f, 1.0f });
            if (type == "Trigger")
            {
                // An arrow pointing right (an event, like an execution pin).
                const float w = r * 1.2f, h = r;
                NoxUI::Icons::DrawSegment(d, { center.x - w, center.y - h }, { center.x + w * 0.4f, center.y - h }, 1.6f * m_View.Zoom, color);
                NoxUI::Icons::DrawSegment(d, { center.x + w * 0.4f, center.y - h }, { center.x + w, center.y }, 1.6f * m_View.Zoom, color);
                NoxUI::Icons::DrawSegment(d, { center.x + w, center.y }, { center.x + w * 0.4f, center.y + h }, 1.6f * m_View.Zoom, color);
                NoxUI::Icons::DrawSegment(d, { center.x + w * 0.4f, center.y + h }, { center.x - w, center.y + h }, 1.6f * m_View.Zoom, color);
                NoxUI::Icons::DrawSegment(d, { center.x - w, center.y + h }, { center.x - w, center.y - h }, 1.6f * m_View.Zoom, color);
                if (connected)
                    d.AddRoundedRect({ center.x - w * 0.5f, center.y - h * 0.5f }, { center.x + w * 0.3f, center.y + h * 0.5f }, color);
                return;
            }
            const float corner = type == "Audio" ? 1.0f : type == "Pose" ? r * 0.45f : r;
            d.AddRoundedRect(center - UIVec2{ r }, center + UIVec2{ r }, fill, glm::vec4(corner), 1.6f * m_View.Zoom, color);
        };
        auto isConnected = [this](uint32_t node, uint32_t pin, bool output)
        {
            return std::any_of(m_Graph->Links.begin(), m_Graph->Links.end(), [&](const GraphLink& l)
            {
                return output ? (l.FromNode == node && l.FromPin == pin) : (l.ToNode == node && l.ToPin == pin);
            });
        };

        // Nodes: the selected ones last, on top.
        std::vector<const GraphNode*> order;
        for (const GraphNode& node : m_Graph->Nodes)
            if (!selected.contains(node.Id))
                order.push_back(&node);
        for (const GraphNode& node : m_Graph->Nodes)
            if (selected.contains(node.Id))
                order.push_back(&node);
        for (const GraphNode* node : order)
        {
            const NodeLayout* layout = LayoutOf(node->Id);
            if (!layout)
                continue;
            const UIVec2 min = GraphToScreen(node->EditorPosition), max = GraphToScreen(UIVec2(node->EditorPosition) + layout->Size);
            if (max.x < b.Min.x || min.x > b.Max.x || max.y < b.Min.y || min.y > b.Max.y)
                continue;
            const bool isSelected = selected.contains(node->Id);
            const bool isHovered = node->Id == m_HoveredNode;

            if (layout->Reroute)
            {
                const std::string type = layout->Outputs.front().Type;
                const UIVec2 center = (min + max) * 0.5f;
                if (isSelected)
                    d.AddRoundedRect(center - UIVec2{ 9.0f * m_View.Zoom }, center + UIVec2{ 9.0f * m_View.Zoom }, { accent.r, accent.g, accent.b, 0.35f }, glm::vec4(9.0f * m_View.Zoom));
                drawPin(center, type, true, isHovered);
                continue;
            }

            const float radius = 8.0f * m_View.Zoom;
            d.AddRoundedRect(min, max, NoxUI::ThemeColor("NodeBody", { 0.11f, 0.13f, 0.18f, 0.97f }), glm::vec4(radius), isSelected ? 2.0f : 1.0f,
                             isSelected ? accent : (isHovered ? UIColor{ border.r + 0.12f, border.g + 0.12f, border.b + 0.14f, 1.0f } : border),
                             NoxUI::ThemeColor("NodeShadow", { 0.0f, 0.0f, 0.0f, 0.45f }), { 0.0f, 4.0f * m_View.Zoom }, 12.0f * m_View.Zoom);
            d.AddRoundedRect(min, { max.x, min.y + kHeader * m_View.Zoom }, layout->Header, { radius, radius, 0.0f, 0.0f });
            if (showText)
            {
                const float titleSize = kTitleFont * m_View.Zoom;
                const UIVec2 titleExtent = NoxUI::UILabel::MeasureText(layout->Title, nullptr, titleSize);
                d.PushClipRect(min, { max.x - 6.0f * m_View.Zoom, min.y + kHeader * m_View.Zoom });
                d.AddText(layout->Title, nullptr, { min.x + 10.0f * m_View.Zoom, min.y + (kHeader * m_View.Zoom - titleExtent.y) * 0.5f }, titleSize, NoxUI::ThemeColor("NodeTitle", { 1.0f, 1.0f, 1.0f, 0.96f }));
                d.PopClipRect();
            }

            const float pinSize = kPinFont * m_View.Zoom;
            for (uint32_t i = 0; i < layout->Inputs.size(); ++i)
            {
                const NodePinDesc& pin = layout->Inputs[i];
                const UIVec2 p = GraphToScreen(PinPosition(*node, *layout, i, false));
                const bool connected = isConnected(node->Id, i, false);
                drawPin(p, pin.Type, connected, m_HoveredPin.Valid() && m_HoveredPin.Node == node->Id && !m_HoveredPin.Output && m_HoveredPin.Pin == i);
                if (!showText)
                    continue;
                const UIVec2 extent = NoxUI::UILabel::MeasureText(pin.Name, nullptr, pinSize);
                d.AddText(pin.Name, nullptr, { p.x + 12.0f * m_View.Zoom, p.y - extent.y * 0.5f }, pinSize, NoxUI::ThemeColor("PinLabel", { 0.86f, 0.88f, 0.92f, 1.0f }));
                // An input without a wire shows the value it uses, in a field it can be changed in.
                if (!connected)
                    if (const auto literal = node->Properties.find(pin.Name); literal != node->Properties.end())
                    {
                        const std::string text = ShortValue(literal->second);
                        const UIVec2 textPos{ p.x + 12.0f * m_View.Zoom + extent.x + 8.0f * m_View.Zoom, p.y - extent.y * 0.5f };
                        const UIVec2 textSize = NoxUI::UILabel::MeasureText(text.empty() ? std::string("...") : text, nullptr, pinSize);
                        const NoxUI::UIRect field{ textPos - UIVec2{ 4.0f * m_View.Zoom, 2.0f * m_View.Zoom }, textPos + textSize + UIVec2{ 4.0f * m_View.Zoom, 2.0f * m_View.Zoom } };
                        const bool fieldHovered = field.Contains(m_DragLast) && m_Drag == Drag::None;
                        const bool fieldActive = m_Drag == Drag::Value && m_ValueDrag.Node == node->Id && m_ValueDrag.Property == pin.Name;
                        d.AddRoundedRect(field.Min, field.Max, { 1.0f, 1.0f, 1.0f, fieldActive ? 0.14f : fieldHovered ? 0.10f : 0.05f }, glm::vec4(3.0f * m_View.Zoom));
                        d.AddText(text.empty() ? std::string("...") : text, nullptr, textPos, pinSize, fieldHovered || fieldActive ? NoxUI::ThemeColor("PinValueHovered", { 0.95f, 0.96f, 1.0f, 1.0f }) : muted);
                        m_ValueHits.push_back({ node->Id, pin.Name, field });
                    }
            }
            for (uint32_t i = 0; i < layout->Outputs.size(); ++i)
            {
                const NodePinDesc& pin = layout->Outputs[i];
                const UIVec2 p = GraphToScreen(PinPosition(*node, *layout, i, true));
                drawPin(p, pin.Type, isConnected(node->Id, i, true), m_HoveredPin.Valid() && m_HoveredPin.Node == node->Id && m_HoveredPin.Output && m_HoveredPin.Pin == i);
                if (!showText)
                    continue;
                const UIVec2 extent = NoxUI::UILabel::MeasureText(pin.Name, nullptr, pinSize);
                d.AddText(pin.Name, nullptr, { p.x - 12.0f * m_View.Zoom - extent.x, p.y - extent.y * 0.5f }, pinSize, NoxUI::ThemeColor("PinLabel", { 0.86f, 0.88f, 0.92f, 1.0f }));
            }
            if (showText)
            {
                const NodeTypeDesc* type = TypeOf(*node);
                if (type && type->OwnsSubGraphs)
                {
                    const std::string states = std::to_string(node->SubGraphs.size()) + (node->SubGraphs.size() == 1 ? " state" : " states") + "  (double-click)";
                    d.AddText(states, nullptr, { min.x + 10.0f * m_View.Zoom, max.y - (kRow + kBottom - 4.0f) * m_View.Zoom }, pinSize, muted);
                }
                if (node->Id == m_Graph->OutputNode)
                {
                    const UIVec2 extent = NoxUI::UILabel::MeasureText("OUTPUT", nullptr, 10.0f * m_View.Zoom);
                    d.AddText("OUTPUT", nullptr, { max.x - extent.x - 8.0f * m_View.Zoom, min.y - extent.y - 2.0f * m_View.Zoom }, 10.0f * m_View.Zoom, NoxUI::ThemeColor("NodeOutputTag", { 0.45f, 0.85f, 0.55f, 1.0f }));
                }
            }
        }

        // The wire being dragged: green over a pin it can join, red over one it cannot.
        if (m_Drag == Drag::Wire)
        {
            const GraphNode* node = FindNode(m_WireFrom.Node);
            const NodeLayout* layout = LayoutOf(m_WireFrom.Node);
            if (node && layout)
            {
                const UIVec2 pinPos = GraphToScreen(PinPosition(*node, *layout, m_WireFrom.Pin, m_WireFrom.Output));
                UIColor color = PinColor(PinType(m_WireFrom));
                if (m_HoveredPin.Valid())
                    color = CanConnect(m_WireFrom, m_HoveredPin, nullptr, nullptr) ? NoxUI::ThemeColor("WireValid", { 0.45f, 0.90f, 0.50f, 1.0f }) : NoxUI::ThemeColor("WireInvalid", { 0.95f, 0.35f, 0.35f, 1.0f });
                if (m_WireFrom.Output)
                    wire(pinPos, m_DragLast, color, 2.5f * std::max(m_View.Zoom, 0.6f));
                else
                    wire(m_DragLast, pinPos, color, 2.5f * std::max(m_View.Zoom, 0.6f));
            }
        }
        if (m_Drag == Drag::Box && m_DragMoved)
        {
            const UIVec2 min = glm::min(m_DragStart, m_DragLast), max = glm::max(m_DragStart, m_DragLast);
            d.AddRoundedRect(min, max, { accent.r, accent.g, accent.b, 0.12f }, glm::vec4(2.0f), 1.0f, accent);
        }

        // Where the view is.
        m_View.PaintZoomLabel(d, b, theme);
        d.PopClipRect();
    }
}
