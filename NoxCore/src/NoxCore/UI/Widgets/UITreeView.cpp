#include "UITreeView.h"

#include <algorithm>
#include <cctype>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>

#include "NoxCore/Core/Input.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UITextInput.h"

namespace Nox::UI
{
    namespace
    {
        constexpr float kScrollbarHitW = 14.0f;
        constexpr float kEyeHitW = 24.0f;
    }

    UITreeView::UITreeView()
    {
        StyleClass = "TreeView";
        DrawBackground = true;
        ClipChildren = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;

        m_RenameInput = Ref<UITextInput>(new UITextInput("", ""));
        m_RenameInput->Visible = false;
        m_RenameInput->OnSubmit = [this](const std::string&) { CommitRename(); };
        AddChild(m_RenameInput);

        OnDragStart = [this]() -> std::optional<UIDragPayload>
        {
            const UIContext* ctx = UIContext::GetCurrent();
            const UIVec2 pos = ctx ? ctx->GetLastMousePos() : UIVec2{ 0.0f, 0.0f };
            const int idx = RowIndexAt(pos.y);
            if (idx < 0)
                return std::nullopt;
            const FlatRow& row = m_FlatRows[idx];
            if (!row.Node || !row.Node->Draggable)
                return std::nullopt;
            if (OnBuildDragPayload)
                return OnBuildDragPayload(row.Node->Id);
            return UIDragPayload{ "TREE_NODE", row.Node->Label, row.Node->Id };
        };

        OnDrop = [this](const UIDragPayload& payload) -> bool
        {
            if (payload.Type != "SCENE_HIERARCHY_ENTITY" && payload.Type != "SCENE_HIERARCHY_FOLDER" && payload.Type != "TREE_NODE")
                return false;
            const UIContext* ctx = UIContext::GetCurrent();
            const UIVec2 pos = ctx ? ctx->GetLastMousePos() : UIVec2{ 0.0f, 0.0f };
            const int idx = RowIndexAt(pos.y);
            const uint64_t targetId = (idx >= 0 && m_FlatRows[idx].Node) ? m_FlatRows[idx].Node->Id : 0;
            if (OnDropNode)
                OnDropNode(payload.UserId, targetId);
            return true;
        };
    }

    void UITreeView::SetNodes(std::vector<UITreeNode> nodes)
    {
        m_Nodes = std::move(nodes);
        RebuildFlatRows();
    }

    void UITreeView::SetSelectedId(uint64_t id)
    {
        m_SelectedIds.clear();
        if (id != 0)
            m_SelectedIds.insert(id);
        m_PrimarySelectedId = id;
    }

    void UITreeView::SetSelectedIds(std::unordered_set<uint64_t> ids, uint64_t primaryId)
    {
        m_SelectedIds = std::move(ids);
        if (primaryId != 0 && m_SelectedIds.contains(primaryId))
            m_PrimarySelectedId = primaryId;
        else if (!m_SelectedIds.empty())
            m_PrimarySelectedId = *m_SelectedIds.begin();
        else
            m_PrimarySelectedId = 0;
    }

    std::vector<uint64_t> UITreeView::GetVisibleIdsInOrder() const
    {
        std::vector<uint64_t> ids;
        ids.reserve(m_FlatRows.size());
        for (const FlatRow& row : m_FlatRows)
        {
            if (row.Node)
                ids.push_back(row.Node->Id);
        }
        return ids;
    }

    int UITreeView::FindFlatIndex(uint64_t id) const
    {
        for (int i = 0; i < static_cast<int>(m_FlatRows.size()); ++i)
        {
            if (m_FlatRows[i].Node && m_FlatRows[i].Node->Id == id)
                return i;
        }
        return -1;
    }

    void UITreeView::EnsureVisible(uint64_t id)
    {
        const int idx = FindFlatIndex(id);
        if (idx < 0)
            return;
        const float viewH = GetBounds().Height();
        if (viewH <= 1.0f)
            return;
        const float rowTop = static_cast<float>(idx) * RowHeight;
        const float rowBottom = rowTop + RowHeight;
        if (rowTop < m_ScrollY)
            m_ScrollY = rowTop;
        else if (rowBottom > m_ScrollY + viewH)
            m_ScrollY = rowBottom - viewH;
    }

