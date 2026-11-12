#include "NodeGraphPropertyEditor.h"

#include <imgui.h>

#include <algorithm>

#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    bool DrawNodeGraphProperty(const std::string& name, NodeGraphValue& value, const std::pair<float, float>* range, const std::vector<std::string>* options)
    {
        if (bool* b = std::get_if<bool>(&value))
            return ImGui::Checkbox(name.c_str(), b);
        if (int32_t* i = std::get_if<int32_t>(&value))
        {
            // Options: a slider over them labelled with the chosen one's name (no popup: combos do not work inside imgui-node-editor
            // nodes, see the uint64_t case below).
            if (options && !options->empty())
            {
                *i = std::clamp(*i, 0, static_cast<int32_t>(options->size()) - 1);
                return ImGui::SliderInt(name.c_str(), i, 0, static_cast<int32_t>(options->size()) - 1, (*options)[*i].c_str(), ImGuiSliderFlags_AlwaysClamp);
            }
            return ImGui::DragInt(name.c_str(), i);
        }
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
        if (NodeGraphCurve* curve = std::get_if<NodeGraphCurve>(&value))
        {
            // A small plot of the curve over its keys' range, then the keys (x, y) to drag, + / - and Smooth. (Inside a node only
            // drag widgets work reliably: imgui-node-editor takes the clicks of free-drawn canvases.)
            bool changed = false;
            ImGui::TextUnformatted(name.c_str());
            const ImVec2 size(200.0f, 80.0f);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Dummy(size);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(25, 25, 30, 255));
            if (!curve->Keys.empty())
            {
                float x0 = curve->Keys.front().x, x1 = curve->Keys.back().x, y0 = curve->Keys.front().y, y1 = y0;
                for (const glm::vec2& key : curve->Keys)
                {
                    y0 = std::min(y0, key.y);
                    y1 = std::max(y1, key.y);
                }
                if (x1 - x0 < 1e-6f) x1 = x0 + 1.0f;
                if (y1 - y0 < 1e-6f) { y0 -= 0.5f; y1 += 0.5f; }
                auto toScreen = [&](float x, float y) { return ImVec2(origin.x + 4.0f + (x - x0) / (x1 - x0) * (size.x - 8.0f), origin.y + size.y - 4.0f - (y - y0) / (y1 - y0) * (size.y - 8.0f)); };
                ImVec2 previous = toScreen(x0, curve->Evaluate(x0));
                for (int i = 1; i <= 64; ++i)
                {
                    const float x = x0 + (x1 - x0) * static_cast<float>(i) / 64.0f;
                    const ImVec2 point = toScreen(x, curve->Evaluate(x));
                    draw->AddLine(previous, point, IM_COL32(120, 200, 255, 255), 1.5f);
                    previous = point;
                }
                for (const glm::vec2& key : curve->Keys)
                    draw->AddCircleFilled(toScreen(key.x, key.y), 3.0f, IM_COL32(255, 200, 70, 255));
            }
            int remove = -1;
            bool sort = false; // (when a drag ends: re-sorting under the cursor would hand the drag to another key)
            for (int i = 0; i < static_cast<int>(curve->Keys.size()); ++i)
            {
                ImGui::PushID(i);
                if (ImGui::DragFloat2("##key", &curve->Keys[i].x, 0.01f))
                    changed = true;
                sort |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                    remove = i;
                ImGui::PopID();
            }
            if (remove >= 0)
            {
                curve->Keys.erase(curve->Keys.begin() + remove);
                changed = true;
            }
            if (ImGui::SmallButton("+ Key"))
            {
                curve->Keys.push_back(curve->Keys.empty() ? glm::vec2(0.0f) : curve->Keys.back() + glm::vec2(1.0f, 0.0f));
                changed = sort = true;
            }
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Smooth", &curve->Smooth);
            if (sort)
                std::stable_sort(curve->Keys.begin(), curve->Keys.end(), [](const glm::vec2& a, const glm::vec2& b) { return a.x < b.x; });
            return changed;
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
