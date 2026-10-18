#pragma once
#include <string>

#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    // An animation clip's window (Future.md item 3, the start of plan A7): double-click a clip in the Content Browser. Shows its
    // length and channels and edits its notifies -- named moments ("FootL" at 0.25 s) that clips, blend spaces and montages raise
    // and the entity's scripts receive as OnAnimationEvent(name). Edits apply to the loaded clip at once and are saved beside
    // it (<clip>.nanim.notifies), so a re-cook of the clip keeps them. No 3D preview yet (needs a second render view).
    class AnimationClipPanel
    {
    public:
        explicit AnimationClipPanel(AssetHandle clip);

        void OnImGuiRender();
        bool IsOpen() const { return m_Open; }
        AssetHandle GetClip() const { return m_Clip; }
        const std::string& GetTitle() const { return m_Title; }

    private:
        void Save();

        AssetHandle m_Clip = 0;
        std::string m_Title;
        bool m_Open = true;
        float m_Cursor = 0.0f; // the timeline's playhead, where Add puts a notify
        char m_NewName[64] = "Notify";
    };
}
