#include "StateMachineViewV2.h"

#include <algorithm>
#include <cmath>
#include <format>

#include <SDL3/SDL_scancode.h>

#include "NoxCore/Animation/AnimationGraphNodes.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Rendering/UIIcons.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIMenu.h"

namespace Nox
{
    namespace
    {
        using UI::UIColor;
        using UI::UIVec2;

        constexpr float kHeader = 26.0f;
        constexpr float kBoxHeight = 50.0f;
        constexpr float kMinWidth = 140.0f;
        constexpr float kTitleFont = 13.5f;
        constexpr float kHandleRadius = 6.0f;
        constexpr float kButtonRadius = 7.0f; // a transition's middle button

        float SegmentDistance(const UIVec2& p, const UIVec2& a, const UIVec2& b)
        {
            const UIVec2 ab = b - a;
            const float t = glm::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-6f), 0.0f, 1.0f);
            return glm::length(p - (a + ab * t));
        }

        // Where the line from a box's centre towards `toward` leaves the box.
        UIVec2 BoxEdge(const UIVec2& min, const UIVec2& max, const UIVec2& toward)
        {
            const UIVec2 center = (min + max) * 0.5f;
            const UIVec2 half = (max - min) * 0.5f;
            const UIVec2 d = toward - center;
            if (std::abs(d.x) < 1e-4f && std::abs(d.y) < 1e-4f)
                return center;
            const float sx = std::abs(d.x) > 1e-4f ? half.x / std::abs(d.x) : 1e9f;
            const float sy = std::abs(d.y) > 1e-4f ? half.y / std::abs(d.y) : 1e9f;
            return center + d * std::min(sx, sy);
        }

        NodeGraph MakeEmptyStateGraph(const std::string& domain)
        {
            if (domain == kAnimationGraphDomain)
                return CreateEmptyAnimationGraph();
            NodeGraph graph;
            graph.Domain = domain;
            return graph;
        }
    }

    StateMachineViewV2::StateMachineViewV2()
    {
        HitTestMode = UI::UIHitTestMode::Self;
        Focusable = true;
        ClipChildren = true;
        WidthMode = UI::UISizeMode::Fill;
        HeightMode = UI::UISizeMode::Fill;
    }

    uint32_t StateMachineViewV2::EntryState(const GraphNode& machine)
    {
        const auto entry = machine.Properties.find("EntryState");
        const int32_t* value = entry != machine.Properties.end() ? std::get_if<int32_t>(&entry->second) : nullptr;
        return value ? static_cast<uint32_t>(*value) : 0;
    }

    void StateMachineViewV2::SetEntryState(GraphNode& machine, uint32_t state)
    {
        machine.Properties["EntryState"] = static_cast<int32_t>(state);
    }

    void StateMachineViewV2::DeleteState(GraphNode& machine, uint32_t state)
    {
        const bool wasEntry = EntryState(machine) == state;
        std::erase_if(machine.SubGraphs, [&](const NodeSubGraph& s) { return s.Id == state; });
        std::erase_if(machine.Transitions, [&](const NodeTransition& t) { return t.FromState == state || t.ToState == state; });
        if (wasEntry)
            SetEntryState(machine, machine.SubGraphs.empty() ? 0 : machine.SubGraphs.front().Id);
    }

    void StateMachineViewV2::SetMachine(GraphNode* machine, const std::string& domain)
    {
        if (machine == m_Machine && domain == m_Domain)
            return;
        const bool another = !machine || !m_Machine || machine->Id != m_Machine->Id;
        m_Machine = machine;
        m_Domain = domain;
        if (another)
        {
            SelectedState = 0;
            SelectedTransition = -1;
            m_NeedsFrame = true;
            m_Drag = Drag::None;
        }
        else if (m_Machine && SelectedTransition >= static_cast<int>(m_Machine->Transitions.size()))
            SelectedTransition = -1; // (the same machine, replaced: an undo)
    }

    void StateMachineViewV2::Select(uint32_t state, int transition)
    {
        if (state == SelectedState && transition == SelectedTransition)
            return;
        SelectedState = state;
        SelectedTransition = transition;
        if (OnSelectionChanged)
            OnSelectionChanged();
    }

    void StateMachineViewV2::Edited(bool structural)
    {
        if (OnEdited)
            OnEdited(structural);
    }

    StateMachineViewV2::Box StateMachineViewV2::BoxOf(const NodeSubGraph& state) const
    {
        const std::string name = state.Name.empty() ? std::string("State") : state.Name;
        const float width = std::max(kMinWidth, UI::UILabel::MeasureText(name, nullptr, kTitleFont).x + 40.0f);
        return { state.EditorPosition, UIVec2(state.EditorPosition) + UIVec2{ width, kBoxHeight } };
    }

    UIVec2 StateMachineViewV2::GraphToScreen(const UIVec2& p) const { return m_View.ToScreen(GetBounds(), p); }
    UIVec2 StateMachineViewV2::ScreenToGraph(const UIVec2& p) const { return m_View.ToGraph(GetBounds(), p); }

    bool StateMachineViewV2::ArrowOf(size_t index, UIVec2& from, UIVec2& to) const
    {
        if (!m_Machine || index >= m_Machine->Transitions.size())
            return false;
        const NodeTransition& transition = m_Machine->Transitions[index];
        const NodeSubGraph* a = nullptr;
        const NodeSubGraph* b = nullptr;
        for (const NodeSubGraph& state : m_Machine->SubGraphs)
        {
            if (state.Id == transition.FromState)
                a = &state;
            if (state.Id == transition.ToState)
                b = &state;
        }
        if (!a || !b || a == b)
            return false;
        const Box boxA = BoxOf(*a), boxB = BoxOf(*b);
        const UIVec2 minA = GraphToScreen(boxA.Min), maxA = GraphToScreen(boxA.Max);
        const UIVec2 minB = GraphToScreen(boxB.Min), maxB = GraphToScreen(boxB.Max);
        const UIVec2 centerA = (minA + maxA) * 0.5f, centerB = (minB + maxB) * 0.5f;
        // Beside the line between the centres: always to the arrow's right, so A -> B and B -> A run side by side; transitions
        // joining the same two states the same way spread further out.
        UIVec2 direction = centerB - centerA;
        const float length = glm::length(direction);
        direction = length > 1e-3f ? direction / length : UIVec2{ 1.0f, 0.0f };
        const UIVec2 side{ -direction.y, direction.x };
        int lane = 0;
        bool opposite = false;
        for (size_t j = 0; j < m_Machine->Transitions.size(); ++j)
        {
            const NodeTransition& other = m_Machine->Transitions[j];
            if (j < index && other.FromState == transition.FromState && other.ToState == transition.ToState)
                ++lane;
            if (other.FromState == transition.ToState && other.ToState == transition.FromState)
                opposite = true;
        }
        const float offset = ((opposite ? 6.0f : 0.0f) + 10.0f * static_cast<float>(lane)) * std::max(m_View.Zoom, 0.5f);
        const UIVec2 shift = side * offset;
        from = BoxEdge(minA, maxA, centerB) + shift;
        to = BoxEdge(minB, maxB, centerA) + shift;
        return true;
    }

    uint32_t StateMachineViewV2::StateAt(const UIVec2& screen) const
    {
        if (!m_Machine)
            return 0;
        const UIVec2 p = ScreenToGraph(screen);
        for (auto it = m_Machine->SubGraphs.rbegin(); it != m_Machine->SubGraphs.rend(); ++it)
        {
            const Box box = BoxOf(*it);
            if (p.x >= box.Min.x && p.x <= box.Max.x && p.y >= box.Min.y && p.y <= box.Max.y)
                return it->Id;
        }
        return 0;
    }

    uint32_t StateMachineViewV2::HandleAt(const UIVec2& screen) const
    {
        if (!m_Machine)
            return 0;
        for (auto it = m_Machine->SubGraphs.rbegin(); it != m_Machine->SubGraphs.rend(); ++it)
        {
            const Box box = BoxOf(*it);
            const UIVec2 handle = GraphToScreen({ box.Max.x, (box.Min.y + kHeader + box.Max.y) * 0.5f });
            if (glm::length(handle - screen) <= (kHandleRadius + 3.0f) * std::max(m_View.Zoom, 0.6f))
                return it->Id;
        }
        return 0;
    }

    int StateMachineViewV2::TransitionAt(const UIVec2& screen) const
    {
        if (!m_Machine)
            return -1;
        int nearest = -1;
        float best = 6.0f;
        for (size_t i = 0; i < m_Machine->Transitions.size(); ++i)
        {
            UIVec2 from, to;
            if (!ArrowOf(i, from, to))
                continue;
            const float button = glm::length((from + to) * 0.5f - screen) - kButtonRadius * std::max(m_View.Zoom, 0.6f);
            const float distance = std::min(SegmentDistance(screen, from, to), std::max(button, 0.0f));
            if (distance < best)
            {
                best = distance;
                nearest = static_cast<int>(i);
            }
        }
        return nearest;
    }

    void StateMachineViewV2::FrameAll()
    {
        const UI::UIRect& b = GetBounds();
        if (!m_Machine || b.Width() < 10.0f || b.Height() < 10.0f)
            return;
        m_NeedsFrame = false;
        if (m_Machine->SubGraphs.empty())
        {
            m_View.Zoom = 1.0f;
            m_View.Pan = { b.Width() * 0.3f, b.Height() * 0.4f };
            return;
        }
        UIVec2 min{ 1e9f }, max{ -1e9f };
        for (const NodeSubGraph& state : m_Machine->SubGraphs)
        {
            const Box box = BoxOf(state);
            min = glm::min(min, box.Min);
            max = glm::max(max, box.Max);
        }
        m_View.Fit(b, min, max, 0.3f, 1.5f, 60.0f);
    }

    void StateMachineViewV2::AddState(const UIVec2& screen)
    {
        if (!m_Machine)
            return;
        NodeSubGraph state;
        for (const NodeSubGraph& existing : m_Machine->SubGraphs)
            state.Id = std::max(state.Id, existing.Id);
        state.Id += 1;
        state.Name = "State " + std::to_string(state.Id);
        state.Graph = MakeEmptyStateGraph(m_Domain);
        state.EditorPosition = ScreenToGraph(screen);
        if (m_Machine->SubGraphs.empty())
            SetEntryState(*m_Machine, state.Id);
        const uint32_t id = state.Id;
        m_Machine->SubGraphs.push_back(std::move(state));
        Select(id, -1);
        Edited(true);
    }

    void StateMachineViewV2::OpenMenu(const UIVec2& screen)
    {
        UI::UIContext* ctx = UI::UIContext::GetCurrent();
        if (!ctx || !m_Machine)
            return;
        std::vector<UI::UIMenuItem> items;
        const UI::UIRect at{ screen, screen + UIVec2{ 1.0f } };
        if (const uint32_t state = StateAt(screen); state != 0)
        {
            Select(state, -1);
            const bool entry = EntryState(*m_Machine) == state;
            items.push_back({ "Open State Graph", "Double-click", [this, state]() { if (OnOpenState) OnOpenState(state); } });
            items.push_back({ entry ? "Entry State" : "Set as Entry State", "", [this, state]()
            {
                if (!m_Machine)
                    return;
                SetEntryState(*m_Machine, state);
                Edited(true);
            }, false, !entry, false, false, "The state the machine starts in" });
            items.push_back({ "", "", {}, true });
            items.push_back({ "Delete State", "Del", [this, state]()
            {
                if (!m_Machine)
                    return;
                DeleteState(*m_Machine, state);
                Select(0, -1);
                Edited(true);
            } });
        }
        else if (const int transition = TransitionAt(screen); transition >= 0)
        {
            Select(0, transition);
            items.push_back({ "Delete Transition", "Del", [this, transition]()
            {
                if (!m_Machine || transition >= static_cast<int>(m_Machine->Transitions.size()))
                    return;
                m_Machine->Transitions.erase(m_Machine->Transitions.begin() + transition);
                Select(0, -1);
                Edited(true);
            } });
        }
        else
            items.push_back({ "Add State", "", [this, screen]() { AddState(screen); }, false, true, false, false, "A state with an empty graph of its own" });
        UI::UIMenuPopup::ShowPopupMenu(*ctx, at, std::move(items));
    }

    bool StateMachineViewV2::OnPointerDown(const UI::UIPointerEvent& e)
    {
        if (!m_Machine)
            return true;
        if (UI::UIContext* ctx = UI::UIContext::GetCurrent())
        {
            ctx->SetFocus(this);
            ctx->CancelPendingDrag();
        }
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

        if (const uint32_t handle = HandleAt(e.Position); handle != 0)
        {
            m_Drag = Drag::Transition;
            m_DragState = handle;
            return true;
        }
        if (const uint32_t state = StateAt(e.Position); state != 0)
        {
            if (quick && state == m_LastClickState)
            {
                m_LastClickState = 0;
                if (OnOpenState)
                    OnOpenState(state);
                return true;
            }
            m_LastClickState = state;
            Select(state, -1);
            m_Drag = Drag::State;
            m_DragState = state;
            for (const NodeSubGraph& s : m_Machine->SubGraphs)
                if (s.Id == state)
                    m_DragOrigin = s.EditorPosition;
            return true;
        }
        m_LastClickState = 0;
        Select(0, TransitionAt(e.Position));
        return true;
    }

    bool StateMachineViewV2::OnPointerMove(const UI::UIPointerEvent& e)
    {
        if (!m_Machine)
            return true;
        if (glm::length(e.Position - m_DragStart) > 3.0f)
            m_DragMoved = true;
        switch (m_Drag)
        {
        case Drag::Pan:
            m_View.Pan += e.Position - m_DragLast;
            break;
        case Drag::State:
            for (NodeSubGraph& state : m_Machine->SubGraphs)
                if (state.Id == m_DragState)
                    state.EditorPosition = m_DragOrigin + glm::vec2((e.Position - m_DragStart) / m_View.Zoom);
            break;
        case Drag::Transition:
            m_HoveredState = StateAt(e.Position);
            break;
        default:
            m_HoveredState = HandleAt(e.Position) != 0 ? 0 : StateAt(e.Position);
            m_HoveredTransition = m_HoveredState != 0 ? -1 : TransitionAt(e.Position);
            Tooltip = HandleAt(e.Position) != 0 ? "Drag onto another state: a transition"
                    : m_HoveredState != 0 ? "Double-click: open what this state plays"
                    : m_HoveredTransition >= 0 ? "Click: edit its rules in Details" : std::string();
            break;
        }
        m_DragLast = e.Position;
        return true;
    }

    bool StateMachineViewV2::OnPointerUp(const UI::UIPointerEvent& e)
    {
        if (!m_Machine)
            return true;
        const Drag drag = m_Drag;
        m_Drag = Drag::None;
        switch (drag)
        {
        case Drag::Pan:
            if (!m_DragMoved && m_PanButton == 3)
                OpenMenu(e.Position);
            break;
        case Drag::State:
            if (m_DragMoved)
                Edited(false);
            break;
        case Drag::Transition:
        {
            const uint32_t target = StateAt(e.Position);
            if (target != 0 && target != m_DragState)
            {
                NodeTransition transition;
                transition.FromState = m_DragState;
                transition.ToState = target;
                // A transition without a rule fires the moment its state is active: it starts with one on the first parameter.
                if (const std::string first = FirstParameter ? FirstParameter() : std::string(); !first.empty())
                    transition.Rules.push_back({ first, TransitionCompare::Greater, 0.0f });
                m_Machine->Transitions.push_back(std::move(transition));
                Select(0, static_cast<int>(m_Machine->Transitions.size()) - 1);
                Edited(true);
            }
            m_HoveredState = 0;
            break;
        }
        default:
            break;
        }
        return true;
    }

    bool StateMachineViewV2::OnMouseWheel(const UI::UIPointerEvent& e)
    {
        const UI::UIContext* ctx = UI::UIContext::GetCurrent();
        const UIVec2 pointer = ctx ? ctx->GetLastMousePos() : GetBounds().Center();
        m_View.ZoomAt(GetBounds(), pointer, e.ScrollDelta.y);
        return true;
    }

    bool StateMachineViewV2::OnKeyDown(const UI::UIKeyEvent& e)
    {
        if (!m_Machine)
            return false;
        if (e.KeyCode == SDL_SCANCODE_F)
        {
            FrameAll();
            return true;
        }
        if (e.KeyCode == SDL_SCANCODE_DELETE || e.KeyCode == SDL_SCANCODE_BACKSPACE)
        {
            if (SelectedTransition >= 0 && SelectedTransition < static_cast<int>(m_Machine->Transitions.size()))
            {
                m_Machine->Transitions.erase(m_Machine->Transitions.begin() + SelectedTransition);
                Select(0, -1);
                Edited(true);
                return true;
            }
            if (SelectedState != 0)
            {
                DeleteState(*m_Machine, SelectedState);
                Select(0, -1);
                Edited(true);
                return true;
            }
        }
        return false;
    }

    void StateMachineViewV2::OnPaint(UI::UIDrawList& d, const UI::UITheme& theme)
    {
        const UI::UIRect& b = GetBounds();
        if (m_NeedsFrame && m_Machine && b.Width() > 10.0f)
            FrameAll();
        const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        const UIColor border = theme.GetToken("Border", { 0.20f, 0.235f, 0.32f, 1.0f });
        const UIColor muted = theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f });
        d.AddRoundedRect(b.Min, b.Max, theme.GetToken("Background", { 0.063f, 0.075f, 0.106f, 1.0f }));
        d.PushClipRect(b.Min, b.Max);
        m_View.PaintGrid(d, b);
        if (!m_Machine)
        {
            d.PopClipRect();
            return;
        }
        const float z = std::max(m_View.Zoom, 0.6f);
        const auto arrow = [&](const UIVec2& from, const UIVec2& to, const UIColor& color, float width)
        {
            UIVec2 direction = to - from;
            const float length = glm::length(direction);
            if (length < 1.0f)
                return;
            direction /= length;
            const UIVec2 side{ -direction.y, direction.x };
            const float head = 10.0f * z;
            const UIVec2 base = to - direction * head;
            UI::Icons::DrawSegment(d, from, base, width, color);
            UI::Icons::DrawSegment(d, to, base + side * head * 0.5f, width, color);
            UI::Icons::DrawSegment(d, to, base - side * head * 0.5f, width, color);
            UI::Icons::DrawSegment(d, base + side * head * 0.5f, base - side * head * 0.5f, width, color);
        };

        // Transitions under the states, each with its round button in the middle (the rule count in it).
        for (size_t i = 0; i < m_Machine->Transitions.size(); ++i)
        {
            UIVec2 from, to;
            if (!ArrowOf(i, from, to))
                continue;
            const bool selected = static_cast<int>(i) == SelectedTransition;
            const bool hovered = static_cast<int>(i) == m_HoveredTransition;
            const UIColor color = selected ? UI::ThemeColor("TransitionSelected", { 1.0f, 0.69f, 0.2f, 1.0f }) : hovered ? UI::ThemeColor("TransitionHovered", { 0.2f, 0.69f, 1.0f, 1.0f }) : UI::ThemeColor("Transition", { 0.78f, 0.78f, 0.80f, 1.0f });
            arrow(from, to, color, (selected || hovered ? 2.6f : 1.8f) * z);
            const UIVec2 middle = (from + to) * 0.5f;
            const float r = kButtonRadius * z;
            const NodeTransition& transition = m_Machine->Transitions[i];
            d.AddRoundedRect(middle - UIVec2{ r }, middle + UIVec2{ r }, UI::ThemeColor("TransitionMarker", { 0.12f, 0.14f, 0.19f, 1.0f }), glm::vec4(r), 1.5f * z, color);
            if (m_View.Zoom >= 0.5f)
            {
                const std::string count = transition.AtStateEnd && transition.Rules.empty() ? std::string("E") : std::to_string(transition.Rules.size());
                const UIVec2 extent = UI::UILabel::MeasureText(count, nullptr, 10.0f * z);
                d.AddText(count, nullptr, middle - extent * 0.5f, 10.0f * z, color);
            }
        }

        // The transition being dragged out of a handle.
        if (m_Drag == Drag::Transition)
            for (const NodeSubGraph& state : m_Machine->SubGraphs)
                if (state.Id == m_DragState)
                {
                    const Box box = BoxOf(state);
                    const UIVec2 start = GraphToScreen({ box.Max.x, (box.Min.y + kHeader + box.Max.y) * 0.5f });
                    const bool valid = m_HoveredState != 0 && m_HoveredState != m_DragState;
                    arrow(start, m_DragLast, valid ? UI::ThemeColor("WireValid", { 0.45f, 0.9f, 0.5f, 1.0f }) : accent, 2.2f * z);
                }

        // States.
        const uint32_t entry = EntryState(*m_Machine);
        const bool showText = m_View.Zoom >= 0.4f;
        for (const NodeSubGraph& state : m_Machine->SubGraphs)
        {
            const Box box = BoxOf(state);
            const UIVec2 min = GraphToScreen(box.Min), max = GraphToScreen(box.Max);
            if (max.x < b.Min.x || min.x > b.Max.x || max.y < b.Min.y || min.y > b.Max.y)
                continue;
            const bool selected = state.Id == SelectedState;
            const bool hovered = state.Id == m_HoveredState;
            const bool isEntry = state.Id == entry;
            const float radius = 8.0f * m_View.Zoom;
            d.AddRoundedRect(min, max, UI::ThemeColor("NodeBody", { 0.11f, 0.13f, 0.18f, 0.97f }), glm::vec4(radius), selected ? 2.0f : 1.0f,
                             selected ? accent : (hovered ? UIColor{ border.r + 0.12f, border.g + 0.12f, border.b + 0.14f, 1.0f } : border),
                             UI::ThemeColor("NodeShadow", { 0.0f, 0.0f, 0.0f, 0.45f }), { 0.0f, 4.0f * m_View.Zoom }, 12.0f * m_View.Zoom);
            d.AddRoundedRect(min, { max.x, min.y + kHeader * m_View.Zoom }, isEntry ? UI::ThemeColor("StateHeaderEntry", { 0.22f, 0.53f, 0.35f, 1.0f }) : UI::ThemeColor("StateHeader", { 0.59f, 0.24f, 0.24f, 1.0f }),
                             { radius, radius, 0.0f, 0.0f });
            if (showText)
            {
                const std::string name = state.Name.empty() ? std::string("State") : state.Name;
                const float size = kTitleFont * m_View.Zoom;
                const UIVec2 extent = UI::UILabel::MeasureText(name, nullptr, size);
                d.AddText(name, nullptr, { min.x + 10.0f * m_View.Zoom, min.y + (kHeader * m_View.Zoom - extent.y) * 0.5f }, size, UI::ThemeColor("NodeTitle", { 1.0f, 1.0f, 1.0f, 0.96f }));
                const std::string info = std::to_string(state.Graph.Nodes.size()) + (state.Graph.Nodes.size() == 1 ? " node" : " nodes") + (isEntry ? "   entry" : "");
                d.AddText(info, nullptr, { min.x + 10.0f * m_View.Zoom, min.y + (kHeader + 5.0f) * m_View.Zoom }, 11.5f * m_View.Zoom, muted);
            }
            // The handle transitions are dragged out of.
            const UIVec2 handle = GraphToScreen({ box.Max.x, (box.Min.y + kHeader + box.Max.y) * 0.5f });
            const float r = kHandleRadius * z;
            d.AddRoundedRect(handle - UIVec2{ r }, handle + UIVec2{ r }, UI::ThemeColor("StateHandle", { 0.08f, 0.10f, 0.14f, 1.0f }), glm::vec4(r), 1.6f * z, UI::ThemeColor("StateHandleBorder", { 0.9f, 0.9f, 0.9f, 1.0f }));
        }
        if (m_Machine->SubGraphs.empty())
            d.AddText("No states: right-click > Add State.", nullptr, b.Min + UIVec2{ 12.0f, 12.0f }, 13.5f, muted);
        m_View.PaintZoomLabel(d, b, theme);
        d.PopClipRect();
    }
}
