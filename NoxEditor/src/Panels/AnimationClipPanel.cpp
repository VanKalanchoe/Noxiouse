#include "AnimationClipPanel.h"

#include <algorithm>
#include <cfloat>
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
#include "AudioWidgets.h"
#include "NoxCore/Animation/Skeleton.h"

namespace Nox
{
    AnimationClipPanel::AnimationClipPanel(AssetHandle clip, AssetHandle previewMesh)
        : m_Clip(clip)
    {
        m_Title = AssetManager::GetMetadata(clip).FilePath.stem().string() + " (Animation Clip)##" + std::to_string(static_cast<uint64_t>(clip));
        if (previewMesh != 0)
        {
            SetPreviewMesh(previewMesh);
            m_PreviewSearched = true;
        }
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
        // The skeleton cooked from the mesh's model (EditorAssetManager::FindSkeletonOf: the rule its import links it by).
        m_PreviewSkeleton = Project::GetActive()->GetEditorAssetManager()->FindSkeletonOf(mesh);
    }

    bool AnimationClipPanel::DrawNotifySound(AnimationNotify& notify)
    {
        bool changed = false;
        ImGui::Text("Play Sound: %s", notify.Name.c_str());
        ImGui::Separator();
        changed |= SoundAssetCombo("Sound", notify.Sound);
        ImGui::SetItemTooltip("Played when a playhead crosses the notify -- or, with Trace Surface, when the surface below has no sound of\n"
                              "its own in the list. Its Sound Settings (Content Browser) limit how many play at once.");

        // The bone it plays at: the preview skeleton's bones, or typed.
        const Skeleton* skeleton = m_PreviewSkeleton != 0 ? AssetManager::FindLoadedAsset<Skeleton>(m_PreviewSkeleton) : nullptr;
        if (m_PreviewSkeleton != 0 && !skeleton)
            AssetManager::RequestAsset(m_PreviewSkeleton);
        if (ImGui::BeginCombo("Bone", notify.Bone.empty() ? "(the entity)" : notify.Bone.c_str()))
        {
            if (ImGui::Selectable("(the entity)", notify.Bone.empty()))
            {
                notify.Bone.clear();
                changed = true;
            }
            if (skeleton)
            {
                for (const Node* node : skeleton->AllNodes)
                {
                    if (node && !node->Name.empty() && ImGui::Selectable(node->Name.c_str(), notify.Bone == node->Name))
                    {
                        notify.Bone = node->Name;
                        changed = true;
                    }
                }
            }
            else
                ImGui::TextDisabled("(no preview skeleton: type the name below)");
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Where the sound plays (a foot for a footstep). The entity: its origin.");
        ImGui::InputText("##bone", &notify.Bone);
        changed |= ImGui::IsItemDeactivatedAfterEdit();

        changed |= ImGui::Checkbox("Trace Surface", &notify.TraceSurface);
        ImGui::SetItemTooltip("A ray goes down from the bone (from 20 cm above it); the physical surface it hits picks the sound from the list\n"
                              "below (the material's Physical Surface, or the Rigidbody 3D's). No hit, or no sound for that surface: Sound.");
        if (notify.TraceSurface)
        {
            ImGui::DragFloat("Trace Distance", &notify.TraceDistance, 0.01f, 0.0f, 10.0f, "%.2f m");
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetItemTooltip("How far below the bone the ground may be (the ray is this + 0.2 m long).");
            ImGui::SeparatorText("Sounds by surface");
            int remove = -1;
            for (int i = 0; i < static_cast<int>(notify.SurfaceSounds.size()); ++i)
            {
                AnimationNotify::SurfaceSound& entry = notify.SurfaceSounds[i];
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(130.0f);
                changed |= PhysicalSurfaceCombo("##surface", entry.Surface, "Default");
                if (entry.Surface.empty())
                    entry.Surface = "Default";
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200.0f);
                changed |= SoundAssetCombo("##sound", entry.Sound);
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                    remove = i;
                ImGui::PopID();
            }
            if (remove >= 0)
            {
                notify.SurfaceSounds.erase(notify.SurfaceSounds.begin() + remove);
                changed = true;
            }
            if (ImGui::SmallButton("Add Surface Sound"))
            {
                notify.SurfaceSounds.push_back({ "Grass", 0 });
                changed = true;
            }
        }
        ImGui::Separator();
        ImGui::SliderFloat("Volume", &notify.Volume, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::DragFloat("Pitch Variation", &notify.PitchVariation, 0.005f, 0.0f, 0.5f, "+/- %.3f");
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetItemTooltip("Each play's pitch is random within this (0.05: +/- 5%%): no two footsteps alike.");
        if (notify.HasSound() && ImGui::Button("Remove Sound"))
        {
            notify.Sound = 0;
            notify.SurfaceSounds.clear();
            changed = true;
        }
        return changed;
    }

    void AnimationClipPanel::DrawPreview(float duration)
    {
        const AnimationSequence* clip = AssetManager::FindLoadedAsset<AnimationSequence>(m_Clip);
        if (!m_PreviewSearched)
            FindPreviewModel();

        // Playing moves the playhead inside the loop range (looping or stopping at its end) and reports the notifies it passes.
        const float rangeStart = std::clamp(m_RangeStart, 0.0f, duration);
        const float rangeEnd = m_RangeEnd > rangeStart ? std::min(m_RangeEnd, duration) : duration;
        const float rangeLength = std::max(rangeEnd - rangeStart, 1e-4f);
        if (m_Playing && (m_Cursor < rangeStart || m_Cursor > rangeEnd))
            m_Cursor = m_Speed >= 0.0f ? rangeStart : rangeEnd; // started (or the range moved) outside it
        const float previous = m_Cursor;
        bool wrapped = false;
        if (m_Playing && duration > 0.0f)
        {
            m_Cursor += ImGui::GetIO().DeltaTime * m_Speed;
            if (m_Cursor > rangeEnd || m_Cursor < rangeStart)
            {
                if (m_Loop)
                {
                    m_Cursor = rangeStart + std::fmod(std::fmod(m_Cursor - rangeStart, rangeLength) + rangeLength, rangeLength);
                    wrapped = true;
                }
                else
                {
                    m_Cursor = std::clamp(m_Cursor, rangeStart, rangeEnd);
                    m_Playing = false;
                }
            }
            if (clip)
            {
                // Across the range's ends: the part up to the end, then from the start (the notifies of each).
                std::vector<AnimationNotify> crossed;
                if (wrapped && (rangeStart > 0.0f || rangeEnd < duration))
                {
                    if (m_Speed >= 0.0f)
                    {
                        CollectCrossedNotifies(*clip, previous, rangeEnd, false, crossed);
                        CollectCrossedNotifies(*clip, rangeStart - 1e-5f, m_Cursor, false, crossed);
                    }
                    else
                    {
                        CollectCrossedNotifies(*clip, previous, rangeStart, false, crossed);
                        CollectCrossedNotifies(*clip, rangeEnd + 1e-5f, m_Cursor, false, crossed);
                    }
                }
                else
                    CollectCrossedNotifies(*clip, previous, m_Cursor, wrapped, crossed);
                if (!crossed.empty())
                {
                    m_LastNotify = crossed.back().Name;
                    m_LastNotifyAge = 0.0f;
                }
                // Their sounds, heard as the preview passes them (the default sound: no ground here to trace).
                for (const AnimationNotify& notify : crossed)
                {
                    AssetHandle sound = notify.Sound;
                    if (sound == 0 && !notify.SurfaceSounds.empty())
                        sound = notify.SurfaceSounds.front().Sound;
                    PlayPreviewSound(sound, notify.Volume);
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
            if (!m_Playing && !m_Loop && m_Cursor >= rangeEnd)
                m_Cursor = rangeStart;
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
            // The loop range, shaded.
            const float rangeStart = std::clamp(m_RangeStart, 0.0f, duration);
            const float rangeEnd = m_RangeEnd > rangeStart ? std::min(m_RangeEnd, duration) : duration;
            if (rangeStart > 0.0f || rangeEnd < duration)
            {
                const float x0 = min.x + (max.x - min.x) * rangeStart / duration;
                const float x1 = min.x + (max.x - min.x) * rangeEnd / duration;
                drawList->AddRectFilled(ImVec2(x0, min.y), ImVec2(x1, max.y), IM_COL32(80, 160, 255, 50));
                drawList->AddLine(ImVec2(x0, min.y), ImVec2(x0, max.y), IM_COL32(80, 160, 255, 255), 2.0f);
                drawList->AddLine(ImVec2(x1, min.y), ImVec2(x1, max.y), IM_COL32(80, 160, 255, 255), 2.0f);
            }
            for (const AnimationNotify& notify : clip->Notifies)
            {
                const float x = min.x + (max.x - min.x) * std::clamp(notify.Time / duration, 0.0f, 1.0f);
                drawList->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), notify.SyncMarker ? IM_COL32(90, 220, 140, 255) : IM_COL32(255, 190, 60, 255), 2.0f);
            }
        }

        // The loop range: [ and ] at the playhead, or typed.
        if (ImGui::SmallButton("Range Start Here"))
            m_RangeStart = m_Cursor;
        ImGui::SameLine();
        if (ImGui::SmallButton("Range End Here"))
            m_RangeEnd = m_Cursor;
        ImGui::SameLine();
        if (ImGui::SmallButton("Whole Clip"))
            m_RangeStart = m_RangeEnd = 0.0f;
        ImGui::SameLine();
        float range[2] = { m_RangeStart, m_RangeEnd > 0.0f ? m_RangeEnd : duration };
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat2("##range", range, 0.005f, 0.0f, duration, "%.3f s"))
        {
            m_RangeStart = std::clamp(std::min(range[0], range[1]), 0.0f, duration);
            m_RangeEnd = std::clamp(std::max(range[0], range[1]), 0.0f, duration);
        }
        ImGui::SetItemTooltip("Loop range: playing loops (or stops) inside it");

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

        if (ImGui::BeginTable("##notifies", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, std::max(ImGui::GetContentRegionAvail().y, 80.0f))))
        {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Sync", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("Sound", ImGuiTableColumnFlags_WidthFixed, 150.0f);
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
                changed |= ImGui::Checkbox("##sync", &notify.SyncMarker);
                ImGui::SetItemTooltip("Sync marker: not raised as an event; blend spaces line their clips up at markers of the same name\n(e.g. L and R where each foot plants)");
                ImGui::TableSetColumnIndex(3);
                {
                    // Play Sound: the sound's name (or how many surfaces it has sounds for), the settings in a popup.
                    std::string label = "Add Sound...";
                    if (notify.Sound != 0)
                        label = AssetManager::GetMetadata(notify.Sound).FilePath.stem().string();
                    else if (!notify.SurfaceSounds.empty())
                        label = std::to_string(notify.SurfaceSounds.size()) + " surface sounds";
                    if (!notify.SurfaceSounds.empty() && notify.Sound != 0)
                        label += " +" + std::to_string(notify.SurfaceSounds.size());
                    if (ImGui::Button((label + "##sound").c_str(), ImVec2(-1.0f, 0.0f)))
                        ImGui::OpenPopup("Notify Sound");
                    ImGui::SetItemTooltip("Play Sound: a sound this notify plays where the character is (or at a bone), optionally picked by the\n"
                                          "physical surface under it (footsteps on grass, stone, wood).");
                    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(600.0f, FLT_MAX));
                    if (ImGui::BeginPopup("Notify Sound"))
                    {
                        changed |= DrawNotifySound(notify);
                        ImGui::EndPopup();
                    }
                }
                ImGui::TableSetColumnIndex(4);
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
