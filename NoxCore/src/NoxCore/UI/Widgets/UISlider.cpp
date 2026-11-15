#include "UISlider.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    namespace
    {
        constexpr float kTrackHeight = 6.0f;
        constexpr float kThumbRadius = 7.0f;
        constexpr float kSliderHeight = 24.0f;
        constexpr float kValueWidth = 48.0f;
    }

    UISlider::UISlider(float value, float min, float max, std::function<void(float)> onChanged)
        : Value(value), Min(min), Max(max), OnChanged(std::move(onChanged))
    {
        StyleClass = "Slider";
        DrawBackground = false;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
    }

    void UISlider::UpdateFromPointerX(float x)
    {
        const UIRect& b = GetBounds();
        const float trackLeft = b.Min.x + kThumbRadius;
        const float trackRight = b.Max.x - kThumbRadius - (ShowValue ? kValueWidth : 0.0f);
        const float span = std::max(1.0f, trackRight - trackLeft);
        const float t = std::clamp((x - trackLeft) / span, 0.0f, 1.0f);

        float next = Min + t * (Max - Min);
        if (Step > 0.0f)
            next = Min + std::round((next - Min) / Step) * Step;
        next = std::clamp(next, std::min(Min, Max), std::max(Min, Max));

        if (next != Value)
        {
            Value = next;
            if (OnChanged)
                OnChanged(Value);
        }
    }

    bool UISlider::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button == 1 && IsEnabled())
        {
            m_Dragging = true;
            UpdateFromPointerX(e.Position.x);
            return true;
        }
        return UIElement::OnPointerDown(e);
    }

    bool UISlider::OnPointerMove(const UIPointerEvent& e)
    {
        if (m_Dragging && IsEnabled())
        {
            UpdateFromPointerX(e.Position.x);
            return true;
        }
        return UIElement::OnPointerMove(e);
    }

    bool UISlider::OnPointerUp(const UIPointerEvent& e)
    {
        if (m_Dragging && e.Button == 1)
        {
            m_Dragging = false;
            return true;
        }
        return UIElement::OnPointerUp(e);
    }

    UIVec2 UISlider::ComputeDesiredSize()
    {
        return { 140.0f, kSliderHeight };
    }

    void UISlider::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        const UIStyleBox& style = GetResolvedStyle();
        const UIRect& b = GetBounds();
        const float trackLeft = b.Min.x + kThumbRadius;
        const float trackRight = std::max(trackLeft + 10.0f, b.Max.x - kThumbRadius - (ShowValue ? kValueWidth : 0.0f));
        const float cy = b.Min.y + b.Height() * 0.5f;

        const float range = (Max - Min);
        const float t = std::abs(range) > 1e-6f ? std::clamp((Value - Min) / range, 0.0f, 1.0f) : 0.0f;
        const float thumbX = trackLeft + t * (trackRight - trackLeft);

        const UIColor accent = theme.GetToken("Accent", { 0.24f, 0.56f, 0.96f, 1.0f });

        // Track background
        drawList.AddRoundedRect(
            { trackLeft, cy - kTrackHeight * 0.5f },
            { trackRight, cy + kTrackHeight * 0.5f },
            style.FillColor,
            glm::vec4(kTrackHeight * 0.5f),
            1.0f,
            style.BorderColor
        );

        // Filled active portion
        if (thumbX > trackLeft + 1.0f)
        {
            drawList.AddRoundedRect(
                { trackLeft, cy - kTrackHeight * 0.5f },
                { thumbX, cy + kTrackHeight * 0.5f },
                accent,
                glm::vec4(kTrackHeight * 0.5f)
            );
        }

        // Thumb
        const UIColor thumbColor = HasState(GetState(), UIState::Hovered) || m_Dragging
            ? UIColor{ 1.0f, 1.0f, 1.0f, 1.0f }
            : UIColor{ 0.90f, 0.93f, 0.98f, 1.0f };
        drawList.AddRoundedRect(
            { thumbX - kThumbRadius, cy - kThumbRadius },
            { thumbX + kThumbRadius, cy + kThumbRadius },
            thumbColor,
            glm::vec4(kThumbRadius),
            1.5f,
            accent
        );

        if (ShowValue)
        {
            const std::string text = std::vformat(Format, std::make_format_args(Value));
            const UIVec2 textSize = UILabel::MeasureText(text, nullptr, style.FontSize);
            const float textX = trackRight + kThumbRadius + 6.0f;
            const float textY = cy - textSize.y * 0.5f;
            drawList.AddText(text, nullptr, { textX, textY }, style.FontSize, style.TextColor);
        }
    }
}
