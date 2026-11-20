#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Nox::UI
{
    // A key with its modifiers (by scancode: the key's place on the keyboard).
    struct UIKeyChord
    {
        int Scancode = 0; // 0: none
        bool Ctrl = false;
        bool Shift = false;
        bool Alt = false;

        bool IsSet() const { return Scancode != 0; }
        bool operator==(const UIKeyChord& other) const = default;
        // "Ctrl+Shift+S", "Delete", "Num 7"; empty when not set.
        std::string ToString() const;
        // The text ToString() makes, read back ("" or unknown: not set).
        static UIKeyChord Parse(const std::string& text);
        // A key a text field would type (no Ctrl or Alt, not an F key): a shortcut on it must not run while typing.
        bool TypesText() const;
    };

    struct UICommand
    {
        std::string Id;       // "File.SaveScene" (key bindings are saved by it)
        std::string Label;    // "Save Scene"
        std::string Category; // "File"
        std::string Description;
        uint32_t Scope = 0;   // where its key works: the application's own meaning (FindForKey asks about it)
        UIKeyChord DefaultKey;
        UIKeyChord DefaultAltKey; // a second key (Ctrl+Y and Ctrl+Shift+Z both redo)
        UIKeyChord Key;           // the bound keys (the defaults unless rebound)
        UIKeyChord AltKey;
        std::function<void()> Execute;
        std::function<bool()> CanExecute; // empty: always
    };

    // Commands, as Unreal's FUICommandList: menus run them and show their keys, a key press runs the one bound to it. Keys can be
    // rebound; the ones that differ from the defaults are saved to a text file ("Id: key | second key" lines).
    class UICommandList
    {
    public:
        // Adds (or replaces) a command, its keys set to the defaults; returns it.
        UICommand& Add(UICommand command);
        const UICommand* Find(const std::string& id) const;
        const std::vector<UICommand>& GetCommands() const { return m_Commands; }

        // Runs it when it can run; false otherwise.
        bool Execute(const std::string& id) const;
        bool CanExecute(const std::string& id) const;
        // Its keys for a menu ("Ctrl+Y / Ctrl+Shift+Z"; "" when unbound).
        std::string ShortcutText(const std::string& id) const;

        // The first command (in the order added) bound to `chord` that `allowed` lets run here (its scope; empty: all) and that can
        // run now. One key can serve several commands: Esc ends a tool, else stops Play. Null: none.
        const UICommand* FindForKey(const UIKeyChord& chord, const std::function<bool(const UICommand&)>& allowed = {}) const;

        // Rebinding: `primary` false sets the second key. The chord is taken from any other command that had it (its id is returned
        // so the caller can say so; "" when nothing was taken).
        std::string Bind(const std::string& id, const UIKeyChord& chord, bool primary = true);
        void ResetToDefault(const std::string& id);
        void ResetAll();

        // Keys that differ from the defaults; a missing file: the defaults. `header` is the file's first line (a "#" comment).
        bool Load(const std::filesystem::path& path);
        bool Save(const std::filesystem::path& path, const std::string& header = "# Key bindings that differ from the defaults. Id: key | second key") const;

    protected:
        UICommand* FindMutable(const std::string& id);

    private:
        std::vector<UICommand> m_Commands;
    };
}
