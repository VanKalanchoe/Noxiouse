#include "UIMenu.h"

#include <algorithm>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Rendering/UIIcons.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"

namespace Nox::UI
{
    namespace
    {
        class UIMenuHostBase : public UIAnchorBox
        {
        public:
            explicit UIMenuHostBase(UIContext* ctx) : m_Ctx(ctx)
            {
                HitTestMode = UIHitTestMode::Self;
            }

            void SetRootPopup(const Ref<UIMenuPopup>& popup, const UIRect& anchorRect, bool hasAnchorRect, bool alignRight)
            {
                m_Popups.clear();
                m_Popups.push_back({ popup, anchorRect, hasAnchorRect, alignRight, false });
                AddChild(popup);
            }

            void CloseSubmenusFrom(int depth)
            {
                while (static_cast<int>(m_Popups.size()) > std::max(depth, 1))
                {
                    RemoveChild(m_Popups.back().Popup);
                    m_Popups.pop_back();
                }
            }

            void OpenSubmenu(int depth, UIElement* anchorRow, std::vector<UIMenuItem> subItems)
            {
                CloseSubmenusFrom(depth);
                if (subItems.empty() || !anchorRow)
                    return;

                Ref<UIMenuPopup> sub(new UIMenuPopup(std::move(subItems), m_Ctx, this, depth));
                const UIRect rowBounds = anchorRow->GetBounds();
                m_Popups.push_back({ sub, rowBounds, true, false, true });
                AddChild(sub);
            }

        protected:
            void ArrangeChildren(const UIRect& contentRect) override
            {
                constexpr float kMargin = 6.0f;
                const float availW = std::max(80.0f, contentRect.Width() - kMargin * 2.0f);
                const float availH = std::max(80.0f, contentRect.Height() - kMargin * 2.0f);

                for (const PopupEntry& entry : m_Popups)
                {
                    if (!entry.Popup || !entry.Popup->Visible)
                        continue;

                    entry.Popup->SetMaxContentHeight(availH);
                    const UIVec2 desired = entry.Popup->Measure();
                    const float w = std::min(desired.x, availW);
                    float h = std::min(desired.y, availH);

                    float x = entry.AnchorRect.Min.x;
                    float y = entry.AnchorRect.Min.y;

                    if (entry.IsSubmenu)
                    {
                        const float rightX = entry.AnchorRect.Max.x + 2.0f;
                        const float leftX = entry.AnchorRect.Min.x - w - 2.0f;
                        if (rightX + w <= contentRect.Max.x - kMargin || leftX < contentRect.Min.x + kMargin)
                            x = rightX;
                        else
                            x = leftX;
                        y = entry.AnchorRect.Min.y;
                    }
                    else if (entry.HasAnchorRect)
                    {
                        x = entry.AlignRight ? (entry.AnchorRect.Max.x - w) : entry.AnchorRect.Min.x;

                        const float belowY = entry.AnchorRect.Max.y + 3.0f;
                        const float spaceBelow = contentRect.Max.y - kMargin - belowY;
                        const float spaceAbove = (entry.AnchorRect.Min.y - 3.0f) - (contentRect.Min.y + kMargin);
                        if (h <= spaceBelow || spaceBelow >= spaceAbove)
                        {
                            h = std::min(h, std::max(60.0f, spaceBelow));
                            y = belowY;
                        }
                        else
                        {
                            h = std::min(h, std::max(60.0f, spaceAbove));
                            y = entry.AnchorRect.Min.y - 3.0f - h;
                        }
                    }
                    else
                    {
                        // Cursor-anchored context menu: flip up if bottom clips and top has more space.
                        const float spaceBelow = contentRect.Max.y - kMargin - y;
                        const float spaceAbove = y - (contentRect.Min.y + kMargin);
                        if (h > spaceBelow && spaceAbove > spaceBelow)
                        {
                            h = std::min(h, std::max(60.0f, spaceAbove));
                            y -= h;
                        }
                        if (x + w > contentRect.Max.x - kMargin && x - w >= contentRect.Min.x + kMargin)
                            x -= w;
                    }

                    x = std::clamp(x, contentRect.Min.x + kMargin, std::max(contentRect.Min.x + kMargin, contentRect.Max.x - w - kMargin));
                    y = std::clamp(y, contentRect.Min.y + kMargin, std::max(contentRect.Min.y + kMargin, contentRect.Max.y - h - kMargin));
                    entry.Popup->SetMaxContentHeight(h);
                    entry.Popup->Arrange({ { x, y }, { x + w, y + h } });
                }
            }

