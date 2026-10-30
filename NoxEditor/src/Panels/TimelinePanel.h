#pragma once
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/Animation/Timeline.h"
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/UUID.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Scene.h"

namespace Nox
{
    // A sequencer timeline's window (docs/Editor_Features_Plan_2026-10.md A8): double-click a .ntimeline in the Content Browser, or
    // Open on a Timeline Player. A ruler with the playhead (click / drag to scrub), Play / Pause, the duration and loop; a lane per
    // track with its keys (click to select, drag to move, Del to delete); "Key" on a track keys its target's current value at the
    // playhead (move the entity with the gizmo first); the selected key's time, value, interpolation or event name. Auto-key keys
    // what is moved while previewing; Curves shows the selected track's values over time (drag keys to change them); a Camera Cut
    // track switches the primary camera in Play.
    //
    // It previews on an entity whose Timeline Player plays this timeline: its targets take the timeline's pose at the playhead.
    // Previewing does not edit the scene -- what the targets had is put back when the preview ends (the Preview checkbox, or the
    // window closing), and the editor's undo history waits meanwhile.
    class TimelinePanel
    {
    public:
        explicit TimelinePanel(AssetHandle timeline);

        void OnImGuiRender(const Ref<Scene>& scene, Entity selected, bool editing);
        bool IsOpen() const { return m_Open; }
        AssetHandle GetTimeline() const { return m_Timeline; }
        const std::string& GetTitle() const { return m_Title; }
        // The targets are posed by the preview (EditorLayer holds its undo history until it ends).
        bool IsPreviewing() const { return m_Snapshot.has_value(); }
        // Puts back what the targets had before the preview (closing, leaving Edit mode, another scene).
        void EndPreview();

    private:
        void Save(Timeline& timeline);
        Entity FindPlayer(Scene& scene, Entity selected);
        void BeginPreview(Scene& scene, Entity player, const Timeline& timeline);
        void DrawToolbar(Timeline& timeline, Scene* scene, Entity player);
        void DrawTracks(Timeline& timeline, Scene* scene, Entity player);
        void DrawSelectedKey(Timeline& timeline);
        void DrawSubTimelineTarget(Timeline& timeline, TimelineTrack& track);
        void DrawAddTrack(Timeline& timeline, Scene& scene, Entity player);
        // The selected track's values as curves (a line per component) under the lanes; keys dragged up / down change their value.
        void DrawCurves(Timeline& timeline);
        // Auto-key: a previewed target moved away from the timeline's value at the playhead gets a key there.
        void AutoKey(Timeline& timeline, Scene& scene, Entity player);
        // The value the track's property has on its target now (what "Key" records).
        std::optional<glm::vec4> ReadTargetValue(Scene& scene, Entity player, const TimelineTrack& track);

        AssetHandle m_Timeline = 0;
        std::string m_Title;
        bool m_Open = true;
        float m_Time = 0.0f;
        bool m_Playing = false;
        bool m_PreviewWanted = true;
        float m_LastPreviewTime = -1.0f;
        UUID m_Player = 0;
        int m_SelectedTrack = -1;
        int m_SelectedKey = -1;
        bool m_DraggingKey = false;
        float m_PixelsPerSecond = 120.0f;
        bool m_AutoKey = false;
        bool m_ShowCurves = false;
        int m_DraggingCurveKey = -1; // the key dragged in the curve view
        int m_DraggingCurveComponent = 0;
        // The camera a new Camera Cut key cuts to (a name path from the player).
        std::string m_CutCamera;

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

        char m_NewEventName[64] = "Event";
    };
}
