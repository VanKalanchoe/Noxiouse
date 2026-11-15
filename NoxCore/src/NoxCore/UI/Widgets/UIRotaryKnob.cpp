#include "UIRotaryKnob.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIRotaryKnob::UIRotaryKnob(
        std::string label,
        float value,
        float minValue,
        float maxValue,
        std::function<void(float)> onChanged)
        : Label(std::move(label))
        , MinValue(minValue)
        , MaxValue(std::max(minValue + 1e-4f, maxValue))
        , OnChanged(std::move(onChanged))
    {
        DrawBackground = false;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
        SetValue(value, false);
    }

    void UIRotaryKnob::SetValue(float value, bool notify)
    {
        const float clamped = std::clamp(value, MinValue, MaxValue);
        if (std::abs(m_Value - clamped) < 1e-5f)
            return;
        m_Value = clamped;
        if (notify && OnChanged)
            OnChanged(m_Value);
    }

    bool UIRotaryKnob::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return false;
        m_Dragging = true;
        return true;
    }

    bool UIRotaryKnob::OnPointerMove(const UIPointerEvent& e)
    {
        if (!m_Dragging)
            return false;
        const float range = MaxValue - MinValue;
        SetValue(m_Value - e.Delta.y * (range / 140.0f), true);
        return true;
    }

    bool UIRotaryKnob::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == 1 && m_Dragging)
        {
            m_Dragging = false;
            return true;
        }
        return false;
    }

    UIVec2 UIRotaryKnob::ComputeDesiredSize()
    {
        const UIStyleBox& style = GetResolvedStyle();
        const float fontSize = std::max(10.0f, style.FontSize * 0.82f);
        const float radius = std::max(18.0f, style.FontSize * 1.55f);
        const float labelW = Label.empty() ? 0.0f : UILabel::MeasureText(Label, nullptr, fontSize).x;
        const float w = std::max(radius * 2.0f + 16.0f, labelW + 8.0f);
        const float h = radius * 2.0f + fontSize + (Label.empty() ? 14.0f : fontSize + 18.0f);
        return { std::max(MinSize.x, w), std::max(MinSize.y, h) };
    }

    void UIRotaryKnob::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        const UIStyleBox& style = GetResolvedStyle();
        const UIRect& b = GetBounds();
        const float fontSize = std::max(10.0f, style.FontSize * 0.82f);
        const float radius = std::max(18.0f, style.FontSize * 1.55f);
        const UIVec2 center{ b.Center().x, b.Min.y + radius + 6.0f };
        const float t = std::clamp((m_Value - MinValue) / std::max(1e-5f, MaxValue - MinValue), 0.0f, 1.0f);

        const UIColor bodyFill = theme.GetToken("Surface1", { 0.145f, 0.153f, 0.180f, 1.0f });
        const UIColor borderCol = theme.GetToken("Border", { 0.235f, 0.251f, 0.298f, 1.0f });
        const UIColor trackCol = theme.GetToken("Surface2", { 0.184f, 0.194f, 0.227f, 1.0f });
        const UIColor accentCol = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        const UIColor textCol = theme.GetToken("Text", { 0.918f, 0.929f, 0.953f, 1.0f });
        const UIColor mutedCol = theme.GetToken("TextMuted", { 0.580f, 0.616f, 0.686f, 1.0f });

        // Knob body
        drawList.AddRoundedRect(
            center - UIVec2{ radius - 4.0f, radius - 4.0f },
            center + UIVec2{ radius - 4.0f, radius - 4.0f },
            bodyFill,
            glm::vec4(radius - 4.0f),
            1.5f,
            borderCol
        );

        // Arc angles: 135 deg (bottom-left) -> 405 deg (bottom-right), 270 deg sweep
        constexpr float kStartRad = 2.3561945f;
        constexpr float kSweepRad = 4.7123890f;
        constexpr int kSteps = 20;

        auto arcPt = [&](float frac) -> UIVec2
        {
            const float a = kStartRad + frac * kSweepRad;
            return { center.x + std::cos(a) * radius, center.y + std::sin(a) * radius };
        };

        for (int i = 0; i < kSteps; ++i)
        {
            const float f0 = static_cast<float>(i) / static_cast<float>(kSteps);
            const float f1 = static_cast<float>(i + 1) / static_cast<float>(kSteps);
            const UIVec2 p0 = arcPt(f0);
            const UIVec2 p1 = arcPt(f1);
            const UIVec2 mid = (p0 + p1) * 0.5f;
            drawList.AddBezier(p0, mid, mid, p1, 3.0f, trackCol);
        }

        const int activeSteps = static_cast<int>(std::ceil(t * static_cast<float>(kSteps)));
        for (int i = 0; i < activeSteps; ++i)
        {
            const float f0 = static_cast<float>(i) / static_cast<float>(kSteps);
            const float f1 = std::min(t, static_cast<float>(i + 1) / static_cast<float>(kSteps));
            if (f1 <= f0)
                continue;
            const UIVec2 p0 = arcPt(f0);
            const UIVec2 p1 = arcPt(f1);
            const UIVec2 mid = (p0 + p1) * 0.5f;
            drawList.AddBezier(p0, mid, mid, p1, 3.2f, accentCol);
        }

        // Pointer tick on knob
        const float curAngle = kStartRad + t * kSweepRad;
        const UIVec2 tickStart = center + UIVec2{ std::cos(curAngle), std::sin(curAngle) } * (radius * 0.22f);
        const UIVec2 tickEnd = center + UIVec2{ std::cos(curAngle), std::sin(curAngle) } * (radius - 6.0f);
        drawList.AddBezier(tickStart, (tickStart + tickEnd) * 0.5f, (tickStart + tickEnd) * 0.5f, tickEnd, 2.2f, textCol);

        const Ref<Font> font = Font::GetDefault();
        char valBuf[16]{};
        std::snprintf(valBuf, sizeof(valBuf), "%.2f", m_Value);
        const UIVec2 vSz = UILabel::MeasureText(valBuf, font, fontSize);
        drawList.AddText(valBuf, font, { center.x - vSz.x * 0.5f, center.y + radius + 3.0f }, fontSize, textCol);

        if (!Label.empty())
        {
            const UIVec2 lSz = UILabel::MeasureText(Label, font, fontSize);
            drawList.AddText(Label, font, { center.x - lSz.x * 0.5f, center.y + radius + fontSize + 6.0f }, fontSize, mutedCol);
        }
    }
}
