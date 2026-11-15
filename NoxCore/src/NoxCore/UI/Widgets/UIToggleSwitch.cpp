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

        if (!label.empty())
        {
            m_Label = Ref<UILabel>(new UILabel(std::move(label)));
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
        const UIStyleBox& style = GetResolvedStyle();
        const float trackH = std::max(16.0f, style.FontSize * 1.4f);
        const float trackW = trackH * 1.9f;
        const float gap = style.FontSize * 0.55f;
        if (!m_Label)
            return { trackW + Padding.Horizontal(), std::max(MinSize.y, trackH + Padding.Vertical()) };

        const UIVec2 labelSize = m_Label->Measure();
        return {
            trackW + gap + labelSize.x + Padding.Horizontal(),
            std::max({ MinSize.y, trackH + Padding.Vertical(), labelSize.y + Padding.Vertical() })
        };
    }

    void UIToggleSwitch::ArrangeChildren(const UIRect& contentRect)
    {
        if (!m_Label)
            return;
        const UIStyleBox& style = GetResolvedStyle();
        const float trackH = std::max(16.0f, style.FontSize * 1.4f);
        const float trackW = trackH * 1.9f;
        const float gap = style.FontSize * 0.55f;
        UIRect textRect = contentRect;
        textRect.Min.x = std::min(textRect.Max.x, textRect.Min.x + trackW + gap);
        m_Label->Arrange(textRect);
    }

    void UIToggleSwitch::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIStyleBox& style = GetResolvedStyle();
        const UIRect& b = GetBounds();
        const float trackH = std::max(16.0f, style.FontSize * 1.4f);
        const float trackW = trackH * 1.9f;
        const UIVec2 tMin{ b.Min.x + Padding.Left, b.Min.y + (b.Height() - trackH) * 0.5f };
        const UIVec2 tMax{ tMin.x + trackW, tMin.y + trackH };

        const UIColor offBg = theme.GetToken("Surface2", { 0.184f, 0.194f, 0.227f, 1.0f });
        const UIColor onBg = theme.GetToken("Accent", OnColor);
        const UIColor trackBg{
            offBg.r + (onBg.r - offBg.r) * m_AnimT,
            offBg.g + (onBg.g - offBg.g) * m_AnimT,
            offBg.b + (onBg.b - offBg.b) * m_AnimT,
            1.0f
        };

        const bool hovered = HasState(GetState(), UIState::Hovered);
        const UIColor borderColor = hovered
            ? theme.GetToken("AccentHover", onBg)
            : style.BorderColor;

        drawList.AddRoundedRect(tMin, tMax, trackBg, glm::vec4(trackH * 0.5f), 1.0f, borderColor);

        const float knobR = trackH * 0.375f;
        const float knobPad = trackH * 0.5f;
        const float knobX = tMin.x + knobPad + m_AnimT * (trackW - knobPad * 2.0f);
        const float knobY = tMin.y + trackH * 0.5f;
        drawList.AddRoundedRect(
            { knobX - knobR, knobY - knobR },
            { knobX + knobR, knobY + knobR },
            theme.GetToken("Text", { 0.95f, 0.97f, 1.0f, 1.0f }),
            glm::vec4(knobR)
        );
    }
}
