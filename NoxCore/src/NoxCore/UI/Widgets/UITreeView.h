#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UITextInput;

    struct UITreeNode
    {
        uint64_t Id = 0;
        std::string Label;
        bool Expanded = true;
        UIColor TagColor{ 0.24f, 0.56f, 0.96f, 1.0f };
        std::vector<UITreeNode> Children;

        std::string Badge;
        bool IsFolder = false;
        bool HasVisibilityToggle = false;
        bool IsVisible = true;
        bool IsVisibleInHierarchy = true;
        bool IsDimmed = false;
        bool Draggable = true;
        UIColor LabelColor{ 0.0f, 0.0f, 0.0f, 0.0f }; // alpha > 0 overrides theme text color
    };

    // Virtualized tree view: only paints & hit-tests rows inside the visible scroll window.
    class UITreeView : public UIElement
    {
    public:
        UITreeView();

        void SetNodes(std::vector<UITreeNode> nodes);
        std::vector<UITreeNode>& GetNodes() { return m_Nodes; }

        void SetSelectedId(uint64_t id);
        uint64_t GetSelectedId() const { return m_PrimarySelectedId; }

        void SetSelectedIds(std::unordered_set<uint64_t> ids, uint64_t primaryId = 0);
        const std::unordered_set<uint64_t>& GetSelectedIds() const { return m_SelectedIds; }

        std::vector<uint64_t> GetVisibleIdsInOrder() const;
        void BeginRename(uint64_t id, const std::string& initialText = {});
        void CancelRename();
        bool IsRenaming() const { return m_RenameId != 0; }
        void EnsureVisible(uint64_t id);

        float RowHeight = 24.0f;
        float IndentWidth = 16.0f;
        float ScrollSpeed = 48.0f;
        std::string EmptyText = "No items";

        std::function<void(uint64_t)> OnSelect;
        std::function<void(uint64_t id, bool ctrl, bool shift)> OnRowClick;
        std::function<void()> OnClearSelection;
        std::function<void(uint64_t)> OnDoubleClick;
        std::function<void(uint64_t id, bool expanded)> OnToggleExpand;
        std::function<void(uint64_t id, bool visible)> OnToggleVisibility;
        std::function<void(uint64_t id, const UIVec2& screenPos)> OnContextMenu;
        std::function<void(uint64_t id, const std::string& newName)> OnRename;
        std::function<void(uint64_t draggedId, uint64_t targetId)> OnDropNode;
        std::function<void()> OnDeleteRequested;
        std::function<void()> OnFocusRequested;
        std::function<std::optional<UIDragPayload>(uint64_t draggedId)> OnBuildDragPayload;

        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerLeave(const UIPointerEvent& e) override;
        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;
        bool OnMouseWheel(const UIPointerEvent& e) override;
        bool OnKeyDown(const UIKeyEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        struct FlatRow
        {
            UITreeNode* Node = nullptr;
            uint32_t Depth = 0;
        };

        void RebuildFlatRows();
        void AppendFlatRows(UITreeNode& node, uint32_t depth);
        int RowIndexAt(float y) const;
        int FindFlatIndex(uint64_t id) const;
        void ScrollToPointerY(float y);
        void CommitRename();
        bool HitTestEyeButton(const FlatRow& row, const UIVec2& pos) const;

        std::vector<UITreeNode> m_Nodes;
        std::vector<FlatRow> m_FlatRows;
        std::unordered_set<uint64_t> m_SelectedIds;
        uint64_t m_PrimarySelectedId = 0;

        int m_HoveredRow = -1;
        float m_ScrollY = 0.0f;
        bool m_DraggingScrollbar = false;

        uint64_t m_PendingSingleSelectId = 0;
        uint64_t m_LastClickId = 0;
        std::chrono::steady_clock::time_point m_LastClickTime{};

        uint64_t m_RenameId = 0;
        Ref<UITextInput> m_RenameInput;
    };
}
