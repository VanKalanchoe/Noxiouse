#include "TableViewV2.h"

#include <algorithm>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox
{
    namespace
    {
        constexpr float kRow = 19.0f;
        constexpr float kText = 12.5f;
    }

    TableViewV2::TableViewV2()
    {
        HitTestMode = UI::UIHitTestMode::Self;
        WidthMode = UI::UISizeMode::Fill;
        HeightMode = UI::UISizeMode::Fill;
    }

    float TableViewV2::ColumnX(int column) const
    {
        float total = 0.0f;
        for (const Column& c : Columns)
            total += c.Weight;
        float before = 0.0f;
        for (int i = 0; i < column && i < static_cast<int>(Columns.size()); ++i)
            before += Columns[static_cast<size_t>(i)].Weight;
        const UI::UIRect b = GetBounds();
        return b.Min.x + b.Width() * (total > 0.0f ? before / total : 0.0f);
    }

    int TableViewV2::ColumnAt(float x) const
    {
        for (int column = 0; column < static_cast<int>(Columns.size()); ++column)
            if (x < ColumnX(column + 1))
                return column;
        return static_cast<int>(Columns.size()) - 1;
    }

    int TableViewV2::RowAt(float y) const
    {
        const int row = static_cast<int>((y - GetBounds().Min.y - kRow + m_Scroll) / kRow);
        return y >= GetBounds().Min.y + kRow && row >= 0 && row < static_cast<int>(m_Rows.size()) ? row : -1;
    }

    bool TableViewV2::OnMouseWheel(const UI::UIPointerEvent& e)
    {
        m_Scroll = std::max(0.0f, m_Scroll - e.ScrollDelta.y * 3.0f * kRow);
        return true;
    }

    bool TableViewV2::OnPointerUp(const UI::UIPointerEvent& e)
    {
        if (e.Button != 1)
            return false;
        if (const int row = RowAt(e.Position.y); row >= 0 && OnRowClick)
        {
            const Row picked = m_Rows[static_cast<size_t>(row)]; // (the callback may change what fills the table)
            OnRowClick(picked, ColumnAt(e.Position.x));
        }
        return true;
    }

    bool TableViewV2::OnPointerMove(const UI::UIPointerEvent& e)
    {
        const int row = RowAt(e.Position.y);
        Tooltip = row >= 0 ? m_Rows[static_cast<size_t>(row)].Tooltip : std::string();
        return true;
    }

    void TableViewV2::OnPaint(UI::UIDrawList& d, const UI::UITheme& theme)
    {
        const UI::UIRect b = GetBounds();
        const UI::UIColor background = theme.GetToken("TableBackground", { 0.10f, 0.10f, 0.11f, 1.0f });
        d.AddRoundedRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Max.y }, { background.r, background.g, background.b, background.a });
        m_Rows.clear();
        if (Fill)
            Fill(m_Rows);
        m_Scroll = std::clamp(m_Scroll, 0.0f, std::max(0.0f, static_cast<float>(m_Rows.size()) * kRow - (b.Height() - kRow)));

        const glm::vec4 dim{ 0.55f, 0.57f, 0.62f, 1.0f };
        d.PushClipRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Max.y });
        d.AddRoundedRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Min.y + kRow }, { 1.0f, 1.0f, 1.0f, 0.05f });
        for (int column = 0; column < static_cast<int>(Columns.size()); ++column)
            d.AddText(Columns[static_cast<size_t>(column)].Title, nullptr, { ColumnX(column) + 4.0f, b.Min.y + 3.0f }, kText, dim);
        d.PushClipRect({ b.Min.x, b.Min.y + kRow }, { b.Max.x, b.Max.y });
        float y = b.Min.y + kRow - m_Scroll;
        for (size_t i = 0; i < m_Rows.size(); ++i, y += kRow)
        {
            if (y + kRow < b.Min.y + kRow || y > b.Max.y)
                continue;
            const Row& row = m_Rows[i];
            if (row.Highlight)
                d.AddRoundedRect({ b.Min.x, y }, { b.Max.x, y + kRow }, { 0.30f, 0.45f, 0.75f, 0.45f });
            else if (i % 2)
                d.AddRoundedRect({ b.Min.x, y }, { b.Max.x, y + kRow }, { 1.0f, 1.0f, 1.0f, 0.03f });
            for (int column = 0; column < static_cast<int>(row.Cells.size()) && column < static_cast<int>(Columns.size()); ++column)
            {
                const float x0 = ColumnX(column), x1 = ColumnX(column + 1);
                if (column == row.BarColumn)
                {
                    const float width = x1 - x0 - 8.0f;
                    d.AddRoundedRect({ x0 + 4.0f, y + 4.0f }, { x1 - 4.0f, y + kRow - 4.0f }, { 0.24f, 0.24f, 0.24f, 0.47f });
                    const float a = x0 + 4.0f + width * row.BarBegin, c = std::max(x0 + 4.0f + width * row.BarEnd, a + 2.0f);
                    d.AddRoundedRect({ a, y + 4.0f }, { c, y + kRow - 4.0f }, row.BarColor);
                    continue;
                }
                d.PushClipRect({ x0, y }, { x1 - 4.0f, y + kRow });
                d.AddText(row.Cells[static_cast<size_t>(column)], nullptr, { x0 + 4.0f + (column == 0 ? row.Indent : 0.0f), y + 3.0f }, kText, row.Color);
                d.PopClipRect();
            }
        }
        d.PopClipRect();
        d.PopClipRect();
    }
}
