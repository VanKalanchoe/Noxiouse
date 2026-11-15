#include "UITabPanel.h"

#include <algorithm>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Layout/UIWindow.h"
#include "NoxCore/UI/Widgets/UIButton.h"

namespace Nox::UI
{
    namespace
    {
        class UITabButton final : public UIButton
        {
        public:
            explicit UITabButton(std::string text)
                : UIButton(std::move(text), {})
            {
            }

            std::function<void()> OnActivate;
            std::function<void(UIVec2)> OnDragOut;

            bool OnPointerDown(const UIPointerEvent& e) override
            {
                if (e.Button != 1 || !IsEnabled())
                    return false;
                m_Pressed = true;
                m_DraggedOut = false;
                m_PressPos = e.Position;
                if (OnActivate)
                    OnActivate();
                return true;
            }

            bool OnPointerMove(const UIPointerEvent& e) override
            {
                if (m_Pressed && !m_DraggedOut)
                {
                    const float dx = e.Position.x - m_PressPos.x;
                    const float dy = e.Position.y - m_PressPos.y;
                    if (dx * dx + dy * dy > 64.0f) // > 8px drag threshold
                    {
                        m_DraggedOut = true;
                        m_Pressed = false;
                        auto cb = OnDragOut;
                        if (cb)
                            cb(e.Position);
                        return true;
                    }
                }
                return UIButton::OnPointerMove(e);
            }

            bool OnPointerUp(const UIPointerEvent& e) override
            {
                if (e.Button == 1)
                {
                    m_Pressed = false;
                    m_DraggedOut = false;
                }
                return UIButton::OnPointerUp(e);
            }

        private:
            bool m_Pressed = false;
            bool m_DraggedOut = false;
            UIVec2 m_PressPos{ 0.0f, 0.0f };
        };
    }

    UITabPanel::UITabPanel()
        : UIStackBox(UIOrientation::Vertical, 0.0f)
    {
        StyleClass = "Panel";
        DrawBackground = true;
        Padding = UIThickness::All(0.0f);

        m_TabBar = Ref<UIStackBox>(new UIStackBox(UIOrientation::Horizontal, 2.0f));
        m_TabBar->StyleClass = "TabBar";
        m_TabBar->DrawBackground = true;
        m_TabBar->HeightMode = UISizeMode::Auto;
        m_TabBar->Padding = { 6.0f, 4.0f, 6.0f, 0.0f };

        m_ContentHost = Ref<UIStackBox>(new UIStackBox(UIOrientation::Vertical, 0.0f));
        m_ContentHost->WidthMode = UISizeMode::Fill;
        m_ContentHost->HeightMode = UISizeMode::Fill;
        m_ContentHost->Padding = UIThickness::All(10.0f);

        AddChild(m_TabBar);
        AddChild(m_ContentHost);
    }

    size_t UITabPanel::FindTabIndex(const UIButton* btn) const
    {
        for (size_t i = 0; i < m_Tabs.size(); ++i)
        {
            if (m_Tabs[i].Button.get() == btn)
                return i;
        }
        return m_Tabs.size();
    }

    size_t UITabPanel::AddTab(const std::string& title, const Ref<UIElement>& content)
    {
        return InsertTab(m_Tabs.size(), title, content);
    }

    size_t UITabPanel::InsertTab(size_t index, const std::string& title, const Ref<UIElement>& content)
    {
        const size_t insertIdx = std::min(index, m_Tabs.size());
        Ref<UITabButton> btn(new UITabButton(title));
        UITabButton* rawBtn = btn.get();
        btn->OnActivate = [this, rawBtn]()
        {
            const size_t currentIdx = FindTabIndex(rawBtn);
            if (currentIdx < m_Tabs.size())
                SetActiveTab(currentIdx);
        };
        btn->OnDragOut = [this, rawBtn](UIVec2 pos)
        {
            const size_t currentIdx = FindTabIndex(rawBtn);
            auto cb = OnTabDragOut;
            if (cb && currentIdx < m_Tabs.size())
                cb(currentIdx, pos);
        };
        btn->StyleClass = "Tab";
        btn->VAlign = UIAlignment::Center;

        if (content)
        {
            content->WidthMode = UISizeMode::Fill;
            content->HeightMode = UISizeMode::Fill;
            m_ContentHost->AddChild(content);
        }

        m_Tabs.insert(m_Tabs.begin() + static_cast<ptrdiff_t>(insertIdx), { title, btn, content });
        m_TabBar->ClearChildren();
        for (const auto& t : m_Tabs)
        {
            if (t.Button)
                m_TabBar->AddChild(t.Button);
        }

        if (m_Tabs.size() == 1)
            m_ActiveIndex = 0;
        else if (insertIdx <= m_ActiveIndex && m_ActiveIndex + 1 < m_Tabs.size())
            ++m_ActiveIndex;
        RefreshTabStyles();
        return insertIdx;
    }

