#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    class UIButton;
    class UIContext;

    // Dockable tabbed panel container (tab strip + active content host).
    class UITabPanel : public UIStackBox
    {
    public:
        UITabPanel();

        size_t AddTab(const std::string& title, const Ref<UIElement>& content);
        size_t InsertTab(size_t index, const std::string& title, const Ref<UIElement>& content);
        size_t ComputeTabInsertIndex(float pointerX, float& outInsertX) const;
        bool RemoveTab(size_t index);
        bool DetachTabToWindow(size_t index, UIContext& context);
        void SetActiveTab(size_t index);
        size_t GetActiveTab() const { return m_ActiveIndex; }
        size_t GetTabCount() const { return m_Tabs.size(); }

        const std::string& GetTabTitle(size_t index) const;
        // (Not for a dock space's tabs: it finds its panels by title.)
        void SetTabTitle(size_t index, const std::string& title);
        Ref<UIElement> GetTabContent(size_t index) const;

        const Ref<UIStackBox>& GetTabBar() const { return m_TabBar; }
        const Ref<UIStackBox>& GetContentHost() const { return m_ContentHost; }
        void SetTabBarVisible(bool visible);
        void SetShowUndockCornerHandle(bool show);
        bool GetShowUndockCornerHandle() const { return m_ShowUndockCornerHandle; }

        // Fired when a tab is dragged past the tear-out threshold while pressed.
        std::function<void(size_t index, UIVec2 pointerPos)> OnTabDragOut;

    protected:
        void ArrangeChildren(const UIRect& contentRect) override;

    private:
        struct TabEntry
        {
            std::string Title;
            Ref<UIButton> Button;
            Ref<UIElement> Content;
        };

        size_t FindTabIndex(const UIButton* btn) const;
        void RefreshTabStyles();
        void RefreshCornerHandleVisibility();

        Ref<UIStackBox> m_TabBar;
        Ref<UIStackBox> m_ContentHost;
        Ref<UIElement> m_CornerHandle;
        std::vector<TabEntry> m_Tabs;
        size_t m_ActiveIndex = 0;
        bool m_ShowUndockCornerHandle = false;
    };
}
