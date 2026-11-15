#pragma once

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    enum class UIOrientation : uint8_t
    {
        Horizontal = 0,
        Vertical
    };

    // Stacks children horizontally (HBox) or vertically (VBox) with spacing and Fill weights.
    class UIStackBox : public UIElement
    {
    public:
        explicit UIStackBox(UIOrientation orientation = UIOrientation::Vertical, float spacing = 6.0f);

        UIOrientation Orientation = UIOrientation::Vertical;
        float Spacing = 6.0f;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
    };

    // Godot-style 0..1 anchor layout for overlays and responsive game HUDs.
    class UIAnchorBox : public UIElement
    {
    public:
        UIAnchorBox() = default;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
    };

    // Fixed-column grid container.
    class UIGridBox : public UIElement
    {
    public:
        explicit UIGridBox(uint32_t columns = 2, float colSpacing = 6.0f, float rowSpacing = 6.0f);

        uint32_t Columns = 2;
        float ColumnSpacing = 6.0f;
        float RowSpacing = 6.0f;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
    };
}
