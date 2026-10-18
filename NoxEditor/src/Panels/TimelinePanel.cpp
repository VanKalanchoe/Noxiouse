#include "TimelinePanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/TimelineImporter.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/Entity.h"

namespace Nox
{
    namespace
    {
        constexpr float LabelWidth = 230.0f;
        constexpr float RulerHeight = 22.0f;
        constexpr float LaneHeight = 24.0f;
        constexpr const char* InterpolationNames[] = { "Constant", "Linear", "Smooth" };

        // The entity's name path from the player ("" for the player itself).
        std::string PathFrom(Entity player, Entity entity)
        {
            std::string path;
            Entity current = entity;
            while (current && current.GetUUID() != player.GetUUID())
            {
                path = path.empty() ? current.GetName() : current.GetName() + "/" + path;
                current = current.GetParent();
            }
            return current ? path : std::string();
        }

        float SnapTime(float time)
        {
            // Hundredths of a second (Ctrl: free).
            return ImGui::GetIO().KeyCtrl ? time : std::round(time * 100.0f) / 100.0f;
        }
    }

    TimelinePanel::TimelinePanel(AssetHandle timeline)
        : m_Timeline(timeline)
    {
        m_Title = AssetManager::GetMetadata(timeline).FilePath.stem().string() + " (Timeline)##" + std::to_string(static_cast<uint64_t>(timeline));
    }

    void TimelinePanel::Save(Timeline& timeline)
    {
        for (TimelineTrack& track : timeline.Tracks)
            TimelineEvaluation::SortKeys(track);
        const std::filesystem::path path = Project::GetActiveAssetDirectory() / AssetManager::GetMetadata(m_Timeline).FilePath;
        if (!TimelineImporter::Serialize(path, timeline))
            NOX_CORE_WARN("Could not save the timeline {}", path.string());
        m_LastPreviewTime = -1.0f; // pose again with the edit
    }

    Entity TimelinePanel::FindPlayer(Scene& scene, Entity selected)
    {
        auto plays = [this](Entity entity)
        {
            return entity && entity.HasComponent<TimelinePlayerComponent>() && entity.GetComponent<TimelinePlayerComponent>().Timeline == m_Timeline;
        };
        if (plays(selected))
            m_Player = selected.GetUUID();
        if (m_Player != 0)
        {
            Entity remembered = scene.GetEntityByUUID(m_Player);
            if (plays(remembered))
                return remembered;
        }
        for (auto handle : scene.GetAllEntitiesWith<TimelinePlayerComponent>())
        {
            Entity entity(handle, &scene);
            if (plays(entity))
            {
                m_Player = entity.GetUUID();
                return entity;
            }
        }
        m_Player = 0;
        return {};
    }

    void TimelinePanel::BeginPreview(Scene& scene, Entity player, const Timeline& timeline)
    {
        Snapshot snapshot;
        for (const TimelineTrack& track : timeline.Tracks)
        {
            Entity target = scene.FindTimelineTarget(player, track.Target);
            if (!target || snapshot.Targets.contains(target.GetUUID()))
                continue;
            TargetState state;
            if (target.HasComponent<TransformComponent>())
                state.Transform = target.GetComponent<TransformComponent>();
            if (target.HasComponent<DirectionalLightComponent>())
            {
                state.LightColor = target.GetComponent<DirectionalLightComponent>().Color;
                state.LightIntensity = target.GetComponent<DirectionalLightComponent>().Intensity;
            }
            else if (target.HasComponent<PointLightComponent>())
            {
                state.LightColor = target.GetComponent<PointLightComponent>().Color;
                state.LightIntensity = target.GetComponent<PointLightComponent>().Intensity;
            }
            else if (target.HasComponent<SpotLightComponent>())
            {
                state.LightColor = target.GetComponent<SpotLightComponent>().Color;
                state.LightIntensity = target.GetComponent<SpotLightComponent>().Intensity;
            }
            if (target.HasComponent<CameraComponent>())
                state.CameraFOV = target.GetComponent<CameraComponent>().Camera.GetPerspectiveVerticalFOV();
            snapshot.Targets.emplace(target.GetUUID(), state);
        }
        m_Snapshot = std::move(snapshot);
    }

