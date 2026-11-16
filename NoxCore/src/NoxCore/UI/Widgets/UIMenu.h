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
        bool Enabled = true;
        bool Checked = false;
        bool IsInfo = false;
        std::string Tooltip;
        std::vector<UIMenuItem> SubItems;
    };

    class UIMenuPopup : public UIStackBox
    {
    public:
        explicit UIMenuPopup(std::vector<UIMenuItem> items, UIContext* context = nullptr, UIElement* host = nullptr, int depth = 0);

        static void ShowContextMenu(UIContext& context, const UIVec2& screenPos, std::vector<UIMenuItem> items);
        static void ShowPopupMenu(UIContext& context, const UIRect& anchorRect, std::vector<UIMenuItem> items, bool alignRight = false);

        void SetMaxContentHeight(float maxHeight);

    private:
        Ref<class UIScrollView> m_ScrollView;
    };

    class UIMenuBar : public UIStackBox
    {
    public:
        UIMenuBar();

        void AddMenu(const std::string& title, std::vector<UIMenuItem> items);
        void AddDynamicMenu(const std::string& title, std::function<std::vector<UIMenuItem>()> builder);

    private:
        Ref<UIElement> m_ActiveOverlay;
        UIElement* m_OpenTrigger = nullptr;
    };
}