            struct PopupEntry
            {
                Ref<UIMenuPopup> Popup;
                UIRect AnchorRect{};
                bool HasAnchorRect = false;
                bool AlignRight = false;
                bool IsSubmenu = false;
            };

            UIContext* m_Ctx = nullptr;
            std::vector<PopupEntry> m_Popups;
        };

        class UIMenuRow final : public UIStackBox
        {
        public:
            UIMenuRow(const UIMenuItem& item, UIContext* context, UIMenuHostBase* host, int depth)
                : UIStackBox(UIOrientation::Horizontal, 14.0f)
                , m_Context(context)
                , m_Host(host)
                , m_Depth(depth)
                , m_Checked(item.Checked)
                , m_HasSubmenu(!item.SubItems.empty())
                , m_SubItems(item.SubItems)
                , m_OnTrigger(item.OnTrigger)
            {
                StyleClass = "MenuItem";
                DrawBackground = true;
                HitTestMode = UIHitTestMode::Self;
                WidthMode = UISizeMode::Fill;
                HeightMode = UISizeMode::Auto;
                Tooltip = item.Tooltip;
                SetEnabled(item.Enabled);

                if (m_Checked)
                    Padding = { 24.0f, Padding.Top, m_HasSubmenu ? 22.0f : Padding.Right, Padding.Bottom };
                else if (m_HasSubmenu)
                    Padding = { Padding.Left, Padding.Top, 22.0f, Padding.Bottom };

                m_TitleLabel = Ref<UILabel>(new UILabel(item.Label));
                m_TitleLabel->WidthMode = UISizeMode::Fill;
                m_TitleLabel->VAlign = UIAlignment::Center;
                AddChild(m_TitleLabel);

                if (!m_HasSubmenu && !item.Shortcut.empty())
                {
                    m_RightLabel = Ref<UILabel>(new UILabel(item.Shortcut));
                    m_RightLabel->VAlign = UIAlignment::Center;
                    AddChild(m_RightLabel);
                }
            }

            bool OnPointerEnter(const UIPointerEvent& e) override
            {
                if (m_Host && IsEnabled())
                {
                    if (!m_SubItems.empty())
                        m_Host->OpenSubmenu(m_Depth + 1, this, m_SubItems);
                    else
                        m_Host->CloseSubmenusFrom(m_Depth + 1);
                }
                return UIStackBox::OnPointerEnter(e);
            }

            bool OnPointerUp(const UIPointerEvent& e) override
            {
                if (e.Button == 1 && IsEnabled() && GetBounds().Contains(e.Position))
                {
                    if (!m_SubItems.empty())
                    {
                        if (m_Host)
                            m_Host->OpenSubmenu(m_Depth + 1, this, m_SubItems);
                        return true;
                    }
                    auto cb = m_OnTrigger;
                    if (m_Context)
                        m_Context->ClearOverlay();
                    if (cb)
                        cb();
                    return true;
                }
                return UIStackBox::OnPointerUp(e);
            }

        protected:
            void ArrangeChildren(const UIRect& contentRect) override
            {
                const UIStyleBox& style = GetResolvedStyle();
                if (m_TitleLabel)
                {
                    m_TitleLabel->UseColorOverride = true;
                    m_TitleLabel->ColorOverride = style.TextColor;
                    m_TitleLabel->FontSizeOverride = style.FontSize;
                }
                if (m_RightLabel)
                {
                    m_RightLabel->UseColorOverride = true;
                    m_RightLabel->ColorOverride = HasState(GetState(), UIState::Hovered)
                        ? style.TextColor
                        : UIColor{ 0.58f, 0.62f, 0.69f, style.TextColor.a };
                    m_RightLabel->FontSizeOverride = style.FontSize;
                }
                UIStackBox::ArrangeChildren(contentRect);
            }