    void UITreeView::BeginRename(uint64_t id, const std::string& initialText)
    {
        const int idx = FindFlatIndex(id);
        if (idx < 0 || !m_FlatRows[idx].Node)
            return;
        EnsureVisible(id);
        m_RenameId = id;
        m_RenameInput->Text = initialText.empty() ? m_FlatRows[idx].Node->Label : initialText;
        m_RenameInput->Visible = true;
        if (UIContext* ctx = UIContext::GetCurrent())
            ctx->SetFocus(m_RenameInput.get());
    }

    void UITreeView::CancelRename()
    {
        if (m_RenameId == 0)
            return;
        m_RenameId = 0;
        m_RenameInput->Visible = false;
        if (UIContext* ctx = UIContext::GetCurrent())
            ctx->SetFocus(this);
    }

    void UITreeView::CommitRename()
    {
        if (m_RenameId == 0)
            return;
        const uint64_t id = m_RenameId;
        const std::string newName = m_RenameInput->Text;
        CancelRename();
        if (OnRename && !newName.empty())
            OnRename(id, newName);
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

    bool UITreeView::HitTestEyeButton(const FlatRow& row, const UIVec2& pos) const
    {
        if (!row.Node || !row.Node->HasVisibilityToggle)
            return false;
        const UIRect& b = GetBounds();
        const float rightEdge = b.Max.x - 10.0f;
        return pos.x >= rightEdge - kEyeHitW && pos.x <= rightEdge;
    }

    void UITreeView::ScrollToPointerY(float y)
    {
        const UIRect& b = GetBounds();
        const float contentH = static_cast<float>(m_FlatRows.size()) * RowHeight;
        const float maxScroll = std::max(0.0f, contentH - b.Height());
        if (maxScroll <= 0.0f)
            return;

        const float trackTop = b.Min.y + 4.0f;
        const float trackH = std::max(1.0f, b.Height() - 8.0f);
        const float thumbH = std::max(20.0f, trackH * (b.Height() / contentH));
        const float usable = std::max(1.0f, trackH - thumbH);
        const float t = std::clamp((y - trackTop - thumbH * 0.5f) / usable, 0.0f, 1.0f);
        m_ScrollY = t * maxScroll;
    }

    bool UITreeView::OnPointerMove(const UIPointerEvent& e)
    {
        if (m_DraggingScrollbar)
        {
            ScrollToPointerY(e.Position.y);
            return true;
        }
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
        if (!IsEnabled())
            return UIElement::OnPointerDown(e);

        if (m_RenameId != 0 && !m_RenameInput->GetBounds().Contains(e.Position))
            CommitRename();

        if (e.Button == 1)
        {
            const float contentH = static_cast<float>(m_FlatRows.size()) * RowHeight;
            if (contentH > GetBounds().Height() && e.Position.x >= GetBounds().Max.x - kScrollbarHitW)
            {
                m_DraggingScrollbar = true;
                ScrollToPointerY(e.Position.y);
                return true;
            }
        }

        const int idx = RowIndexAt(e.Position.y);
        if (idx < 0)
        {
            if (e.Button == 1)
            {
                m_SelectedIds.clear();
                m_PrimarySelectedId = 0;
                if (OnClearSelection)
                    OnClearSelection();
                return true;
            }
            if (e.Button == 3 && OnContextMenu)
            {
                OnContextMenu(0, e.Position);
                return true;
            }
            return false;
        }

        FlatRow& row = m_FlatRows[idx];
        const uint64_t nodeId = row.Node->Id;

        if (e.Button == 1)
        {
            if (HitTestEyeButton(row, e.Position))
            {
                if (OnToggleVisibility)
                    OnToggleVisibility(nodeId, !row.Node->IsVisible);
                return true;
            }

            const float indentX = GetBounds().Min.x + 8.0f + static_cast<float>(row.Depth) * IndentWidth;
            if (!row.Node->Children.empty() && e.Position.x >= indentX - 4.0f && e.Position.x <= indentX + 16.0f)
            {
                row.Node->Expanded = !row.Node->Expanded;
                const bool expanded = row.Node->Expanded;
                RebuildFlatRows();
                if (OnToggleExpand)
                    OnToggleExpand(nodeId, expanded);
                return true;
            }

            const auto now = std::chrono::steady_clock::now();
            const bool isDoubleClick = (nodeId == m_LastClickId) &&
                                       (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastClickTime).count() < 350);
            m_LastClickId = nodeId;
            m_LastClickTime = now;

            if (isDoubleClick && OnDoubleClick)
                OnDoubleClick(nodeId);

            const bool ctrl = Input::IsKeyPressed(SDL_SCANCODE_LCTRL) || Input::IsKeyPressed(SDL_SCANCODE_RCTRL);
            const bool shift = Input::IsKeyPressed(SDL_SCANCODE_LSHIFT) || Input::IsKeyPressed(SDL_SCANCODE_RSHIFT);

            if (!ctrl && !shift && m_SelectedIds.contains(nodeId) && m_SelectedIds.size() > 1)
            {
                // Preserve multi-selection on press so dragging a multi-selection works; narrow on clean click release.
                m_PendingSingleSelectId = nodeId;
            }
            else if (OnRowClick)
            {
                OnRowClick(nodeId, ctrl, shift);
            }
            else
            {
                SetSelectedId(nodeId);
                if (OnSelect)
                    OnSelect(nodeId);
            }
            return true;
        }

        if (e.Button == 3)
        {
            if (OnContextMenu)
                OnContextMenu(nodeId, e.Position);
            return true;
        }

        return false;
    }

