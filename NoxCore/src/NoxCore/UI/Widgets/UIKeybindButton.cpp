#include "UIKeybindButton.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

namespace Nox::UI
{
    UIKeybindButton::UIKeybindButton(
        int initialKeyCode,
        uint16_t initialMods,
        std::function<void(int, uint16_t)> onKeybindChanged)
        : UIButton("")
        , OnKeybindChanged(std::move(onKeybindChanged))
        , m_KeyCode(initialKeyCode)
        , m_Mods(initialMods)
    {
        OnClick = [this]()
        {
            m_Listening = true;
            StyleVariant = "primary";
            RefreshLabel();
        };
        RefreshLabel();
    }

    void UIKeybindButton::SetKeybind(int keyCode, uint16_t mods, bool notify)
    {
        m_KeyCode = keyCode;
        m_Mods = mods;
        m_Listening = false;
        StyleVariant.clear();
        RefreshLabel();
        if (notify && OnKeybindChanged)
            OnKeybindChanged(m_KeyCode, m_Mods);
    }

    bool UIKeybindButton::OnKeyDown(const UIKeyEvent& e)
    {
        if (!m_Listening)
            return UIButton::OnKeyDown(e);

        if (e.KeyCode == SDL_SCANCODE_ESCAPE)
        {
            m_Listening = false;
            StyleVariant.clear();
            RefreshLabel();
            return true;
        }

        // Ignore pure modifier keys until a primary key is pressed
        if (e.KeyCode == SDL_SCANCODE_LCTRL || e.KeyCode == SDL_SCANCODE_RCTRL ||
            e.KeyCode == SDL_SCANCODE_LSHIFT || e.KeyCode == SDL_SCANCODE_RSHIFT ||
            e.KeyCode == SDL_SCANCODE_LALT || e.KeyCode == SDL_SCANCODE_RALT ||
            e.KeyCode == SDL_SCANCODE_LGUI || e.KeyCode == SDL_SCANCODE_RGUI)
        {
            return true;
        }

        SetKeybind(e.KeyCode, e.Modifiers, true);
        return true;
    }

    void UIKeybindButton::RefreshLabel()
    {
        if (m_Listening)
            SetText("Press a key... (Esc cancels)");
        else
            SetText(FormatKeybind(m_KeyCode, m_Mods));
    }

    std::string UIKeybindButton::FormatKeybind(int keyCode, uint16_t mods)
    {
        if (keyCode == 0)
            return "<Unbound>";

        std::string out;
        if (mods & SDL_KMOD_CTRL) out += "Ctrl+";
        if (mods & SDL_KMOD_SHIFT) out += "Shift+";
        if (mods & SDL_KMOD_ALT) out += "Alt+";

        const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(keyCode));
        out += (name && name[0] != '\0') ? name : "Key";
        return out;
    }
}
