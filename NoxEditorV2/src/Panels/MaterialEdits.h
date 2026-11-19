#pragma once

#include <vector>

#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    // Materials edited and not saved yet (M1): an edit in the Material window or the Inspector's material card changes the loaded
    // material at once (everything using it shades with it this frame) and marks it unsaved; Save writes its .nmat. Nothing is
    // written on every change (the old editors wrote the file on every step of a slider drag), and an edit can be dropped (Don't
    // Save reads the file back). The editor's "Save changes?" questions list these.
    namespace MaterialEdits
    {
        void MarkEdited(AssetHandle material);
        bool IsUnsaved(AssetHandle material);
        bool Any();
        std::vector<AssetHandle> Unsaved();
        // Writes the material's .nmat; false (logged) when it could not be written (it stays unsaved).
        bool Save(AssetHandle material);
        void SaveAll();
        // The material as its file has it again (read back, drawn with it this frame); false when the file could not be read.
        bool Revert(AssetHandle material);
        void RevertAll();
        // Another project: the list is dropped (its materials are not loaded any more).
        void Clear();
    }
}
