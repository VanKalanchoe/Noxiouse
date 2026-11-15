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
        void AddContent(const Ref<UIElement>& child);
        const Ref<UIStackBox>& GetContentBox() const { return m_Content; }

        std::function<void()> OnClose;

        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        static constexpr float kTitleBarHeight = 30.0f;
        static constexpr float kResizeGripSize = 14.0f;

        bool m_DraggingMove = false;
        bool m_DraggingResize = false;
        Ref<UILabel> m_TitleLabel;
        Ref<UIButton> m_CloseBtn;
        Ref<UIStackBox> m_Content;
    };
}