            void OnPaint(UIDrawList& drawList, const UITheme& theme) override
            {
                UIStackBox::OnPaint(drawList, theme);
                const UIStyleBox& style = GetResolvedStyle();
                const UIRect& b = GetBounds();
                const float cy = (b.Min.y + b.Max.y) * 0.5f;
                if (m_Checked)
                {
                    const UIColor accent = theme.GetToken("Accent", style.TextColor);
                    const UIVec2 cMin{ b.Min.x + 6.0f, cy - 6.0f };
                    const UIVec2 cMax{ b.Min.x + 18.0f, cy + 6.0f };
                    Icons::DrawCheckmark(drawList, cMin, cMax, accent, 1.7f);
                }
                if (m_HasSubmenu)
                {
                    const UIColor chevronCol = HasState(GetState(), UIState::Hovered)
                        ? style.TextColor
                        : theme.GetToken("TextMuted", style.TextColor);
                    Icons::DrawChevron(drawList, { b.Max.x - 10.0f, cy }, 6.5f, false, chevronCol, 1.5f);
                }
            }

        private:
            UIContext* m_Context = nullptr;
            UIMenuHostBase* m_Host = nullptr;
            int m_Depth = 0;
            bool m_Checked = false;
            bool m_HasSubmenu = false;
            std::vector<UIMenuItem> m_SubItems;
            std::function<void()> m_OnTrigger;
            Ref<UILabel> m_TitleLabel;
            Ref<UILabel> m_RightLabel;
        };

        class UIMenuTriggerButton final : public UIButton
        {
        public:
            using UIButton::UIButton;
            std::function<void()> OnHoverEnter;

            bool OnPointerEnter(const UIPointerEvent& e) override
            {
                if (OnHoverEnter)
                    OnHoverEnter();
                return UIButton::OnPointerEnter(e);
            }
        };

        class UIMenuOverlayHost final : public UIMenuHostBase
        {
        public:
            UIMenuOverlayHost(UIContext* ctx, UIMenuBar* bar)
                : UIMenuHostBase(ctx), m_Bar(bar)
            {
            }

            UIElement* HitTest(const UIVec2& pos) override
            {
                for (auto it = GetChildren().rbegin(); it != GetChildren().rend(); ++it)
                {
                    if (UIElement* hit = (*it)->HitTest(pos))
                        return hit;
                }
                if (m_Bar && m_Bar->GetBounds().Contains(pos))
                    return nullptr;
                return this;
            }

            bool OnPointerDown(const UIPointerEvent& e) override
            {
                if (AnyChildContains(e.Position))
                    return true; // on a menu: its row acts on the release
                if ((e.Button == 1 || e.Button == 3) && m_Ctx)
                {
                    m_Ctx->ClearOverlay();
                    return true;
                }
                return false;
            }

        private:
            UIMenuBar* m_Bar = nullptr;
        };

        class UIContextMenuBackdrop final : public UIMenuHostBase
        {
        public:
            using UIMenuHostBase::UIMenuHostBase;

            bool OnPointerDown(const UIPointerEvent& e) override
            {
                if (AnyChildContains(e.Position))
                    return true; // on a menu: its row acts on the release
                if ((e.Button == 1 || e.Button == 3) && m_Ctx)
                {
                    m_Ctx->ClearOverlay();
                    return true;
                }
                return false;
            }
        };
    }

    UIMenuPopup::UIMenuPopup(std::vector<UIMenuItem> items, UIContext* context, UIElement* host, int depth)
        : UIStackBox(UIOrientation::Vertical, 0.0f)
    {
        StyleClass = "MenuPopup";
        DrawBackground = true;
        HitTestMode = UIHitTestMode::Self;
        WidthMode = UISizeMode::Auto;
        MinSize.x = 180.0f;
        Padding = UIThickness::All(6.0f);

        m_ScrollView = Ref<UIScrollView>(new UIScrollView());
        m_ScrollView->WidthMode = UISizeMode::Fill;
        m_ScrollView->HeightMode = UISizeMode::Auto;
        m_ScrollView->ChildSpacing = 2.0f;

        auto* hostBase = dynamic_cast<UIMenuHostBase*>(host);

        for (const auto& item : items)
        {
            if (item.IsSeparator)
            {
                Ref<UIElement> sep(new UIElement());
                sep->StyleClass = "Separator";
                sep->DrawBackground = true;
                sep->HeightMode = UISizeMode::Fixed;
                sep->FixedSize.y = 1.0f;
                sep->Margin = UIThickness::Symmetric(4.0f, 3.0f);
                m_ScrollView->AddChild(sep);
                continue;
            }

            if (item.IsInfo)
            {
                Ref<UILabel> info(new UILabel(item.Label));
                info->UseColorOverride = true;
                info->ColorOverride = { 0.58f, 0.62f, 0.69f, 1.0f };
                info->Margin = UIThickness::Symmetric(10.0f, 4.0f);
                m_ScrollView->AddChild(info);
                continue;
            }

            m_ScrollView->AddChild(Ref<UIMenuRow>(new UIMenuRow(item, context, hostBase, depth)));
        }

        AddChild(m_ScrollView);
    }

