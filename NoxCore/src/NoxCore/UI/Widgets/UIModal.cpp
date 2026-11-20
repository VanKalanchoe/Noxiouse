#include "UIModal.h"

#include "NoxCore/UI/Core/UIContext.h"
#include <algorithm>

#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
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
                DrawBackground = false;
                HitTestMode = UIHitTestMode::Self;
            }

        protected:
            // The editor stays visible behind the dialog, dimmed.
            void OnPaint(UIDrawList& drawList, const UITheme&) override
            {
                drawList.AddRoundedRect(GetBounds().Min, GetBounds().Max, { 0.0f, 0.0f, 0.0f, 0.45f });
            }

            // The dialog in the middle at the size it asks for (at most the window less a margin).
            void ArrangeChildren(const UIRect& contentRect) override
            {
                for (const auto& child : GetChildren())
                {
                    if (!child->Visible)
                        continue;
                    const UIVec2 desired = child->Measure();
                    const float w = std::min(desired.x, std::max(120.0f, contentRect.Width() - 32.0f));
                    const float h = std::min(child->ComputeHeightForWidth(w), std::max(80.0f, contentRect.Height() - 32.0f));
                    const UIVec2 min{ contentRect.Min.x + (contentRect.Width() - w) * 0.5f, contentRect.Min.y + (contentRect.Height() - h) * 0.5f };
                    child->Arrange({ min, min + UIVec2{ w, h } });
                }
            }

        public:

            bool OnPointerDown(const UIPointerEvent& e) override
            {
                if (e.Button == 1 && m_Context)
                {
                    // A press inside the dialog (on its buttons, fields, text) bubbles up to here too: only the dimmed area
                    // around it dismisses.
                    if (AnyChildContains(e.Position))
                        return true;
                    UIContext* ctx = m_Context;
                    auto onDismiss = std::move(m_OnDismiss);
                    ctx->ClearOverlay();
                    if (onDismiss)
                        onDismiss();
                    return true;
                }
                return false;
            }

        private:
            UIContext* m_Context = nullptr;
            std::function<void()> m_OnDismiss;
        };

        Ref<UIStackBox> CreateModalCard(float minWidth, float minHeight)
        {
            Ref<UIStackBox> card(new UIStackBox(UIOrientation::Vertical, 12.0f));
            card->StyleClass = "MenuPopup";
            card->DrawBackground = true;
            card->HitTestMode = UIHitTestMode::Self;
            card->WidthMode = UISizeMode::Auto;
            card->HeightMode = UISizeMode::Auto;
            card->MinSize = { minWidth, minHeight };
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
            // Copied first: closing the dialog destroys this button and the callback with it.
            UIContext* ctx = &context;
            auto run = onCancel;
            ctx->ClearOverlay();
            if (run)
                run();
        }));
        Ref<UIButton> confirmBtn(new UIButton(std::move(confirmLabel), [&context, onConfirm]()
        {
            // Copied first: closing the dialog destroys this button and the callback with it.
            UIContext* ctx = &context;
            auto run = onConfirm;
            ctx->ClearOverlay();
            if (run)
                run();
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
        card->AddChild(Ref<UILabel>(new UILabel(std::move(message), 13.5f)));

        Ref<UITextInput> input(new UITextInput(std::move(defaultValue), std::move(placeholder)));
        input->OnSubmit = [&context, onSubmit](const std::string& val)
        {
            // Copied first: closing the dialog destroys this field and the callback with it.
            UIContext* ctx = &context;
            auto run = onSubmit;
            const std::string value = val;
            ctx->ClearOverlay();
            if (run)
                run(value);
        };
        card->AddChild(input);

        Ref<UIStackBox> btnRow(new UIStackBox(UIOrientation::Horizontal, 8.0f));
        btnRow->HAlign = UIAlignment::End;

        Ref<UIButton> cancelBtn(new UIButton("Cancel", [&context, onCancel]()
        {
            // Copied first: closing the dialog destroys this button and the callback with it.
            UIContext* ctx = &context;
            auto run = onCancel;
            ctx->ClearOverlay();
            if (run)
                run();
        }));
        Ref<UIButton> okBtn(new UIButton("OK", [&context, input, onSubmit]()
        {
            // Copied first: closing the dialog destroys this button and the callback with it.
            const std::string val = input ? input->Text : "";
            UIContext* ctx = &context;
            auto run = onSubmit;
            ctx->ClearOverlay();
            if (run)
                run(val);
        }));
        okBtn->StyleVariant = "primary";

        btnRow->AddChild(cancelBtn);
        btnRow->AddChild(okBtn);
        card->AddChild(btnRow);

        backdrop->AddChild(card);
        context.SetOverlay(backdrop);
        context.SetFocus(input.get());
    }

    void UIModal::ShowCustom(UIContext& context, std::string title, const Ref<UIElement>& content, std::vector<Button> buttons, float width)
    {
        std::function<void()> onDismiss = buttons.empty() ? std::function<void()>{} : buttons.back().OnClick;
        Ref<UIModalBackdrop> backdrop(new UIModalBackdrop(&context, onDismiss));
        Ref<UIStackBox> card = CreateModalCard(width, 0.0f);
        card->MaxSize.x = width;

        Ref<UILabel> heading(new UILabel(std::move(title), 17.0f));
        card->AddChild(heading);
        if (content)
        {
            content->WidthMode = UISizeMode::Fill;
            card->AddChild(content);
        }

        Ref<UIStackBox> btnRow(new UIStackBox(UIOrientation::Horizontal, 8.0f));
        btnRow->HAlign = UIAlignment::End;
        // Shown right to left as Unreal and Windows do: the primary action at the right end.
        for (auto it = buttons.rbegin(); it != buttons.rend(); ++it)
        {
            Ref<UIButton> button(new UIButton(it->Label, [&context, action = it->OnClick]()
            {
                // Copied first: closing the dialog destroys this button and the callback with it.
                UIContext* ctx = &context;
                auto run = action;
                ctx->ClearOverlay();
                if (run)
                    run();
            }));
            button->Padding = UIThickness::Symmetric(14.0f, 6.0f);
            if (it->Primary)
                button->StyleVariant = "primary";
            if (it->Enabled)
            {
                UIButton* raw = button.get();
                button->SetEnabled(it->Enabled());
                button->OnSync = [raw, enabled = it->Enabled]() { raw->SetEnabled(enabled()); };
            }
            btnRow->AddChild(button);
        }
        card->AddChild(btnRow);

        backdrop->AddChild(card);
        context.SetOverlay(backdrop);
    }
}
