#include "UIToggleSwitch.h"

#include <algorithm>
#include <cmath>

#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIToggleSwitch::UIToggleSwitch(
        std::string label,
        bool checked,
        std::function<void(bool)> onToggled)
        : OnToggled(std::move(onToggled))
        , m_Checked(checked)
        , m_AnimT(checked ? 1.0f : 0.0f)
    {
        StyleClass = "CheckBox";
        DrawBackground = false;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
        Padding = UIThickness::Symmetric(2.0f, 2.0f);
        MinSize = { 44.0f, 24.0f };

        if (!label.empty())
        {
            m_Label = Ref<UILabel>(new UILabel(std::move(label), 13.5f));
            m_Label->HitTestMode = UIHitTestMode::None;
            m_Label->VAlign = UIAlignment::Center;
            AddChild(m_Label);
        }
    }

    void UIToggleSwitch::SetChecked(bool checked, bool notify)
    {
        if (m_Checked == checked)
            return;
        m_Checked = checked;
        if (notify && OnToggled)
            OnToggled(m_Checked);
    }

    bool UIToggleSwitch::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == 1 && IsEnabled() && GetBounds().Contains(e.Position))
        {
            SetChecked(!m_Checked, true);
            return true;
        }
        return false;
    }

    void UIToggleSwitch::OnUpdate(float dt)
    {
        const float target = m_Checked ? 1.0f : 0.0f;
        const float alpha = 1.0f - std::exp(-dt * 18.0f);
        m_AnimT += (target - m_AnimT) * alpha;
        if (std::abs(target - m_AnimT) < 0.002f)
            m_AnimT = target;
    }

    UIVec2 UIToggleSwitch::ComputeDesiredSize()
    {
        const float trackW = 40.0f;
        const float trackH = 20.0f;
        if (!m_Label)
            return { trackW + Padding.Horizontal(), std::max(MinSize.y, trackH + Padding.Vertical()) };

        const UIVec2 labelSize = m_Label->Measure();
        return {
            trackW + 8.0f + labelSize.x + Padding.Horizontal(),
            std::max({ MinSize.y, trackH + Padding.Vertical(), labelSize.y + Padding.Vertical() })
        };
    }

    void UIToggleSwitch::ArrangeChildren(const UIRect& contentRect)
    {
        if (!m_Label)
            return;
        UIRect textRect = contentRect;
        textRect.Min.x = std::min(textRect.Max.x, textRect.Min.x + 48.0f);
        m_Label->Arrange(textRect);
    }

    void UIToggleSwitch::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIRect& b = GetBounds();
        const float trackW = 40.0f;
        const float trackH = 20.0f;
        const UIVec2 tMin{ b.Min.x + Padding.Left, b.Min.y + (b.Height() - trackH) * 0.5f };
        const UIVec2 tMax{ tMin.x + trackW, tMin.y + trackH };

        const UIColor offBg{ 0.18f, 0.20f, 0.26f, 1.0f };
        const UIColor trackBg{
            offBg.r + (OnColor.r - offBg.r) * m_AnimT,
            offBg.g + (OnColor.g - offBg.g) * m_AnimT,
            offBg.b + (OnColor.b - offBg.b) * m_AnimT,
            1.0f
        };

        const bool hovered = HasState(GetState(), UIState::Hovered);
        const UIColor borderColor = hovered
            ? UIColor{ 0.55f, 0.62f, 0.74f, 1.0f }
            : UIColor{ 0.28f, 0.32f, 0.40f, 0.9f };

        drawList.AddRoundedRect(tMin, tMax, trackBg, glm::vec4(trackH * 0.5f), 1.0f, borderColor);

        const float knobR = 7.5f;
        const float knobX = tMin.x + 10.0f + m_AnimT * (trackW - 20.0f);
        const float knobY = tMin.y + trackH * 0.5f;
        drawList.AddRoundedRect(
            { knobX - knobR, knobY - knobR },
            { knobX + knobR, knobY + knobR },
            { 0.95f, 0.97f, 1.0f, 1.0f },
            glm::vec4(knobR)
        );
    }
}
