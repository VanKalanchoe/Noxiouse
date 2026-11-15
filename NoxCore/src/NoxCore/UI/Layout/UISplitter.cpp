#include "UISplitter.h"

#include <algorithm>

#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    UISplitter::UISplitter(UIOrientation orientation, float splitRatio)
        : Orientation(orientation), SplitRatio(std::clamp(splitRatio, 0.05f, 0.95f))
    {
        StyleClass = "Splitter";
        DrawBackground = false;
        HitTestMode = UIHitTestMode::Self;
    }

    void UISplitter::SetPanes(const Ref<UIElement>& first, const Ref<UIElement>& second)
    {
        ClearChildren();
        m_First = first;
        m_Second = second;
        if (m_First)
            AddChild(m_First);
        if (m_Second)
            AddChild(m_Second);
    }

    UIRect UISplitter::ComputeDividerRect(const UIRect& contentRect) const
    {
        const float total = Orientation == UIOrientation::Horizontal ? contentRect.Width() : contentRect.Height();
        const float usable = std::max(0.0f, total - DividerThickness);
        const float minPx = std::min(MinPaneSize, usable * 0.45f);
        const float firstSize = std::clamp(usable * SplitRatio, minPx, std::max(minPx, usable - minPx));

        if (Orientation == UIOrientation::Horizontal)
        {
            const float x0 = contentRect.Min.x + firstSize;
            return { { x0, contentRect.Min.y }, { x0 + DividerThickness, contentRect.Max.y } };
        }
        const float y0 = contentRect.Min.y + firstSize;
        return { { contentRect.Min.x, y0 }, { contentRect.Max.x, y0 + DividerThickness } };
    }

    bool UISplitter::OnPointerMove(const UIPointerEvent& e)
    {
        const UIRect& b = GetBounds();
        if (m_Dragging)
        {
            const float total = Orientation == UIOrientation::Horizontal ? b.Width() : b.Height();
            const float usable = std::max(1.0f, total - DividerThickness);
            const float pos = Orientation == UIOrientation::Horizontal
                ? (e.Position.x - b.Min.x - DividerThickness * 0.5f)
                : (e.Position.y - b.Min.y - DividerThickness * 0.5f);
            SplitRatio = std::clamp(pos / usable, 0.05f, 0.95f);
            return true;
        }

        m_DividerHovered = ComputeDividerRect(b).Contains(e.Position);
        return m_DividerHovered;
    }

    bool UISplitter::OnPointerLeave(const UIPointerEvent&)
    {
        m_DividerHovered = false;
        return false;
    }

    bool UISplitter::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button == 1 && ComputeDividerRect(GetBounds()).Contains(e.Position))
        {
            m_Dragging = true;
            return true;
        }
        return UIElement::OnPointerDown(e);
    }

    bool UISplitter::OnPointerUp(const UIPointerEvent& e)
    {
        if (m_Dragging && e.Button == 1)
        {
            m_Dragging = false;
            return true;
        }
        return UIElement::OnPointerUp(e);
    }

    UIVec2 UISplitter::ComputeDesiredSize()
    {
        UIVec2 s1 = m_First && m_First->Visible ? m_First->Measure() : UIVec2{ 0.0f, 0.0f };
        UIVec2 s2 = m_Second && m_Second->Visible ? m_Second->Measure() : UIVec2{ 0.0f, 0.0f };

        if (Orientation == UIOrientation::Horizontal)
            return { s1.x + s2.x + DividerThickness, std::max(s1.y, s2.y) };
        return { std::max(s1.x, s2.x), s1.y + s2.y + DividerThickness };
    }

    void UISplitter::ArrangeChildren(const UIRect& contentRect)
    {
        const UIRect div = ComputeDividerRect(contentRect);

        if (Orientation == UIOrientation::Horizontal)
        {
            if (m_First && m_First->Visible)
                m_First->Arrange({ contentRect.Min, { div.Min.x, contentRect.Max.y } });
            if (m_Second && m_Second->Visible)
                m_Second->Arrange({ { div.Max.x, contentRect.Min.y }, contentRect.Max });
        }
        else
        {
            if (m_First && m_First->Visible)
                m_First->Arrange({ contentRect.Min, { contentRect.Max.x, div.Min.y } });
            if (m_Second && m_Second->Visible)
                m_Second->Arrange({ { contentRect.Min.x, div.Max.y }, contentRect.Max });
        }
    }

    void UISplitter::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIRect div = ComputeDividerRect(GetBounds());
        const UIColor color = (m_Dragging || m_DividerHovered)
            ? theme.GetToken("Accent", { 0.24f, 0.56f, 0.96f, 0.90f })
            : UIColor{ 1.0f, 1.0f, 1.0f, 0.05f };

        if (Orientation == UIOrientation::Horizontal)
        {
            const float cx = (div.Min.x + div.Max.x) * 0.5f;
            drawList.AddRoundedRect({ cx - 1.0f, div.Min.y + 6.0f }, { cx + 1.0f, div.Max.y - 6.0f }, color, glm::vec4(1.0f));
        }
        else
        {
            const float cy = (div.Min.y + div.Max.y) * 0.5f;
            drawList.AddRoundedRect({ div.Min.x + 6.0f, cy - 1.0f }, { div.Max.x - 6.0f, cy + 1.0f }, color, glm::vec4(1.0f));
        }
    }
}
