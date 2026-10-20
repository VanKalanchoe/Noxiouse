#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "NoxCore/Core/Ref.h"
#include "NoxCore/Core/UUID.h"

namespace Nox
{
    class Scene;

    // Editor undo / redo (docs/Future.md item 12), Ctrl+Z / Ctrl+Y.
    //
    // Every step is the whole scene as its scene-file text (SceneSerializer::SerializeToText), so undoing is loading that text into a
    // new scene: the same thing as saving and reopening the scene, whatever was changed -- a field in the Properties panel, a gizmo
    // drag, a rename, a reparent, a folder, a delete, a duplicate, a placed asset or shape, a prefab override -- with no per-command
    // code to forget. The history is a list of states with a cursor: undo moves back, redo forward, a new change drops the states after
    // the cursor.
    //
    // When to take a step: checked when an interaction ends (an ImGui item or the gizmo is released, an editor shortcut ran). A cheap
    // fingerprint of the scene (entity ids, names, parents, folders, transforms) catches structural changes and transforms on any
    // entity; the text of the entity the Properties panel shows catches its component edits. Only when one of the two differs is the
    // whole scene serialized and compared with the current state.
    //
    // What happens on its own after a step (a placed model spawning and flattening once its asset loads, a level import) is folded
    // into that step ("settling"), so undoing a placement removes everything it produced and redoing it does not wait for a respawn.
    class EditorHistory
    {
    public:
        struct FrameInfo
        {
            bool InteractionActive = false; // an ImGui item is held or the gizmo is dragged: nothing is taken until it ends
            bool InteractionEnded = false; // an ImGui item / the gizmo was released or an editor shortcut ran this frame
            bool GizmoEnded = false;       // the gizmo was released this frame (names the step)
            bool AssetsLoading = false;    // background loads still running: what they spawn may still change the scene
            UUID Watched = 0;              // the entity the Properties panel shows (0 = none)
        };

        // Once per frame in Edit mode, after the UI has applied its changes. A different scene than the bound one (another scene
        // opened, Prefab Mode entered) starts a new history with the scene as it is as its first state.
        void Update(const Ref<Scene>& scene, const FrameInfo& frame);
        void Clear();

        bool CanUndo() const { return m_Index > 0; }
        bool CanRedo() const { return m_Index + 1 < m_States.size(); }
        // Changes with every commit, undo and redo: something in the scene changed (the extra viewports' "realtime off" redraws).
        size_t GetPosition() const { return m_Index; }
        // The name of the step an undo / redo would revert / repeat ("Delete Entity", "Edit Cube", ...).
        const std::string& GetUndoName() const;
        const std::string& GetRedoName() const;

        // Moves the cursor and returns the scene text to load (empty when there is nothing to undo / redo). The caller loads it into a
        // new scene, makes that the edited scene and hands it to OnSceneRestored.
        std::string StepBack();
        std::string StepForward();
        void OnSceneRestored(const Ref<Scene>& scene);

        size_t GetStateCount() const { return m_States.size(); }
        size_t GetMemoryBytes() const { return m_TotalBytes; }

    private:
        struct State
        {
            std::string Text;
            std::string Name; // of the change that led to this state (empty for the first one)
        };

        void Bind(const Ref<Scene>& scene);
        void Commit(std::string text, std::string name);
        void Amend(std::string text);
        void Trim();
        void Watch(UUID entity);
        std::string MakeName(const FrameInfo& frame, bool watchedChanged, size_t entityCount) const;

        static uint64_t Fingerprint(Scene& scene, size_t& entityCount);
        static std::string SceneText(const Ref<Scene>& scene);
        static std::string EntityText(Scene& scene, UUID entity);

    private:
        Ref<Scene> m_Scene;
        std::vector<State> m_States;
        size_t m_Index = 0;
        size_t m_TotalBytes = 0;

        // The current state, in the cheap forms the per-frame check compares.
        uint64_t m_Fingerprint = 0;
        size_t m_EntityCount = 0;
        UUID m_Watched = 0;
        std::string m_WatchedText;

        bool m_Settling = false;
        uint32_t m_StableFrames = 0;
    };
}
