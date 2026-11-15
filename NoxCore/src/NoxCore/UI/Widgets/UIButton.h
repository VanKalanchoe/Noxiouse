#pragma once

#include <functional>
#include <string>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UILabel;

    class UIButton : public UIElement
    {
    public:
        explicit UIButton(std::string text = {}, std::function<void()> onClick = {});
        ~UIButton() override;

        void SetText(const std::string& text);
        const std::string& GetText() const;

        std::function<void()> OnClick;

        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;

    private:
        Ref<UILabel> m_Label;
    };
}