    void TimelinePanel::EndPreview()
    {
        if (!m_Snapshot)
            return;
        if (Scene* scene = m_Snapshot->SceneRef.get())
        {
            for (const auto& [id, state] : m_Snapshot->Targets)
            {
                Entity target = scene->GetEntityByUUID(id);
                if (!target)
                    continue;
                if (state.Transform && target.HasComponent<TransformComponent>())
                {
                    target.GetComponent<TransformComponent>() = *state.Transform;
                    target.MarkTransformDirty();
                }
                auto restoreLight = [&](auto& light)
                {
                    if (state.LightColor)
                        light.Color = *state.LightColor;
                    if (state.LightIntensity)
                        light.Intensity = *state.LightIntensity;
                };
                if (target.HasComponent<DirectionalLightComponent>())
                    restoreLight(target.GetComponent<DirectionalLightComponent>());
                if (target.HasComponent<PointLightComponent>())
                    restoreLight(target.GetComponent<PointLightComponent>());
                if (target.HasComponent<SpotLightComponent>())
                    restoreLight(target.GetComponent<SpotLightComponent>());
                if (state.CameraFOV && target.HasComponent<CameraComponent>())
                    target.GetComponent<CameraComponent>().Camera.SetPerspectiveVerticalFOV(*state.CameraFOV);
            }
        }
        m_Snapshot.reset();
        m_LastPreviewTime = -1.0f;
    }

    std::optional<glm::vec4> TimelinePanel::ReadTargetValue(Scene& scene, Entity player, const TimelineTrack& track)
    {
        Entity target = scene.FindTimelineTarget(player, track.Target);
        if (!target)
            return std::nullopt;
        switch (track.Property)
        {
        case TimelineProperty::Translation:
            return target.HasComponent<TransformComponent>() ? std::optional(glm::vec4(target.GetComponent<TransformComponent>().Translation, 0.0f)) : std::nullopt;
        case TimelineProperty::Rotation:
            return target.HasComponent<TransformComponent>() ? std::optional(glm::vec4(glm::degrees(target.GetComponent<TransformComponent>().Rotation), 0.0f)) : std::nullopt;
        case TimelineProperty::Scale:
            return target.HasComponent<TransformComponent>() ? std::optional(glm::vec4(target.GetComponent<TransformComponent>().Scale, 0.0f)) : std::nullopt;
        case TimelineProperty::LightColor:
            if (target.HasComponent<DirectionalLightComponent>()) return glm::vec4(target.GetComponent<DirectionalLightComponent>().Color, 0.0f);
            if (target.HasComponent<PointLightComponent>()) return glm::vec4(target.GetComponent<PointLightComponent>().Color, 0.0f);
            if (target.HasComponent<SpotLightComponent>()) return glm::vec4(target.GetComponent<SpotLightComponent>().Color, 0.0f);
            return std::nullopt;
        case TimelineProperty::LightIntensity:
            if (target.HasComponent<DirectionalLightComponent>()) return glm::vec4(target.GetComponent<DirectionalLightComponent>().Intensity);
            if (target.HasComponent<PointLightComponent>()) return glm::vec4(target.GetComponent<PointLightComponent>().Intensity);
            if (target.HasComponent<SpotLightComponent>()) return glm::vec4(target.GetComponent<SpotLightComponent>().Intensity);
            return std::nullopt;
        case TimelineProperty::CameraFOV:
            return target.HasComponent<CameraComponent>()
                ? std::optional(glm::vec4(glm::degrees(target.GetComponent<CameraComponent>().Camera.GetPerspectiveVerticalFOV())))
                : std::nullopt;
        default:
            return glm::vec4(0.0f);
        }
    }

