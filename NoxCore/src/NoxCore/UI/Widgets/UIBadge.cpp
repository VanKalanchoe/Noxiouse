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
        Padding = UIThickness::Symmetric(9.0f, 3.0f);
        MinSize = { 32.0f, 20.0f };

        m_Label = Ref<UILabel>(new UILabel(std::move(text), 11.5f));
        m_Label->HitTestMode = UIHitTestMode::None;
        m_Label->HAlign = UIAlignment::Center;
        m_Label->VAlign = UIAlignment::Center;
        m_Label->UseColorOverride = true;
        m_Label->ColorOverride = { 0.96f, 0.98f, 1.0f, 1.0f };
        AddChild(m_Label);
    }

    void UIBadge::SetText(const std::string& text)
    {
        if (m_Label)
            m_Label->Text = text;
    }

    UIVec2 UIBadge::ComputeDesiredSize()
    {
        const UIVec2 labelSize = m_Label ? m_Label->Measure() : UIVec2{ 20.0f, 14.0f };
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
