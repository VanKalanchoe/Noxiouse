#pragma once

#include <functional>
#include <string>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UICheckBox : public UIElement
    {
    public:
        explicit UICheckBox(std::string label = {}, bool checked = false, std::function<void(bool)> onChanged = {});

        std::string Label;
        bool Checked = false;
        std::function<void(bool)> OnChanged;

        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;
    };
}
