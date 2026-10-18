#include "AnimationClipPanel.h"

#include <algorithm>
#include <cmath>
#include <imgui.h>
#include "misc/cpp/imgui_stdlib.h"

#include "NoxCore/Animation/AnimPose.h"
#include "NoxCore/Animation/AnimationSequence.h"
#include "NoxCore/Asset/AnimationImporter.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "AssetPreviewRenderer.h"

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

    void AnimationClipPanel::FindPreviewModel()
    {
        m_PreviewSearched = true;
        const auto& registry = Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry();
        const AssetMetadata& clipMetadata = AssetManager::GetMetadata(m_Clip);

        // A skeletal mesh cooked from the same model, else the first one in the clip's folder.
        AssetHandle sameSource = 0, sameFolder = 0;
        for (const auto& [handle, metadata] : registry)
        {
            if (metadata.Type != AssetType::Mesh)
                continue;
            if (!clipMetadata.SourceFilePath.empty() && metadata.SourceFilePath == clipMetadata.SourceFilePath && sameSource == 0)
                sameSource = handle;
            if (metadata.FilePath.parent_path() == clipMetadata.FilePath.parent_path() && sameFolder == 0)
                sameFolder = handle;
        }
        SetPreviewMesh(sameSource != 0 ? sameSource : sameFolder);
    }

    void AnimationClipPanel::SetPreviewMesh(AssetHandle mesh)
    {
        m_PreviewMesh = mesh;
        m_PreviewSkeleton = 0;
        if (mesh == 0)
            return;
        // The skeleton cooked from the mesh's model.
        const AssetMetadata& meshMetadata = AssetManager::GetMetadata(mesh);
        for (const auto& [handle, metadata] : Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry())
        {
            if (metadata.Type == AssetType::Skeleton && metadata.SourceFilePath == meshMetadata.SourceFilePath)
            {
                m_PreviewSkeleton = handle;
                break;
            }
        }
    }

    void AnimationClipPanel::DrawPreview(float duration)
    {
        const AnimationSequence* clip = AssetManager::FindLoadedAsset<AnimationSequence>(m_Clip);
        if (!m_PreviewSearched)
            FindPreviewModel();

        // Playing moves the playhead (looping or stopping at the end) and reports the notifies it passes.
        const float previous = m_Cursor;
        bool wrapped = false;
        if (m_Playing && duration > 0.0f)
        {
            m_Cursor += ImGui::GetIO().DeltaTime * m_Speed;
            if (m_Cursor > duration || m_Cursor < 0.0f)
            {
                if (m_Loop)
                {
                    m_Cursor = std::fmod(std::fmod(m_Cursor, duration) + duration, duration);
                    wrapped = true;
                }
                else
                {
                    m_Cursor = std::clamp(m_Cursor, 0.0f, duration);
                    m_Playing = false;
                }
            }
            if (clip)
            {
                std::vector<std::string> crossed;
                CollectCrossedNotifies(*clip, previous, m_Cursor, wrapped, crossed);
                if (!crossed.empty())
                {
                    m_LastNotify = crossed.back();
                    m_LastNotifyAge = 0.0f;
                }
            }
        }
        m_LastNotifyAge += ImGui::GetIO().DeltaTime;

        constexpr float PreviewSize = 256.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        Texture2D* image = m_PreviewMesh != 0
            ? AssetPreviewRenderer::Get().GetAnimationPreview(m_PreviewMesh, m_PreviewSkeleton, m_Clip, m_Cursor, static_cast<uint32_t>(PreviewSize))
            : nullptr;
        if (image)
            ImGui::Image(image->getImTextureID(), ImVec2(PreviewSize, PreviewSize));
        else
        {
            ImGui::Dummy(ImVec2(PreviewSize, PreviewSize));
            ImGui::GetWindowDrawList()->AddRectFilled(origin, ImVec2(origin.x + PreviewSize, origin.y + PreviewSize), IM_COL32(30, 30, 34, 255));
            ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + 10.0f, origin.y + 10.0f), IM_COL32(170, 170, 170, 255),
                                                m_PreviewMesh != 0 ? "Loading..." : "No skeletal mesh to preview with");
        }
        if (m_LastNotifyAge < 0.6f && !m_LastNotify.empty())
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 textSize = ImGui::CalcTextSize(m_LastNotify.c_str());
            const ImVec2 min(origin.x + 8.0f, origin.y + PreviewSize - textSize.y - 14.0f);
            draw->AddRectFilled(min, ImVec2(min.x + textSize.x + 12.0f, min.y + textSize.y + 6.0f), IM_COL32(255, 190, 60, 220), 3.0f);
            draw->AddText(ImVec2(min.x + 6.0f, min.y + 3.0f), IM_COL32(20, 20, 20, 255), m_LastNotify.c_str());
        }

        // Controls to the right of the image.
        ImGui::SameLine();
        ImGui::BeginGroup();
        if (ImGui::Button(m_Playing ? "Pause" : "Play", ImVec2(70.0f, 0.0f)))
        {
            if (!m_Playing && !m_Loop && m_Cursor >= duration)
                m_Cursor = 0.0f;
            m_Playing = !m_Playing;
        }
        ImGui::Checkbox("Loop", &m_Loop);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("Speed", &m_Speed, -2.0f, 2.0f, "%.2fx");
        ImGui::Spacing();
        ImGui::TextDisabled("Preview mesh");
        ImGui::SetNextItemWidth(180.0f);
        const std::string meshName = m_PreviewMesh != 0 ? AssetManager::GetMetadata(m_PreviewMesh).FilePath.stem().string() : std::string("(none)");
        if (ImGui::BeginCombo("##previewMesh", meshName.c_str()))
        {
            for (const auto& [handle, metadata] : Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry())
            {
                if (metadata.Type != AssetType::Mesh)
                    continue;
                ImGui::PushID(static_cast<int>(static_cast<uint64_t>(handle) & 0x7fffffff));
                if (ImGui::Selectable(metadata.FilePath.stem().string().c_str(), handle == m_PreviewMesh))
                    SetPreviewMesh(handle);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", metadata.FilePath.generic_string().c_str());
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (m_PreviewMesh != 0 && m_PreviewSkeleton == 0)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "No skeleton from its model:\nshown in its rest pose");
        ImGui::EndGroup();
    }

    void AnimationClipPanel::OnImGuiRender()
    {
        ImGui::SetNextWindowSize(ImVec2(560.0f, 640.0f), ImGuiCond_FirstUseEver);
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

        DrawPreview(duration);

        // Timeline: the playhead (drag) and a marker per notify.
        ImGui::SeparatorText("Timeline");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##cursor", &m_Cursor, 0.0f, duration, "%.3f s"))
            m_Playing = false; // scrubbing
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
