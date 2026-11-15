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
        WidthMode = UISizeMode::Fixed;
        FixedSize.x = 210.0f;
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
            btn->HeightMode = UISizeMode::Fixed;
            btn->FixedSize.y = 26.0f;
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
        HeightMode = UISizeMode::Fixed;
        FixedSize.y = 30.0f;
        Padding = UIThickness::Symmetric(8.0f, 3.0f);
    }

    void UIMenuBar::AddMenu(const std::string& title, std::vector<UIMenuItem> items, UIContext& context)
    {
        Ref<UIButton> trigger(new UIButton(title));
        trigger->StyleClass = "MenuTrigger";
        trigger->VAlign = UIAlignment::Center;
        UIButton* triggerPtr = trigger.get();

        trigger->OnClick = [triggerPtr, items = std::move(items), &context]()
        {
            Ref<UIAnchorBox> overlayHost(new UIAnchorBox());
            overlayHost->HitTestMode = UIHitTestMode::PassThrough;

            Ref<UIMenuPopup> popup(new UIMenuPopup(items, &context));
            const UIRect& btnBounds = triggerPtr->GetBounds();
            popup->AnchorMin = { 0.0f, 0.0f };
            popup->AnchorMax = { 0.0f, 0.0f };
            popup->AnchorOffsets = { btnBounds.Min.x, btnBounds.Max.y + 2.0f, 0.0f, 0.0f };

            overlayHost->AddChild(popup);
            context.SetOverlay(overlayHost);
        };

        AddChild(trigger);
    }
}
