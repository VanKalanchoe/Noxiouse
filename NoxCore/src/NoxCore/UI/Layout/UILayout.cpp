#include "UILayout.h"

#include <algorithm>
#include <numeric>
#include <vector>

namespace Nox::UI
{
    UIStackBox::UIStackBox(UIOrientation orientation, float spacing)
        : Orientation(orientation), Spacing(spacing)
    {
    }

    float UIStackBox::ComputeHeightForWidth(float width)
    {
        if (HeightMode == UISizeMode::Fixed)
            return FixedSize.y;

        const float innerW = std::max(0.0f, width - Padding.Horizontal());
        if (Orientation == UIOrientation::Horizontal)
        {
            float fixedW = 0.0f;
            float totalFlex = 0.0f;
            size_t visibleCount = 0;
            for (const auto& child : GetChildren())
            {
                if (!child->Visible)
                    continue;
                if (child->WidthMode == UISizeMode::Fill)
                    totalFlex += std::max(0.0f, child->FlexWeight);
                else
                    fixedW += child->GetDesiredSize().x;
                ++visibleCount;
            }
            const float totalSpacing = visibleCount > 1 ? Spacing * static_cast<float>(visibleCount - 1) : 0.0f;
            const float flexSpace = std::max(0.0f, innerW - fixedW - totalSpacing);
            const float shrinkScale = (fixedW > 0.0f && innerW < fixedW + totalSpacing)
                ? std::max(0.0f, innerW - totalSpacing) / fixedW
                : 1.0f;

            float maxH = 0.0f;
            for (const auto& child : GetChildren())
            {
                if (!child->Visible)
                    continue;
                float slotW = child->GetDesiredSize().x * shrinkScale;
                if (child->WidthMode == UISizeMode::Fill && totalFlex > 0.0f)
                    slotW = flexSpace * (std::max(0.0f, child->FlexWeight) / totalFlex);
                const float cw = std::max(0.0f, slotW - child->Margin.Horizontal());
                maxH = std::max(maxH, child->ComputeHeightForWidth(cw) + child->Margin.Vertical());
            }
            return maxH + Padding.Vertical();
        }

        float totalH = 0.0f;
        size_t visibleCount = 0;
        for (const auto& child : GetChildren())
        {
            if (!child->Visible)
                continue;
            const float cw = std::max(0.0f, innerW - child->Margin.Horizontal());
            totalH += child->ComputeHeightForWidth(cw) + child->Margin.Vertical();
            ++visibleCount;
        }
        if (visibleCount > 1)
            totalH += Spacing * static_cast<float>(visibleCount - 1);
        return totalH + Padding.Vertical();
    }

    UIVec2 UIStackBox::ComputeDesiredSize()
    {
        const bool horiz = (Orientation == UIOrientation::Horizontal);
        float mainSum = 0.0f;
        float crossMax = 0.0f;
        size_t visibleCount = 0;

        for (const auto& child : GetChildren())
        {
            if (!child->Visible)
                continue;
            const UIVec2 d = child->Measure();
            mainSum += horiz ? d.x : d.y;
            crossMax = std::max(crossMax, horiz ? d.y : d.x);
            ++visibleCount;
        }

        if (visibleCount > 1)
            mainSum += Spacing * static_cast<float>(visibleCount - 1);

        UIVec2 content = horiz ? UIVec2{ mainSum, crossMax } : UIVec2{ crossMax, mainSum };
        return { content.x + Padding.Horizontal(), content.y + Padding.Vertical() };
    }

