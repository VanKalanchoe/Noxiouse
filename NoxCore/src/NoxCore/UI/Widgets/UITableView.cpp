#include "UITableView.h"

#include <algorithm>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    UITableView::UITableView()
    {
        StyleClass = "Panel";
        DrawBackground = true;
        ClipChildren = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
        WidthMode = UISizeMode::Fill;
        MinSize = { 240.0f, 140.0f };
    }

    void UITableView::SetColumns(std::vector<UITableColumn> columns)
    {
        m_Columns = std::move(columns);
        m_SortColumn = -1;
    }

    void UITableView::SetRows(std::vector<UITableRow> rows)
    {
        m_Rows = std::move(rows);
        m_HoveredRow = -1;
        if (m_SortColumn >= 0)
            SortByColumn(m_SortColumn);
    }

    std::vector<float> UITableView::ComputeColumnWidths(float totalWidth) const
    {
        std::vector<float> widths(m_Columns.size(), 0.0f);
        if (m_Columns.empty())
            return widths;

        float fixedSum = 0.0f;
        float flexSum = 0.0f;
        for (size_t i = 0; i < m_Columns.size(); ++i)
        {
            if (m_Columns[i].FixedWidth > 0.0f)
            {
                widths[i] = m_Columns[i].FixedWidth;
                fixedSum += m_Columns[i].FixedWidth;
            }
            else
            {
                flexSum += std::max(0.01f, m_Columns[i].FlexWeight);
            }
        }

        const float rem = std::max(0.0f, totalWidth - fixedSum);
        if (flexSum > 0.0f)
        {
            for (size_t i = 0; i < m_Columns.size(); ++i)
            {
                if (m_Columns[i].FixedWidth <= 0.0f)
                    widths[i] = rem * (std::max(0.01f, m_Columns[i].FlexWeight) / flexSum);
            }
        }
        return widths;
    }

    void UITableView::SortByColumn(int colIndex)
    {
        if (colIndex < 0 || colIndex >= static_cast<int>(m_Columns.size()))
            return;

        const bool asc = m_SortAscending;
        std::stable_sort(m_Rows.begin(), m_Rows.end(), [colIndex, asc](const UITableRow& a, const UITableRow& b)
        {
            const std::string& sa = (colIndex < static_cast<int>(a.Cells.size())) ? a.Cells[colIndex] : "";
            const std::string& sb = (colIndex < static_cast<int>(b.Cells.size())) ? b.Cells[colIndex] : "";
            return asc ? (sa < sb) : (sa > sb);
        });
    }

    bool UITableView::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return false;

        const UIRect& b = GetBounds();
        if (!b.Contains(e.Position))
            return false;

        // Header click -> sort
        if (e.Position.y <= b.Min.y + HeaderHeight)
        {
            const auto widths = ComputeColumnWidths(b.Width() - 8.0f);
            float x = b.Min.x;
            for (int c = 0; c < static_cast<int>(m_Columns.size()); ++c)
            {
                if (e.Position.x >= x && e.Position.x < x + widths[c])
                {
                    if (m_Columns[c].Sortable)
                    {
                        if (m_SortColumn == c)
                            m_SortAscending = !m_SortAscending;
                        else
                        {
                            m_SortColumn = c;
                            m_SortAscending = true;
                        }
                        SortByColumn(c);
                    }
                    return true;
                }
                x += widths[c];
            }
            return true;
        }

        // Body click -> select row
        const float relY = (e.Position.y - (b.Min.y + HeaderHeight)) + m_ScrollOffset;
        const int rowIdx = static_cast<int>(relY / RowHeight);
        if (rowIdx >= 0 && rowIdx < static_cast<int>(m_Rows.size()))
        {
            m_SelectedId = m_Rows[rowIdx].Id;
            if (OnSelect)
                OnSelect(m_SelectedId, rowIdx);
        }
        return true;
    }

    bool UITableView::OnPointerMove(const UIPointerEvent& e)
    {
        const UIRect& b = GetBounds();
        if (e.Position.y <= b.Min.y + HeaderHeight || !b.Contains(e.Position))
        {
            m_HoveredRow = -1;
            return false;
        }
        const float relY = (e.Position.y - (b.Min.y + HeaderHeight)) + m_ScrollOffset;
        const int rowIdx = static_cast<int>(relY / RowHeight);
        m_HoveredRow = (rowIdx >= 0 && rowIdx < static_cast<int>(m_Rows.size())) ? rowIdx : -1;
        return false;
    }

    bool UITableView::OnPointerLeave(const UIPointerEvent&)
    {
        m_HoveredRow = -1;
        return false;
    }

    bool UITableView::OnMouseWheel(const UIPointerEvent& e)
    {
        const float bodyH = std::max(0.0f, GetBounds().Height() - HeaderHeight);
        const float totalH = static_cast<float>(m_Rows.size()) * RowHeight;
        const float maxScroll = std::max(0.0f, totalH - bodyH);
        m_ScrollOffset = std::clamp(m_ScrollOffset - e.ScrollDelta.y * 36.0f, 0.0f, maxScroll);
        return true;
    }

    UIVec2 UITableView::ComputeDesiredSize()
    {
        return { MinSize.x, MinSize.y };
    }

    void UITableView::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIRect& b = GetBounds();
        const float bodyW = std::max(1.0f, b.Width() - 8.0f);
        const auto widths = ComputeColumnWidths(bodyW);
        const Ref<Font> font = Font::GetDefault();

        // Header bar
        drawList.AddRoundedRect(
            b.Min,
            { b.Max.x, b.Min.y + HeaderHeight },
            { 0.14f, 0.16f, 0.21f, 1.0f }
        );

        float colX = b.Min.x;
        for (int c = 0; c < static_cast<int>(m_Columns.size()); ++c)
        {
            std::string title = m_Columns[c].Title;
            if (m_SortColumn == c)
                title += (m_SortAscending ? " ^" : " v");

            drawList.PushClipRect({ colX, b.Min.y }, { colX + widths[c], b.Min.y + HeaderHeight });
            drawList.AddText(title, font, { colX + 8.0f, b.Min.y + 6.0f }, 13.0f, { 0.86f, 0.90f, 0.96f, 1.0f });
            drawList.PopClipRect();

            if (c > 0)
            {
                drawList.AddRoundedRect(
                    { colX, b.Min.y + 4.0f },
                    { colX + 1.0f, b.Min.y + HeaderHeight - 4.0f },
                    { 0.24f, 0.28f, 0.35f, 0.8f }
                );
            }
            colX += widths[c];
        }

        // Virtualized body rows
        const float bodyTop = b.Min.y + HeaderHeight;
        const float bodyH = std::max(0.0f, b.Max.y - bodyTop);
        const float totalH = static_cast<float>(m_Rows.size()) * RowHeight;
        const float maxScroll = std::max(0.0f, totalH - bodyH);
        m_ScrollOffset = std::clamp(m_ScrollOffset, 0.0f, maxScroll);

        drawList.PushClipRect({ b.Min.x, bodyTop }, b.Max);

        const int firstRow = std::max(0, static_cast<int>(m_ScrollOffset / RowHeight));
        const int lastRow = std::min(
            static_cast<int>(m_Rows.size()) - 1,
            static_cast<int>((m_ScrollOffset + bodyH) / RowHeight) + 1
        );

        for (int r = firstRow; r <= lastRow; ++r)
        {
            const float y0 = bodyTop + static_cast<float>(r) * RowHeight - m_ScrollOffset;
            const float y1 = y0 + RowHeight;
            const bool selected = (m_Rows[r].Id == m_SelectedId);
            const bool hovered = (r == m_HoveredRow);

            if (selected)
                drawList.AddRoundedRect({ b.Min.x, y0 }, { b.Max.x, y1 }, { 0.22f, 0.44f, 0.78f, 0.55f });
            else if (hovered)
                drawList.AddRoundedRect({ b.Min.x, y0 }, { b.Max.x, y1 }, { 0.22f, 0.25f, 0.32f, 0.55f });
            else if ((r % 2) == 1)
                drawList.AddRoundedRect({ b.Min.x, y0 }, { b.Max.x, y1 }, { 0.10f, 0.11f, 0.15f, 0.45f });

            float cx = b.Min.x;
            for (int c = 0; c < static_cast<int>(m_Columns.size()); ++c)
            {
                if (c < static_cast<int>(m_Rows[r].Cells.size()))
                {
                    drawList.PushClipRect({ cx, y0 }, { cx + widths[c], y1 });
                    drawList.AddText(
                        m_Rows[r].Cells[c],
                        font,
                        { cx + 8.0f, y0 + 4.0f },
                        12.5f,
                        selected ? UIColor{ 0.96f, 0.98f, 1.0f, 1.0f } : UIColor{ 0.78f, 0.82f, 0.88f, 1.0f }
                    );
                    drawList.PopClipRect();
                }
                cx += widths[c];
            }
        }

        // Scrollbar thumb
        if (maxScroll > 0.0f && bodyH > 12.0f)
        {
            const float thumbH = std::max(20.0f, (bodyH / totalH) * bodyH);
            const float thumbY = bodyTop + (m_ScrollOffset / maxScroll) * (bodyH - thumbH);
            drawList.AddRoundedRect(
                { b.Max.x - 6.0f, thumbY + 2.0f },
                { b.Max.x - 2.0f, thumbY + thumbH - 2.0f },
                { 0.42f, 0.47f, 0.56f, 0.75f },
                glm::vec4(2.0f)
            );
        }

        drawList.PopClipRect();
    }
}
