#include "TimelineEditorV2.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include "Panels/InspectorWidgetsV2.h"
#include <NoxUI/Widgets/UIChannelCurves.h>
#include <NoxUI/Widgets/UIPropertyRows.h>
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Asset/TimelineImporter.h"
#include "NoxCore/Audio/AudioClip.h"
#include "NoxCore/Audio/AudioEngine.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/Mesh.h"
#include "NoxCore/Scene/Scene.h"
#include <NoxUI/Core/UIBuilder.h>
#include <NoxUI/Core/UIContext.h>
#include "NoxCore/UI/Core/UILayer.h"
#include <NoxUI/Layout/UILayout.h>
#include <NoxUI/Rendering/UIDrawList.h>
#include <NoxUI/Widgets/UIButton.h>
#include <NoxUI/Widgets/UICheckBox.h>
#include <NoxUI/Widgets/UIDragScalar.h>
#include <NoxUI/Widgets/UILabel.h>
#include <NoxUI/Widgets/UIMenu.h>
#include <NoxUI/Widgets/UIScrollView.h>
#include <NoxUI/Widgets/UITextInput.h>

namespace Nox
{
    namespace
    {
        constexpr float kLabelWidth = 250.0f;
        constexpr float kRuler = 22.0f;
        constexpr float kLane = 24.0f;

        NoxUI::UIContext* Context()
        {
            UILayer* uiLayer = Application::Get().GetUILayer();
            return uiLayer ? &uiLayer->GetContext() : nullptr;
        }

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

        // Hundredths of a second (Ctrl: free).
        float SnapTime(float time)
        {
            return (NoxUI::UIContext::GetCurrentModifiers() & NoxUI::UIMod::Ctrl) ? time : std::round(time * 100.0f) / 100.0f;
        }

        std::string TrackName(const TimelineTrack& track, Entity player)
        {
            const std::string target = track.Target.empty() ? (player ? player.GetName() : std::string("(player)")) : track.Target;
            std::string parameter = track.Parameter;
            if (track.Property == TimelineProperty::SubTimeline)
            {
                const AssetHandle child = static_cast<AssetHandle>(std::strtoull(track.Parameter.c_str(), nullptr, 10));
                parameter = child != 0 && AssetManager::IsAssetHandleValid(child) ? AssetManager::GetMetadata(child).FilePath.stem().string() : std::string("(pick a timeline)");
            }
            return target + " : " + TimelineEvaluation::GetPropertyName(track.Property) + (parameter.empty() ? std::string() : " " + parameter);
        }

        // The tracks: a label column (mute box, name, Key, X) and a lane each, the ruler, the playhead. Wheel: scroll sideways
        // (Shift: up / down).
        class TracksView final : public NoxUI::UIElement
        {
        public:
            explicit TracksView(TimelineEditorV2* editor) : m_Editor(editor)
            {
                HitTestMode = NoxUI::UIHitTestMode::Self;
                WidthMode = NoxUI::UISizeMode::Fill;
                HeightMode = NoxUI::UISizeMode::Fill;
                Focusable = true; // (Delete removes the selected key while this view has the keyboard)
            }

            bool OnMouseWheel(const NoxUI::UIPointerEvent& e) override
            {
                if (e.Modifiers & NoxUI::UIMod::Shift)
                    m_ScrollY = std::max(0.0f, m_ScrollY - e.ScrollDelta.y * kLane * 2.0f);
                else
                    m_ScrollX = std::max(0.0f, m_ScrollX - (e.ScrollDelta.y + e.ScrollDelta.x) * 60.0f);
                return true;
            }
            bool OnKeyDown(const NoxUI::UIKeyEvent& e) override { return m_Editor->HandleKey(e); }

            bool OnPointerDown(const NoxUI::UIPointerEvent& e) override
            {
                if (e.Button != 1)
                    return false;
                Timeline* timeline = m_Editor->GetTimeline();
                if (!timeline)
                    return true;
                const NoxUI::UIRect b = GetBounds();
                m_Drag = Drag::None;
                if (e.Position.y < b.Min.y + kRuler && e.Position.x >= LaneLeft())
                {
                    m_Drag = Drag::Scrub;
                    Scrub(e.Position.x);
                    return true;
                }
                const int track = TrackAt(e.Position.y);
                if (track < 0)
                    return true;
                if (e.Position.x < b.Min.x + kLabelWidth)
                {
                    // Label column: mute box, Key, X, or the name (selects the track).
                    const float x = e.Position.x - b.Min.x;
                    if (x < 22.0f)
                    {
                        timeline->Tracks[static_cast<size_t>(track)].Muted = !timeline->Tracks[static_cast<size_t>(track)].Muted;
                        m_Editor->Edited();
                    }
                    else if (x > kLabelWidth - 30.0f)
                        m_Editor->RemoveTrack(track);
                    else if (x > kLabelWidth - 70.0f)
                        m_Editor->AddKeyAtPlayhead(track);
                    else
                    {
                        m_Editor->SelectedTrack = track;
                        m_Editor->SelectedKey = -1;
                    }
                    return true;
                }
                // A key near the pointer on this lane: select it (and drag it).
                const TimelineTrack& lane = timeline->Tracks[static_cast<size_t>(track)];
                m_Editor->SelectedTrack = track;
                m_Editor->SelectedKey = -1;
                for (int k = 0; k < static_cast<int>(lane.Keys.size()); ++k)
                    if (std::abs(TimeToX(lane.Keys[static_cast<size_t>(k)].Time) - e.Position.x) <= 7.0f)
                    {
                        m_Editor->SelectedKey = k;
                        m_Drag = Drag::Key;
                        m_Moved = false;
                        break;
                    }
                return true;
            }
            bool OnPointerMove(const NoxUI::UIPointerEvent& e) override
            {
                Timeline* timeline = m_Editor->GetTimeline();
                if (!timeline)
                    return true;
                if (m_Drag == Drag::Scrub)
                    Scrub(e.Position.x);
                else if (m_Drag == Drag::Key)
                {
                    const int t = m_Editor->SelectedTrack, k = m_Editor->SelectedKey;
                    if (t >= 0 && t < static_cast<int>(timeline->Tracks.size()) && k >= 0 && k < static_cast<int>(timeline->Tracks[static_cast<size_t>(t)].Keys.size()))
                    {
                        timeline->Tracks[static_cast<size_t>(t)].Keys[static_cast<size_t>(k)].Time = SnapTime(XToTime(e.Position.x, timeline->Duration));
                        m_Moved = true;
                        m_Editor->Changing();
                    }
                }
                UpdateTooltip(e.Position);
                return true;
            }
            bool OnPointerUp(const NoxUI::UIPointerEvent& e) override
            {
                if (e.Button != 1)
                    return false;
                if (m_Drag == Drag::Key && m_Moved)
                {
                    // Sorted again, the moved key kept selected.
                    Timeline* timeline = m_Editor->GetTimeline();
                    const int t = m_Editor->SelectedTrack, k = m_Editor->SelectedKey;
                    if (timeline && t >= 0 && t < static_cast<int>(timeline->Tracks.size()) && k >= 0 && k < static_cast<int>(timeline->Tracks[static_cast<size_t>(t)].Keys.size()))
                    {
                        TimelineTrack& track = timeline->Tracks[static_cast<size_t>(t)];
                        const float moved = track.Keys[static_cast<size_t>(k)].Time;
                        TimelineEvaluation::SortKeys(track);
                        m_Editor->SelectedKey = static_cast<int>(std::find_if(track.Keys.begin(), track.Keys.end(), [&](const TimelineKey& key) { return key.Time == moved; }) - track.Keys.begin());
                    }
                    m_Editor->Edited();
                }
                m_Drag = Drag::None;
                return true;
            }

