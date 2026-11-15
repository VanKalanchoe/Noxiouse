#include "UIMenu.h"

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIMenuPopup::UIMenuPopup(std::vector<UIMenuItem> items, UIContext* context)
        : UIStackBox(UIOrientation::Vertical, 2.0f)
    {
        StyleClass = "MenuPopup";
        DrawBackground = true;
        HitTestMode = UIHitTestMode::Self;
        WidthMode = UISizeMode::Auto;
        MinSize.x = 160.0f;
        Padding = UIThickness::All(6.0f);

        for (auto& item : items)
        {
            if (item.IsSeparator)
            {
                Ref<UIElement> sep(new UIElement());
                sep->StyleClass = "Separator";
                sep->DrawBackground = true;
                sep->HeightMode = UISizeMode::Fixed;
                sep->FixedSize.y = 1.0f;
                sep->Margin = UIThickness::Symmetric(4.0f, 3.0f);
                AddChild(sep);
                continue;
            }

            Ref<UIButton> btn(new UIButton(item.Label, [cb = std::move(item.OnTrigger), context]()
            {
                if (context)
                    context->ClearOverlay();
                if (cb)
                    cb();
            }));
            btn->StyleClass = "MenuItem";
            btn->WidthMode = UISizeMode::Fill;
            btn->HeightMode = UISizeMode::Auto;
            AddChild(btn);
        }
    }

    void UIMenuPopup::ShowContextMenu(UIContext& context, const UIVec2& screenPos, std::vector<UIMenuItem> items)
    {
        if (items.empty())
            return;

        class UIContextMenuBackdrop : public UIAnchorBox
        {
        public:
            explicit UIContextMenuBackdrop(UIContext* ctx) : m_Ctx(ctx)
            {
                HitTestMode = UIHitTestMode::Self;
            }

            bool OnPointerDown(const UIPointerEvent& e) override
            {
                if ((e.Button == 1 || e.Button == 3) && m_Ctx)
                {
                    m_Ctx->ClearOverlay();
                    return true;
                }
                return false;
            }

        private:
            UIContext* m_Ctx = nullptr;
        };

        Ref<UIContextMenuBackdrop> backdrop(new UIContextMenuBackdrop(&context));
        Ref<UIMenuPopup> popup(new UIMenuPopup(std::move(items), &context));
        popup->AnchorMin = { 0.0f, 0.0f };
        popup->AnchorMax = { 0.0f, 0.0f };
        popup->AnchorOffsets = { screenPos.x, screenPos.y, 0.0f, 0.0f };
        backdrop->AddChild(popup);
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

        class UIMenuOverlayHost final : public UIAnchorBox
        {
        public:
            UIMenuOverlayHost(UIContext* ctx, UIMenuBar* bar)
                : m_Ctx(ctx), m_Bar(bar)
            {
                HitTestMode = UIHitTestMode::Self;
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
                if ((e.Button == 1 || e.Button == 3) && m_Ctx)
                {
                    m_Ctx->ClearOverlay();
                    return true;
                }
                return false;
            }

        private:
            UIContext* m_Ctx = nullptr;
            UIMenuBar* m_Bar = nullptr;
        };

        Ref<UIMenuTriggerButton> trigger(new UIMenuTriggerButton(title));
        trigger->StyleClass = "MenuTrigger";
        trigger->VAlign = UIAlignment::Center;
        UIMenuTriggerButton* triggerPtr = trigger.get();

        auto openMenu = [this, triggerPtr, items = std::move(items)]()
        {
            UIContext* ctx = UIContext::GetCurrent();
            if (!ctx)
                return;
            Ref<UIMenuOverlayHost> overlayHost(new UIMenuOverlayHost(ctx, this));
            Ref<UIMenuPopup> popup(new UIMenuPopup(items, ctx));
            const UIRect& btnBounds = triggerPtr->GetBounds();
            popup->AnchorMin = { 0.0f, 0.0f };
            popup->AnchorMax = { 0.0f, 0.0f };
            popup->AnchorOffsets = { btnBounds.Min.x, btnBounds.Max.y + 2.0f, 0.0f, 0.0f };

            overlayHost->AddChild(popup);
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
