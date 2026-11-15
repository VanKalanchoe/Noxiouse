#pragma once

#include <functional>
#include <string>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    // Circular rotary dial knob (vertical drag to adjust value, displays active arc & readout).
    class UIRotaryKnob : public UIElement
    {
    public:
        explicit UIRotaryKnob(
            std::string label = {},
            float value = 0.5f,
            float minValue = 0.0f,
            float maxValue = 1.0f,
            std::function<void(float)> onChanged = {});

        void SetValue(float value, bool notify = false);
        float GetValue() const { return m_Value; }

        std::string Label;
        float MinValue = 0.0f;
        float MaxValue = 1.0f;
        std::function<void(float)> OnChanged;

        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        float m_Value = 0.5f;
        bool m_Dragging = false;
    };
}
