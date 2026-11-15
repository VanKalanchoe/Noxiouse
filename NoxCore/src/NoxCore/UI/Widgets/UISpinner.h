#pragma once

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    // Animated orbiting-dots activity spinner driven by UIElement::OnUpdate(dt).
    class UISpinner : public UIElement
    {
    public:
        explicit UISpinner(float size = 24.0f, const UIColor& color = { 0.24f, 0.56f, 0.96f, 1.0f });

        float Size = 24.0f;
        float Speed = 4.5f;
        UIColor Color{ 0.24f, 0.56f, 0.96f, 1.0f };

    protected:
        void OnUpdate(float dt) override;
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        float m_Phase = 0.0f;
    };
}
