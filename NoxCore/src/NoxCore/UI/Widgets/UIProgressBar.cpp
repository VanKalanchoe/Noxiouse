#include "UIProgressBar.h"

#include <algorithm>
#include <cmath>

#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIProgressBar::UIProgressBar(float fraction, std::string overlayText)
        : m_Fraction(std::clamp(fraction, 0.0f, 1.0f))
        , m_OverlayText(std::move(overlayText))
    {
        StyleClass = "TextInput";
        DrawBackground = true;
        HitTestMode = UIHitTestMode::None;
        WidthMode = UISizeMode::Fill;
        HeightMode = UISizeMode::Auto;

        m_Label = Ref<UILabel>(new UILabel(""));
        m_Label->HitTestMode = UIHitTestMode::None;
        m_Label->HAlign = UIAlignment::Center;
        m_Label->VAlign = UIAlignment::Center;
        AddChild(m_Label);

        SyncLabel();
    }

    void UIProgressBar::SetFraction(float fraction)
    {
        float clamped = std::clamp(fraction, 0.0f, 1.0f);
        if (std::abs(m_Fraction - clamped) < 1e-5f)
            return;
        m_Fraction = clamped;
        SyncLabel();
    }

    void UIProgressBar::SetOverlayText(const std::string& text)
    {
        m_OverlayText = text;
        SyncLabel();
    }

    UIVec2 UIProgressBar::ComputeDesiredSize()
    {
        const UIStyleBox& style = GetResolvedStyle();
        if (m_Label && m_Label->FontSizeOverride <= 0.0f)
            m_Label->FontSizeOverride = std::max(10.0f, style.FontSize * 0.85f);
        const UIVec2 labelSz = m_Label ? m_Label->Measure() : UIVec2{ 0.0f, style.FontSize };
        const float h = std::max(style.FontSize * 1.5f, labelSz.y + style.Padding.Vertical());
        return { std::max(MinSize.x, labelSz.x + style.Padding.Horizontal() + 24.0f), h };
    }

    void UIProgressBar::ArrangeChildren(const UIRect& contentRect)
    {
        if (m_Label)
            m_Label->Arrange(contentRect);
    }

    void UIProgressBar::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        if (m_Fraction <= 0.0f)
            return;

        const UIRect& b = GetBounds();
        const float inset = 2.0f;
        const float innerW = std::max(0.0f, b.Width() - inset * 2.0f) * m_Fraction;
        if (innerW <= 0.5f)
            return;

        const float r = std::max(0.0f, GetResolvedStyle().CornerRadius.TopLeft - 1.0f);
        const UIColor barCol = theme.GetToken("Accent", FillColor);
        drawList.AddRoundedRect(
            { b.Min.x + inset, b.Min.y + inset },
            { b.Min.x + inset + innerW, b.Max.y - inset },
            barCol,
            glm::vec4(r)
        );
    }

    void UIProgressBar::SyncLabel()
    {
        if (!m_Label)
            return;

        if (!m_OverlayText.empty())
        {
            m_Label->Text = m_OverlayText;
        }
        else if (ShowPercentage)
        {
            const int pct = static_cast<int>(std::round(m_Fraction * 100.0f));
            m_Label->Text = std::to_string(pct) + "%";
        }
        else
        {
            m_Label->Text.clear();
        }
    }
}
