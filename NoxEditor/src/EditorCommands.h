#pragma once

#include <cstdint>

#include <NoxUI/Core/UICommandList.h>

namespace Nox
{
    using KeyChord = NoxUI::UIKeyChord;
    using EditorCommand = NoxUI::UICommand;

    // Where a command's key works (UICommand::Scope; Unreal's command lists bound per editor):
    //  - Global: anywhere in the editor's main window, also while a text field is being typed in (with Ctrl or Alt only).
    //  - Level: the level editor has the keyboard -- nothing focused, or the viewport, Scene Hierarchy, Inspector or toolbar. Keys
    //    typed in the Content Browser, the Output Log or an asset editor never reach these (Delete there is not "delete entities").
    //  - Viewport: like Level, and the pointer is over a viewport (the view keys).
    enum class CommandScope : uint8_t { Global, Level, Viewport };

    // The editor's commands (UI::UICommandList: menus run them and show their keys, keys can be rebound -- Edit > Keyboard
    // Shortcuts... -- and are saved per project, per user in Saved/Config), with the editor's scopes for key presses.
    class EditorCommandList : public NoxUI::UICommandList
    {
    public:
        // What the editor knows when a key is pressed.
        struct KeyContext
        {
            bool LevelFocus = false;     // the level editor has the keyboard
            bool ViewportHovered = false;
            bool TextInputFocused = false;
        };
        // The command bound to `chord` that may run here and can run now (in the order added); null: none.
        const EditorCommand* FindForKey(const KeyChord& chord, const KeyContext& context) const;
        bool Save(const std::filesystem::path& path) const;
        using NoxUI::UICommandList::Load;
    };
}
