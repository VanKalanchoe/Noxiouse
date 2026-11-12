#pragma once
#include <string>

#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    // Editor widgets shared by the panels (Inspector, Material editor, Animation Clip editor, Settings).

    // A sound asset (an audio clip or an audio graph) by name, or dropped from the Content Browser. True when it changed.
    bool SoundAssetCombo(const char* label, AssetHandle& sound);

    // A physical surface from the project's list (Settings > Physical Surfaces). `emptyLabel`: what the empty name is called
    // ("Default", "From Material"); empty names are kept empty. True when it changed.
    bool PhysicalSurfaceCombo(const char* label, std::string& surface, const char* emptyLabel);

    // Plays a sound once in the editor (2D, the Editor group): an Animation Clip editor's notify while it previews. Nothing when
    // the sound is not loaded yet (it is requested).
    void PlayPreviewSound(AssetHandle sound, float volume);
}