    bool UITreeView::OnPointerUp(const UIPointerEvent& e)
    {
        if (m_DraggingScrollbar && e.Button == 1)
        {
            m_DraggingScrollbar = false;
            return true;
        }
        if (e.Button == 1 && m_PendingSingleSelectId != 0)
        {
            const uint64_t pending = m_PendingSingleSelectId;
            m_PendingSingleSelectId = 0;
            const int idx = RowIndexAt(e.Position.y);
            if (idx >= 0 && m_FlatRows[idx].Node && m_FlatRows[idx].Node->Id == pending)
            {
                if (OnRowClick)
                    OnRowClick(pending, false, false);
                else
                {
                    SetSelectedId(pending);
                    if (OnSelect)
                        OnSelect(pending);
                }
            }
            return true;
        }
        return UIElement::OnPointerUp(e);
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

    bool UITreeView::OnKeyDown(const UIKeyEvent& e)
    {
        if (m_RenameId != 0)
        {
            if (e.KeyCode == SDL_SCANCODE_ESCAPE)
            {
                CancelRename();
                return true;
            }
            return false;
        }

        if (m_FlatRows.empty())
            return false;

        if (e.KeyCode == SDL_SCANCODE_UP || e.KeyCode == SDL_SCANCODE_DOWN)
        {
            int cur = FindFlatIndex(m_PrimarySelectedId);
            if (cur < 0)
                cur = 0;
            else
                cur = std::clamp(cur + (e.KeyCode == SDL_SCANCODE_UP ? -1 : 1), 0, static_cast<int>(m_FlatRows.size()) - 1);
            const uint64_t nextId = m_FlatRows[cur].Node->Id;
            const bool shift = (e.Modifiers & SDL_KMOD_SHIFT) != 0;
            if (OnRowClick)
                OnRowClick(nextId, false, shift);
            else
            {
                SetSelectedId(nextId);
                if (OnSelect)
                    OnSelect(nextId);
            }
            EnsureVisible(nextId);
            return true;
        }

        if (e.KeyCode == SDL_SCANCODE_RIGHT || e.KeyCode == SDL_SCANCODE_LEFT)
        {
            const int cur = FindFlatIndex(m_PrimarySelectedId);
            if (cur < 0 || !m_FlatRows[cur].Node)
                return false;
            UITreeNode* node = m_FlatRows[cur].Node;
            const bool wantExpand = (e.KeyCode == SDL_SCANCODE_RIGHT);
            if (!node->Children.empty() && node->Expanded != wantExpand)
            {
                node->Expanded = wantExpand;
                const uint64_t id = node->Id;
                RebuildFlatRows();
                if (OnToggleExpand)
                    OnToggleExpand(id, wantExpand);
                return true;
            }
            return false;
        }

        if (e.KeyCode == SDL_SCANCODE_F2 && !e.Repeat && m_PrimarySelectedId != 0)
        {
            BeginRename(m_PrimarySelectedId);
            return true;
        }

        if (e.KeyCode == SDL_SCANCODE_DELETE && !e.Repeat && OnDeleteRequested)
        {
            OnDeleteRequested();
            return true;
        }

        if (e.KeyCode == SDL_SCANCODE_F && !e.Repeat && OnFocusRequested)
        {
            OnFocusRequested();
            return true;
        }

        return false;
    }

    UIVec2 UITreeView::ComputeDesiredSize()
    {
        return { std::max(MinSize.x, 180.0f), std::max(MinSize.y, RowHeight * 4.0f) };
    }

    void UITreeView::ArrangeChildren(const UIRect& contentRect)
    {
        if (m_RenameId == 0 || !m_RenameInput->Visible)
            return;

        const int idx = FindFlatIndex(m_RenameId);
        if (idx < 0)
        {
            CancelRename();
            return;
        }

        const float y0 = contentRect.Min.y + static_cast<float>(idx) * RowHeight - m_ScrollY;
        const float x0 = contentRect.Min.x + 8.0f + static_cast<float>(m_FlatRows[idx].Depth) * IndentWidth + 28.0f;
        const float x1 = std::max(x0 + 40.0f, contentRect.Max.x - 36.0f);
        m_RenameInput->Measure();
        m_RenameInput->Arrange({ { x0, y0 + 1.0f }, { x1, y0 + RowHeight - 1.0f } });
    }

    void UITreeView::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIRect& b = GetBounds();
        const UIStyleBox& style = GetResolvedStyle();
        if (b.Height() <= 1.0f)
            return;

        drawList.PushClipRect(b.Min, b.Max, style.CornerRadius.ToVec4());

        const UIColor accent = theme.GetToken("Accent", { 0.24f, 0.56f, 0.96f, 1.0f });
        const UIColor textMuted = theme.GetToken("TextMuted", { 0.62f, 0.67f, 0.76f, 1.0f });

        if (m_FlatRows.empty())
        {
            if (!EmptyText.empty())
            {
                const UIVec2 msgSize = UILabel::MeasureText(EmptyText, nullptr, style.FontSize);
                drawList.AddText(
                    EmptyText,
                    nullptr,
                    { b.Min.x + (b.Width() - msgSize.x) * 0.5f, b.Min.y + std::min(40.0f, b.Height() * 0.25f) },
                    style.FontSize,
                    textMuted
                );
            }
            drawList.PopClipRect();
            return;
        }

        const UIContext* ctx = UIContext::GetCurrent();
        int dragTargetRow = -1;
        if (ctx && ctx->IsDragging() && b.Contains(ctx->GetLastMousePos()))
        {
            const std::string& type = ctx->GetDragPayload().Type;
            if (type == "SCENE_HIERARCHY_ENTITY" || type == "SCENE_HIERARCHY_FOLDER" || type == "TREE_NODE")
                dragTargetRow = RowIndexAt(ctx->GetLastMousePos().y);
        }

        const int firstRow = std::max(0, static_cast<int>(m_ScrollY / RowHeight));
        const int lastRow = std::min(static_cast<int>(m_FlatRows.size()),
                                     static_cast<int>((m_ScrollY + b.Height()) / RowHeight) + 1);

        const float rightContentEnd = b.Max.x - 10.0f;

        for (int i = firstRow; i < lastRow; ++i)
        {
            const FlatRow& row = m_FlatRows[i];
            if (!row.Node)
                continue;
            const float y0 = b.Min.y + static_cast<float>(i) * RowHeight - m_ScrollY;
            const float y1 = y0 + RowHeight;

            const bool selected = m_SelectedIds.contains(row.Node->Id);
            if (i == dragTargetRow)
            {
                drawList.AddRoundedRect(
                    { b.Min.x + 3.0f, y0 + 1.0f },
                    { rightContentEnd, y1 - 1.0f },
                    { accent.r, accent.g, accent.b, 0.22f },
                    glm::vec4(4.0f),
                    1.5f,
                    accent
                );
            }
            else if (selected)
            {
                drawList.AddRoundedRect(
                    { b.Min.x + 3.0f, y0 + 1.0f },
                    { rightContentEnd, y1 - 1.0f },
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
                    { rightContentEnd, y1 - 1.0f },
                    { 1.0f, 1.0f, 1.0f, 0.06f },
                    glm::vec4(4.0f)
                );
            }

            const float alphaMult = row.Node->IsDimmed ? 0.5f : 1.0f;
            const float xBase = b.Min.x + 8.0f + static_cast<float>(row.Depth) * IndentWidth;
            const float cy = (y0 + y1) * 0.5f;

            if (!row.Node->Children.empty())
            {
                const char* arrow = row.Node->Expanded ? "v" : ">";
                const float arrowSize = std::max(10.0f, style.FontSize * 0.85f);
                const UIVec2 aSize = UILabel::MeasureText(arrow, nullptr, arrowSize);
                drawList.AddText(arrow, nullptr, { xBase, cy - aSize.y * 0.5f }, arrowSize, textMuted);
            }

            if (row.Node->IsFolder)
            {
                // Folder icon (clean tabbed folder shape)
                drawList.AddRoundedRect(
                    { xBase + 15.0f, cy - 5.5f },
                    { xBase + 22.0f, cy - 3.0f },
                    { row.Node->TagColor.r, row.Node->TagColor.g, row.Node->TagColor.b, 0.9f * alphaMult },
                    glm::vec4(1.5f)
                );
                drawList.AddRoundedRect(
                    { xBase + 15.0f, cy - 3.5f },
                    { xBase + 27.0f, cy + 5.0f },
                    { row.Node->TagColor.r, row.Node->TagColor.g, row.Node->TagColor.b, alphaMult },
                    glm::vec4(2.0f)
                );
            }
            else
            {
                drawList.AddRoundedRect(
                    { xBase + 16.0f, cy - 3.5f },
                    { xBase + 23.0f, cy + 3.5f },
                    { row.Node->TagColor.r, row.Node->TagColor.g, row.Node->TagColor.b, row.Node->TagColor.a * alphaMult },
                    glm::vec4(3.5f)
                );
            }

            // Reserve space on the right for eye button and optional badge
            float labelRightLimit = rightContentEnd - 6.0f;
            if (row.Node->HasVisibilityToggle)
            {
                const float eyeCx = rightContentEnd - kEyeHitW * 0.5f;
                const bool eyeHover = (i == m_HoveredRow);
                const float eyeAlpha = row.Node->IsVisible
                    ? (row.Node->IsVisibleInHierarchy ? (eyeHover ? 0.95f : 0.72f) : 0.35f)
                    : (eyeHover ? 0.85f : 0.40f);
                const UIColor eyeColor = eyeHover ? style.TextColor : textMuted;
                if (row.Node->IsVisible)
                {
                    drawList.AddRoundedRect(
                        { eyeCx - 6.0f, cy - 3.5f },
                        { eyeCx + 6.0f, cy + 3.5f },
                        { 0.0f, 0.0f, 0.0f, 0.0f },
                        glm::vec4(4.0f),
                        1.4f,
                        { eyeColor.r, eyeColor.g, eyeColor.b, eyeAlpha }
                    );
                    drawList.AddRoundedRect(
                        { eyeCx - 2.0f, cy - 2.0f },
                        { eyeCx + 2.0f, cy + 2.0f },
                        { eyeColor.r, eyeColor.g, eyeColor.b, eyeAlpha },
                        glm::vec4(2.0f)
                    );
                }
                else
                {
                    drawList.AddRoundedRect(
                        { eyeCx - 5.5f, cy - 1.0f },
                        { eyeCx + 5.5f, cy + 1.0f },
                        { eyeColor.r, eyeColor.g, eyeColor.b, eyeAlpha },
                        glm::vec4(1.0f)
                    );
                }
                labelRightLimit -= kEyeHitW;
            }

            if (!row.Node->Badge.empty())
            {
                const float badgeFontSize = std::max(11.0f, style.FontSize * 0.80f);
                const UIVec2 bSize = UILabel::MeasureText(row.Node->Badge, nullptr, badgeFontSize);
                const float bx0 = labelRightLimit - bSize.x - 10.0f;
                if (bx0 > xBase + 60.0f)
                {
                    drawList.AddRoundedRect(
                        { bx0, cy - bSize.y * 0.5f - 1.0f },
                        { labelRightLimit - 2.0f, cy + bSize.y * 0.5f + 1.0f },
                        { 1.0f, 1.0f, 1.0f, 0.07f },
                        glm::vec4(3.0f)
                    );
                    drawList.AddText(
                        row.Node->Badge,
                        nullptr,
                        { bx0 + 4.0f, cy - bSize.y * 0.5f },
                        badgeFontSize,
                        { textMuted.r, textMuted.g, textMuted.b, 0.9f * alphaMult }
                    );
                    labelRightLimit = bx0 - 6.0f;
                }
            }

            if (row.Node->Id != m_RenameId)
            {
                const float textStartX = xBase + (row.Node->IsFolder ? 33.0f : 30.0f);
                if (labelRightLimit > textStartX + 4.0f)
                {
                    drawList.PushClipRect({ textStartX, y0 }, { labelRightLimit, y1 });
                    UIColor textColor = row.Node->LabelColor.a > 0.0f ? row.Node->LabelColor : style.TextColor;
                    textColor.a *= alphaMult;
                    const UIVec2 tSize = UILabel::MeasureText(row.Node->Label, nullptr, style.FontSize);
                    drawList.AddText(
                        row.Node->Label,
                        nullptr,
                        { textStartX, cy - tSize.y * 0.5f },
                        style.FontSize,
                        textColor
                    );
                    drawList.PopClipRect();
                }
            }
        }

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
