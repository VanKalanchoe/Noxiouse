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
        HeightMode = UISizeMode::Fixed;
        FixedSize.y = 22.0f;

        m_Label = Ref<UILabel>(new UILabel("", 12.0f));
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
        return { 160.0f, FixedSize.y > 0.0f ? FixedSize.y : 22.0f };
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
        drawList.AddRoundedRect(
            { b.Min.x + inset, b.Min.y + inset },
            { b.Min.x + inset + innerW, b.Max.y - inset },
            { FillColor.r, FillColor.g, FillColor.b, FillColor.a },
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
