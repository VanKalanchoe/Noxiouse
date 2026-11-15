#include "UIScrollView.h"

#include <algorithm>

#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    UIScrollView::UIScrollView()
    {
        StyleClass = "ScrollView";
        ClipChildren = true;
        HitTestMode = UIHitTestMode::Self;
    }

    bool UIScrollView::OnMouseWheel(const UIPointerEvent& e)
    {
        const float maxScroll = std::max(0.0f, m_ContentHeight - m_ViewportHeight);
        if (maxScroll <= 0.0f)
            return false;

        ScrollOffsetY = std::clamp(ScrollOffsetY - e.ScrollDelta.y * ScrollSpeed, 0.0f, maxScroll);
        return true;
    }

    UIVec2 UIScrollView::ComputeDesiredSize()
    {
        float maxW = 0.0f;
        float totalH = 0.0f;
        int visibleCount = 0;
        for (const auto& child : GetChildren())
        {
            if (!child->Visible)
                continue;
            const UIVec2 cs = child->Measure();
            maxW = std::max(maxW, cs.x + child->Margin.Horizontal());
            totalH += cs.y + child->Margin.Vertical();
            ++visibleCount;
        }
        if (visibleCount > 1)
            totalH += ChildSpacing * static_cast<float>(visibleCount - 1);
        return { maxW, totalH };
    }

    void UIScrollView::ArrangeChildren(const UIRect& contentRect)
    {
        m_ViewportHeight = std::max(0.0f, contentRect.Height());

        float totalH = 0.0f;
        int visibleCount = 0;
        for (const auto& child : GetChildren())
        {
            if (!child->Visible)
                continue;
            totalH += child->GetDesiredSize().y + child->Margin.Vertical();
            ++visibleCount;
        }
        if (visibleCount > 1)
            totalH += ChildSpacing * static_cast<float>(visibleCount - 1);

        m_ContentHeight = totalH;
        const float maxScroll = std::max(0.0f, m_ContentHeight - m_ViewportHeight);
        ScrollOffsetY = std::clamp(ScrollOffsetY, 0.0f, maxScroll);

        const float scrollbarReserve = maxScroll > 0.0f ? 10.0f : 0.0f;
        const float availW = std::max(0.0f, contentRect.Width() - scrollbarReserve);

        float cursorY = contentRect.Min.y - ScrollOffsetY;
        for (const auto& child : GetChildren())
        {
            if (!child->Visible)
                continue;

            const float ch = child->GetDesiredSize().y;
            const UIRect slot{
                { contentRect.Min.x + child->Margin.Left, cursorY + child->Margin.Top },
                { contentRect.Min.x + availW - child->Margin.Right, cursorY + child->Margin.Top + ch }
            };
            child->Arrange(slot);
            cursorY += ch + child->Margin.Vertical() + ChildSpacing;
        }
    }

    void UIScrollView::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        if (m_ContentHeight <= m_ViewportHeight || m_ViewportHeight <= 1.0f)
            return;

        const UIRect& b = GetBounds();
        const float trackH = b.Height() - 8.0f;
        const float ratio = std::clamp(m_ViewportHeight / m_ContentHeight, 0.08f, 1.0f);
        const float thumbH = std::max(20.0f, trackH * ratio);
        const float maxScroll = std::max(1.0f, m_ContentHeight - m_ViewportHeight);
        const float scrollT = std::clamp(ScrollOffsetY / maxScroll, 0.0f, 1.0f);
        const float thumbY = b.Min.y + 4.0f + scrollT * (trackH - thumbH);

        drawList.AddRoundedRect(
            { b.Max.x - 7.0f, thumbY },
            { b.Max.x - 2.0f, thumbY + thumbH },
            theme.GetToken("Border", { 0.35f, 0.40f, 0.52f, 0.8f }),
            glm::vec4(2.5f)
        );
    }
}
