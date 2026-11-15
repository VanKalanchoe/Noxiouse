#include "UIModal.h"

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UITextInput.h"

namespace Nox::UI
{
    namespace
    {
        class UIModalBackdrop : public UIAnchorBox
        {
        public:
            UIModalBackdrop(UIContext* context, std::function<void()> onDismiss)
                : m_Context(context)
                , m_OnDismiss(std::move(onDismiss))
            {
                StyleClass = "Background";
                DrawBackground = true;
                HitTestMode = UIHitTestMode::Self;
            }

            bool OnPointerDown(const UIPointerEvent& e) override
            {
                if (e.Button == 1 && m_Context)
                {
                    m_Context->ClearOverlay();
                    if (m_OnDismiss)
                        m_OnDismiss();
                    return true;
                }
                return false;
            }

        private:
            UIContext* m_Context = nullptr;
            std::function<void()> m_OnDismiss;
        };

        Ref<UIStackBox> CreateModalCard(float width, float height)
        {
            Ref<UIStackBox> card(new UIStackBox(UIOrientation::Vertical, 12.0f));
            card->StyleClass = "MenuPopup";
            card->DrawBackground = true;
            card->HitTestMode = UIHitTestMode::Self;
            card->WidthMode = UISizeMode::Fixed;
            card->HeightMode = UISizeMode::Fixed;
            card->FixedSize = { width, height };
            card->AnchorMin = { 0.5f, 0.5f };
            card->AnchorMax = { 0.5f, 0.5f };
            card->AnchorOffsets = { -width * 0.5f, -height * 0.5f, 0.0f, 0.0f };
            card->Padding = UIThickness::All(18.0f);
            return card;
        }
    }

    void UIModal::ShowConfirm(
        UIContext& context,
        std::string title,
        std::string message,
        std::function<void()> onConfirm,
        std::function<void()> onCancel,
        std::string confirmLabel,
        std::string cancelLabel)
    {
        Ref<UIModalBackdrop> backdrop(new UIModalBackdrop(&context, onCancel));
        Ref<UIStackBox> card = CreateModalCard(400.0f, 175.0f);

        card->AddChild(Ref<UILabel>(new UILabel(std::move(title), 16.0f)));
        auto msg = Ref<UILabel>(new UILabel(std::move(message), 13.5f));
        msg->HeightMode = UISizeMode::Fill;
        card->AddChild(msg);

        Ref<UIStackBox> btnRow(new UIStackBox(UIOrientation::Horizontal, 8.0f));
        btnRow->HAlign = UIAlignment::End;

        Ref<UIButton> cancelBtn(new UIButton(std::move(cancelLabel), [&context, onCancel]()
        {
            context.ClearOverlay();
            if (onCancel)
                onCancel();
        }));
        Ref<UIButton> confirmBtn(new UIButton(std::move(confirmLabel), [&context, onConfirm]()
        {
            context.ClearOverlay();
            if (onConfirm)
                onConfirm();
        }));
        confirmBtn->StyleVariant = "primary";

        btnRow->AddChild(cancelBtn);
        btnRow->AddChild(confirmBtn);
        card->AddChild(btnRow);

        backdrop->AddChild(card);
        context.SetOverlay(backdrop);
    }

    void UIModal::ShowPrompt(
        UIContext& context,
        std::string title,
        std::string message,
        std::string defaultValue,
        std::string placeholder,
        std::function<void(const std::string&)> onSubmit,
        std::function<void()> onCancel)
    {
        Ref<UIModalBackdrop> backdrop(new UIModalBackdrop(&context, onCancel));
        Ref<UIStackBox> card = CreateModalCard(420.0f, 205.0f);

        card->AddChild(Ref<UILabel>(new UILabel(std::move(title), 16.0f)));
        card->AddChild(Ref<UILabel>(std::move(message), 13.5f));

        Ref<UITextInput> input(new UITextInput(std::move(defaultValue), std::move(placeholder)));
        input->OnSubmit = [&context, onSubmit](const std::string& val)
        {
            context.ClearOverlay();
            if (onSubmit)
                onSubmit(val);
        };
        card->AddChild(input);

        Ref<UIStackBox> btnRow(new UIStackBox(UIOrientation::Horizontal, 8.0f));
        btnRow->HAlign = UIAlignment::End;

        Ref<UIButton> cancelBtn(new UIButton("Cancel", [&context, onCancel]()
        {
            context.ClearOverlay();
            if (onCancel)
                onCancel();
        }));
        Ref<UIButton> okBtn(new UIButton("OK", [&context, input, onSubmit]()
        {
            const std::string val = input ? input->Text : "";
            context.ClearOverlay();
            if (onSubmit)
                onSubmit(val);
        }));
        okBtn->StyleVariant = "primary";

        btnRow->AddChild(cancelBtn);
        btnRow->AddChild(okBtn);
        card->AddChild(btnRow);

        backdrop->AddChild(card);
        context.SetOverlay(backdrop);
        context.SetFocus(input.get());
    }
}
