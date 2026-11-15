#include "UITreeView.h"

#include <algorithm>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UITreeView::UITreeView()
    {
        StyleClass = "TreeView";
        DrawBackground = true;
        ClipChildren = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
    }

    void UITreeView::SetNodes(std::vector<UITreeNode> nodes)
    {
        m_Nodes = std::move(nodes);
        RebuildFlatRows();
    }

    void UITreeView::SetSelectedId(uint64_t id)
    {
        m_SelectedId = id;
    }

    void UITreeView::RebuildFlatRows()
    {
        m_FlatRows.clear();
        for (auto& node : m_Nodes)
            AppendFlatRows(node, 0);

        const float contentH = static_cast<float>(m_FlatRows.size()) * RowHeight;
        const float maxScroll = std::max(0.0f, contentH - GetBounds().Height());
        m_ScrollY = std::clamp(m_ScrollY, 0.0f, maxScroll);
    }

    void UITreeView::AppendFlatRows(UITreeNode& node, uint32_t depth)
    {
        m_FlatRows.push_back({ &node, depth });
        if (node.Expanded)
        {
            for (auto& child : node.Children)
                AppendFlatRows(child, depth + 1);
        }
    }

    int UITreeView::RowIndexAt(float y) const
    {
        const UIRect& b = GetBounds();
        if (y < b.Min.y || y > b.Max.y)
            return -1;
        const int idx = static_cast<int>((y - b.Min.y + m_ScrollY) / RowHeight);
        if (idx < 0 || idx >= static_cast<int>(m_FlatRows.size()))
            return -1;
        return idx;
    }

    bool UITreeView::OnPointerMove(const UIPointerEvent& e)
    {
        m_HoveredRow = RowIndexAt(e.Position.y);
        return true;
    }

    bool UITreeView::OnPointerLeave(const UIPointerEvent&)
    {
        m_HoveredRow = -1;
        return false;
    }

    bool UITreeView::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return UIElement::OnPointerDown(e);

        const int idx = RowIndexAt(e.Position.y);
        if (idx < 0)
            return false;

        FlatRow& row = m_FlatRows[idx];
        const float indentX = GetBounds().Min.x + 8.0f + static_cast<float>(row.Depth) * IndentWidth;
        if (!row.Node->Children.empty() && e.Position.x >= indentX - 4.0f && e.Position.x <= indentX + 16.0f)
        {
            row.Node->Expanded = !row.Node->Expanded;
            RebuildFlatRows();
            return true;
        }

        m_SelectedId = row.Node->Id;
        if (OnSelect)
            OnSelect(m_SelectedId);
        return true;
    }

    bool UITreeView::OnMouseWheel(const UIPointerEvent& e)
    {
        const float contentH = static_cast<float>(m_FlatRows.size()) * RowHeight;
        const float maxScroll = std::max(0.0f, contentH - GetBounds().Height());
        if (maxScroll <= 0.0f)
            return false;

        m_ScrollY = std::clamp(m_ScrollY - e.ScrollDelta.y * ScrollSpeed, 0.0f, maxScroll);
        return true;
    }

    UIVec2 UITreeView::ComputeDesiredSize()
    {
        return { 220.0f, 180.0f };
    }

    void UITreeView::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIRect& b = GetBounds();
        const UIStyleBox& style = GetResolvedStyle();
        if (m_FlatRows.empty() || b.Height() <= 1.0f)
            return;

        drawList.PushClipRect(b.Min, b.Max, style.CornerRadius.ToVec4());

        const int firstRow = std::max(0, static_cast<int>(m_ScrollY / RowHeight));
        const int lastRow = std::min(static_cast<int>(m_FlatRows.size()),
                                     static_cast<int>((m_ScrollY + b.Height()) / RowHeight) + 1);

        const UIColor accent = theme.GetToken("Accent", { 0.24f, 0.56f, 0.96f, 1.0f });
        const UIColor textMuted = theme.GetToken("TextMuted", { 0.62f, 0.67f, 0.76f, 1.0f });

        for (int i = firstRow; i < lastRow; ++i)
        {
            const FlatRow& row = m_FlatRows[i];
            const float y0 = b.Min.y + static_cast<float>(i) * RowHeight - m_ScrollY;
            const float y1 = y0 + RowHeight;

            if (row.Node->Id == m_SelectedId)
            {
                drawList.AddRoundedRect(
                    { b.Min.x + 3.0f, y0 + 1.0f },
                    { b.Max.x - 10.0f, y1 - 1.0f },
                    { accent.r, accent.g, accent.b, 0.28f },
                    glm::vec4(4.0f),
                    1.0f,
                    { accent.r, accent.g, accent.b, 0.70f }
                );
            }
            else if (i == m_HoveredRow)
            {
                drawList.AddRoundedRect(
                    { b.Min.x + 3.0f, y0 + 1.0f },
                    { b.Max.x - 10.0f, y1 - 1.0f },
                    { 1.0f, 1.0f, 1.0f, 0.06f },
                    glm::vec4(4.0f)
                );
            }

            const float xBase = b.Min.x + 8.0f + static_cast<float>(row.Depth) * IndentWidth;
            const float cy = (y0 + y1) * 0.5f;

            if (!row.Node->Children.empty())
            {
                const char* arrow = row.Node->Expanded ? "v" : ">";
                const UIVec2 aSize = UILabel::MeasureText(arrow, nullptr, 12.0f);
                drawList.AddText(arrow, nullptr, { xBase, cy - aSize.y * 0.5f }, 12.0f, textMuted);
            }

            // Small color tag dot
            drawList.AddRoundedRect(
                { xBase + 16.0f, cy - 3.5f },
                { xBase + 23.0f, cy + 3.5f },
                row.Node->TagColor,
                glm::vec4(3.5f)
            );

            const UIVec2 tSize = UILabel::MeasureText(row.Node->Label, nullptr, style.FontSize);
            drawList.AddText(
                row.Node->Label,
                nullptr,
                { xBase + 30.0f, cy - tSize.y * 0.5f },
                style.FontSize,
                style.TextColor
            );
        }

        // Scrollbar thumb
        const float contentH = static_cast<float>(m_FlatRows.size()) * RowHeight;
        if (contentH > b.Height())
        {
            const float trackH = b.Height() - 8.0f;
            const float thumbH = std::max(20.0f, trackH * (b.Height() / contentH));
            const float scrollT = m_ScrollY / std::max(1.0f, contentH - b.Height());
            const float thumbY = b.Min.y + 4.0f + scrollT * (trackH - thumbH);
            drawList.AddRoundedRect(
                { b.Max.x - 7.0f, thumbY },
                { b.Max.x - 2.0f, thumbY + thumbH },
                theme.GetToken("Border", { 0.35f, 0.40f, 0.52f, 0.8f }),
                glm::vec4(2.5f)
            );
        }

        drawList.PopClipRect();
    }
}
