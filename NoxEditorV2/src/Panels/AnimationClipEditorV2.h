#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Panels/AssetEditorV2.h"

namespace Nox
{
    namespace UI
    {
        class UIStackBox;
    }
    struct AnimationNotify;

    // An animation clip (AnimationSequence) in its own window (T1): the clip on a skeletal mesh (play, pause, loop, speed, the preview
    // mesh), a timeline (drag the playhead; notifies as markers -- orange events, green sync markers; the loop range shaded), the loop
    // range, and the notifies (time, name, sync marker, Play Sound settings, go to, delete; add at the playhead). Notify edits are
    // unsaved until Save (they were written to the file on every change); closing asks Save / Don't Save / Cancel.
    class AnimationClipEditorV2 final : public AssetEditorV2
    {
    public:
        explicit AnimationClipEditorV2(AssetHandle clip);

        AssetHandle GetHandle() const override { return m_Clip; }
        const std::string& GetName() const override { return m_Name; }
        const char* GetKind() const override { return "animation clip"; }
        std::string GetWindowTitle() const override { return "Animation: " + m_Name; }
        UI::UIVec2 GetDefaultSize() const override { return { 620.0f, 720.0f }; }

        Ref<UI::UIElement> BuildUI() override;
        void Update() override;
        bool IsDirty() const override { return m_Dirty; }
        void Save() override;
        bool RevertToDisk() override;

        // (The timeline element reads and moves these.)
        float Cursor = 0.0f;
        float RangeStart = 0.0f, RangeEnd = 0.0f; // end 0: the clip's end
        bool Playing = true;

    private:
        void FindPreviewMesh();
        void SetPreviewMesh(AssetHandle mesh);
        void RebuildNotifies();
        void ShowSoundSettings(size_t index, const UI::UIRect& anchor);
        void Edited();

        AssetHandle m_Clip = 0;
        std::string m_Name;
        bool m_Dirty = false;
        bool m_Loop = true;
        float m_Speed = 1.0f;
        AssetHandle m_PreviewMesh = 0;
        AssetHandle m_PreviewSkeleton = 0;
        bool m_PreviewSearched = false;
        std::string m_LastNotify;
        float m_LastNotifyAge = 10.0f;
        std::string m_NewName = "Notify";
        std::chrono::steady_clock::time_point m_LastUpdate{};
        size_t m_NotifyCount = ~size_t(0);

        Ref<UI::UIElement> m_Root;
        Ref<UI::UIStackBox> m_Notifies;
        std::vector<std::function<void()>> m_Syncs;
        // An open popup's rows (run while it is open).
        std::shared_ptr<std::vector<std::function<void()>>> m_PopupSyncs;
        std::shared_ptr<const bool> m_PopupOpen;
        std::vector<std::function<void()>> m_NotifySyncs;
    };
}
