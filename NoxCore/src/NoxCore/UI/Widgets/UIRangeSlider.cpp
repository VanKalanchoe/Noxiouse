#include "UIRangeSlider.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIRangeSlider::UIRangeSlider(
        float lowValue,
        float highValue,
        float minLimit,
        float maxLimit,
        std::function<void(float, float)> onRangeChanged)
        : MinLimit(minLimit)
        , MaxLimit(std::max(minLimit + 1e-4f, maxLimit))
        , OnRangeChanged(std::move(onRangeChanged))
    {
        StyleClass = "TextInput";
        DrawBackground = false;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
        WidthMode = UISizeMode::Fill;
        HeightMode = UISizeMode::Auto;
        SetRange(lowValue, highValue, false);
    }

    void UIRangeSlider::SetRange(float low, float high, bool notify)
    {
        low = std::clamp(low, MinLimit, MaxLimit);
        high = std::clamp(high, MinLimit, MaxLimit);
        if (low > high)
            std::swap(low, high);
        m_Low = low;
        m_High = high;
        if (notify && OnRangeChanged)
            OnRangeChanged(m_Low, m_High);
    }

    float UIRangeSlider::ValueFromX(float x) const
    {
        const UIStyleBox& style = GetResolvedStyle();
        const float fontSize = std::max(10.0f, style.FontSize * 0.9f);
        char buf[48]{};
        std::snprintf(buf, sizeof(buf), "%.2f - %.2f", m_Low, m_High);
        const float textW = UILabel::MeasureText(buf, Font::GetDefault(), fontSize).x + 12.0f;

        const UIRect& b = GetBounds();
        const float pad = 8.0f;
        const float w = std::max(1.0f, b.Width() - pad * 2.0f - textW);
        const float t = std::clamp((x - (b.Min.x + pad)) / w, 0.0f, 1.0f);
        return MinLimit + t * (MaxLimit - MinLimit);
    }

    bool UIRangeSlider::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return false;

        const float val = ValueFromX(e.Position.x);
        const float distLow = std::abs(val - m_Low);
        const float distHigh = std::abs(val - m_High);
        m_Dragging = (distLow <= distHigh) ? DragHandle::Low : DragHandle::High;
        if (m_Dragging == DragHandle::Low)
            SetRange(std::min(val, m_High), m_High, true);
        else
            SetRange(m_Low, std::max(val, m_Low), true);
        return true;
    }

    bool UIRangeSlider::OnPointerMove(const UIPointerEvent& e)
    {
        if (m_Dragging == DragHandle::None)
            return false;
        const float val = ValueFromX(e.Position.x);
        if (m_Dragging == DragHandle::Low)
            SetRange(std::min(val, m_High), m_High, true);
        else
            SetRange(m_Low, std::max(val, m_Low), true);
        return true;
    }

    bool UIRangeSlider::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == 1 && m_Dragging != DragHandle::None)
        {
            m_Dragging = DragHandle::None;
            return true;
        }
        return false;
    }

    UIVec2 UIRangeSlider::ComputeDesiredSize()
    {
        const UIStyleBox& style = GetResolvedStyle();
        return { std::max(MinSize.x, 120.0f), std::max(22.0f, style.FontSize * 1.9f) };
    }

    void UIRangeSlider::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        const UIStyleBox& style = GetResolvedStyle();
        const float fontSize = std::max(10.0f, style.FontSize * 0.9f);
        char buf[48]{};
        std::snprintf(buf, sizeof(buf), "%.2f - %.2f", m_Low, m_High);
        const UIVec2 textSize = UILabel::MeasureText(buf, Font::GetDefault(), fontSize);

        const UIRect& b = GetBounds();
        const float pad = 8.0f;
        const float trackW = std::max(1.0f, b.Width() - pad * 2.0f - textSize.x - 12.0f);
        const float cy = (b.Min.y + b.Max.y) * 0.5f;
        const float trackH = std::max(4.0f, style.FontSize * 0.42f);
        const float invRange = 1.0f / std::max(1e-5f, MaxLimit - MinLimit);

        const float x0 = b.Min.x + pad;
        const float xLow = x0 + ((m_Low - MinLimit) * invRange) * trackW;
        const float xHigh = x0 + ((m_High - MinLimit) * invRange) * trackW;

        const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        const UIColor trackBg = style.FillColor;

        // Track
        drawList.AddRoundedRect(
            { x0, cy - trackH * 0.5f },
            { x0 + trackW, cy + trackH * 0.5f },
            trackBg,
            glm::vec4(trackH * 0.5f)
        );

        // Selected span
        drawList.AddRoundedRect(
            { xLow, cy - trackH * 0.5f },
            { std::max(xLow + 2.0f, xHigh), cy + trackH * 0.5f },
            accent,
            glm::vec4(trackH * 0.5f)
        );

        // Handles
        const float handleR = std::max(5.0f, style.FontSize * 0.5f);
        auto drawHandle = [&](float hx, bool active)
        {
            drawList.AddRoundedRect(
                { hx - handleR, cy - handleR },
                { hx + handleR, cy + handleR },
                active ? UIColor{ 1.0f, 1.0f, 1.0f, 1.0f } : theme.GetToken("Text", { 0.918f, 0.929f, 0.953f, 1.0f }),
                glm::vec4(handleR),
                1.5f,
                accent
            );
        };
        drawHandle(xLow, m_Dragging == DragHandle::Low);
        drawHandle(xHigh, m_Dragging == DragHandle::High);

        drawList.AddText(
            buf,
            Font::GetDefault(),
            { x0 + trackW + 10.0f, cy - textSize.y * 0.5f },
            fontSize,
            style.TextColor
        );
    }
}
