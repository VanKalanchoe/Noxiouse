#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Nox
{
    // A key with its modifiers (by scancode: the key's place on the keyboard; letters follow the keyboard layout when matched).
    struct KeyChord
    {
        int Scancode = 0; // 0: none
        bool Ctrl = false;
        bool Shift = false;
        bool Alt = false;

        bool IsSet() const { return Scancode != 0; }
        bool operator==(const KeyChord& other) const = default;
        // "Ctrl+Shift+S", "Delete", "Num 7"; empty when not set.
        std::string ToString() const;
        // The text ToString() makes, read back ("" or unknown: not set).
        static KeyChord Parse(const std::string& text);
    };

    // Where a command's key works (Unreal's command lists bound per editor):
    //  - Global: anywhere in the editor's main window, also while a text field is being typed in (with Ctrl or Alt only).
    //  - Level: the level editor has the keyboard -- nothing focused, or the viewport, Scene Hierarchy, Inspector or toolbar. Keys
    //    typed in the Content Browser, the Output Log or an asset editor never reach these (Delete there is not "delete entities").
    //  - Viewport: like Level, and the pointer is over a viewport (the view keys).
    enum class CommandScope : uint8_t { Global, Level, Viewport };

    struct EditorCommand
    {
        std::string Id;       // "File.SaveScene" (key bindings are saved by it)
        std::string Label;    // "Save Scene"
        std::string Category; // "File"
        std::string Description;
        CommandScope Scope = CommandScope::Level;
        KeyChord DefaultKey;
        KeyChord DefaultAltKey; // a second key (Ctrl+Y and Ctrl+Shift+Z both redo)
        KeyChord Key;           // the bound keys (the defaults unless rebound)
        KeyChord AltKey;
        std::function<void()> Execute;
        std::function<bool()> CanExecute; // empty: always
    };

    // The editor's commands: the menus run them and show their keys, a key press runs the one bound to it (Unreal's
    // FUICommandList). Keys can be rebound (Edit > Keyboard Shortcuts...) and are saved per project, per user (Saved/Config).
    class EditorCommandList
    {
    public:
        // Adds (or replaces) a command; returns it.
        EditorCommand& Add(EditorCommand command);
        const EditorCommand* Find(const std::string& id) const;
        const std::vector<EditorCommand>& GetCommands() const { return m_Commands; }

        // Runs it when it can run; false otherwise.
        bool Execute(const std::string& id) const;
        bool CanExecute(const std::string& id) const;
        // Its keys for a menu ("Ctrl+Y / Ctrl+Shift+Z"; "" when unbound).
        std::string ShortcutText(const std::string& id) const;

        // What the editor knows when a key is pressed.
        struct KeyContext
        {
            bool LevelFocus = false;     // the level editor has the keyboard
            bool ViewportHovered = false;
            bool TextInputFocused = false;
        };
        // The command bound to `chord` that may run here and can run now (in the order added); null: none.
        const EditorCommand* FindForKey(const KeyChord& chord, const KeyContext& context) const;

        // Rebinding: `primary` false sets the second key. A chord another command of an overlapping scope uses is taken from it
        // (its id is returned so the caller can say so; "" when nothing was taken).
        std::string Bind(const std::string& id, const KeyChord& chord, bool primary = true);
        void ResetToDefault(const std::string& id);
        void ResetAll();

        // Keys that differ from the defaults, as "Id: key | key" lines; missing file: the defaults.
        bool Load(const std::filesystem::path& path);
        bool Save(const std::filesystem::path& path) const;

    private:
        EditorCommand* FindMutable(const std::string& id);
        std::vector<EditorCommand> m_Commands;
    };
}
