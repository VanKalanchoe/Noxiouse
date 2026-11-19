#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "NoxCore/NodeGraph/NodeGraph.h"
#include "NoxCore/UI/Core/UIElement.h"

namespace Nox
{
    // A state machine node's own view in the graph window (G3): its states as boxes and its transitions as arrows, in Unreal's way
    // (straight arrows between the boxes, two in opposite directions side by side, a round button in the middle of each to pick it).
    //
    //  - Pan: right or middle drag; zoom: wheel around the pointer; F frames the states.
    //  - States: click selects, drag moves, double-click opens its graph (what it plays). The entry state is green, the others red.
    //  - Transitions: drag from a state's round handle (its right edge) onto another state; click an arrow's middle button (or the
    //    arrow) to select it.
    //  - Delete removes the selected state (with its transitions; another becomes the entry) or transition. Right click: on empty
    //    space Add State, on a state Set as Entry State / Open State Graph / Delete State, on a transition Delete Transition.
    // Edits change the GraphNode in place and are reported once each (OnEdited): one undo step for the window.
    class StateMachineViewV2 final : public UI::UIElement
    {
    public:
        StateMachineViewV2();

        // The state machine node to show (null: none) and the graph's domain (a new state gets an empty graph of it). The view frames
        // the states the first time a machine is shown.
        void SetMachine(GraphNode* machine, const std::string& domain);
        GraphNode* GetMachine() const { return m_Machine; }
        void FrameAll();

        uint32_t SelectedState = 0;   // NodeSubGraph::Id, 0: none
        int SelectedTransition = -1;  // index into Transitions, -1: none
        void Select(uint32_t state, int transition);

        // After each finished edit: structural = what the machine does changed; false = only where states are.
        std::function<void(bool structural)> OnEdited;
        std::function<void()> OnSelectionChanged;
        std::function<void(uint32_t stateId)> OnOpenState;
        // The first parameter's name (a new transition starts with a rule on it, so it does not fire the moment its state is active).
        std::function<std::string()> FirstParameter;

        bool OnPointerDown(const UI::UIPointerEvent& e) override;
        bool OnPointerMove(const UI::UIPointerEvent& e) override;
        bool OnPointerUp(const UI::UIPointerEvent& e) override;
        bool OnMouseWheel(const UI::UIPointerEvent& e) override;
        bool OnKeyDown(const UI::UIKeyEvent& e) override;

        // The machine's entry state id (its EntryState property), 0 when none.
        static uint32_t EntryState(const GraphNode& machine);
        static void SetEntryState(GraphNode& machine, uint32_t state);
        // Removes a state, its transitions, and makes another the entry state if it was.
        static void DeleteState(GraphNode& machine, uint32_t state);

    protected:
        UI::UIVec2 ComputeDesiredSize() override { return { 300.0f, 200.0f }; }
        void OnPaint(UI::UIDrawList& drawList, const UI::UITheme& theme) override;

    private:
        struct Box
        {
            UI::UIVec2 Min{ 0.0f }, Max{ 0.0f }; // graph space
        };
        Box BoxOf(const NodeSubGraph& state) const;
        UI::UIVec2 GraphToScreen(const UI::UIVec2& p) const;
        UI::UIVec2 ScreenToGraph(const UI::UIVec2& p) const;
        // A transition's arrow (screen space), from the edge of its From box to the edge of its To box, moved aside when the opposite
        // transition exists too. False when a state is missing.
        bool ArrowOf(size_t transition, UI::UIVec2& from, UI::UIVec2& to) const;
        uint32_t StateAt(const UI::UIVec2& screen) const;
        uint32_t HandleAt(const UI::UIVec2& screen) const;
        int TransitionAt(const UI::UIVec2& screen) const;
        void AddState(const UI::UIVec2& screen);
        void OpenMenu(const UI::UIVec2& screen);
        void Edited(bool structural);

        GraphNode* m_Machine = nullptr;
        std::string m_Domain;
        UI::UIVec2 m_Pan{ 0.0f };
        float m_Zoom = 1.0f;
        bool m_NeedsFrame = true;

        enum class Drag : uint8_t { None, Pan, State, Transition };
        Drag m_Drag = Drag::None;
        UI::UIVec2 m_DragStart{ 0.0f }, m_DragLast{ 0.0f };
        glm::vec2 m_DragOrigin{ 0.0f };
        bool m_DragMoved = false;
        int m_PanButton = 0;
        uint32_t m_DragState = 0;
        uint32_t m_HoveredState = 0;
        int m_HoveredTransition = -1;
        uint32_t m_LastClickState = 0;
        std::chrono::steady_clock::time_point m_LastClickTime{};
    };
}