    void TimelinePanel::OnImGuiRender(const Ref<Scene>& sceneRef, Entity selected, bool editing)
    {
        ImGui::SetNextWindowSize(ImVec2(900.0f, 360.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(m_Title.c_str(), &m_Open))
        {
            ImGui::End();
            return;
        }

        if (!AssetManager::IsAssetHandleValid(m_Timeline) || AssetManager::GetAssetType(m_Timeline) != AssetType::Timeline)
        {
            ImGui::TextDisabled("This timeline is no longer in the project.");
            ImGui::End();
            return;
        }
        Timeline* timeline = AssetManager::FindLoadedAsset<Timeline>(m_Timeline);
        if (!timeline)
        {
            AssetManager::RequestAsset(m_Timeline);
            ImGui::TextDisabled("Loading...");
            ImGui::End();
            return;
        }

        // The preview runs on the edited scene only (Play and Simulate play the timeline themselves); another scene ends it.
        Scene* scene = editing ? sceneRef.get() : nullptr;
        if (m_Snapshot && m_Snapshot->SceneRef != sceneRef)
            EndPreview();
        Entity player = scene ? FindPlayer(*scene, selected) : Entity();
        if (!player || !m_PreviewWanted || !editing)
            EndPreview();

        if (m_Playing)
        {
            m_Time += ImGui::GetIO().DeltaTime;
            if (m_Time > timeline->Duration)
            {
                if (timeline->Loop)
                    m_Time = std::fmod(m_Time, std::max(timeline->Duration, 0.01f));
                else
                {
                    m_Time = timeline->Duration;
                    m_Playing = false;
                }
            }
        }

        DrawToolbar(*timeline, scene, player);
        ImGui::Separator();
        DrawTracks(*timeline, scene, player);

        // Pose the targets at the playhead (the first time the snapshot of what they had is taken).
        if (scene && player && m_PreviewWanted)
        {
            if (!m_Snapshot)
            {
                BeginPreview(*scene, player, *timeline);
                m_Snapshot->SceneRef = sceneRef;
                m_LastPreviewTime = -1.0f;
            }
            if (m_Time != m_LastPreviewTime)
            {
                scene->PreviewTimeline(player, m_Time);
                m_LastPreviewTime = m_Time;
            }
        }
        ImGui::End();
    }

    void TimelinePanel::DrawToolbar(Timeline& timeline, Scene* scene, Entity player)
    {
        if (ImGui::Button(m_Playing ? "Pause" : "Play", ImVec2(60.0f, 0.0f)))
        {
            if (!m_Playing && m_Time >= timeline.Duration)
                m_Time = 0.0f;
            m_Playing = !m_Playing;
        }
        ImGui::SameLine();
        if (ImGui::Button("|<", ImVec2(28.0f, 0.0f)))
        {
            m_Time = 0.0f;
            m_Playing = false;
        }
        ImGui::SetItemTooltip("Back to the start");
        ImGui::SameLine();
        ImGui::Text("%6.2f s", m_Time);

        ImGui::SameLine(0.0f, 20.0f);
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::DragFloat("Duration", &timeline.Duration, 0.05f, 0.1f, 3600.0f, "%.2f s"))
            timeline.Duration = std::max(timeline.Duration, 0.1f);
        if (ImGui::IsItemDeactivatedAfterEdit())
            Save(timeline);
        ImGui::SameLine();
        if (ImGui::Checkbox("Loop", &timeline.Loop))
            Save(timeline);
        ImGui::SameLine(0.0f, 20.0f);
        ImGui::Checkbox("Preview", &m_PreviewWanted);
        ImGui::SetItemTooltip("Poses the targets at the playhead while editing; what they had comes back when it is off or the window closes.");

        ImGui::SameLine(0.0f, 20.0f);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("##zoom", &m_PixelsPerSecond, 20.0f, 600.0f, "Zoom %.0f px/s", ImGuiSliderFlags_Logarithmic);

        if (!scene)
            ImGui::TextDisabled("Previews while editing (Play and Simulate play it on its Timeline Players).");
        else if (!player)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "No entity plays this timeline: add a Timeline Player to one and pick it.");
        else
            ImGui::TextDisabled("Playing on: %s", player.GetName().c_str());
    }

    void TimelinePanel::DrawAddTrack(Timeline& timeline, Scene& scene, Entity player)
    {
        if (!ImGui::BeginPopup("##add_track"))
            return;

        // The player and every entity below it, each with the properties its components have.
        std::function<void(Entity)> entityMenu = [&](Entity entity)
        {
            const std::string path = PathFrom(player, entity);
            const std::string label = path.empty() ? player.GetName() + " (player)" : path;
            if (ImGui::BeginMenu(label.c_str()))
            {
                auto propertyItem = [&](TimelineProperty property)
                {
                    if (ImGui::MenuItem(TimelineEvaluation::GetPropertyName(property)))
                    {
                        TimelineTrack track;
                        track.Target = path;
                        track.Property = property;
                        timeline.Tracks.push_back(std::move(track));
                        m_SelectedTrack = static_cast<int>(timeline.Tracks.size()) - 1;
                        m_SelectedKey = -1;
                        EndPreview(); // the new target joins the snapshot
                        Save(timeline);
                    }
                };
                propertyItem(TimelineProperty::Translation);
                propertyItem(TimelineProperty::Rotation);
                propertyItem(TimelineProperty::Scale);
                if (entity.HasComponent<DirectionalLightComponent>() || entity.HasComponent<PointLightComponent>() || entity.HasComponent<SpotLightComponent>())
                {
                    propertyItem(TimelineProperty::LightColor);
                    propertyItem(TimelineProperty::LightIntensity);
                }
                if (entity.HasComponent<CameraComponent>())
                    propertyItem(TimelineProperty::CameraFOV);
                if (path.empty())
                    propertyItem(TimelineProperty::Event);
                ImGui::EndMenu();
            }
            if (entity.HasComponent<RelationshipComponent>())
            {
                for (UUID childID : entity.GetComponent<RelationshipComponent>().Children)
                {
                    if (Entity child = scene.GetEntityByUUID(childID))
                        entityMenu(child);
                }
            }
        };
        entityMenu(player);
        ImGui::EndPopup();
    }

    void TimelinePanel::DrawTracks(Timeline& timeline, Scene* scene, Entity player)
    {
        const float duration = std::max(timeline.Duration, 0.01f);

        if (scene && player)
        {
            if (ImGui::Button("+ Track"))
                ImGui::OpenPopup("##add_track");
            DrawAddTrack(timeline, *scene, player);
            ImGui::SameLine();
        }
        ImGui::TextDisabled("Click the ruler to scrub; click a key to select it, drag to move it (Ctrl: no snapping), Del deletes it.");

        const float detailsHeight = 110.0f;
        ImGui::BeginChild("##timeline_tracks", ImVec2(0.0f, -detailsHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float laneWidth = duration * m_PixelsPerSecond;
        const float laneLeft = origin.x + LabelWidth;
        auto timeToX = [&](float time) { return laneLeft + time * m_PixelsPerSecond; };
        auto xToTime = [&](float x) { return std::clamp((x - laneLeft) / m_PixelsPerSecond, 0.0f, duration); };

        // Ruler: a tick per step, labelled every few.
        {
            ImGui::InvisibleButton("##ruler", ImVec2(LabelWidth + laneWidth + 20.0f, RulerHeight));
            if (ImGui::IsItemActive() && ImGui::GetIO().MousePos.x >= laneLeft)
            {
                m_Time = SnapTime(xToTime(ImGui::GetIO().MousePos.x));
                m_Playing = false;
            }
            draw->AddRectFilled(ImVec2(laneLeft, origin.y), ImVec2(laneLeft + laneWidth, origin.y + RulerHeight), IM_COL32(40, 40, 46, 255));
            const float step = m_PixelsPerSecond >= 200.0f ? 0.1f : (m_PixelsPerSecond >= 60.0f ? 0.5f : 1.0f);
            for (float time = 0.0f; time <= duration + 1e-4f; time += step)
            {
                const float x = timeToX(time);
                const bool major = std::fmod(time + 1e-4f, 1.0f) < 2e-4f;
                draw->AddLine(ImVec2(x, origin.y + (major ? 4.0f : 12.0f)), ImVec2(x, origin.y + RulerHeight), IM_COL32(150, 150, 150, 255));
                if (major)
                {
                    char label[16];
                    snprintf(label, sizeof(label), "%.0f", time);
                    draw->AddText(ImVec2(x + 2.0f, origin.y + 1.0f), IM_COL32(200, 200, 200, 255), label);
                }
            }
        }

        // Lanes.
        int removeTrack = -1;
        for (int trackIndex = 0; trackIndex < static_cast<int>(timeline.Tracks.size()); ++trackIndex)
        {
            TimelineTrack& track = timeline.Tracks[trackIndex];
            ImGui::PushID(trackIndex);
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const bool trackSelected = trackIndex == m_SelectedTrack;

            // Label column: mute, name, Key, remove.
            if (ImGui::Checkbox("##mute", &track.Muted))
                Save(timeline);
            ImGui::SetItemTooltip("Muted tracks do not play");
            ImGui::SameLine();
            const std::string targetName = track.Target.empty() ? (player ? player.GetName() : std::string("(player)")) : track.Target;
            const std::string name = targetName + " : " + TimelineEvaluation::GetPropertyName(track.Property);
            ImGui::PushStyleColor(ImGuiCol_Text, trackSelected ? ImVec4(1.0f, 0.8f, 0.4f, 1.0f) : ImGui::GetStyle().Colors[ImGuiCol_Text]);
            ImGui::TextUnformatted(name.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemClicked())
            {
                m_SelectedTrack = trackIndex;
                m_SelectedKey = -1;
            }
            ImGui::SetItemTooltip("%s", name.c_str());

            const float buttonsX = rowMin.x + LabelWidth - 64.0f;
            ImGui::SameLine(buttonsX - ImGui::GetWindowPos().x + ImGui::GetScrollX());
            bool addKey = ImGui::SmallButton("Key");
            // The target's value now, at the playhead (replacing a key already there); an event key is named in the details.
            TimelineKey key;
            key.Time = m_Time;
            if (addKey && track.Property == TimelineProperty::Event)
                key.Event = m_NewEventName;
            else if (addKey)
            {
                const std::optional<glm::vec4> value = scene && player ? ReadTargetValue(*scene, player, track) : std::nullopt;
                if (value)
                    key.Value = *value;
                else
                    addKey = false; // the target is missing, or has no such component
            }
            if (addKey)
            {
                auto existing = std::find_if(track.Keys.begin(), track.Keys.end(), [&](const TimelineKey& other) { return std::abs(other.Time - key.Time) < 1e-3f; });
                if (existing != track.Keys.end())
                {
                    key.Interpolation = existing->Interpolation;
                    *existing = key;
                }
                else
                    track.Keys.push_back(key);
                TimelineEvaluation::SortKeys(track);
                m_SelectedTrack = trackIndex;
                m_SelectedKey = static_cast<int>(std::find_if(track.Keys.begin(), track.Keys.end(), [&](const TimelineKey& other) { return std::abs(other.Time - key.Time) < 1e-3f; }) - track.Keys.begin());
                Save(timeline);
            }
            ImGui::SetItemTooltip(track.Property == TimelineProperty::Event ? "Adds an event key at the playhead (named below)"
                                                                            : "Keys the target's current value at the playhead (move it with the gizmo first)");
            ImGui::SameLine();
            if (ImGui::SmallButton("X"))
                removeTrack = trackIndex;
            ImGui::SetItemTooltip("Remove the track");

            // Lane background and keys.
            const float laneTop = rowMin.y;
            draw->AddRectFilled(ImVec2(laneLeft, laneTop), ImVec2(laneLeft + laneWidth, laneTop + LaneHeight - 2.0f),
                                trackSelected ? IM_COL32(52, 52, 64, 255) : IM_COL32(36, 36, 42, 255));
            for (int keyIndex = 0; keyIndex < static_cast<int>(track.Keys.size()); ++keyIndex)
            {
                TimelineKey& key = track.Keys[keyIndex];
                const ImVec2 center(timeToX(key.Time), laneTop + LaneHeight * 0.5f - 1.0f);
                ImGui::SetCursorScreenPos(ImVec2(center.x - 6.0f, center.y - 6.0f));
                ImGui::PushID(keyIndex);
                ImGui::InvisibleButton("##key", ImVec2(12.0f, 12.0f));
                if (ImGui::IsItemClicked())
                {
                    m_SelectedTrack = trackIndex;
                    m_SelectedKey = keyIndex;
                }
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
                {
                    key.Time = SnapTime(xToTime(ImGui::GetIO().MousePos.x));
                    m_DraggingKey = true;
                }
                if (ImGui::IsItemDeactivated() && m_DraggingKey)
                {
                    m_DraggingKey = false;
                    const float movedTime = key.Time;
                    TimelineEvaluation::SortKeys(track);
                    m_SelectedKey = static_cast<int>(std::find_if(track.Keys.begin(), track.Keys.end(), [&](const TimelineKey& other) { return other.Time == movedTime; }) - track.Keys.begin());
                    Save(timeline);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%.2f s%s%s", key.Time, key.Event.empty() ? "" : "  ", key.Event.c_str());
                ImGui::PopID();

                const bool keySelected = trackSelected && keyIndex == m_SelectedKey;
                const ImU32 color = keySelected ? IM_COL32(255, 200, 70, 255) : (track.Property == TimelineProperty::Event ? IM_COL32(120, 200, 255, 255) : IM_COL32(220, 220, 220, 255));
                const ImVec2 diamond[4] = { ImVec2(center.x, center.y - 6.0f), ImVec2(center.x + 6.0f, center.y), ImVec2(center.x, center.y + 6.0f), ImVec2(center.x - 6.0f, center.y) };
                draw->AddConvexPolyFilled(diamond, 4, color);
                draw->AddPolyline(diamond, 4, IM_COL32(20, 20, 20, 255), ImDrawFlags_Closed, 1.0f);
            }
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMin.y + LaneHeight));
            ImGui::Dummy(ImVec2(LabelWidth + laneWidth + 20.0f, 0.0f));
            ImGui::PopID();
        }

        // The playhead over everything.
        const float bottom = ImGui::GetCursorScreenPos().y;
        const float playheadX = timeToX(std::clamp(m_Time, 0.0f, duration));
        draw->AddLine(ImVec2(playheadX, origin.y), ImVec2(playheadX, bottom), IM_COL32(255, 80, 60, 255), 2.0f);
        draw->AddTriangleFilled(ImVec2(playheadX - 6.0f, origin.y), ImVec2(playheadX + 6.0f, origin.y), ImVec2(playheadX, origin.y + 8.0f), IM_COL32(255, 80, 60, 255));

        if (timeline.Tracks.empty())
            ImGui::TextDisabled(player ? "No tracks: + Track picks an entity and a property." : "No tracks yet.");

        // Del removes the selected key while this window has focus.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Delete) &&
            m_SelectedTrack >= 0 && m_SelectedTrack < static_cast<int>(timeline.Tracks.size()) &&
            m_SelectedKey >= 0 && m_SelectedKey < static_cast<int>(timeline.Tracks[m_SelectedTrack].Keys.size()))
        {
            auto& keys = timeline.Tracks[m_SelectedTrack].Keys;
            keys.erase(keys.begin() + m_SelectedKey);
            m_SelectedKey = -1;
            Save(timeline);
        }
        ImGui::EndChild();

        if (removeTrack >= 0)
        {
            timeline.Tracks.erase(timeline.Tracks.begin() + removeTrack);
            m_SelectedTrack = -1;
            m_SelectedKey = -1;
            EndPreview();
            Save(timeline);
        }

        DrawSelectedKey(timeline);
    }

    void TimelinePanel::DrawSelectedKey(Timeline& timeline)
    {
        if (m_SelectedTrack < 0 || m_SelectedTrack >= static_cast<int>(timeline.Tracks.size()))
        {
            ImGui::TextDisabled("Select a key to edit it.");
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputText("New event name", m_NewEventName, sizeof(m_NewEventName));
            return;
        }
        TimelineTrack& track = timeline.Tracks[m_SelectedTrack];
        if (m_SelectedKey < 0 || m_SelectedKey >= static_cast<int>(track.Keys.size()))
        {
            ImGui::TextDisabled("%s: select a key to edit it.", TimelineEvaluation::GetPropertyName(track.Property));
            if (track.Property == TimelineProperty::Event)
            {
                ImGui::SetNextItemWidth(160.0f);
                ImGui::InputText("New event name", m_NewEventName, sizeof(m_NewEventName));
            }
            return;
        }

        TimelineKey& key = track.Keys[m_SelectedKey];
        bool changed = false;
        ImGui::SetNextItemWidth(100.0f);
        ImGui::DragFloat("Time", &key.Time, 0.01f, 0.0f, timeline.Duration, "%.2f s");
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        if (ImGui::SmallButton("Go to"))
            m_Time = key.Time;

        switch (track.Property)
        {
        case TimelineProperty::Translation:
        case TimelineProperty::Scale:
            ImGui::DragFloat3("Value", glm::value_ptr(key.Value), 0.01f);
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            break;
        case TimelineProperty::Rotation:
            ImGui::DragFloat3("Value (degrees)", glm::value_ptr(key.Value), 0.5f);
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            break;
        case TimelineProperty::LightColor:
            changed |= ImGui::ColorEdit3("Value", glm::value_ptr(key.Value));
            break;
        case TimelineProperty::LightIntensity:
            ImGui::DragFloat("Value", &key.Value.x, 0.05f, 0.0f, 100000.0f);
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            break;
        case TimelineProperty::CameraFOV:
            ImGui::DragFloat("Value (degrees)", &key.Value.x, 0.1f, 1.0f, 179.0f);
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            break;
        case TimelineProperty::Event:
        {
            char name[64];
            snprintf(name, sizeof(name), "%s", key.Event.c_str());
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::InputText("Event name", name, sizeof(name)))
                key.Event = name;
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetItemTooltip("The player entity's scripts receive OnAnimationEvent(\"%s\") when the playhead passes it.", key.Event.c_str());
            break;
        }
        default:
            break;
        }
        if (track.Property != TimelineProperty::Event)
        {
            int interpolation = static_cast<int>(key.Interpolation);
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::Combo("To next key", &interpolation, InterpolationNames, 3))
            {
                key.Interpolation = static_cast<TimelineInterpolation>(interpolation);
                changed = true;
            }
        }
        if (changed)
            Save(timeline);
        else if (ImGui::IsAnyItemActive())
            m_LastPreviewTime = -1.0f; // a value being dragged shows at once
    }
}
