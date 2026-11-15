#include "UIWrapBox.h"

#include <algorithm>

namespace Nox::UI
{
    UIWrapBox::UIWrapBox(float hSpacing, float vSpacing)
        : HSpacing(hSpacing)
        , VSpacing(vSpacing)
    {
        WidthMode = UISizeMode::Fill;
        ClipChildren = true;
    }

    UIVec2 UIWrapBox::ComputeWrappedSize(float wrapWidth, bool measureChildren)
    {
        float x = 0.0f;
        float y = 0.0f;
        float rowH = 0.0f;
        float maxW = 0.0f;

        for (const auto& child : GetChildren())
        {
            if (!child || !child->Visible)
                continue;

            const UIVec2 sz = measureChildren ? child->Measure() : child->GetDesiredSize();
            if (x > 0.0f && x + sz.x > wrapWidth)
            {
                maxW = std::max(maxW, x - HSpacing);
                x = 0.0f;
                y += rowH + VSpacing;
                rowH = 0.0f;
            }

            x += sz.x + HSpacing;
            rowH = std::max(rowH, sz.y);
        }

        if (x > 0.0f)
            maxW = std::max(maxW, x - HSpacing);

        return {
            maxW + Padding.Horizontal(),
            y + rowH + Padding.Vertical()
        };
    }

    float UIWrapBox::ComputeHeightForWidth(float width)
    {
        const float wrapWidth = std::max(1.0f, width - Padding.Horizontal());
        return ComputeWrappedSize(wrapWidth, false).y;
    }

    UIVec2 UIWrapBox::ComputeDesiredSize()
    {
        float availWidth = GetBounds().Width() > 1.0f
            ? std::max(1.0f, GetBounds().Width() - Padding.Horizontal())
            : 0.0f;
        if (availWidth <= 0.0f)
        {
            for (const UIElement* p = GetParent(); p; p = p->GetParent())
            {
                if (p->GetBounds().Width() > 1.0f)
                {
                    availWidth = std::max(1.0f, p->GetBounds().Width() - p->Padding.Horizontal() - Padding.Horizontal());
                    break;
                }
            }
        }
        const float wrapWidth = availWidth > 0.0f ? availWidth : (MinSize.x > 0.0f ? MinSize.x : 1e6f);
        return ComputeWrappedSize(wrapWidth, true);
    }

    void UIWrapBox::ArrangeChildren(const UIRect& contentRect)
    {
        float x = contentRect.Min.x;
        float y = contentRect.Min.y;
        float rowH = 0.0f;

        for (const auto& child : GetChildren())
        {
            if (!child || !child->Visible)
                continue;

            const UIVec2 sz = child->GetDesiredSize();
            if (x > contentRect.Min.x && x + sz.x > contentRect.Max.x)
            {
                x = contentRect.Min.x;
                y += rowH + VSpacing;
                rowH = 0.0f;
            }

            const float w = std::min(sz.x, std::max(0.0f, contentRect.Max.x - x));
            child->Arrange({ { x, y }, { x + w, y + sz.y } });

            x += sz.x + HSpacing;
            rowH = std::max(rowH, sz.y);
        }
    }
}
