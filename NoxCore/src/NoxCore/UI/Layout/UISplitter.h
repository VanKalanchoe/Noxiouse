#pragma once

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    // Resizable 2-pane split container (horizontal or vertical).
    class UISplitter : public UIElement
    {
    public:
        explicit UISplitter(UIOrientation orientation = UIOrientation::Horizontal, float splitRatio = 0.5f);

        void SetPanes(const Ref<UIElement>& first, const Ref<UIElement>& second);

        UIOrientation Orientation = UIOrientation::Horizontal;
        float SplitRatio = 0.5f;
        float DividerThickness = 6.0f;
        float MinPaneSize = 120.0f;

        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerLeave(const UIPointerEvent& e) override;
        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        UIRect ComputeDividerRect(const UIRect& contentRect) const;

        Ref<UIElement> m_First;
        Ref<UIElement> m_Second;
        bool m_Dragging = false;
        bool m_DividerHovered = false;
    };
}
