#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    // Scrollable multi-line text editor with optional line-number gutter and keyboard navigation.
    class UITextArea : public UIElement
    {
    public:
        explicit UITextArea(std::string text = {}, std::string placeholder = {});

        std::string Text;
        std::string Placeholder;
        bool ShowLineNumbers = true;

        std::function<void(const std::string&)> OnChanged;

        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnMouseWheel(const UIPointerEvent& e) override;
        bool OnKeyDown(const UIKeyEvent& e) override;
        bool OnTextInput(std::string_view text) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        std::vector<std::pair<size_t, size_t>> SplitLines() const;
        void CursorToLineCol(const std::vector<std::pair<size_t, size_t>>& lines, size_t& outLine, size_t& outCol) const;

        size_t m_Cursor = 0;
        float m_ScrollY = 0.0f;
    };
}
