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
        Ref<UIElement> GetTabContent(size_t index) const;

        const Ref<UIStackBox>& GetTabBar() const { return m_TabBar; }
        const Ref<UIStackBox>& GetContentHost() const { return m_ContentHost; }
        void SetTabBarVisible(bool visible) { if (m_TabBar) m_TabBar->Visible = visible; }

        // Fired when a tab is dragged past the tear-out threshold while pressed.
        std::function<void(size_t index, UIVec2 pointerPos)> OnTabDragOut;

    private:
        struct TabEntry
        {
            std::string Title;
            Ref<UIButton> Button;
            Ref<UIElement> Content;
        };

        size_t FindTabIndex(const UIButton* btn) const;
        void RefreshTabStyles();

        Ref<UIStackBox> m_TabBar;
        Ref<UIStackBox> m_ContentHost;
        std::vector<TabEntry> m_Tabs;
        size_t m_ActiveIndex = 0;
    };
}
