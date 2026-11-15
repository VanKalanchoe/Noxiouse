#include "UIRangeSlider.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

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
        HeightMode = UISizeMode::Fixed;
        FixedSize.y = 26.0f;
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
        const UIRect& b = GetBounds();
        const float pad = 8.0f;
        const float w = std::max(1.0f, b.Width() - pad * 2.0f);
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
        return { 160.0f, FixedSize.y > 0.0f ? FixedSize.y : 26.0f };
    }

    void UIRangeSlider::OnPaint(UIDrawList& drawList, const UITheme&)
    {
        const UIRect& b = GetBounds();
        const float pad = 8.0f;
        const float trackW = std::max(1.0f, b.Width() - pad * 2.0f);
        const float cy = (b.Min.y + b.Max.y) * 0.5f;
        const float trackH = 6.0f;
        const float invRange = 1.0f / std::max(1e-5f, MaxLimit - MinLimit);

        const float x0 = b.Min.x + pad;
        const float xLow = x0 + ((m_Low - MinLimit) * invRange) * trackW;
        const float xHigh = x0 + ((m_High - MinLimit) * invRange) * trackW;

        // Track
        drawList.AddRoundedRect(
            { x0, cy - trackH * 0.5f },
            { x0 + trackW, cy + trackH * 0.5f },
            { 0.14f, 0.16f, 0.21f, 1.0f },
            glm::vec4(3.0f)
        );

        // Selected span
        drawList.AddRoundedRect(
            { xLow, cy - trackH * 0.5f },
            { std::max(xLow + 2.0f, xHigh), cy + trackH * 0.5f },
            { 0.24f, 0.56f, 0.96f, 1.0f },
            glm::vec4(3.0f)
        );

        // Handles
        const float handleR = 7.0f;
        auto drawHandle = [&](float hx, bool active)
        {
            drawList.AddRoundedRect(
                { hx - handleR, cy - handleR },
                { hx + handleR, cy + handleR },
                active ? UIColor{ 0.98f, 0.99f, 1.0f, 1.0f } : UIColor{ 0.88f, 0.92f, 0.98f, 1.0f },
                glm::vec4(handleR),
                1.5f,
                { 0.24f, 0.56f, 0.96f, 1.0f }
            );
        };
        drawHandle(xLow, m_Dragging == DragHandle::Low);
        drawHandle(xHigh, m_Dragging == DragHandle::High);

        char buf[48]{};
        std::snprintf(buf, sizeof(buf), "%.2f - %.2f", m_Low, m_High);
        drawList.AddText(buf, Font::GetDefault(), { b.Min.x + 12.0f, b.Min.y - 2.0f }, 11.5f, { 0.72f, 0.78f, 0.86f, 0.9f });
    }
}
