#include "NodeGraphPropertyEditor.h"

#include <imgui.h>

#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    bool DrawNodeGraphProperty(const std::string& name, NodeGraphValue& value, const std::pair<float, float>* range)
    {
        if (bool* b = std::get_if<bool>(&value))
            return ImGui::Checkbox(name.c_str(), b);
        if (int32_t* i = std::get_if<int32_t>(&value))
            return ImGui::DragInt(name.c_str(), i);
        if (float* f = std::get_if<float>(&value))
        {
            if (range)
                return ImGui::SliderFloat(name.c_str(), f, range->first, range->second, "%.3f", ImGuiSliderFlags_AlwaysClamp); // Ctrl+click typing can't escape the range
            return ImGui::DragFloat(name.c_str(), f, 0.01f);
        }
        if (glm::vec2* v = std::get_if<glm::vec2>(&value))
            return ImGui::DragFloat2(name.c_str(), &v->x, 0.01f);
        if (uint64_t* u = std::get_if<uint64_t>(&value))
        {
            // AssetHandle-shaped values (e.g. a Clip node's "Clip" property): shown read-only here. A plain
            // ImGui::BeginCombo doesn't work inside an imgui-node-editor node -- the popup opens in the wrong
            // place and isn't clickable, a known upstream limitation with no fix other than a documented
            // workaround (a button that defers the popup to after EndNode(), wrapped in ed::Suspend/Resume; see
            // github.com/thedmd/imgui-node-editor issues #48/#154). That's backend-specific, so it lives in
            // ThedmdCanvasBackend, which handles uint64_t properties itself before ever calling this function.
            std::string label = std::to_string(*u);
            if (*u != 0 && Project::GetActive())
            {
                const auto& registry = Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry();
                auto it = registry.find(*u);
                if (it != registry.end())
                    label = it->second.FilePath.stem().string();
            }
            ImGui::Text("%s: %s", name.c_str(), label.c_str());
            return false;
        }
        if (std::string* s = std::get_if<std::string>(&value))
        {
            char buffer[256];
            strncpy_s(buffer, s->c_str(), sizeof(buffer) - 1);
            if (ImGui::InputText(name.c_str(), buffer, sizeof(buffer)))
            {
                *s = buffer;
                return true;
            }
            return false;
        }
        return false;
    }
}
