#include "EditorCommands.h"

namespace Nox
{
    const EditorCommand* EditorCommandList::FindForKey(const KeyChord& chord, const KeyContext& context) const
    {
        return UI::UICommandList::FindForKey(chord, [&context, &chord](const EditorCommand& command)
        {
            switch (static_cast<CommandScope>(command.Scope))
            {
            case CommandScope::Global:
                // While typing, only keys a text field does not type (with Ctrl or Alt, or the F keys).
                return !context.TextInputFocused || !chord.TypesText();
            case CommandScope::Level:
                return context.LevelFocus && !context.TextInputFocused;
            case CommandScope::Viewport:
                return context.LevelFocus && !context.TextInputFocused && context.ViewportHovered;
            }
            return false;
        });
    }

    bool EditorCommandList::Save(const std::filesystem::path& path) const
    {
        return UI::UICommandList::Save(path, "# NoxEditorV2 key bindings that differ from the defaults (Edit > Keyboard Shortcuts...). Id: key | second key");
    }
}
