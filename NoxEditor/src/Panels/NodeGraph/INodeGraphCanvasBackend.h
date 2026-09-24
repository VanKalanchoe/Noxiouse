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
        virtual void Draw(NodeGraph& graph, const std::function<void()>& onGraphEdited) = 0;
    };
}
