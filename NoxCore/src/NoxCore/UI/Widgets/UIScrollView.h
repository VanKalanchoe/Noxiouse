#pragma once

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UIScrollView : public UIElement
    {
    public:
        UIScrollView();

        float ScrollOffsetY = 0.0f;
        float ScrollSpeed = 36.0f;
        float ChildSpacing = 4.0f;

        bool OnMouseWheel(const UIPointerEvent& e) override;
        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        void ScrollToPointerY(float y);

        float m_ContentHeight = 0.0f;
        float m_ViewportHeight = 0.0f;
        bool m_DraggingScrollbar = false;
    };
}
