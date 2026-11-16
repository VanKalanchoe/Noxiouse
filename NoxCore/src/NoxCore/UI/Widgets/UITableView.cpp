#include "UITableView.h"

#include <algorithm>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

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

        SyncScrollBar();
        if (m_ScrollBar.OnPointerDown(e.Position, m_ScrollOffset))
        {
            if (UIContext* ctx = UIContext::GetCurrent())
                ctx->CancelPendingDrag();
            return true;
        }

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

    void UITableView::SyncScrollBar()
    {
        const UIRect& b = GetBounds();
        const UIRect body{ { b.Min.x, std::min(b.Max.y, b.Min.y + HeaderHeight) }, b.Max };
        m_ScrollBar.SetGeometry(body, static_cast<float>(m_Rows.size()) * RowHeight, body.Height());
    }

    void UITableView::OnUpdate(float dt)
    {
        SyncScrollBar();
        bool hovered = false;
        if (const UIContext* ctx = UIContext::GetCurrent())
            hovered = ctx->GetHovered() == this && m_ScrollBar.Contains(ctx->GetLastMousePos());
        m_ScrollBar.Update(dt, hovered);
    }

    bool UITableView::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == 1 && m_ScrollBar.OnPointerUp())
            return true;
        return UIElement::OnPointerUp(e);
    }

    bool UITableView::OnPointerMove(const UIPointerEvent& e)
    {
        if (m_ScrollBar.OnPointerMove(e.Position, m_ScrollOffset))
            return true;

        const UIRect& b = GetBounds();
        if (e.Position.y <= b.Min.y + HeaderHeight || !b.Contains(e.Position) || m_ScrollBar.Contains(e.Position))
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
        m_ScrollBar.NotifyScrolled();
        return true;
    }

    UIVec2 UITableView::ComputeDesiredSize()
    {
        const float contentH = HeaderHeight + static_cast<float>(m_Rows.size()) * RowHeight;
        return { std::max(MinSize.x, 160.0f), std::max(MinSize.y, contentH) };
    }

    void UITableView::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIStyleBox& style = GetResolvedStyle();
        const UIRect& b = GetBounds();
        const float bodyW = std::max(1.0f, b.Width() - 8.0f);
        const auto widths = ComputeColumnWidths(bodyW);
        const Ref<Font> font = Font::GetDefault();
        const float headerFontSize = std::max(11.0f, style.FontSize * 0.92f);
        const float rowFontSize = std::max(10.5f, style.FontSize * 0.90f);

        const UIColor headerBg = theme.GetToken("Surface1", { 0.145f, 0.153f, 0.180f, 1.0f });
        const UIColor borderCol = theme.GetToken("Border", { 0.235f, 0.251f, 0.298f, 0.85f });
        const UIColor accentCol = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        const UIColor textCol = theme.GetToken("Text", style.TextColor);
        const UIColor mutedCol = theme.GetToken("TextMuted", { 0.580f, 0.616f, 0.686f, 1.0f });

        // Header bar
        drawList.AddRoundedRect(
            b.Min,
            { b.Max.x, b.Min.y + HeaderHeight },
            headerBg
        );

        float colX = b.Min.x;
        for (int c = 0; c < static_cast<int>(m_Columns.size()); ++c)
        {
            std::string title = m_Columns[c].Title;
            if (m_SortColumn == c)
                title += (m_SortAscending ? " ^" : " v");

            drawList.PushClipRect({ colX, b.Min.y }, { colX + widths[c], b.Min.y + HeaderHeight });
            const UIVec2 hSz = UILabel::MeasureText(title, font, headerFontSize);
            drawList.AddText(title, font, { colX + 8.0f, b.Min.y + (HeaderHeight - hSz.y) * 0.5f }, headerFontSize, textCol);
            drawList.PopClipRect();

            if (c > 0)
            {
                drawList.AddRoundedRect(
                    { colX, b.Min.y + 4.0f },
                    { colX + 1.0f, b.Min.y + HeaderHeight - 4.0f },
                    borderCol
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

        SyncScrollBar();
        drawList.PushClipRect({ b.Min.x, bodyTop }, { b.Max.x - m_ScrollBar.GetReservedWidth(), b.Max.y });

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
                drawList.AddRoundedRect({ b.Min.x, y0 }, { b.Max.x, y1 }, { accentCol.r, accentCol.g, accentCol.b, 0.28f });
            else if (hovered)
                drawList.AddRoundedRect({ b.Min.x, y0 }, { b.Max.x, y1 }, { 1.0f, 1.0f, 1.0f, 0.05f });
            else if ((r % 2) == 1)
                drawList.AddRoundedRect({ b.Min.x, y0 }, { b.Max.x, y1 }, { 0.0f, 0.0f, 0.0f, 0.14f });

            float cx = b.Min.x;
            for (int c = 0; c < static_cast<int>(m_Columns.size()); ++c)
            {
                if (c < static_cast<int>(m_Rows[r].Cells.size()))
                {
                    drawList.PushClipRect({ cx, y0 }, { cx + widths[c], y1 });
                    const UIVec2 cSz = UILabel::MeasureText(m_Rows[r].Cells[c], font, rowFontSize);
                    drawList.AddText(
                        m_Rows[r].Cells[c],
                        font,
                        { cx + 8.0f, y0 + (RowHeight - cSz.y) * 0.5f },
                        rowFontSize,
                        selected ? textCol : mutedCol
                    );
                    drawList.PopClipRect();
                }
                cx += widths[c];
            }
        }

        drawList.PopClipRect();
        m_ScrollBar.Paint(drawList, theme, m_ScrollOffset);
    }
}
