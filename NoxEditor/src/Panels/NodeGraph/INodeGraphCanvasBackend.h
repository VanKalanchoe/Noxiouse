#pragma once

#include <functional>

#include "NoxCore/NodeGraph/NodeGraph.h"

namespace Nox
{
    // ImGui drag-drop payload type the panel's Parameters list drags (the parameter's name, as a null-terminated
    // string). A backend accepts it over its canvas and creates a node reading that parameter where it's dropped
    // -- the panel doesn't know how a canvas turns screen positions into graph positions, so the drop lives here.
    inline constexpr const char* kNodeGraphParameterPayload = "NODEGRAPH_PARAMETER";

    // What NodeGraphEditorPanel needs from a node-graph canvas implementation. Kept deliberately small so a
    // different canvas library can be swapped in later (docs/Animation_Graph_Architecture_Plan_2026.md Step 3)
    // by adding a new backend here, without touching the panel or the domain-agnostic NodeGraph core it edits.
    // One instance per open graph editor window.
    class INodeGraphCanvasBackend
    {
    public:
        virtual ~INodeGraphCanvasBackend() = default;

        // Draws the whole canvas for one frame, inside the panel's already-open ImGui window (Begin/BeginMenuBar
        // already called). Reads and mutates graph directly -- node/link add/remove, position changes, property
        // edits -- and calls onGraphEdited after any change, so the panel can mark itself dirty and recompile
        // the live asset. Node types come from NodeTypeRegistry, filtered to graph.Domain.
        // outOpenNode (optional) is set to the id of a node that owns sub graphs when the user double-clicked it.
        virtual void Draw(NodeGraph& graph, const std::function<void()>& onGraphEdited, uint32_t* outOpenNode = nullptr) = 0;

        // What the state-machine view has selected, for the panel's inspector below it.
        struct StateMachineSelection
        {
            int Transition = -1; // index into GraphNode::Transitions, -1 = none
            uint32_t State = 0;  // NodeSubGraph::Id, 0 = none
        };

        // Draws a node's sub graphs as boxes (states) and its transitions as arrows between them, inside the panel's
        // already-open child window. Edits `machine` directly (add / delete / move states, connect transitions) and calls
        // onGraphEdited. rootGraph supplies the parameters a new transition's default rule uses. outOpenState is set to a
        // state's id when it is double-clicked.
        virtual void DrawStateMachine(GraphNode& machine, const NodeGraph& rootGraph, const std::function<void()>& onGraphEdited,
                                      uint32_t& outOpenState, StateMachineSelection& selection) = 0;

        // The panel switched to another graph or view: forget per-view canvas state (node positions seeded, fit-to-content).
        virtual void ResetView() = 0;
    };
}
