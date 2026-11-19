#include "EditorCommands.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

namespace Nox
{
    namespace
    {
        // Shorter names than SDL's for a few keys (menus are narrow); read back by Parse too.
        struct KeyName { SDL_Scancode Code; const char* Name; };
        constexpr KeyName kKeyNames[] = {
            { SDL_SCANCODE_KP_0, "Num 0" }, { SDL_SCANCODE_KP_1, "Num 1" }, { SDL_SCANCODE_KP_2, "Num 2" }, { SDL_SCANCODE_KP_3, "Num 3" },
            { SDL_SCANCODE_KP_4, "Num 4" }, { SDL_SCANCODE_KP_5, "Num 5" }, { SDL_SCANCODE_KP_6, "Num 6" }, { SDL_SCANCODE_KP_7, "Num 7" },
            { SDL_SCANCODE_KP_8, "Num 8" }, { SDL_SCANCODE_KP_9, "Num 9" }, { SDL_SCANCODE_GRAVE, "`" }, { SDL_SCANCODE_ESCAPE, "Esc" },
        };

        std::string Trim(std::string text)
        {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
            text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
            return text;
        }
    }

    std::string KeyChord::ToString() const
    {
        if (!IsSet())
            return {};
        std::string text;
        if (Ctrl) text += "Ctrl+";
        if (Shift) text += "Shift+";
        if (Alt) text += "Alt+";
        for (const KeyName& name : kKeyNames)
            if (name.Code == Scancode)
                return text + name.Name;
        const char* sdlName = SDL_GetScancodeName(static_cast<SDL_Scancode>(Scancode));
        return text + (sdlName && *sdlName ? sdlName : ("Key " + std::to_string(Scancode)).c_str());
    }

    KeyChord KeyChord::Parse(const std::string& source)
    {
        KeyChord chord;
        std::string text = Trim(source);
        if (text.empty())
            return chord;
        // Modifiers first ("+" alone is the key "+" only at the end).
        for (;;)
        {
            if (text.rfind("Ctrl+", 0) == 0) { chord.Ctrl = true; text.erase(0, 5); }
            else if (text.rfind("Shift+", 0) == 0) { chord.Shift = true; text.erase(0, 6); }
            else if (text.rfind("Alt+", 0) == 0) { chord.Alt = true; text.erase(0, 4); }
            else break;
        }
        for (const KeyName& name : kKeyNames)
            if (text == name.Name)
                chord.Scancode = name.Code;
        if (!chord.Scancode)
            chord.Scancode = static_cast<int>(SDL_GetScancodeFromName(text.c_str()));
        if (chord.Scancode == SDL_SCANCODE_UNKNOWN)
            return {};
        return chord;
    }

    EditorCommand& EditorCommandList::Add(EditorCommand command)
    {
        command.Key = command.DefaultKey;
        command.AltKey = command.DefaultAltKey;
        if (EditorCommand* existing = FindMutable(command.Id))
        {
            *existing = std::move(command);
            return *existing;
        }
        m_Commands.push_back(std::move(command));
        return m_Commands.back();
    }

    const EditorCommand* EditorCommandList::Find(const std::string& id) const
    {
        for (const EditorCommand& command : m_Commands)
            if (command.Id == id)
                return &command;
        return nullptr;
    }

    EditorCommand* EditorCommandList::FindMutable(const std::string& id)
    {
        return const_cast<EditorCommand*>(Find(id));
    }

    bool EditorCommandList::CanExecute(const std::string& id) const
    {
        const EditorCommand* command = Find(id);
        return command && command->Execute && (!command->CanExecute || command->CanExecute());
    }

    bool EditorCommandList::Execute(const std::string& id) const
    {
        if (!CanExecute(id))
            return false;
        const EditorCommand* command = Find(id);
        const std::function<void()> run = command->Execute; // (the command may rebuild the list it is in)
        run();
        return true;
    }

