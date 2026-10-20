#pragma once
#include <string>

#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    // An animation clip's window (Future.md item 3, plan A7): double-click a clip in the Content Browser. A 3D preview of a
    // skeletal mesh playing it (AssetPreviewRenderer; the mesh cooked from the same model by default, any skeletal mesh can be
    // picked), play / pause, loop, speed and a scrubbable playhead; its length and channels; and its notifies -- named moments
    // ("FootL" at 0.25 s) that clips, blend spaces and montages raise and the entity's scripts receive as OnAnimationEvent(name),
    // shown here as they pass while it plays. Edits apply to the loaded clip at once and are saved beside it
    // (<clip>.nanim.notifies), so a re-cook of the clip keeps them.
    class AnimationClipPanel
    {
    public:
        // previewMesh: the skeletal mesh to show it on (0: the one cooked from the clip's model).
        explicit AnimationClipPanel(AssetHandle clip, AssetHandle previewMesh = 0);

        void OnImGuiRender();
        bool IsOpen() const { return m_Open; }
        AssetHandle GetClip() const { return m_Clip; }
        const std::string& GetTitle() const { return m_Title; }

    private:
        void Save();
        // The skeletal mesh cooked from the clip's model (else one in its folder) and that mesh's skeleton.
        void FindPreviewModel();
        void SetPreviewMesh(AssetHandle mesh);
        void DrawPreview(float duration);

        AssetHandle m_Clip = 0;
        std::string m_Title;
        bool m_Open = true;
        float m_Cursor = 0.0f; // the timeline's playhead, where Add puts a notify
        char m_NewName[64] = "Notify";

        AssetHandle m_PreviewMesh = 0;
        AssetHandle m_PreviewSkeleton = 0;
        bool m_PreviewSearched = false;
        bool m_Playing = true;
        bool m_Loop = true;
        float m_Speed = 1.0f;
        std::string m_LastNotify; // the last notify the playing preview passed, shown for a moment
        float m_LastNotifyAge = 10.0f;
    };
}
