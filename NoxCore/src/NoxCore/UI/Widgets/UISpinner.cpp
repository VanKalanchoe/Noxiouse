#include "UISpinner.h"

#include <cmath>

#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    UISpinner::UISpinner(float size, const UIColor& color)
        : Size(size)
        , Color(color)
    {
        DrawBackground = false;
        HitTestMode = UIHitTestMode::None;
    }

    void UISpinner::OnUpdate(float dt)
    {
        m_Phase += dt * Speed;
        if (m_Phase > 6.2831853f)
            m_Phase = std::fmod(m_Phase, 6.2831853f);
    }

    UIVec2 UISpinner::ComputeDesiredSize()
    {
        return { Size, Size };
    }

    void UISpinner::OnPaint(UIDrawList& drawList, const UITheme&)
    {
        const UIRect& b = GetBounds();
        const UIVec2 center = b.Center();
        const float radius = std::min(b.Width(), b.Height()) * 0.36f;
        constexpr int kDots = 8;

        for (int i = 0; i < kDots; ++i)
        {
            const float angle = m_Phase + (static_cast<float>(i) / static_cast<float>(kDots)) * 6.2831853f;
            const float alpha = 0.20f + 0.80f * (static_cast<float>(i) / static_cast<float>(kDots - 1));
            const float dotR = std::max(1.5f, Size * 0.085f * (0.65f + 0.35f * alpha));
            const UIVec2 p{
                center.x + std::cos(angle) * radius,
                center.y + std::sin(angle) * radius
            };
            drawList.AddRoundedRect(
                p - UIVec2{ dotR, dotR },
                p + UIVec2{ dotR, dotR },
                { Color.r, Color.g, Color.b, Color.a * alpha },
                glm::vec4(dotR)
            );
        }
    }
}
