#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    class UIContext;

    struct UIMenuItem
    {
        std::string Label;
        std::string Shortcut;
        std::function<void()> OnTrigger;
        bool IsSeparator = false;
    };

    class UIMenuPopup : public UIStackBox
    {
    public:
        explicit UIMenuPopup(std::vector<UIMenuItem> items, UIContext* context = nullptr);

        static void ShowContextMenu(UIContext& context, const UIVec2& screenPos, std::vector<UIMenuItem> items);
    };

    class UIMenuBar : public UIStackBox
    {
    public:
        UIMenuBar();

        void AddMenu(const std::string& title, std::vector<UIMenuItem> items);

    private:
        Ref<UIElement> m_ActiveOverlay;
        UIElement* m_OpenTrigger = nullptr;
    };
}
