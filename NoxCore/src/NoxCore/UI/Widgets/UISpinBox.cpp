#include "UISpinBox.h"

#include <algorithm>

#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UIDragScalar.h"

namespace Nox::UI
{
    UISpinBox::UISpinBox(
        float initialValue,
        float step,
        float minValue,
        float maxValue,
        std::function<void(float)> onChanged)
        : UIStackBox(UIOrientation::Horizontal, 3.0f)
        , Step(step)
        , MinValue(minValue)
        , MaxValue(maxValue)
        , OnChanged(std::move(onChanged))
    {
        auto decBtn = Ref<UIButton>(new UIButton("-", [this]()
        {
            SetValue(GetValue() - Step, true);
        }));

        m_Scalar = Ref<UIDragScalar>(new UIDragScalar(std::clamp(initialValue, MinValue, MaxValue), step * 0.25f));
        m_Scalar->Min = MinValue;
        m_Scalar->Max = MaxValue;
        m_Scalar->WidthMode = UISizeMode::Fill;
        m_Scalar->OnChanged = [this](float v)
        {
            if (OnChanged)
                OnChanged(v);
        };

        auto incBtn = Ref<UIButton>(new UIButton("+", [this]()
        {
            SetValue(GetValue() + Step, true);
        }));

        AddChild(decBtn);
        AddChild(m_Scalar);
        AddChild(incBtn);
    }

    void UISpinBox::SetValue(float value, bool notify)
    {
        if (!m_Scalar)
            return;
        const float clamped = std::clamp(value, MinValue, MaxValue);
        m_Scalar->Value = clamped;
        if (notify && OnChanged)
            OnChanged(clamped);
    }

    float UISpinBox::GetValue() const
    {
        return m_Scalar ? m_Scalar->Value : 0.0f;
    }
}