    std::string EditorCommandList::ShortcutText(const std::string& id) const
    {
        const EditorCommand* command = Find(id);
        if (!command)
            return {};
        std::string text = command->Key.ToString();
        if (command->AltKey.IsSet())
            text += (text.empty() ? "" : " / ") + command->AltKey.ToString();
        return text;
    }

    const EditorCommand* EditorCommandList::FindForKey(const KeyChord& chord, const KeyContext& context) const
    {
        if (!chord.IsSet())
            return nullptr;
        for (const EditorCommand& command : m_Commands)
        {
            if (!(command.Key == chord) && !(command.AltKey == chord))
                continue;
            switch (command.Scope)
            {
            case CommandScope::Global:
                // While typing, only keys a text field does not type (with Ctrl or Alt, or the F keys).
                if (context.TextInputFocused && !chord.Ctrl && !chord.Alt &&
                    !(chord.Scancode >= SDL_SCANCODE_F1 && chord.Scancode <= SDL_SCANCODE_F12))
                    continue;
                break;
            case CommandScope::Level:
                if (!context.LevelFocus || context.TextInputFocused)
                    continue;
                break;
            case CommandScope::Viewport:
                if (!context.LevelFocus || context.TextInputFocused || !context.ViewportHovered)
                    continue;
                break;
            }
            // (one key can serve several commands -- Esc ends the measure tool, else stops Play: the first that can run now)
            if (command.CanExecute && !command.CanExecute())
                continue;
            return &command;
        }
        return nullptr;
    }

    std::string EditorCommandList::Bind(const std::string& id, const KeyChord& chord, bool primary)
    {
        EditorCommand* command = FindMutable(id);
        if (!command)
            return {};
        std::string taken;
        if (chord.IsSet())
        {
            for (EditorCommand& other : m_Commands)
            {
                // (every scope overlaps another: Global covers all, a viewport key needs level focus too)
                if (&other == command)
                    continue;
                if (other.Key == chord) { other.Key = {}; taken = other.Id; }
                if (other.AltKey == chord) { other.AltKey = {}; taken = other.Id; }
            }
        }
        (primary ? command->Key : command->AltKey) = chord;
        return taken;
    }

    void EditorCommandList::ResetToDefault(const std::string& id)
    {
        EditorCommand* command = FindMutable(id);
        if (!command)
            return;
        // The defaults may be taken by another command by now: taken back.
        Bind(id, command->DefaultKey, true);
        Bind(id, command->DefaultAltKey, false);
    }

    void EditorCommandList::ResetAll()
    {
        for (EditorCommand& command : m_Commands)
        {
            command.Key = command.DefaultKey;
            command.AltKey = command.DefaultAltKey;
        }
    }

    bool EditorCommandList::Load(const std::filesystem::path& path)
    {
        ResetAll();
        std::ifstream in(path);
        if (!in)
            return false;
        std::string line;
        while (std::getline(in, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#')
                continue;
            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            EditorCommand* command = FindMutable(Trim(line.substr(0, colon)));
            if (!command)
                continue; // (a command this editor no longer has)
            const std::string keys = line.substr(colon + 1);
            const size_t bar = keys.find('|');
            command->Key = KeyChord::Parse(keys.substr(0, bar));
            command->AltKey = bar == std::string::npos ? KeyChord{} : KeyChord::Parse(keys.substr(bar + 1));
        }
        return true;
    }

    bool EditorCommandList::Save(const std::filesystem::path& path) const
    {
        std::ostringstream out;
        out << "# NoxEditorV2 key bindings that differ from the defaults (Edit > Keyboard Shortcuts...). Id: key | second key\n";
        bool any = false;
        for (const EditorCommand& command : m_Commands)
        {
            if (command.Key == command.DefaultKey && command.AltKey == command.DefaultAltKey)
                continue;
            out << command.Id << ": " << command.Key.ToString() << " | " << command.AltKey.ToString() << "\n";
            any = true;
        }
        std::error_code error;
        if (!any)
        {
            std::filesystem::remove(path, error);
            return true;
        }
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream file(path, std::ios::trunc);
        if (!file)
            return false;
        file << out.str();
        return static_cast<bool>(file);
    }
}
