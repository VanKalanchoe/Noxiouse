#include "UIButton.h"

#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIButton::UIButton(std::string text, std::function<void()> onClick)
        : OnClick(std::move(onClick))
    {
        StyleClass = "Button";
        DrawBackground = true;
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

    UIVec2 UIButton::ComputeDesiredSize()
    {
        const UIStyleBox& style = GetResolvedStyle();
        const UIVec2 labelSize = m_Label->Measure();
        return {
            labelSize.x + style.Padding.Horizontal(),
            labelSize.y + style.Padding.Vertical()
        };
    }

    void UIButton::ArrangeChildren(const UIRect& contentRect)
    {
        const UIStyleBox& style = GetResolvedStyle();
        m_Label->UseColorOverride = true;
        m_Label->ColorOverride = style.TextColor;
        m_Label->FontSizeOverride = style.FontSize;
        const UIRect inner = contentRect.Inset(style.Padding);
        m_Label->Arrange(inner);
    }
}
