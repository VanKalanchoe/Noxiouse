#pragma once

#include <functional>
#include <string>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UIDragScalar : public UIElement
    {
    public:
        explicit UIDragScalar(float value = 0.0f, float speed = 0.05f,
                              std::function<void(float)> onChanged = {});

        float Value = 0.0f;
        float Speed = 0.05f;
        float Min = 0.0f;
        float Max = 0.0f; // Min < Max enables clamping
        std::string Prefix;
        UIColor PrefixColor{ 0.24f, 0.56f, 0.96f, 1.0f };
        std::string Format = "{:.2f}";
        std::function<void(float)> OnChanged;

        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        bool m_Dragging = false;
    };
}