    void UIStackBox::ArrangeChildren(const UIRect& contentRect)
    {
        const bool horiz = (Orientation == UIOrientation::Horizontal);
        const auto& children = GetChildren();

        float fixedMain = 0.0f;
        float totalFlex = 0.0f;
        size_t visibleCount = 0;

        auto computeFixedMain = [&](const Ref<UIElement>& child) -> float
        {
            if (horiz)
                return child->GetDesiredSize().x;
            if (child->HeightMode == UISizeMode::Auto)
            {
                const float cw = std::max(0.0f, contentRect.Width() - child->Margin.Horizontal());
                return child->ComputeHeightForWidth(cw) + child->Margin.Vertical();
            }
            return child->GetDesiredSize().y;
        };

        for (const auto& child : children)
        {
            if (!child->Visible)
                continue;
            const UISizeMode mainMode = horiz ? child->WidthMode : child->HeightMode;
            if (mainMode == UISizeMode::Fill)
                totalFlex += std::max(0.0f, child->FlexWeight);
            else
                fixedMain += computeFixedMain(child);
            ++visibleCount;
        }

        const float totalSpacing = visibleCount > 1 ? Spacing * static_cast<float>(visibleCount - 1) : 0.0f;
        const float availableMain = horiz ? contentRect.Width() : contentRect.Height();
        const float flexSpace = std::max(0.0f, availableMain - fixedMain - totalSpacing);
        const float shrinkScale = (fixedMain > 0.0f && availableMain < fixedMain + totalSpacing)
            ? std::max(0.0f, availableMain - totalSpacing) / fixedMain
            : 1.0f;

        float cursor = horiz ? contentRect.Min.x : contentRect.Min.y;
        for (const auto& child : children)
        {
            if (!child->Visible)
                continue;

            const UISizeMode mainMode = horiz ? child->WidthMode : child->HeightMode;
            const UIVec2 desired = child->GetDesiredSize();
            float childMain = computeFixedMain(child) * shrinkScale;
            if (mainMode == UISizeMode::Fill && totalFlex > 0.0f)
                childMain = flexSpace * (std::max(0.0f, child->FlexWeight) / totalFlex);

            UIRect slot = contentRect;
            if (horiz)
            {
                slot.Min.x = cursor;
                slot.Max.x = cursor + childMain;

                if (child->VAlign != UIAlignment::Stretch && child->HeightMode != UISizeMode::Fill)
                {
                    const float cw = std::max(0.0f, childMain - child->Margin.Horizontal());
                    const float childH = child->ComputeHeightForWidth(cw) + child->Margin.Vertical();
                    const float h = std::min(childH, contentRect.Height());
                    if (child->VAlign == UIAlignment::Center)
                        slot.Min.y = contentRect.Min.y + (contentRect.Height() - h) * 0.5f;
                    else if (child->VAlign == UIAlignment::End)
                        slot.Min.y = contentRect.Max.y - h;
                    slot.Max.y = slot.Min.y + h;
                }
            }
            else
            {
                slot.Min.y = cursor;
                slot.Max.y = cursor + childMain;

                if (child->HAlign != UIAlignment::Stretch && child->WidthMode != UISizeMode::Fill)
                {
                    const float w = std::min(desired.x, contentRect.Width());
                    if (child->HAlign == UIAlignment::Center)
                        slot.Min.x = contentRect.Min.x + (contentRect.Width() - w) * 0.5f;
                    else if (child->HAlign == UIAlignment::End)
                        slot.Min.x = contentRect.Max.x - w;
                    slot.Max.x = slot.Min.x + w;
                }
            }

            child->Arrange(slot);
            cursor += childMain + Spacing;
        }
    }

    UIVec2 UIAnchorBox::ComputeDesiredSize()
    {
        UIVec2 maxSize{ 0.0f, 0.0f };
        for (const auto& child : GetChildren())
        {
            if (child->Visible)
                maxSize = glm::max(maxSize, child->Measure());
        }
        return { maxSize.x + Padding.Horizontal(), maxSize.y + Padding.Vertical() };
    }

    void UIAnchorBox::ArrangeChildren(const UIRect& contentRect)
    {
        const UIVec2 parentSize = contentRect.Size();
        for (const auto& child : GetChildren())
        {
            if (!child->Visible)
                continue;

            const UIVec2 aMin = glm::clamp(child->AnchorMin, UIVec2(0.0f), UIVec2(1.0f));
            const UIVec2 aMax = glm::clamp(child->AnchorMax, aMin, UIVec2(1.0f));

            UIRect rect;
            rect.Min = contentRect.Min + aMin * parentSize + UIVec2(child->AnchorOffsets.Left, child->AnchorOffsets.Top);
            rect.Max = contentRect.Min + aMax * parentSize - UIVec2(child->AnchorOffsets.Right, child->AnchorOffsets.Bottom);

            // If anchored to a single point on an axis, size to desired size from that anchor point.
            const UIVec2 desired = child->GetDesiredSize();
            if (aMin.x == aMax.x)
                rect.Max.x = rect.Min.x + desired.x;
            if (aMin.y == aMax.y)
                rect.Max.y = rect.Min.y + desired.y;

            rect.Max = glm::max(rect.Max, rect.Min);
            child->Arrange(rect);
        }
    }

    UIGridBox::UIGridBox(uint32_t columns, float colSpacing, float rowSpacing)
        : Columns(std::max(1u, columns)), ColumnSpacing(colSpacing), RowSpacing(rowSpacing)
    {
        ClipChildren = true;
    }

    uint32_t UIGridBox::ResolveColumnCount(float availWidth) const
    {
        const uint32_t maxCols = std::max(1u, Columns);
        if (MinColumnWidth <= 0.0f || availWidth <= 0.0f)
            return maxCols;
        const uint32_t fit = static_cast<uint32_t>((availWidth + ColumnSpacing) / (MinColumnWidth + ColumnSpacing));
        return std::clamp(fit, 1u, maxCols);
    }

    float UIGridBox::ComputeHeightForWidth(float width)
    {
        const float contentW = std::max(0.0f, width - Padding.Horizontal());
        const uint32_t cols = ResolveColumnCount(contentW);
        const float totalColSpacing = cols > 1 ? ColumnSpacing * static_cast<float>(cols - 1) : 0.0f;
        const float cellW = std::max(1.0f, (contentW - totalColSpacing) / static_cast<float>(cols));

        std::vector<float> rowHeights;
        uint32_t idx = 0;
        for (const auto& child : GetChildren())
        {
            if (!child->Visible)
                continue;
            const uint32_t row = idx / cols;
            if (row >= rowHeights.size())
                rowHeights.resize(row + 1, 0.0f);
            rowHeights[row] = std::max(rowHeights[row], child->ComputeHeightForWidth(cellW));
            ++idx;
        }
        const float totalH = std::accumulate(rowHeights.begin(), rowHeights.end(), 0.0f)
            + (rowHeights.size() > 1 ? RowSpacing * static_cast<float>(rowHeights.size() - 1) : 0.0f);
        return totalH + Padding.Vertical();
    }