    size_t UITabPanel::ComputeTabInsertIndex(float pointerX, float& outInsertX) const
    {
        const UIRect& barBounds = m_TabBar->GetBounds();
        if (m_Tabs.empty())
        {
            outInsertX = barBounds.Min.x + 8.0f;
            return 0;
        }

        for (size_t i = 0; i < m_Tabs.size(); ++i)
        {
            if (!m_Tabs[i].Button)
                continue;
            const UIRect& tb = m_Tabs[i].Button->GetBounds();
            const float midX = (tb.Min.x + tb.Max.x) * 0.5f;
            if (pointerX < midX)
            {
                outInsertX = (i == 0) ? (tb.Min.x - 2.0f) : (tb.Min.x - 2.0f);
                return i;
            }
        }

        const UIRect& lastB = m_Tabs.back().Button ? m_Tabs.back().Button->GetBounds() : barBounds;
        outInsertX = lastB.Max.x + 2.0f;
        return m_Tabs.size();
    }

    void UITabPanel::SetActiveTab(size_t index)
    {
        if (index >= m_Tabs.size())
            return;
        m_ActiveIndex = index;
        RefreshTabStyles();
    }

    const std::string& UITabPanel::GetTabTitle(size_t index) const
    {
        static const std::string kEmpty;
        return index < m_Tabs.size() ? m_Tabs[index].Title : kEmpty;
    }

    Ref<UIElement> UITabPanel::GetTabContent(size_t index) const
    {
        return index < m_Tabs.size() ? m_Tabs[index].Content : nullptr;
    }

    bool UITabPanel::RemoveTab(size_t index)
    {
        if (index >= m_Tabs.size())
            return false;

        if (m_Tabs[index].Button)
            m_TabBar->RemoveChild(m_Tabs[index].Button);
        if (m_Tabs[index].Content)
            m_ContentHost->RemoveChild(m_Tabs[index].Content);

        m_Tabs.erase(m_Tabs.begin() + static_cast<ptrdiff_t>(index));
        if (!m_Tabs.empty())
            m_ActiveIndex = std::min(m_ActiveIndex, m_Tabs.size() - 1);
        else
            m_ActiveIndex = 0;
        RefreshTabStyles();
        return true;
    }

    bool UITabPanel::DetachTabToWindow(size_t index, UIContext& context)
    {
        if (index >= m_Tabs.size())
            return false;

        std::string title = m_Tabs[index].Title;
        Ref<UIElement> content = m_Tabs[index].Content;
        if (!RemoveTab(index))
            return false;

        Ref<UIAnchorBox> host(new UIAnchorBox());
        host->HitTestMode = UIHitTestMode::PassThrough;

        Ref<UIWindow> win(new UIWindow(title, [this, &context, title, content]()
        {
            UITabPanel* self = this;
            UIContext& ctx = context;
            std::string tabTitle = title;
            Ref<UIElement> tabContent = content;
            ctx.ClearOverlay();
            if (tabContent)
                self->AddTab(tabTitle, tabContent);
        }));
        win->FixedSize = { 460.0f, 320.0f };
        if (content)
        {
            content->Visible = true;
            win->AddContent(content);
        }
        host->AddChild(win);
        context.SetOverlay(host);
        return true;
    }

    void UITabPanel::RefreshTabStyles()
    {
        for (size_t i = 0; i < m_Tabs.size(); ++i)
        {
            const bool active = (i == m_ActiveIndex);
            if (m_Tabs[i].Button)
                m_Tabs[i].Button->StyleClass = active ? "Tab.active" : "Tab";
            if (m_Tabs[i].Content)
                m_Tabs[i].Content->Visible = active;
        }
    }
}