    void UIMenuPopup::SetMaxContentHeight(float maxHeight)
    {
        if (m_ScrollView)
            m_ScrollView->MaxSize.y = std::max(40.0f, maxHeight - Padding.Vertical());
    }

    void UIMenuPopup::ShowContextMenu(UIContext& context, const UIVec2& screenPos, std::vector<UIMenuItem> items)
    {
        if (items.empty())
            return;

        Ref<UIContextMenuBackdrop> backdrop(new UIContextMenuBackdrop(&context));
        Ref<UIMenuPopup> popup(new UIMenuPopup(std::move(items), &context, backdrop.get(), 0));
        backdrop->SetRootPopup(popup, { screenPos, screenPos }, false, false);
        context.SetOverlay(backdrop);
    }

    void UIMenuPopup::ShowPopupMenu(UIContext& context, const UIRect& anchorRect, std::vector<UIMenuItem> items, bool alignRight)
    {
        if (items.empty())
            return;

        Ref<UIContextMenuBackdrop> backdrop(new UIContextMenuBackdrop(&context));
        Ref<UIMenuPopup> popup(new UIMenuPopup(std::move(items), &context, backdrop.get(), 0));
        backdrop->SetRootPopup(popup, anchorRect, true, alignRight);
        context.SetOverlay(backdrop);
    }

    UIMenuBar::UIMenuBar()
        : UIStackBox(UIOrientation::Horizontal, 4.0f)
    {
        StyleClass = "MenuBar";
        DrawBackground = true;
        HeightMode = UISizeMode::Auto;
        Padding = UIThickness::Symmetric(8.0f, 3.0f);
    }

    void UIMenuBar::AddMenu(const std::string& title, std::vector<UIMenuItem> items)
    {
        AddDynamicMenu(title, [items = std::move(items)]() { return items; });
    }

    void UIMenuBar::AddDynamicMenu(const std::string& title, std::function<std::vector<UIMenuItem>()> builder)
    {
        Ref<UIMenuTriggerButton> trigger(new UIMenuTriggerButton(title));
        trigger->StyleClass = "MenuTrigger";
        trigger->VAlign = UIAlignment::Center;
        UIMenuTriggerButton* triggerPtr = trigger.get();

        auto openMenu = [this, triggerPtr, builder = std::move(builder)]()
        {
            UIContext* ctx = UIContext::GetCurrent();
            if (!ctx || !builder)
                return;
            Ref<UIMenuOverlayHost> overlayHost(new UIMenuOverlayHost(ctx, this));
            Ref<UIMenuPopup> popup(new UIMenuPopup(builder(), ctx, overlayHost.get(), 0));
            const UIRect& btnBounds = triggerPtr->GetBounds();

            overlayHost->SetRootPopup(popup, btnBounds, true, false);
            m_ActiveOverlay = overlayHost;
            m_OpenTrigger = triggerPtr;
            ctx->SetOverlay(overlayHost);
        };

        trigger->OnClick = [this, triggerPtr, openMenu]()
        {
            UIContext* ctx = UIContext::GetCurrent();
            if (ctx && m_ActiveOverlay && ctx->GetOverlay() == m_ActiveOverlay && m_OpenTrigger == triggerPtr)
            {
                ctx->ClearOverlay();
                m_ActiveOverlay = nullptr;
                m_OpenTrigger = nullptr;
                return;
            }
            openMenu();
        };

        trigger->OnHoverEnter = [this, triggerPtr, openMenu]()
        {
            UIContext* ctx = UIContext::GetCurrent();
            if (ctx && m_ActiveOverlay && ctx->GetOverlay() == m_ActiveOverlay && m_OpenTrigger != triggerPtr)
                openMenu();
        };

        AddChild(trigger);
    }
}