    UIVec2 UIGridBox::ComputeDesiredSize()
    {
        float availW = GetBounds().Width() > 1.0f ? std::max(1.0f, GetBounds().Width() - Padding.Horizontal()) : 0.0f;
        if (availW <= 0.0f)
        {
            for (const UIElement* p = GetParent(); p; p = p->GetParent())
            {
                if (p->GetBounds().Width() > 1.0f)
                {
                    availW = std::max(1.0f, p->GetBounds().Width() - p->Padding.Horizontal() - Padding.Horizontal());
                    break;
                }
            }
        }
        const uint32_t cols = ResolveColumnCount(availW);
        std::vector<float> colWidths(cols, 0.0f);
        std::vector<float> rowHeights;

        uint32_t idx = 0;
        for (const auto& child : GetChildren())
        {
            if (!child->Visible)
                continue;
            const UIVec2 d = child->Measure();
            const uint32_t col = idx % cols;
            const uint32_t row = idx / cols;
            if (row >= rowHeights.size())
                rowHeights.resize(row + 1, 0.0f);
            colWidths[col] = std::max(colWidths[col], d.x);
            rowHeights[row] = std::max(rowHeights[row], d.y);
            ++idx;
        }

        const float totalW = std::accumulate(colWidths.begin(), colWidths.end(), 0.0f)
            + (cols > 1 ? ColumnSpacing * static_cast<float>(cols - 1) : 0.0f);
        const float totalH = std::accumulate(rowHeights.begin(), rowHeights.end(), 0.0f)
            + (rowHeights.size() > 1 ? RowSpacing * static_cast<float>(rowHeights.size() - 1) : 0.0f);

        return { totalW + Padding.Horizontal(), totalH + Padding.Vertical() };
    }

    void UIGridBox::ArrangeChildren(const UIRect& contentRect)
    {
        const uint32_t cols = ResolveColumnCount(contentRect.Width());
        const float totalColSpacing = cols > 1 ? ColumnSpacing * static_cast<float>(cols - 1) : 0.0f;
        const float availW = std::max(0.0f, contentRect.Width() - totalColSpacing);

        std::vector<float> colWidths(cols, 0.0f);
        std::vector<float> rowHeights;
        uint32_t idx = 0;
        for (const auto& child : GetChildren())
        {
            if (!child->Visible)
                continue;
            const uint32_t col = idx % cols;
            const uint32_t row = idx / cols;
            if (row >= rowHeights.size())
                rowHeights.resize(row + 1, 0.0f);
            const UIVec2 d = child->GetDesiredSize();
            colWidths[col] = std::max(colWidths[col], d.x);
            rowHeights[row] = std::max(rowHeights[row], d.y);
            ++idx;
        }

        const float desiredTotalW = std::accumulate(colWidths.begin(), colWidths.end(), 0.0f);
        if (desiredTotalW <= 0.0f || MinColumnWidth > 0.0f)
        {
            const float equalW = availW / static_cast<float>(cols);
            std::fill(colWidths.begin(), colWidths.end(), equalW);
        }
        else if (availW >= desiredTotalW)
        {
            const float extraPerCol = (availW - desiredTotalW) / static_cast<float>(cols);
            for (float& w : colWidths)
                w += extraPerCol;
        }
        else
        {
            const float scale = availW / desiredTotalW;
            for (float& w : colWidths)
                w *= scale;
        }

        // Recompute row heights using actual column widths when responsive
        if (MinColumnWidth > 0.0f)
        {
            std::fill(rowHeights.begin(), rowHeights.end(), 0.0f);
            idx = 0;
            for (const auto& child : GetChildren())
            {
                if (!child->Visible)
                    continue;
                const uint32_t col = idx % cols;
                const uint32_t row = idx / cols;
                rowHeights[row] = std::max(rowHeights[row], child->ComputeHeightForWidth(colWidths[col]));
                ++idx;
            }
        }

        std::vector<float> colX(cols, contentRect.Min.x);
        for (uint32_t c = 1; c < cols; ++c)
            colX[c] = colX[c - 1] + colWidths[c - 1] + ColumnSpacing;

        std::vector<float> rowY(rowHeights.size(), contentRect.Min.y);
        for (size_t r = 1; r < rowHeights.size(); ++r)
            rowY[r] = rowY[r - 1] + rowHeights[r - 1] + RowSpacing;

        idx = 0;
        for (const auto& child : GetChildren())
        {
            if (!child->Visible)
                continue;
            const uint32_t col = idx % cols;
            const uint32_t row = idx / cols;
            child->Arrange({ { colX[col], rowY[row] }, { colX[col] + colWidths[col], rowY[row] + rowHeights[row] } });
            ++idx;
        }
    }
}
