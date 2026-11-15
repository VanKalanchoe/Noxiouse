#include "UIBadge.h"

#include <algorithm>

#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIBadge::UIBadge(std::string text, const UIColor& badgeColor)
        : BadgeColor(badgeColor)
    {
        StyleClass = "Label";
        DrawBackground = false;
        HitTestMode = UIHitTestMode::None;
        Padding = UIThickness::Symmetric(8.0f, 3.0f);

        m_Label = Ref<UILabel>(new UILabel(std::move(text)));
        m_Label->HitTestMode = UIHitTestMode::None;
        m_Label->HAlign = UIAlignment::Center;
        m_Label->VAlign = UIAlignment::Center;
        AddChild(m_Label);
    }

    void UIBadge::SetText(const std::string& text)
    {
        if (m_Label)
            m_Label->Text = text;
    }

    UIVec2 UIBadge::ComputeDesiredSize()
    {
        const UIStyleBox& style = GetResolvedStyle();
        if (m_Label && m_Label->FontSizeOverride <= 0.0f)
            m_Label->FontSizeOverride = std::max(10.0f, style.FontSize * 0.85f);
        const UIVec2 labelSize = m_Label ? m_Label->Measure() : UIVec2{ style.FontSize * 1.4f, style.FontSize };
        return {
            std::max(MinSize.x, labelSize.x + Padding.Horizontal()),
            std::max(MinSize.y, labelSize.y + Padding.Vertical())
        };
    }

    void UIBadge::ArrangeChildren(const UIRect& contentRect)
    {
        if (m_Label)
            m_Label->Arrange(contentRect);
    }

    void UIBadge::OnPaint(UIDrawList& drawList, const UITheme&)
    {
        const UIRect& b = GetBounds();
        const float r = b.Height() * 0.5f;
        drawList.AddRoundedRect(
            b.Min,
            b.Max,
            { BadgeColor.r, BadgeColor.g, BadgeColor.b, 0.24f },
            glm::vec4(r),
            1.0f,
            { BadgeColor.r, BadgeColor.g, BadgeColor.b, 0.85f }
        );
    }
}
