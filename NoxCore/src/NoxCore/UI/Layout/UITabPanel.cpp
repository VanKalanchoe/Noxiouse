#include "UITabPanel.h"

#include <algorithm>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Layout/UIWindow.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
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

        // ImGui-style top-left corner triangle handle shown only on docked NoTabBar panels that allow undocking.
        class UICornerUndockHandle final : public UIElement
        {
        public:
            explicit UICornerUndockHandle(UITabPanel& owner) : m_Owner(owner)
            {
                HitTestMode = UIHitTestMode::Self;
                DrawBackground = false;
                WidthMode = UISizeMode::Fixed;
                HeightMode = UISizeMode::Fixed;
                FixedSize = { 0.0f, 0.0f };
                Visible = false;
                Tooltip = "Drag corner to move / undock";
            }

            UIElement* HitTest(const UIVec2& pos) override
            {
                if (!Visible || !IsEnabled())
                    return nullptr;
                const UIRect& b = GetBounds();
                if (!b.Contains(pos))
                    return nullptr;
                const float lx = pos.x - b.Min.x;
                const float ly = pos.y - b.Min.y;
                return (lx + ly <= kHandleExtent + 2.0f) ? this : nullptr;
            }

            bool OnPointerDown(const UIPointerEvent& e) override
            {
                if (e.Button != 1 || !IsEnabled())
                    return false;
                m_Pressed = true;
                m_DraggedOut = false;
                m_PressPos = e.Position;
                return true;
            }

            bool OnPointerMove(const UIPointerEvent& e) override
            {
                if (m_Pressed && !m_DraggedOut)
                {
                    const float dx = e.Position.x - m_PressPos.x;
                    const float dy = e.Position.y - m_PressPos.y;
                    if (dx * dx + dy * dy > 36.0f)
                    {
                        m_DraggedOut = true;
                        m_Pressed = false;
                        if (m_Owner.OnTabDragOut && m_Owner.GetActiveTab() < m_Owner.GetTabCount())
                            m_Owner.OnTabDragOut(m_Owner.GetActiveTab(), e.Position);
                        return true;
                    }
                }
                return false;
            }

            bool OnPointerUp(const UIPointerEvent& e) override
            {
                if (e.Button == 1)
                {
                    m_Pressed = false;
                    m_DraggedOut = false;
                }
                return true;
            }

        protected:
            void OnPaint(UIDrawList& drawList, const UITheme& theme) override
            {
                const UIRect& b = GetBounds();
                const bool hovered = HasState(GetState(), UIState::Hovered) || m_Pressed;
                const UIColor col = hovered
                    ? theme.GetToken("Accent", { 0.35f, 0.62f, 0.98f, 0.92f })
                    : UIColor{ 0.48f, 0.54f, 0.64f, 0.55f };

                // Solid upper-left right-triangle drawn cleanly in 1px horizontal strips.
                constexpr int kSteps = 10;
                for (int i = 0; i < kSteps; ++i)
                {
                    const float w = static_cast<float>(kSteps - i);
                    const float y = b.Min.y + static_cast<float>(i);
                    drawList.AddRoundedRect({ b.Min.x, y }, { b.Min.x + w, y + 1.0f }, col);
                }
            }

        private:
            static constexpr float kHandleExtent = 12.0f;
            UITabPanel& m_Owner;
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

        m_CornerHandle = Ref<UIElement>(new UICornerUndockHandle(*this));

        AddChild(m_TabBar);
        AddChild(m_ContentHost);
        AddChild(m_CornerHandle);
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

    void UITabPanel::SetTabTitle(size_t index, const std::string& title)
    {
        if (index >= m_Tabs.size())
            return;
        m_Tabs[index].Title = title;
        if (m_Tabs[index].Button)
            m_Tabs[index].Button->SetText(title);
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

    void UITabPanel::SetTabBarVisible(bool visible)
    {
        if (m_TabBar)
            m_TabBar->Visible = visible;
        RefreshCornerHandleVisibility();
    }

    void UITabPanel::SetShowUndockCornerHandle(bool show)
    {
        m_ShowUndockCornerHandle = show;
        RefreshCornerHandleVisibility();
    }

    void UITabPanel::RefreshCornerHandleVisibility()
    {
        if (m_CornerHandle)
            m_CornerHandle->Visible = m_ShowUndockCornerHandle && (!m_TabBar || !m_TabBar->Visible) && !m_Tabs.empty();
    }

    void UITabPanel::ArrangeChildren(const UIRect& contentRect)
    {
        UIStackBox::ArrangeChildren(contentRect);
        if (m_CornerHandle && m_CornerHandle->Visible)
        {
            const UIRect& b = GetBounds();
            m_CornerHandle->Arrange({ b.Min, { b.Min.x + 13.0f, b.Min.y + 13.0f } });
        }
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
        RefreshCornerHandleVisibility();
    }
}
