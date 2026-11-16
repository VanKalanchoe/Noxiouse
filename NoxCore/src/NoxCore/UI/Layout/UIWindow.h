#pragma once

#include <functional>
#include <string>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UIButton;
    class UILabel;
    class UIStackBox;

    // Draggable, resizable floating window designed for use inside a UIAnchorBox or overlay.
    class UIWindow : public UIElement
    {
    public:
        explicit UIWindow(std::string title = "Window", std::function<void()> onClose = {});

        void SetTitle(const std::string& title);
        const std::string& GetTitle() const;
        void AddContent(const Ref<UIElement>& child);
        const Ref<UIStackBox>& GetContentBox() const { return m_Content; }

        void BeginProgrammaticDrag();
        bool IsDraggingMove() const { return m_DraggingMove; }

        std::function<void()> OnClose;
        std::function<void()> OnActivate;
        std::function<void(UIVec2 pointerPos)> OnDragMove;
        std::function<void(UIVec2 pointerPos)> OnDragEnd;

        UIElement* HitTest(const UIVec2& pos) override;
        UICursorShape GetCursorShape(const UIVec2& pos) const override;
        bool OnPointerLeave(const UIPointerEvent& e) override;
        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        uint8_t HitTestResizeEdge(const UIVec2& pos) const;

        enum ResizeEdge : uint8_t
        {
            EdgeNone   = 0,
            EdgeLeft   = 1 << 0,
            EdgeRight  = 1 << 1,
            EdgeBottom = 1 << 2
        };

        static constexpr float kTitleBarHeight = 30.0f;
        static constexpr float kResizeEdgeThickness = 6.0f;
        static constexpr float kResizeCornerExtent = 12.0f;
        float m_TitleBarHeight = kTitleBarHeight;

        bool m_DraggingMove = false;
        bool m_DraggingResize = false;
        uint8_t m_ResizeEdges = EdgeNone;
        uint8_t m_HoverResizeEdges = EdgeNone;
        Ref<UILabel> m_TitleLabel;
        Ref<UIButton> m_CloseBtn;
        Ref<UIStackBox> m_Content;
    };
}
