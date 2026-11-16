#include "UIScrollView.h"

#include <algorithm>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    UIScrollView::UIScrollView()
    {
        StyleClass = "ScrollView";
        ClipChildren = true;
        HitTestMode = UIHitTestMode::Self;
    }

    UIElement* UIScrollView::HitTest(const UIVec2& pos)
    {
        // The scroll bar's lane comes before the children: whatever lies under it (a row's background, a card), a press there
        // is the bar's, and the bar keeps the pointer for the whole drag.
        if (Visible && HitTestMode != UIHitTestMode::None && m_ScrollBar.Contains(pos))
            return this;
        return UIElement::HitTest(pos);
    }

    bool UIScrollView::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button == 1 && m_ScrollBar.OnPointerDown(e.Position, ScrollOffsetY))
        {
            if (UIContext* ctx = UIContext::GetCurrent())
            {
                ctx->SetPointerCapture(Ref<UIElement>(this));
                ctx->CancelPendingDrag();
            }
            return true;
        }
        return UIElement::OnPointerDown(e);
    }

    bool UIScrollView::OnPointerMove(const UIPointerEvent& e)
    {
        if (m_ScrollBar.OnPointerMove(e.Position, ScrollOffsetY))
            return true;
        return UIElement::OnPointerMove(e);
    }

    bool UIScrollView::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == 1 && m_ScrollBar.OnPointerUp())
            return true;
        return UIElement::OnPointerUp(e);
    }

    bool UIScrollView::OnMouseWheel(const UIPointerEvent& e)
    {
        const float maxScroll = std::max(0.0f, m_ContentHeight - m_ViewportHeight);
        if (maxScroll <= 0.0f)
            return false;

        const float next = std::clamp(ScrollOffsetY - e.ScrollDelta.y * ScrollSpeed, 0.0f, maxScroll);
        if (next == ScrollOffsetY)
            return false; // at the end: an outer scroll view takes the wheel
        ScrollOffsetY = next;
        m_ScrollBar.NotifyScrolled();
        return true;
    }

    void UIScrollView::OnUpdate(float dt)
    {
        bool hovered = false;
        if (const UIContext* ctx = UIContext::GetCurrent())
            hovered = ctx->GetHovered() == this && m_ScrollBar.Contains(ctx->GetLastMousePos());
        m_ScrollBar.Update(dt, hovered);
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
        const float rawAvailW = std::max(0.0f, contentRect.Width());

        auto computeTotalH = [&](float width)
        {
            float sum = 0.0f;
            int count = 0;
            for (const auto& child : GetChildren())
            {
                if (!child->Visible)
                    continue;
                const float cw = std::max(0.0f, width - child->Margin.Horizontal());
                sum += child->ComputeHeightForWidth(cw) + child->Margin.Vertical();
                ++count;
            }
            if (count > 1)
                sum += ChildSpacing * static_cast<float>(count - 1);
            return sum;
        };

        // First as if everything fitted; when it does not, the scroll bar's lane leaves the children's width (and the
        // content, now narrower, may have become taller).
        m_ContentHeight = computeTotalH(rawAvailW);
        float availW = rawAvailW;
        if (m_ContentHeight > m_ViewportHeight + 0.5f)
        {
            availW = std::max(0.0f, rawAvailW - UIScrollBar::kLaneWidth);
            m_ContentHeight = computeTotalH(availW);
        }
        const float maxScroll = std::max(0.0f, m_ContentHeight - m_ViewportHeight);
        ScrollOffsetY = std::clamp(ScrollOffsetY, 0.0f, maxScroll);
        m_ScrollBar.SetGeometry(GetBounds(), m_ContentHeight, m_ViewportHeight);

        float cursorY = contentRect.Min.y - ScrollOffsetY;
        for (const auto& child : GetChildren())
        {
            if (!child->Visible)
                continue;

            const float cw = std::max(0.0f, availW - child->Margin.Horizontal());
            const float ch = child->ComputeHeightForWidth(cw);
            const UIRect slot{
                { contentRect.Min.x + child->Margin.Left, cursorY + child->Margin.Top },
                { contentRect.Min.x + availW - child->Margin.Right, cursorY + child->Margin.Top + ch }
            };
            child->Arrange(slot);
            cursorY += ch + child->Margin.Vertical() + ChildSpacing;
        }
    }

    UIRect UIScrollView::GetChildClipRect() const
    {
        UIRect clip = GetBounds();
        clip.Max.x = std::max(clip.Min.x, clip.Max.x - m_ScrollBar.GetReservedWidth());
        return clip;
    }

    void UIScrollView::OnPaintOverlay(UIDrawList& drawList, const UITheme& theme)
    {
        m_ScrollBar.Paint(drawList, theme, ScrollOffsetY);
    }
}
