#pragma once

#include <string>
#include <utility>

#include "NoxCore/NodeGraph/NodeGraphTypes.h"

namespace Nox
{
    // Renders one node Property as an editable widget matching its live NodeGraphValue alternative. Shared by
    // every canvas backend (NoxEditor/src/Panels/NodeGraph/) so property editing looks and behaves the same
    // regardless of which one is active. Returns true if the value changed this frame. A non-null range (from
    // NodeTypeDesc::PropertyRanges) turns a float property into a slider clamped to it.
    bool DrawNodeGraphProperty(const std::string& name, NodeGraphValue& value, const std::pair<float, float>* range = nullptr);
}
