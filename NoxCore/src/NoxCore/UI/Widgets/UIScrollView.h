#pragma once

#include "NoxCore/UI/Core/UIElement.h"
#include "NoxCore/UI/Widgets/UIScrollBar.h"

namespace Nox::UI
{
    // A vertical list of children that scrolls. The scroll bar's lane at the right edge is never given to the children: they
    // are laid out and clipped beside it, and the lane takes the pointer before any child does.
    class UIScrollView : public UIElement
    {
    public:
        UIScrollView();

        float ScrollOffsetY = 0.0f;
        float ScrollSpeed = 36.0f;
        float ChildSpacing = 4.0f;

        UIElement* HitTest(const UIVec2& pos) override;
        bool OnMouseWheel(const UIPointerEvent& e) override;
        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        void OnUpdate(float dt) override;
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
        void OnPaintOverlay(UIDrawList& drawList, const UITheme& theme) override;
        UIRect GetChildClipRect() const override;

    private:
        UIScrollBar m_ScrollBar;
        float m_ContentHeight = 0.0f;
        float m_ViewportHeight = 0.0f;
    };
}
