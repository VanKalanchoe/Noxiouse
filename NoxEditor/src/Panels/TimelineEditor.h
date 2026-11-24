#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "Panels/AssetEditor.h"
#include "NoxCore/Animation/Timeline.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Entity.h"

namespace NoxUI
{
        class UIStackBox;
    }

namespace Nox
{
    class Scene;

    // A timeline (.ntimeline, the sequencer) in its own window (T1). Toolbar: Save, Undo / Redo, Play / Pause, back to the start,
    // the time, Duration, Loop, Preview, Auto-Key, Curves, zoom, the player. Tracks (a label column -- mute box, name, Key, X -- and
    // a lane each with its keys; Audio sections as bars): click the ruler to scrub, click a key to select it, drag to move it
    // (snapped to 0.01 s; Ctrl: free), Delete removes it; + Track adds a property of the player or an entity under it. Curves: the
    // selected track's values; drag a key's dot up or down (Shift: also its time). The selected key's details below.
    //
    // While editing, the preview poses the targets at the playhead; what they had comes back when it ends (Preview off, the window
    // closes, the scene is saved or Play starts) and the scene's undo history waits while it runs (the poses are not edits). Edits
    // are unsaved until Save (they were written on every change); its own history: Ctrl+Z / Ctrl+Y on the window.
    class TimelineEditor final : public AssetEditor
    {
    public:
        struct Hooks
        {
            std::function<Ref<Scene>()> ActiveScene;
            std::function<bool()> Editing;     // false in Play / Simulate (they play the timeline themselves)
            std::function<Entity()> Selected;  // the primary selected entity (it picks the player when it has this timeline)
        };

        TimelineEditor(AssetHandle timeline, Hooks hooks);
        ~TimelineEditor() override;

        AssetHandle GetHandle() const override { return m_Handle; }
        const std::string& GetName() const override { return m_Name; }
        const char* GetKind() const override { return "timeline"; }
        std::string GetWindowTitle() const override { return "Timeline: " + m_Name; }
        NoxUI::UIVec2 GetDefaultSize() const override { return { 1000.0f, 520.0f }; }

        Ref<NoxUI::UIElement> BuildUI() override;
        void Update() override;
        bool IsDirty() const override { return m_Dirty; }
        void Save() override;
        bool RevertToDisk() override;
        bool IsPreviewing() const override { return m_Snapshot.has_value(); }
        void EndPreview() override;

        // What the tracks view and the curves read and change.
        Timeline* GetTimeline() const;
        Entity GetPlayer() const { return m_PlayerEntity; }
        float Time = 0.0f;
        bool Playing = false;
        int SelectedTrack = -1;
        int SelectedKey = -1;
        float PixelsPerSecond = 120.0f;
        // A finished change (a drag let go, a value set): one history step, unsaved, posed again.
        void Edited();
        // A change still going on (a drag): shown at once, the step taken when it ends.
        void Changing() { m_LastPreviewTime = -1.0f; m_ChangePending = true; }
        void AddKeyAtPlayhead(int trackIndex);
        void RemoveTrack(int trackIndex);
        bool HandleKey(const NoxUI::UIKeyEvent& e);

    private:
        Entity FindPlayer(Scene& scene, Entity selected);
        void BeginPreview(Scene& scene, Entity player, const Timeline& timeline);
        void StopPreviewAudio();
        void AutoKey(Timeline& timeline, Scene& scene, Entity player);
        std::optional<glm::vec4> ReadTargetValue(Scene& scene, Entity player, const TimelineTrack& track);
        void ShowAddTrackMenu(const NoxUI::UIRect& anchor);
        void RebuildDetails();
        void CommitHistory();
        void Undo();
        void Redo();

        struct State
        {
            float Duration = 5.0f;
            bool Loop = false;
            std::vector<TimelineTrack> Tracks;
        };
        State Capture() const;
        void Restore(const State& state);

        AssetHandle m_Handle = 0;
        std::string m_Name;
        Hooks m_Hooks;
        bool m_Dirty = false;
        bool m_ChangePending = false;
        bool m_PreviewWanted = true;
        bool m_AutoKey = false;
        bool m_ShowCurves = false;
        float m_LastPreviewTime = -1.0f;
        UUID m_Player = 0;
        Entity m_PlayerEntity;
        std::string m_NewEventName = "Event";
        std::string m_CutCamera;
        std::chrono::steady_clock::time_point m_LastUpdate{};

        std::vector<State> m_UndoStack, m_RedoStack;
        State m_Committed;
        bool m_HaveCommitted = false;

        // What the preview's targets had before it began, by entity.
        struct TargetState
        {
            std::optional<TransformComponent> Transform;
            std::optional<glm::vec3> LightColor;
            std::optional<float> LightIntensity;
            std::optional<float> CameraFOV; // radians
            std::optional<std::map<std::string, float>> SequencerMorphWeights;
            std::optional<std::vector<MaterialComponent::Parameter>> SequencerParameters;
        };
        struct Snapshot
        {
            Ref<Scene> SceneRef;
            std::unordered_map<UUID, TargetState> Targets;
        };
        std::optional<Snapshot> m_Snapshot;
        std::unordered_map<uint64_t, TimelineAudioVoice> m_PreviewAudio;

        Ref<NoxUI::UIElement> m_Root;
        Ref<NoxUI::UIStackBox> m_Details;
        Ref<NoxUI::UIElement> m_Curves;
        int m_DetailsTrack = -2, m_DetailsKey = -2;
        size_t m_DetailsKeyCount = 0;
    };
}
