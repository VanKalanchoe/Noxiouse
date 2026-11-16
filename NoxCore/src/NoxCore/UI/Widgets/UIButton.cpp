#include "UIButton.h"

#include <algorithm>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    namespace
    {
        UIStyleBox ResolveButtonStyle(const UIButton& button, const UIStyleBox& fallback)
        {
            if (const UIContext* ctx = UIContext::GetCurrent(); ctx && ctx->GetTheme())
                return ctx->GetTheme()->Resolve(button.StyleClass, button.StyleVariant, button.GetState());
            return fallback;
        }
    }

    UIButton::UIButton(std::string text, std::function<void()> onClick)
        : OnClick(std::move(onClick))
    {
        StyleClass = "Button";
        DrawBackground = true;
        ClipChildren = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;

        m_Label = Ref<UILabel>(new UILabel(std::move(text)));
        m_Label->HAlign = UIAlignment::Center;
        m_Label->VAlign = UIAlignment::Center;
        AddChild(m_Label);
    }

    UIButton::~UIButton() = default;

    void UIButton::SetText(const std::string& text)
    {
        m_Label->Text = text;
    }

    const std::string& UIButton::GetText() const
    {
        return m_Label->Text;
    }

    bool UIButton::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == 1 && IsEnabled() && GetBounds().Contains(e.Position))
        {
            if (OnClick)
                OnClick();
            return true;
        }
        return UIElement::OnPointerUp(e);
    }

    namespace
    {
        // The space between the button's edge and its text: the button's own Padding where it sets one (a compact button),
        // else its style's. Never both -- measured and arranged with the same one.
        UIThickness ButtonPadding(const UIButton& button, const UIStyleBox& style)
        {
            const UIThickness& own = button.Padding;
            const bool hasOwn = own.Left != 0.0f || own.Top != 0.0f || own.Right != 0.0f || own.Bottom != 0.0f;
            return hasOwn ? own : style.Padding;
        }
    }

    UIVec2 UIButton::ComputeDesiredSize()
    {
        const UIStyleBox style = ResolveButtonStyle(*this, GetResolvedStyle());
        const UIThickness padding = ButtonPadding(*this, style);
        m_Label->FontSizeOverride = style.FontSize;
        const UIVec2 labelSize = m_Label->Measure();
        return {
            labelSize.x + padding.Horizontal(),
            labelSize.y + padding.Vertical()
        };
    }

    void UIButton::ArrangeChildren(const UIRect&)
    {
        const UIStyleBox style = ResolveButtonStyle(*this, GetResolvedStyle());
        m_Label->UseColorOverride = true;
        m_Label->ColorOverride = style.TextColor;
        m_Label->FontSizeOverride = style.FontSize;
        m_Label->HAlign = (StyleClass == "MenuItem" || StyleClass == "TreeRow" || StyleClass == "SectionHeader")
            ? UIAlignment::Start
            : UIAlignment::Center;
        m_Label->Elide = TextElide;

        const UIRect& bounds = GetBounds();
        UIRect inner = bounds.Inset(ButtonPadding(*this, style));

        // A button smaller than its text and padding, where the text would fit without the padding (a fixed 24 px "x"): the
        // padding gives way before the text is cut. A text too long even for that keeps the padding and is shortened.
        const float textWidth = UILabel::MeasureText(m_Label->Text, m_Label->FontAsset, style.FontSize).x;
        const float widest = std::max(0.0f, bounds.Width() - 2.0f);
        if (textWidth > inner.Width() && textWidth <= widest)
        {
            const float width = textWidth;
            const float left = std::clamp((inner.Min.x + inner.Max.x - width) * 0.5f, bounds.Min.x + 1.0f, std::max(bounds.Min.x + 1.0f, bounds.Max.x - 1.0f - width));
            inner.Min.x = left;
            inner.Max.x = left + width;
        }
        m_Label->Arrange(inner);

        // A shortened text is readable in full by resting the pointer on the button.
        const bool cut = TextElide != UITextElide::None && inner.Width() > 1.0f && textWidth > inner.Width() + 0.5f;
        if (cut && (Tooltip.empty() || m_AutoTooltip))
        {
            Tooltip = m_Label->Text;
            m_AutoTooltip = true;
        }
        else if (!cut && m_AutoTooltip)
        {
            Tooltip.clear();
            m_AutoTooltip = false;
        }
    }
}
