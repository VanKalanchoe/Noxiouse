#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "NoxCore/UI/Widgets/UIButton.h"

namespace Nox::UI
{
    // Clickable shortcut capture button (e.g. "Ctrl+S") that records the next key + modifier press.
    class UIKeybindButton : public UIButton
    {
    public:
        explicit UIKeybindButton(
            int initialKeyCode = 0,
            uint16_t initialMods = 0,
            std::function<void(int keyCode, uint16_t mods)> onKeybindChanged = {});

        void SetKeybind(int keyCode, uint16_t mods, bool notify = false);
        int GetKeyCode() const { return m_KeyCode; }
        uint16_t GetModifiers() const { return m_Mods; }

        std::function<void(int keyCode, uint16_t mods)> OnKeybindChanged;

        bool OnKeyDown(const UIKeyEvent& e) override;

    private:
        void RefreshLabel();
        static std::string FormatKeybind(int keyCode, uint16_t mods);

        int m_KeyCode = 0;
        uint16_t m_Mods = 0;
        bool m_Listening = false;
    };
}
