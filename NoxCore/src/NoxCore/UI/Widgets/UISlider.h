#pragma once

#include <functional>
#include <string>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UISlider : public UIElement
    {
    public:
        explicit UISlider(float value = 0.0f, float min = 0.0f, float max = 1.0f,
                          std::function<void(float)> onChanged = {});

        float Value = 0.0f;
        float Min = 0.0f;
        float Max = 1.0f;
        float Step = 0.0f;
        bool ShowValue = true;
        std::string Format = "{:.2f}";
        std::function<void(float)> OnChanged;

        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        void UpdateFromPointerX(float x);
        bool m_Dragging = false;
    };
}
