#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    struct UITableColumn
    {
        std::string Title;
        float FixedWidth = 0.0f; // > 0 uses fixed width; 0 uses FlexWeight
        float FlexWeight = 1.0f;
        bool Sortable = true;
    };

    struct UITableRow
    {
        uint64_t Id = 0;
        std::vector<std::string> Cells;
    };

    // Virtualized multi-column data table with sortable headers, row selection, and scrolling.
    class UITableView : public UIElement
    {
    public:
        UITableView();

        void SetColumns(std::vector<UITableColumn> columns);
        void SetRows(std::vector<UITableRow> rows);
        void SetSelectedId(uint64_t id) { m_SelectedId = id; }
        uint64_t GetSelectedId() const { return m_SelectedId; }

        float HeaderHeight = 28.0f;
        float RowHeight = 24.0f;

        std::function<void(uint64_t rowId, int rowIndex)> OnSelect;

        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerLeave(const UIPointerEvent& e) override;
        bool OnMouseWheel(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        std::vector<float> ComputeColumnWidths(float totalWidth) const;
        void SortByColumn(int colIndex);

        std::vector<UITableColumn> m_Columns;
        std::vector<UITableRow> m_Rows;
        uint64_t m_SelectedId = 0;
        int m_HoveredRow = -1;
        int m_SortColumn = -1;
        bool m_SortAscending = true;
        float m_ScrollOffset = 0.0f;
    };
}
