#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    struct UITreeNode
    {
        uint64_t Id = 0;
        std::string Label;
        bool Expanded = true;
        UIColor TagColor{ 0.24f, 0.56f, 0.96f, 1.0f };
        std::vector<UITreeNode> Children;
    };

    // Virtualized tree view: only paints & hit-tests rows inside the visible scroll window.
    class UITreeView : public UIElement
    {
    public:
        UITreeView();

        void SetNodes(std::vector<UITreeNode> nodes);
        std::vector<UITreeNode>& GetNodes() { return m_Nodes; }

        void SetSelectedId(uint64_t id);
        uint64_t GetSelectedId() const { return m_SelectedId; }

        float RowHeight = 24.0f;
        float IndentWidth = 16.0f;
        float ScrollSpeed = 48.0f;

        std::function<void(uint64_t)> OnSelect;

        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerLeave(const UIPointerEvent& e) override;
        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;
        bool OnMouseWheel(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
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
        void ScrollToPointerY(float y);

        std::vector<UITreeNode> m_Nodes;
        std::vector<FlatRow> m_FlatRows;
        uint64_t m_SelectedId = 0;
        int m_HoveredRow = -1;
        float m_ScrollY = 0.0f;
        bool m_DraggingScrollbar = false;
    };
}
