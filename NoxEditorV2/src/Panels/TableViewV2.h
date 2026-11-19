#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox
{
    // A table drawn in one element (thousands of rows cost one pass of text, not thousands of widgets): a header, rows filled each
    // frame by a callback, the wheel scrolls, a click on a row reports it (and the column). For live tables (render graph passes and
    // resources, profiler scopes) whose rows change every frame.
    class TableViewV2 final : public UI::UIElement
    {
    public:
        struct Column
        {
            std::string Title;
            float Weight = 1.0f; // share of the width
        };
        struct Row
        {
            std::vector<std::string> Cells;
            uint64_t Id = 0;
            glm::vec4 Color{ 0.86f, 0.88f, 0.92f, 1.0f };
            float Indent = 0.0f;   // of the first cell, in pixels
            bool Highlight = false;
            // A bar in one column (a resource's lifetime): from Begin to End as fractions of the cell.
            int BarColumn = -1;
            float BarBegin = 0.0f, BarEnd = 0.0f;
            glm::vec4 BarColor{ 0.43f, 0.75f, 0.35f, 1.0f };
            std::string Tooltip;
        };

        std::vector<Column> Columns;
        std::function<void(std::vector<Row>&)> Fill;                   // each frame
        std::function<void(const Row& row, int column)> OnRowClick;

        TableViewV2();
        bool OnMouseWheel(const UI::UIPointerEvent& e) override;
        bool OnPointerUp(const UI::UIPointerEvent& e) override;
        bool OnPointerMove(const UI::UIPointerEvent& e) override;

    protected:
        UI::UIVec2 ComputeDesiredSize() override { return { 300.0f, 200.0f }; }
        void OnPaint(UI::UIDrawList& drawList, const UI::UITheme& theme) override;

    private:
        float ColumnX(int column) const;
        int RowAt(float y) const;
        int ColumnAt(float x) const;

        std::vector<Row> m_Rows;
        float m_Scroll = 0.0f;
    };
}