        protected:
            void OnPaint(NoxUI::UIDrawList& d, const NoxUI::UITheme&) override
            {
                const NoxUI::UIRect b = GetBounds();
                d.AddRoundedRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Max.y }, NoxUI::ThemeColor("TimelineBackground", { 0.09f, 0.09f, 0.11f, 1.0f }));
                Timeline* timeline = m_Editor->GetTimeline();
                if (!timeline)
                {
                    d.AddText("Loading...", nullptr, { b.Min.x + 8.0f, b.Min.y + 8.0f }, 13.0f, NoxUI::ThemeColor("TextMuted", { 0.6f, 0.6f, 0.65f, 1.0f }));
                    return;
                }
                const float duration = std::max(timeline->Duration, 0.01f);
                const float laneLeft = LaneLeft(), laneRight = TimeToX(duration);
                const Entity player = m_Editor->GetPlayer();
                const glm::vec4 text = NoxUI::ThemeColor("TimelineText", { 0.86f, 0.88f, 0.92f, 1.0f });
                const glm::vec4 dim = NoxUI::ThemeColor("TextMuted", { 0.55f, 0.57f, 0.62f, 1.0f });
                m_ScrollY = std::clamp(m_ScrollY, 0.0f, std::max(0.0f, static_cast<float>(timeline->Tracks.size()) * kLane - (b.Height() - kRuler)));
                d.PushClipRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Max.y });

                // Ruler.
                d.PushClipRect({ b.Min.x + kLabelWidth, b.Min.y }, { b.Max.x, b.Max.y });
                d.AddRoundedRect({ laneLeft, b.Min.y }, { laneRight, b.Min.y + kRuler }, NoxUI::ThemeColor("TimelineRuler", { 0.16f, 0.16f, 0.18f, 1.0f }));
                const float step = PixelsPerSecond() >= 200.0f ? 0.1f : (PixelsPerSecond() >= 60.0f ? 0.5f : 1.0f);
                for (float time = 0.0f; time <= duration + 1e-4f; time += step)
                {
                    const float x = TimeToX(time);
                    const bool major = std::fmod(time + 1e-4f, 1.0f) < 2e-4f;
                    d.AddRoundedRect({ x, b.Min.y + (major ? 4.0f : 12.0f) }, { x + 1.0f, b.Min.y + kRuler }, NoxUI::ThemeColor("TimelineTick", { 0.59f, 0.59f, 0.59f, 1.0f }));
                    if (major)
                        d.AddText(std::format("{:.0f}", time), nullptr, { x + 2.0f, b.Min.y + 1.0f }, 11.5f, NoxUI::ThemeColor("TimelineTickText", { 0.78f, 0.78f, 0.78f, 1.0f }));
                }
                d.PopClipRect();

                // Lanes.
                for (size_t t = 0; t < timeline->Tracks.size(); ++t)
                {
                    const TimelineTrack& track = timeline->Tracks[t];
                    const float top = b.Min.y + kRuler + static_cast<float>(t) * kLane - m_ScrollY;
                    if (top + kLane < b.Min.y + kRuler || top > b.Max.y)
                        continue;
                    const bool selected = static_cast<int>(t) == m_Editor->SelectedTrack;
                    // Label column.
                    d.PushClipRect({ b.Min.x, std::max(top, b.Min.y + kRuler) }, { b.Min.x + kLabelWidth, top + kLane });
                    d.AddRoundedRect({ b.Min.x + 4.0f, top + 5.0f }, { b.Min.x + 18.0f, top + 19.0f }, track.Muted ? NoxUI::ThemeColor("TimelineMuted", { 0.25f, 0.25f, 0.28f, 1.0f }) : NoxUI::ThemeColor("TimelineMuteBox", { 0.35f, 0.62f, 0.95f, 1.0f }),
                                     glm::vec4(3.0f));
                    d.AddText(TrackName(track, player), nullptr, { b.Min.x + 24.0f, top + 5.0f }, 12.5f,
                              selected ? NoxUI::ThemeColor("TimelineTrackSelected", { 1.0f, 0.8f, 0.4f, 1.0f }) : (track.Muted ? dim : text));
                    d.AddRoundedRect({ b.Min.x + kLabelWidth - 70.0f, top + 3.0f }, { b.Min.x + kLabelWidth - 34.0f, top + kLane - 3.0f }, NoxUI::ThemeColor("TimelineKeyButton", { 0.24f, 0.26f, 0.32f, 1.0f }), glm::vec4(3.0f));
                    d.AddText("Key", nullptr, { b.Min.x + kLabelWidth - 64.0f, top + 5.0f }, 12.0f, text);
                    d.AddRoundedRect({ b.Min.x + kLabelWidth - 30.0f, top + 3.0f }, { b.Min.x + kLabelWidth - 6.0f, top + kLane - 3.0f }, NoxUI::ThemeColor("TimelineRemoveButton", { 0.32f, 0.20f, 0.20f, 1.0f }), glm::vec4(3.0f));
                    d.AddText("X", nullptr, { b.Min.x + kLabelWidth - 22.0f, top + 5.0f }, 12.0f, text);
                    d.PopClipRect();

                    d.PushClipRect({ b.Min.x + kLabelWidth, std::max(top, b.Min.y + kRuler) }, { b.Max.x, top + kLane });
                    d.AddRoundedRect({ laneLeft, top + 1.0f }, { laneRight, top + kLane - 1.0f }, selected ? NoxUI::ThemeColor("TimelineLaneSelected", { 0.20f, 0.20f, 0.25f, 1.0f }) : NoxUI::ThemeColor("TimelineLane", { 0.14f, 0.14f, 0.16f, 1.0f }));
                    if (track.Property == TimelineProperty::Audio)
                        for (size_t k = 0; k < track.Keys.size(); ++k)
                        {
                            // A section: from its key for its length (or what is left of its sound), up to the next section.
                            const TimelineKey& section = track.Keys[k];
                            float length = section.Value.x;
                            if (length <= 0.0f)
                            {
                                const AudioClip* clip = section.Asset != 0 && AssetManager::GetAssetType(section.Asset) == AssetType::AudioClip
                                    ? AssetManager::FindLoadedAsset<AudioClip>(section.Asset) : nullptr;
                                if (section.Asset != 0 && !clip)
                                    AssetManager::RequestAsset(section.Asset);
                                length = clip ? std::max(clip->GetDuration() - std::max(section.Value.y, 0.0f), 0.0f) : duration;
                            }
                            float end = std::min(section.Time + length, duration);
                            if (k + 1 < track.Keys.size())
                                end = std::min(end, track.Keys[k + 1].Time);
                            const float x0 = TimeToX(section.Time), x1 = TimeToX(end);
                            d.AddRoundedRect({ x0, top + 3.0f }, { x1, top + kLane - 4.0f }, section.Asset != 0 ? NoxUI::ThemeColor("TimelineAudioSection", { 0.24f, 0.47f, 0.35f, 0.8f }) : NoxUI::ThemeColor("TimelineAudioSectionMissing", { 0.43f, 0.24f, 0.24f, 0.8f }), glm::vec4(3.0f));
                            d.PushClipRect({ x0, top }, { x1, top + kLane });
                            d.AddText(section.Asset != 0 ? AssetManager::GetMetadata(section.Asset).FilePath.stem().string() : std::string("(no sound)"), nullptr, { x0 + 9.0f, top + 5.0f }, 11.5f, text);
                            d.PopClipRect();
                        }
                    for (size_t k = 0; k < track.Keys.size(); ++k)
                    {
                        const float x = TimeToX(track.Keys[k].Time), y = top + kLane * 0.5f;
                        const bool keySelected = selected && static_cast<int>(k) == m_Editor->SelectedKey;
                        const glm::vec4 color = keySelected ? NoxUI::ThemeColor("TimelineKeySelected", { 1.0f, 0.78f, 0.27f, 1.0f })
                                              : track.Property == TimelineProperty::Event ? NoxUI::ThemeColor("TimelineKeyEvent", { 0.47f, 0.78f, 1.0f, 1.0f })
                                              : track.Property == TimelineProperty::CameraCut ? NoxUI::ThemeColor("TimelineKeyCameraCut", { 0.78f, 0.55f, 1.0f, 1.0f }) : NoxUI::ThemeColor("TimelineKey", { 0.86f, 0.86f, 0.86f, 1.0f });
                        d.AddRoundedRect({ x - 5.0f, y - 5.0f }, { x + 5.0f, y + 5.0f }, color, glm::vec4(2.0f), 1.0f, NoxUI::ThemeColor("TimelineKeyBorder", { 0.08f, 0.08f, 0.08f, 1.0f }));
                    }
                    d.PopClipRect();
                }
                if (timeline->Tracks.empty())
                    d.AddText(player ? "No tracks: + Track picks an entity and a property." : "No tracks yet.", nullptr, { b.Min.x + 8.0f, b.Min.y + kRuler + 8.0f }, 13.0f, dim);

                // Playhead.
                d.PushClipRect({ b.Min.x + kLabelWidth, b.Min.y }, { b.Max.x, b.Max.y });
                const float playhead = TimeToX(std::clamp(m_Editor->Time, 0.0f, duration));
                d.AddRoundedRect({ playhead - 1.0f, b.Min.y }, { playhead + 1.0f, b.Max.y }, NoxUI::ThemeColor("TimelinePlayhead", { 1.0f, 0.31f, 0.24f, 1.0f }));
                d.AddRoundedRect({ playhead - 5.0f, b.Min.y }, { playhead + 5.0f, b.Min.y + 8.0f }, NoxUI::ThemeColor("TimelinePlayhead", { 1.0f, 0.31f, 0.24f, 1.0f }), glm::vec4(0.0f, 0.0f, 5.0f, 5.0f));
                d.PopClipRect();
                d.PopClipRect();
            }

        private:
            enum class Drag { None, Scrub, Key };
            float PixelsPerSecond() const { return m_Editor->PixelsPerSecond; }
            float LaneLeft() const { return GetBounds().Min.x + kLabelWidth - m_ScrollX; }
            float TimeToX(float time) const { return LaneLeft() + time * PixelsPerSecond(); }
            float XToTime(float x, float duration) const { return std::clamp((x - LaneLeft()) / PixelsPerSecond(), 0.0f, std::max(duration, 0.01f)); }
            int TrackAt(float y) const
            {
                const Timeline* timeline = m_Editor->GetTimeline();
                const int track = static_cast<int>((y - GetBounds().Min.y - kRuler + m_ScrollY) / kLane);
                return timeline && y >= GetBounds().Min.y + kRuler && track >= 0 && track < static_cast<int>(timeline->Tracks.size()) ? track : -1;
            }
            void Scrub(float x)
            {
                if (const Timeline* timeline = m_Editor->GetTimeline())
                {
                    m_Editor->Time = SnapTime(XToTime(x, timeline->Duration));
                    m_Editor->Playing = false;
                }
            }
            void UpdateTooltip(const NoxUI::UIVec2& p)
            {
                const Timeline* timeline = m_Editor->GetTimeline();
                const int track = TrackAt(p.y);
                Tooltip.clear();
                if (!timeline)
                    return;
                if (p.y < GetBounds().Min.y + kRuler)
                {
                    Tooltip = "Click or drag: scrub (snapped to 0.01 s; Ctrl: free)";
                    return;
                }
                if (track < 0)
                    return;
                const TimelineTrack& lane = timeline->Tracks[static_cast<size_t>(track)];
                if (p.x < GetBounds().Min.x + kLabelWidth)
                {
                    const float x = p.x - GetBounds().Min.x;
                    Tooltip = x < 22.0f ? "Muted tracks do not play (click to switch)"
                            : x > kLabelWidth - 30.0f ? "Remove the track"
                            : x > kLabelWidth - 70.0f ? (lane.Property == TimelineProperty::Event ? "Adds an event key at the playhead (named below)"
                                                       : lane.Property == TimelineProperty::CameraCut ? "Cuts to the camera given below at the playhead (in Play)"
                                                       : lane.Property == TimelineProperty::SubTimeline ? "Starts a section of the child timeline at the playhead"
                                                       : lane.Property == TimelineProperty::Audio ? "Starts a sound section at the playhead (pick its sound below)"
                                                                                                  : "Keys the target's current value at the playhead (move it with the gizmo first)")
                            : TrackName(lane, m_Editor->GetPlayer());
                    return;
                }
                for (const TimelineKey& key : lane.Keys)
                    if (std::abs(TimeToX(key.Time) - p.x) <= 7.0f)
                    {
                        Tooltip = std::format("{:.2f} s{}{}\nDrag: move (snapped; Ctrl: free). Delete: remove.", key.Time, key.Event.empty() ? "" : "  ", key.Event);
                        return;
                    }
            }

            TimelineEditorV2* m_Editor = nullptr;
            float m_ScrollX = 0.0f, m_ScrollY = 0.0f;
            Drag m_Drag = Drag::None;
            bool m_Moved = false;
        };


    }

    TimelineEditorV2::TimelineEditorV2(AssetHandle timeline, Hooks hooks)
        : m_Handle(timeline), m_Name(AssetDisplayName(timeline)), m_Hooks(std::move(hooks))
    {
    }

    TimelineEditorV2::~TimelineEditorV2()
    {
        EndPreview(); // (what the targets had comes back)
    }

    Timeline* TimelineEditorV2::GetTimeline() const
    {
        if (!AssetManager::IsAssetHandleValid(m_Handle) || AssetManager::GetAssetType(m_Handle) != AssetType::Timeline)
            return nullptr;
        return AssetManager::FindLoadedAsset<Timeline>(m_Handle);
    }

    TimelineEditorV2::State TimelineEditorV2::Capture() const
    {
        State state;
        if (const Timeline* timeline = GetTimeline())
            state = { timeline->Duration, timeline->Loop, timeline->Tracks };
        return state;
    }

    void TimelineEditorV2::Restore(const State& state)
    {
        Timeline* timeline = GetTimeline();
        if (!timeline)
            return;
        timeline->Duration = state.Duration;
        timeline->Loop = state.Loop;
        timeline->Tracks = state.Tracks;
        if (SelectedTrack >= static_cast<int>(timeline->Tracks.size()))
            SelectedTrack = -1;
        SelectedKey = -1;
        EndPreview(); // (the targets may have changed: posed again from a new snapshot)
        m_Dirty = true;
    }

    void TimelineEditorV2::Edited()
    {
        m_Dirty = true;
        m_ChangePending = true;
        m_LastPreviewTime = -1.0f; // pose again with the edit
    }

    void TimelineEditorV2::CommitHistory()
    {
        m_UndoStack.push_back(std::move(m_Committed));
        if (m_UndoStack.size() > 200)
            m_UndoStack.erase(m_UndoStack.begin());
        m_RedoStack.clear();
        m_Committed = Capture();
        m_ChangePending = false;
    }

    void TimelineEditorV2::Undo()
    {
        if (m_ChangePending)
            CommitHistory();
        if (m_UndoStack.empty())
            return;
        m_RedoStack.push_back(Capture());
        Restore(m_UndoStack.back());
        m_UndoStack.pop_back();
        m_Committed = Capture();
    }

    void TimelineEditorV2::Redo()
    {
        if (m_RedoStack.empty() || m_ChangePending)
            return;
        m_UndoStack.push_back(Capture());
        Restore(m_RedoStack.back());
        m_RedoStack.pop_back();
        m_Committed = Capture();
    }

    bool TimelineEditorV2::HandleKey(const NoxUI::UIKeyEvent& e)
    {
        const int keyCode = e.KeyCode;
        const uint16_t mods = e.Modifiers;
        if (keyCode == SDL_SCANCODE_DELETE && !(mods & NoxUI::UIMod::Ctrl))
        {
            Timeline* timeline = GetTimeline();
            if (!timeline || SelectedTrack < 0 || SelectedTrack >= static_cast<int>(timeline->Tracks.size()))
                return false;
            auto& keys = timeline->Tracks[static_cast<size_t>(SelectedTrack)].Keys;
            if (SelectedKey < 0 || SelectedKey >= static_cast<int>(keys.size()))
                return false;
            keys.erase(keys.begin() + SelectedKey);
            SelectedKey = -1;
            Edited();
            return true;
        }
        if (!(mods & NoxUI::UIMod::Ctrl))
            return false;
        switch (keyCode)
        {
        case SDL_SCANCODE_S: Save(); return true;
        case SDL_SCANCODE_Z: if (mods & NoxUI::UIMod::Shift) Redo(); else Undo(); return true;
        case SDL_SCANCODE_Y: Redo(); return true;
        default: return false;
        }
    }

    void TimelineEditorV2::Save()
    {
        Timeline* timeline = GetTimeline();
        if (!timeline || !m_Dirty)
            return;
        for (TimelineTrack& track : timeline->Tracks)
            TimelineEvaluation::SortKeys(track);
        const std::filesystem::path path = Project::GetActiveAssetDirectory() / AssetManager::GetMetadata(m_Handle).FilePath;
        if (!TimelineImporter::Serialize(path, *timeline))
        {
            NOX_CORE_WARN("Could not save the timeline {}", path.string());
            if (Notify)
                Notify({ NoxUI::UINotificationSeverity::Error, "Timeline not saved", path.filename().string() + " could not be written." });
            return;
        }
        if (auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr)
            assets->NoteSourceWritten(m_Handle);
        m_Dirty = false;
        if (Notify)
            Notify({ NoxUI::UINotificationSeverity::Success, "Timeline saved", m_Name });
    }

    bool TimelineEditorV2::RevertToDisk()
    {
        Timeline* timeline = GetTimeline();
        if (!timeline)
            return true;
        const std::filesystem::path path = Project::GetActiveAssetDirectory() / AssetManager::GetMetadata(m_Handle).FilePath;
        Timeline fromFile;
        if (!TimelineImporter::Deserialize(path, fromFile))
        {
            if (Notify)
                Notify({ NoxUI::UINotificationSeverity::Error, "Changes not discarded", m_Name + ": the file could not be read back. The edits stay." });
            return false;
        }
        EndPreview();
        timeline->Duration = fromFile.Duration;
        timeline->Loop = fromFile.Loop;
        timeline->Tracks = std::move(fromFile.Tracks);
        m_Dirty = false;
        m_UndoStack.clear();
        m_RedoStack.clear();
        m_Committed = Capture();
        m_ChangePending = false;
        SelectedTrack = SelectedKey = -1;
        return true;
    }

    Entity TimelineEditorV2::FindPlayer(Scene& scene, Entity selected)
    {
        const auto plays = [this](Entity entity)
        {
            return entity && entity.HasComponent<TimelinePlayerComponent>() && entity.GetComponent<TimelinePlayerComponent>().Timeline == m_Handle;
        };
        if (plays(selected))
            m_Player = selected.GetUUID();
        if (m_Player != 0)
            if (Entity remembered = scene.GetEntityByUUID(m_Player); plays(remembered))
                return remembered;
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

    void TimelineEditorV2::BeginPreview(Scene& scene, Entity player, const Timeline& timeline)
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
            if (target.HasComponent<MeshComponent>())
                state.SequencerMorphWeights = target.GetComponent<MeshComponent>().SequencerMorphWeights;
            if (target.HasComponent<MaterialComponent>())
                state.SequencerParameters = target.GetComponent<MaterialComponent>().SequencerParameters;
            snapshot.Targets.emplace(target.GetUUID(), state);
        }
        m_Snapshot = std::move(snapshot);
    }

    void TimelineEditorV2::StopPreviewAudio()
    {
        for (const auto& [id, section] : m_PreviewAudio)
            AudioEngine::Stop(section.Voice, 0.1f);
        m_PreviewAudio.clear();
    }

    void TimelineEditorV2::EndPreview()
    {
        StopPreviewAudio();
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
                const auto restoreLight = [&](auto& light)
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
                if (state.SequencerMorphWeights && target.HasComponent<MeshComponent>())
                    target.GetComponent<MeshComponent>().SequencerMorphWeights = *state.SequencerMorphWeights;
                if (state.SequencerParameters && target.HasComponent<MaterialComponent>())
                    target.GetComponent<MaterialComponent>().SequencerParameters = *state.SequencerParameters;
            }
        }
        m_Snapshot.reset();
        m_LastPreviewTime = -1.0f;
    }

    std::optional<glm::vec4> TimelineEditorV2::ReadTargetValue(Scene& scene, Entity player, const TimelineTrack& track)
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
            return target.HasComponent<CameraComponent>() ? std::optional(glm::vec4(glm::degrees(target.GetComponent<CameraComponent>().Camera.GetPerspectiveVerticalFOV()))) : std::nullopt;
        case TimelineProperty::MorphWeight:
        {
            if (!target.HasComponent<MeshComponent>())
                return std::nullopt;
            const MeshComponent& mesh = target.GetComponent<MeshComponent>();
            for (const std::map<std::string, float>* weights : { &mesh.MorphWeights, &mesh.SequencerMorphWeights, &mesh.AnimatedMorphWeights })
                if (const auto found = weights->find(track.Parameter); found != weights->end())
                    return glm::vec4(found->second);
            for (const auto& [name, weight] : GetMeshMorphTargets(mesh.Mesh))
                if (name == track.Parameter)
                    return glm::vec4(weight);
            return glm::vec4(0.0f);
        }
        case TimelineProperty::MaterialParameter:
        {
            const MaterialParameter parameter = MaterialParameters::FromName(track.Parameter);
            if (parameter == MaterialParameter::Count || !target.HasComponent<MaterialComponent>())
                return std::nullopt;
            const MaterialComponent& material = target.GetComponent<MaterialComponent>();
            for (const std::vector<MaterialComponent::Parameter>* list : { &material.SequencerParameters, &material.Parameters })
                for (auto it = list->rbegin(); it != list->rend(); ++it)
                    if (it->Name == parameter)
                        return it->Value;
            if (target.HasComponent<MeshComponent>())
            {
                const AssetHandle mesh = target.GetComponent<MeshComponent>().Mesh;
                const uint32_t slot = target.GetComponent<MeshComponent>().SubmeshIndex;
                AssetHandle asset = slot < material.MaterialAssets.size() ? material.MaterialAssets[slot] : AssetHandle(0);
                if (asset == 0)
                {
                    if (const StaticMesh* staticMesh = AssetManager::FindLoadedAsset<StaticMesh>(mesh); staticMesh && slot < staticMesh->GetMaterialAssets().size())
                        asset = staticMesh->GetMaterialAssets()[slot];
                    else if (const Mesh* skeletal = AssetManager::FindLoadedAsset<Mesh>(mesh); skeletal && slot < skeletal->GetMaterialAssets().size())
                        asset = skeletal->GetMaterialAssets()[slot];
                }
                if (const Material* loaded = asset != 0 ? AssetManager::FindLoadedAsset<Material>(asset) : nullptr)
                    return MaterialParameters::Read(loaded->GetData(), parameter);
            }
            return MaterialParameters::Read(MaterialData{}, parameter);
        }
        default:
            return glm::vec4(0.0f);
        }
    }

    void TimelineEditorV2::AddKeyAtPlayhead(int trackIndex)
    {
        Timeline* timeline = GetTimeline();
        if (!timeline || trackIndex < 0 || trackIndex >= static_cast<int>(timeline->Tracks.size()))
            return;
        TimelineTrack& track = timeline->Tracks[static_cast<size_t>(trackIndex)];
        // The target's value now, at the playhead (replacing a key already there); an event key is named in the details.
        TimelineKey key;
        key.Time = Time;
        if (track.Property == TimelineProperty::Event)
            key.Event = m_NewEventName;
        else if (track.Property == TimelineProperty::CameraCut)
        {
            key.Event = m_CutCamera;
            key.Interpolation = TimelineInterpolation::Constant;
        }
        else if (track.Property == TimelineProperty::Audio || track.Property == TimelineProperty::SubTimeline)
        {
            key.Value = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f); // the whole sound / child, from its start, full volume / own speed
            key.Interpolation = TimelineInterpolation::Constant;
        }
        else
        {
            Ref<Scene> scene = m_Hooks.ActiveScene ? m_Hooks.ActiveScene() : nullptr;
            const std::optional<glm::vec4> value = scene && m_PlayerEntity ? ReadTargetValue(*scene, m_PlayerEntity, track) : std::nullopt;
            if (!value)
            {
                if (Notify)
                    Notify({ NoxUI::UINotificationSeverity::Warning, "No key added", "The track's target is missing, or has no such component." });
                return;
            }
            key.Value = *value;
        }
        auto existing = std::find_if(track.Keys.begin(), track.Keys.end(), [&](const TimelineKey& other) { return std::abs(other.Time - key.Time) < 1e-3f; });
        if (existing != track.Keys.end())
        {
            key.Interpolation = existing->Interpolation;
            *existing = key;
        }
        else
            track.Keys.push_back(key);
        TimelineEvaluation::SortKeys(track);
        SelectedTrack = trackIndex;
        SelectedKey = static_cast<int>(std::find_if(track.Keys.begin(), track.Keys.end(), [&](const TimelineKey& other) { return std::abs(other.Time - key.Time) < 1e-3f; }) - track.Keys.begin());
        Edited();
    }

    void TimelineEditorV2::RemoveTrack(int trackIndex)
    {
        Timeline* timeline = GetTimeline();
        if (!timeline || trackIndex < 0 || trackIndex >= static_cast<int>(timeline->Tracks.size()))
            return;
        EndPreview(); // (what its target had comes back before it leaves the snapshot)
        timeline->Tracks.erase(timeline->Tracks.begin() + trackIndex);
        SelectedTrack = SelectedKey = -1;
        Edited();
    }

    void TimelineEditorV2::AutoKey(Timeline& timeline, Scene& scene, Entity player)
    {
        // Not while a mouse button is held (a gizmo drag): the key goes in once the move is done.
        if (NoxUI::UIContext::IsMouseButtonHeld())
            return;
        bool keyed = false;
        for (TimelineTrack& track : timeline.Tracks)
        {
            const int components = TimelineEvaluation::GetComponentCount(track);
            if (track.Muted || track.Keys.empty() || components == 0)
                continue;
            const std::optional<glm::vec4> value = ReadTargetValue(scene, player, track);
            if (!value)
                continue;
            const glm::vec4 expected = TimelineEvaluation::Evaluate(track, Time);
            bool moved = false;
            for (int c = 0; c < components; ++c)
                moved |= std::abs((*value)[c] - expected[c]) > 1e-3f * std::max(1.0f, std::abs(expected[c]));
            if (!moved)
                continue;
            auto existing = std::find_if(track.Keys.begin(), track.Keys.end(), [&](const TimelineKey& key) { return std::abs(key.Time - Time) < 1e-3f; });
            if (existing != track.Keys.end())
                existing->Value = *value;
            else
            {
                TimelineKey key;
                key.Time = Time;
                key.Value = *value;
                track.Keys.push_back(key);
                TimelineEvaluation::SortKeys(track);
            }
            keyed = true;
        }
        if (keyed)
            Edited();
    }

    void TimelineEditorV2::ShowAddTrackMenu(const NoxUI::UIRect& anchor)
    {
        NoxUI::UIContext* context = Context();
        Ref<Scene> scene = m_Hooks.ActiveScene ? m_Hooks.ActiveScene() : nullptr;
        if (!context || !scene || !m_PlayerEntity)
            return;
        Entity player = m_PlayerEntity;
        // The player and every entity below it, each with the properties its components have.
        std::function<NoxUI::UIMenuItem(Entity)> entityMenu = [&](Entity entity) -> NoxUI::UIMenuItem
        {
            const std::string path = PathFrom(player, entity);
            std::vector<NoxUI::UIMenuItem> properties;
            const auto property = [this, &properties, path](TimelineProperty kind, const std::string& parameter = {})
            {
                properties.push_back({ parameter.empty() ? TimelineEvaluation::GetPropertyName(kind) : parameter, "", [this, path, kind, parameter]()
                {
                    Timeline* timeline = GetTimeline();
                    if (!timeline)
                        return;
                    TimelineTrack track;
                    track.Target = path;
                    track.Property = kind;
                    track.Parameter = parameter;
                    timeline->Tracks.push_back(std::move(track));
                    SelectedTrack = static_cast<int>(timeline->Tracks.size()) - 1;
                    SelectedKey = -1;
                    EndPreview(); // (the new target joins the snapshot)
                    Edited();
                } });
            };
            property(TimelineProperty::Translation);
            property(TimelineProperty::Rotation);
            property(TimelineProperty::Scale);
            if (entity.HasComponent<DirectionalLightComponent>() || entity.HasComponent<PointLightComponent>() || entity.HasComponent<SpotLightComponent>())
            {
                property(TimelineProperty::LightColor);
                property(TimelineProperty::LightIntensity);
            }
            if (entity.HasComponent<CameraComponent>())
                property(TimelineProperty::CameraFOV);
            if (entity.HasComponent<MeshComponent>())
            {
                std::vector<NoxUI::UIMenuItem> morphs;
                for (const auto& [target, defaultWeight] : GetMeshMorphTargets(entity.GetComponent<MeshComponent>().Mesh))
                {
                    std::vector<NoxUI::UIMenuItem> holder;
                    std::swap(holder, properties);
                    property(TimelineProperty::MorphWeight, target);
                    morphs.push_back(properties.back());
                    std::swap(holder, properties);
                }
                if (!morphs.empty())
                    properties.push_back({ TimelineEvaluation::GetPropertyName(TimelineProperty::MorphWeight), "", {}, false, true, false, false, "", std::move(morphs) });
                std::vector<NoxUI::UIMenuItem> parameters;
                for (uint8_t index = 0; index < static_cast<uint8_t>(MaterialParameter::Count); ++index)
                {
                    std::vector<NoxUI::UIMenuItem> holder;
                    std::swap(holder, properties);
                    property(TimelineProperty::MaterialParameter, MaterialParameters::Name(static_cast<MaterialParameter>(index)));
                    parameters.push_back(properties.back());
                    std::swap(holder, properties);
                }
                properties.push_back({ TimelineEvaluation::GetPropertyName(TimelineProperty::MaterialParameter), "", {}, false, true, false, false, "", std::move(parameters) });
            }
            if (path.empty())
            {
                property(TimelineProperty::Event);
                property(TimelineProperty::CameraCut);
            }
            property(TimelineProperty::SubTimeline);
            property(TimelineProperty::Audio);
            // The children, below its own properties.
            if (entity.HasComponent<RelationshipComponent>())
                for (UUID childID : entity.GetComponent<RelationshipComponent>().Children)
                    if (Entity child = scene->GetEntityByUUID(childID))
                        properties.push_back(entityMenu(child));
            return { path.empty() ? player.GetName() + " (player)" : path, "", {}, false, true, false, false, "", std::move(properties) };
        };
        NoxUI::UIMenuPopup::ShowPopupMenu(*context, anchor, { entityMenu(player) });
    }

    Ref<NoxUI::UIElement> TimelineEditorV2::BuildUI()
    {
        NoxUI::UIContext* context = Context();
        // The window's root: keys typed in it that its children leave (Ctrl+S, Ctrl+Z, Ctrl+Y, Delete) come here.
        auto root = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 6.0f);
        root->OnKey = [this](const NoxUI::UIKeyEvent& e) { return HandleKey(e); };
        root->WidthMode = NoxUI::UISizeMode::Fill;
        root->HeightMode = NoxUI::UISizeMode::Fill;
        root->StyleClass = "Surface";
        root->DrawBackground = true;
        root->Padding = NoxUI::UIThickness::All(8.0f);
        root->Focusable = true;
        m_Root = root;

        auto bar = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 6.0f);
        bar->WidthMode = NoxUI::UISizeMode::Fill;
        Ref<NoxUI::UIButton> save(new NoxUI::UIButton("Save", [this]() { Save(); }));
        Ref<NoxUI::UIButton> undo(new NoxUI::UIButton("Undo", [this]() { Undo(); }));
        Ref<NoxUI::UIButton> redo(new NoxUI::UIButton("Redo", [this]() { Redo(); }));
        Ref<NoxUI::UIButton> play(new NoxUI::UIButton("Play", [this]()
        {
            const Timeline* timeline = GetTimeline();
            if (!Playing && timeline && Time >= timeline->Duration)
                Time = 0.0f;
            Playing = !Playing;
        }));
        Ref<NoxUI::UIButton> start(new NoxUI::UIButton("|<", [this]() { Time = 0.0f; Playing = false; }));
        start->Tooltip = "Back to the start";
        auto timeLabel = NoxUI::Label("", 13.5f);
        Ref<NoxUI::UILabel> timeRef = timeLabel;
        timeRef->VAlign = NoxUI::UIAlignment::Center;
        auto duration = NoxUI::MakeUI<NoxUI::UIDragScalar>(5.0f, 0.05f, [this](float v) { if (Timeline* t = GetTimeline()) { t->Duration = std::max(v, 0.1f); Changing(); } });
        duration->OnCommitted = [this](float) { Edited(); };
        duration->Min = 0.1f;
        duration->Max = 3600.0f;
        duration->Prefix = "Duration ";
        duration->Format = "{:.2f} s";
        duration->WidthMode = NoxUI::UISizeMode::Fixed;
        duration->FixedSize = { 140.0f, 24.0f };
        Ref<NoxUI::UICheckBox> loop(new NoxUI::UICheckBox("Loop", false, [this](bool v) { if (Timeline* t = GetTimeline()) { t->Loop = v; Edited(); } }));
        Ref<NoxUI::UICheckBox> preview(new NoxUI::UICheckBox("Preview", m_PreviewWanted, [this](bool v) { m_PreviewWanted = v; }));
        preview->Tooltip = "Poses the targets at the playhead while editing; what they had comes back when it is off, the window closes, the scene is saved or Play starts.";
        Ref<NoxUI::UICheckBox> autoKey(new NoxUI::UICheckBox("Auto-Key", m_AutoKey, [this](bool v) { m_AutoKey = v; }));
        autoKey->Tooltip = "While previewing: moving a track's target (gizmo, Inspector) keys its new value at the playhead.";
        Ref<NoxUI::UICheckBox> curves(new NoxUI::UICheckBox("Curves", m_ShowCurves, [this](bool v) { m_ShowCurves = v; }));
        curves->Tooltip = "The selected track's values over time; drag a key up or down to change it.";
        Ref<NoxUI::UIDragScalar> zoom = CreateSliderField(PixelsPerSecond, 20.0f, 600.0f, [this](float v) { PixelsPerSecond = v; });
        zoom->Format = "Zoom {:.0f} px/s";
        zoom->WidthMode = NoxUI::UISizeMode::Fixed;
        zoom->FixedSize = { 150.0f, 24.0f };
        Ref<NoxUI::UIButton> addTrack(new NoxUI::UIButton("+ Track"));
        NoxUI::UIButton* addRaw = addTrack.get();
        addTrack->OnClick = [this, addRaw]() { ShowAddTrackMenu(addRaw->GetBounds()); };
        addTrack->Tooltip = "A property of the player or an entity under it";
        for (const Ref<NoxUI::UIElement>& element : std::initializer_list<Ref<NoxUI::UIElement>>{ save, undo, redo, play, start, timeRef, duration, loop, preview, autoKey, curves, zoom, addTrack })
            bar->AddChild(element);
        root->AddChild(bar);
        auto status = NoxUI::Label("", 13.0f);
        Ref<NoxUI::UILabel> statusRef = status;
        statusRef->WordWrap = true;
        statusRef->WidthMode = NoxUI::UISizeMode::Fill;
        root->AddChild(statusRef);
        NoxUI::AppendSync(*root, [this, save, undo, redo, play, timeRef, duration, loop, preview, autoKey, curves, zoom, addTrack, statusRef]()
        {
            const Timeline* timeline = GetTimeline();
            save->SetEnabled(m_Dirty);
            save->StyleVariant = m_Dirty ? "primary" : "";
            undo->SetEnabled(!m_UndoStack.empty() || m_ChangePending);
            redo->SetEnabled(!m_RedoStack.empty() && !m_ChangePending);
            play->SetText(Playing ? "Pause" : "Play");
            timeRef->Text = std::format("{:6.2f} s", Time);
            if (timeline && !duration->IsInteracting())
                duration->Value = timeline->Duration;
            loop->Checked = timeline && timeline->Loop;
            preview->Checked = m_PreviewWanted;
            autoKey->Checked = m_AutoKey;
            autoKey->SetEnabled(m_PreviewWanted);
            curves->Checked = m_ShowCurves;
            if (!zoom->IsInteracting())
                zoom->Value = PixelsPerSecond;
            addTrack->SetEnabled(m_PlayerEntity);
            const bool editing = m_Hooks.Editing && m_Hooks.Editing();
            statusRef->Text = !timeline ? std::string("Loading...")
                            : !editing ? std::string("Previews while editing (Play and Simulate play it on its Timeline Players).")
                            : !m_PlayerEntity ? std::string("No entity plays this timeline: add a Timeline Player to one and pick it.")
                                              : "Playing on: " + m_PlayerEntity.GetName() + (m_Dirty ? "   |   unsaved" : "");
        });

        root->AddChild(Ref<TracksView>(new TracksView(this)));
        // The selected track's values over time (UI::UIChannelCurves): drag a key's dot up or down (Shift: also its time).
        Ref<NoxUI::UIChannelCurves> curvesView(new NoxUI::UIChannelCurves());
        curvesView->Tooltip = "The selected track's values. Drag a key's dot up / down to change its value (Shift: also its time).";
        const auto selectedTrack = [this]() -> TimelineTrack*
        {
            Timeline* timeline = GetTimeline();
            return timeline && SelectedTrack >= 0 && SelectedTrack < static_cast<int>(timeline->Tracks.size()) ? &timeline->Tracks[static_cast<size_t>(SelectedTrack)]
                                                                                                                : nullptr;
        };
        curvesView->ChannelCount = [selectedTrack]() { const TimelineTrack* t = selectedTrack(); return t ? TimelineEvaluation::GetComponentCount(*t) : -1; };
        curvesView->KeyCount = [selectedTrack]() { const TimelineTrack* t = selectedTrack(); return t ? static_cast<int>(t->Keys.size()) : 0; };
        curvesView->KeyTime = [selectedTrack](int k) { return selectedTrack()->Keys[static_cast<size_t>(k)].Time; };
        curvesView->KeyValue = [selectedTrack](int k, int c) { return selectedTrack()->Keys[static_cast<size_t>(k)].Value[c]; };
        curvesView->Evaluate = [selectedTrack](float time, int c) { const TimelineTrack* t = selectedTrack(); return t ? TimelineEvaluation::Evaluate(*t, time)[c] : 0.0f; };
        curvesView->SetKey = [selectedTrack](int k, int c, float time, float value)
        {
            if (TimelineTrack* t = selectedTrack(); t && k < static_cast<int>(t->Keys.size()))
            {
                t->Keys[static_cast<size_t>(k)].Time = time;
                t->Keys[static_cast<size_t>(k)].Value[c] = value;
            }
        };
        curvesView->Duration = [this]() { const Timeline* timeline = GetTimeline(); return timeline ? timeline->Duration : 1.0f; };
        curvesView->Playhead = [this]() { return Time; };
        curvesView->SelectedKey = [this]() { return SelectedKey; };
        curvesView->SnapTime = [](float time) { return SnapTime(time); };
        curvesView->OnSelectKey = [this](int key) { SelectedKey = key; };
        curvesView->OnChanging = [this]() { Changing(); };
        curvesView->OnChanged = [this, selectedTrack]()
        {
            if (TimelineTrack* t = selectedTrack())
                TimelineEvaluation::SortKeys(*t);
            Edited();
        };
        curvesView->NothingText = "Select a track to see its curves.";
        curvesView->NoValuesText = "This track has no values.";
        m_Curves = curvesView;
        root->AddChild(curvesView);
        NoxUI::AppendSync(*root, [this]() { m_Curves->Visible = m_ShowCurves; });

        m_Details = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 4.0f);
        m_Details->WidthMode = NoxUI::UISizeMode::Fill;
        root->AddChild(m_Details);
        m_DetailsTrack = m_DetailsKey = -2;
        (void)context;
        return m_Root;
    }

    void TimelineEditorV2::RebuildDetails()
    {
        m_Details->ClearChildren();
        Timeline* timeline = GetTimeline();
        m_DetailsTrack = SelectedTrack;
        m_DetailsKey = SelectedKey;
        m_DetailsKeyCount = timeline && SelectedTrack >= 0 && SelectedTrack < static_cast<int>(timeline->Tracks.size()) ? timeline->Tracks[static_cast<size_t>(SelectedTrack)].Keys.size() : 0;
        NoxUI::UIPropertyRows rows(m_Details, Context());
        if (!timeline || SelectedTrack < 0 || SelectedTrack >= static_cast<int>(timeline->Tracks.size()))
        {
            rows.Note("Select a key to edit it.");
            rows.String("New Event Name", &m_NewEventName, "Event", {}, "The name an event track's Key button gives its new key.");
            return;
        }
        const int trackIndex = SelectedTrack;
        TimelineTrack& track = timeline->Tracks[static_cast<size_t>(trackIndex)];
        // Index-based: keys move when they are sorted, the rows read the selected one each time.
        const auto key = [this, trackIndex]() -> TimelineKey*
        {
            Timeline* t = GetTimeline();
            if (!t || trackIndex >= static_cast<int>(t->Tracks.size()))
                return nullptr;
            auto& keys = t->Tracks[static_cast<size_t>(trackIndex)].Keys;
            return SelectedKey >= 0 && SelectedKey < static_cast<int>(keys.size()) ? &keys[static_cast<size_t>(SelectedKey)] : nullptr;
        };
        const auto pickChildTimeline = [this, trackIndex, &rows]()
        {
            InspectorCardBuilder slot{ m_Details, {}, Context() };
            Timeline* t = GetTimeline();
            const AssetHandle child = t ? static_cast<AssetHandle>(std::strtoull(t->Tracks[static_cast<size_t>(trackIndex)].Parameter.c_str(), nullptr, 10)) : AssetHandle(0);
            slot.AddAssetSlot("Child Timeline", child, { AssetType::Timeline }, [this, trackIndex](AssetHandle picked)
            {
                Timeline* now = GetTimeline();
                if (!now || picked == m_Handle || trackIndex >= static_cast<int>(now->Tracks.size()))
                    return;
                now->Tracks[static_cast<size_t>(trackIndex)].Parameter = std::to_string(static_cast<uint64_t>(picked));
                AssetManager::RequestAsset(picked);
                Edited();
            });
            (void)rows;
        };
        if (!key())
        {
            rows.Note(std::string(TimelineEvaluation::GetPropertyName(track.Property)) + ": select a key to edit it.");
            if (track.Property == TimelineProperty::Event)
                rows.String("New Event Name", &m_NewEventName, "Event");
            if (track.Property == TimelineProperty::CameraCut)
                rows.String("Cut to Camera", &m_CutCamera, "name path from the player, e.g. Cameras/Close");
            if (track.Property == TimelineProperty::SubTimeline)
                pickChildTimeline();
            return;
        }
        const auto changing = [this]() { Changing(); };
        rows.Drag("Time (s)", [key]() { TimelineKey* k = key(); return k ? k->Time : 0.0f; }, [key, changing](float v) { if (TimelineKey* k = key()) { k->Time = v; changing(); } },
                  0.01f, 0.0f, timeline->Duration, "{:.2f}");
        rows.Button("Go to", [this, key]() { if (TimelineKey* k = key()) { Time = k->Time; Playing = false; } });
        const auto component = [&rows, key, changing](const std::string& label, int c, float speed, float min, float max, const char* format)
        {
            rows.Drag(label, [key, c]() { TimelineKey* k = key(); return k ? k->Value[c] : 0.0f; }, [key, c, changing](float v) { if (TimelineKey* k = key()) { k->Value[c] = v; changing(); } },
                      speed, min, max, format);
        };
        switch (track.Property)
        {
        case TimelineProperty::Translation:
        case TimelineProperty::Scale:
            component("X", 0, 0.01f, 0.0f, 0.0f, "{:.3f}");
            component("Y", 1, 0.01f, 0.0f, 0.0f, "{:.3f}");
            component("Z", 2, 0.01f, 0.0f, 0.0f, "{:.3f}");
            break;
        case TimelineProperty::Rotation:
            component("X (deg)", 0, 0.5f, 0.0f, 0.0f, "{:.1f}");
            component("Y (deg)", 1, 0.5f, 0.0f, 0.0f, "{:.1f}");
            component("Z (deg)", 2, 0.5f, 0.0f, 0.0f, "{:.1f}");
            break;
        case TimelineProperty::LightColor:
            component("Red", 0, 0.01f, 0.0f, 1.0f, "{:.3f}");
            component("Green", 1, 0.01f, 0.0f, 1.0f, "{:.3f}");
            component("Blue", 2, 0.01f, 0.0f, 1.0f, "{:.3f}");
            break;
        case TimelineProperty::LightIntensity:
            component("Value", 0, 0.05f, 0.0f, 100000.0f, "{:.2f}");
            break;
        case TimelineProperty::CameraFOV:
            component("Value (deg)", 0, 0.1f, 1.0f, 179.0f, "{:.1f}");
            break;
        case TimelineProperty::MorphWeight:
            component("Weight", 0, 0.01f, 0.0f, 1.0f, "{:.3f}");
            break;
        case TimelineProperty::CameraCut:
        {
            if (TimelineKey* k = key())
                rows.String("Camera", &k->Event, "name path from the player", [this]() { Edited(); }, "From this key on (in Play) this camera, below the player, is the primary one.");
            break;
        }
        case TimelineProperty::MaterialParameter:
            if (TimelineEvaluation::GetComponentCount(track) == 3)
            {
                component("Red", 0, 0.01f, 0.0f, 0.0f, "{:.3f}");
                component("Green", 1, 0.01f, 0.0f, 0.0f, "{:.3f}");
                component("Blue", 2, 0.01f, 0.0f, 0.0f, "{:.3f}");
            }
            else
                component("Value", 0, 0.01f, 0.0f, 1000.0f, "{:.3f}");
            break;
        case TimelineProperty::SubTimeline:
            pickChildTimeline();
            component("Length (s, 0: whole)", 0, 0.01f, 0.0f, 3600.0f, "{:.2f}");
            component("Start in Child (s)", 1, 0.01f, 0.0f, 3600.0f, "{:.2f}");
            component("Rate", 2, 0.01f, -10.0f, 10.0f, "{:.2f}x");
            rows.Check("Loop Child", [key]() { TimelineKey* k = key(); return k && k->Value.w > 0.5f; }, [this, key](bool v) { if (TimelineKey* k = key()) { k->Value.w = v ? 1.0f : 0.0f; Edited(); } });
            break;
        case TimelineProperty::Audio:
        {
            InspectorCardBuilder slot{ m_Details, {}, Context() };
            if (TimelineKey* k = key())
                slot.AddAssetSlot("Sound", k->Asset, { AssetType::AudioClip, AssetType::AudioGraph }, [this, key](AssetHandle picked) { if (TimelineKey* now = key()) { now->Asset = picked; Edited(); } });
            component("Length (s, 0: whole sound)", 0, 0.01f, 0.0f, 3600.0f, "{:.2f}");
            component("Start in Sound (s)", 1, 0.01f, 0.0f, 3600.0f, "{:.2f}");
            component("Volume", 2, 0.01f, 0.0f, 1.0f, "{:.2f}");
            rows.Check("3D (at the track's entity)", [key]() { TimelineKey* k = key(); return k && k->Value.w > 0.5f; },
                       [this, key](bool v) { if (TimelineKey* k = key()) { k->Value.w = v ? 1.0f : 0.0f; Edited(); } },
                       "3D: heard from the track's target entity (it follows it). Off: 2D, as the file is (music, dialogue).");
            break;
        }
        case TimelineProperty::Event:
            if (TimelineKey* k = key())
                rows.String("Event Name", &k->Event, "Event", [this]() { Edited(); }, "The player entity's scripts receive OnAnimationEvent(name) when the playhead passes it.");
            break;
        default:
            break;
        }
        if (!TimelineEvaluation::HasNamedKeys(track.Property))
            rows.Choice("To Next Key", { "Constant", "Linear", "Smooth" }, [key]() { TimelineKey* k = key(); return k ? static_cast<int>(k->Interpolation) : 2; },
                        [this, key](int v) { if (TimelineKey* k = key()) { k->Interpolation = static_cast<TimelineInterpolation>(v); Edited(); } });
    }

    void TimelineEditorV2::Update()
    {
        Timeline* timeline = GetTimeline();
        if (!timeline)
        {
            if (AssetManager::IsAssetHandleValid(m_Handle))
                AssetManager::RequestAsset(m_Handle);
            return;
        }
        if (!m_HaveCommitted)
        {
            m_Committed = Capture();
            m_HaveCommitted = true;
        }
        const auto now = std::chrono::steady_clock::now();
        const float dt = m_LastUpdate.time_since_epoch().count() == 0 ? 0.0f : std::min(std::chrono::duration<float>(now - m_LastUpdate).count(), 0.1f);
        m_LastUpdate = now;

        // The preview runs on the edited scene only; another scene ends it.
        const bool editing = m_Hooks.Editing && m_Hooks.Editing();
        Ref<Scene> sceneRef = editing && m_Hooks.ActiveScene ? m_Hooks.ActiveScene() : nullptr;
        if (m_Snapshot && m_Snapshot->SceneRef != sceneRef)
            EndPreview();
        m_PlayerEntity = sceneRef ? FindPlayer(*sceneRef, m_Hooks.Selected ? m_Hooks.Selected() : Entity{}) : Entity();
        if (!m_PlayerEntity || !m_PreviewWanted || !editing)
            EndPreview();

        if (Playing)
        {
            Time += dt;
            if (Time > timeline->Duration)
            {
                if (timeline->Loop)
                    Time = std::fmod(Time, std::max(timeline->Duration, 0.01f));
                else
                {
                    Time = timeline->Duration;
                    Playing = false;
                }
            }
        }

        // Pose the targets at the playhead (the first time the snapshot of what they had is taken).
        if (sceneRef && m_PlayerEntity && m_PreviewWanted)
        {
            if (!m_Snapshot)
            {
                BeginPreview(*sceneRef, m_PlayerEntity, *timeline);
                m_Snapshot->SceneRef = sceneRef;
                m_LastPreviewTime = -1.0f;
            }
            if (Time != m_LastPreviewTime)
            {
                sceneRef->PreviewTimeline(m_PlayerEntity, Time);
                m_LastPreviewTime = Time;
            }
            else if (m_AutoKey && !Playing)
                AutoKey(*timeline, *sceneRef, m_PlayerEntity);
            // The Audio tracks heard while it plays (as in Play: from the right place in each section; silent when paused).
            std::vector<Scene::TimelineAudioSection> sections;
            if (Playing)
                sceneRef->CollectTimelineAudio(m_PlayerEntity, *timeline, Time, sections);
            sceneRef->SyncTimelineAudio(sections, m_PreviewAudio, NoxAudio::AudioVoiceGroup::Editor);
        }
        else
            StopPreviewAudio();

        // A finished change is one history step: taken when no mouse button is held.
        if (m_ChangePending && !NoxUI::UIContext::IsMouseButtonHeld())
            CommitHistory();

        const size_t keyCount = SelectedTrack >= 0 && SelectedTrack < static_cast<int>(timeline->Tracks.size()) ? timeline->Tracks[static_cast<size_t>(SelectedTrack)].Keys.size() : 0;
        if (m_Details && (SelectedTrack != m_DetailsTrack || SelectedKey != m_DetailsKey || keyCount != m_DetailsKeyCount))
            RebuildDetails();
    }
}
