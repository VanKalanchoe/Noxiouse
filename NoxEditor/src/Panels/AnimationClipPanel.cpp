#include "AnimationClipPanel.h"

#include <algorithm>
#include <imgui.h>
#include "misc/cpp/imgui_stdlib.h"

#include "NoxCore/Animation/AnimationSequence.h"
#include "NoxCore/Asset/AnimationImporter.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    AnimationClipPanel::AnimationClipPanel(AssetHandle clip)
        : m_Clip(clip)
    {
        m_Title = AssetManager::GetMetadata(clip).FilePath.stem().string() + " (Animation Clip)##" + std::to_string(static_cast<uint64_t>(clip));
    }

    void AnimationClipPanel::Save()
    {
        const AnimationSequence* clip = AssetManager::FindLoadedAsset<AnimationSequence>(m_Clip);
        if (!clip)
            return;
        const std::filesystem::path path = Project::GetActiveAssetDirectory() / AssetManager::GetMetadata(m_Clip).FilePath;
        if (!AnimationImporter::SaveNotifies(path, clip->Notifies))
            NOX_CORE_WARN("Could not save the notifies of {}", path.string());
    }

    void AnimationClipPanel::OnImGuiRender()
    {
        ImGui::SetNextWindowSize(ImVec2(520.0f, 380.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(m_Title.c_str(), &m_Open))
        {
            ImGui::End();
            return;
        }

        AnimationSequence* clip = AssetManager::FindLoadedAsset<AnimationSequence>(m_Clip);
        if (!clip)
        {
            AssetManager::RequestAsset(m_Clip);
            ImGui::TextDisabled("Loading...");
            ImGui::End();
            return;
        }

        const float duration = std::max(clip->Duration, 1e-3f);
        ImGui::Text("%s", AssetManager::GetMetadata(m_Clip).FilePath.generic_string().c_str());
        ImGui::TextDisabled("%.3f s, %zu channels, %.0f ticks per second", clip->Duration, clip->Channels.size(), clip->TicksPerSecond);

        // Timeline: the playhead (drag) and a marker per notify.
        ImGui::SeparatorText("Timeline");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##cursor", &m_Cursor, 0.0f, duration, "%.3f s");
        {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            for (const AnimationNotify& notify : clip->Notifies)
            {
                const float x = min.x + (max.x - min.x) * std::clamp(notify.Time / duration, 0.0f, 1.0f);
                drawList->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), IM_COL32(255, 190, 60, 255), 2.0f);
            }
        }

        ImGui::SeparatorText("Notifies");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("##new_notify", m_NewName, sizeof(m_NewName));
        ImGui::SameLine();
        bool changed = false;
        if (ImGui::Button("Add at Playhead") && m_NewName[0] != '\0')
        {
            clip->Notifies.push_back({ m_Cursor, m_NewName });
            changed = true;
        }
        ImGui::SetItemTooltip("Scripts on the animated entity receive OnAnimationEvent(\"%s\") when a playhead crosses it.", m_NewName);

        if (ImGui::BeginTable("##notifies", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, std::max(ImGui::GetContentRegionAvail().y, 80.0f))))
        {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableHeadersRow();
            int remove = -1;
            for (int i = 0; i < static_cast<int>(clip->Notifies.size()); ++i)
            {
                AnimationNotify& notify = clip->Notifies[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::DragFloat("##time", &notify.Time, 0.005f, 0.0f, duration, "%.3f s");
                changed |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##name", &notify.Name);
                changed |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::TableSetColumnIndex(2);
                if (ImGui::SmallButton("Go to"))
                    m_Cursor = notify.Time;
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete"))
                    remove = i;
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (remove >= 0)
            {
                clip->Notifies.erase(clip->Notifies.begin() + remove);
                changed = true;
            }
        }

        if (changed)
        {
            std::erase_if(clip->Notifies, [](const AnimationNotify& notify) { return notify.Name.empty(); });
            std::sort(clip->Notifies.begin(), clip->Notifies.end(), [](const AnimationNotify& a, const AnimationNotify& b) { return a.Time < b.Time; });
            Save();
        }
        ImGui::End();
    }
}
