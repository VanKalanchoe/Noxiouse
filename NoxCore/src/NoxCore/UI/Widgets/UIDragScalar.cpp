#include "UIDragScalar.h"

#include <algorithm>
#include <format>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIDragScalar::UIDragScalar(float value, float speed, std::function<void(float)> onChanged)
        : Value(value), Speed(speed), OnChanged(std::move(onChanged))
    {
        StyleClass = "DragScalar";
        DrawBackground = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
    }

    bool UIDragScalar::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button == 1 && IsEnabled())
        {
            m_Dragging = true;
            return true;
        }
        return UIElement::OnPointerDown(e);
    }

    bool UIDragScalar::OnPointerMove(const UIPointerEvent& e)
    {
        if (m_Dragging && IsEnabled())
        {
            float next = Value + e.Delta.x * Speed;
            if (Max > Min)
                next = std::clamp(next, Min, Max);
            if (next != Value)
            {
                Value = next;
                if (OnChanged)
                    OnChanged(Value);
            }
            return true;
        }
        return UIElement::OnPointerMove(e);
    }

    bool UIDragScalar::OnPointerUp(const UIPointerEvent& e)
    {
        if (m_Dragging && e.Button == 1)
        {
            m_Dragging = false;
            return true;
        }
        return UIElement::OnPointerUp(e);
    }

    UIVec2 UIDragScalar::ComputeDesiredSize()
    {
        const UIStyleBox& style = GetResolvedStyle();
        return { 76.0f, std::max(24.0f, style.FontSize + style.Padding.Vertical()) };
    }

    void UIDragScalar::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIStyleBox& style = GetResolvedStyle();
        const UIRect& b = GetBounds();
        float textLeft = b.Min.x + style.Padding.Left;

        if (!Prefix.empty())
        {
            constexpr float kTagW = 18.0f;
            drawList.AddRoundedRect(
                b.Min,
                { b.Min.x + kTagW, b.Max.y },
                PrefixColor,
                { style.CornerRadius.TopLeft, 0.0f, 0.0f, style.CornerRadius.BottomLeft }
            );
            const UIVec2 pSize = UILabel::MeasureText(Prefix, nullptr, style.FontSize);
            drawList.AddText(
                Prefix, nullptr,
                { b.Min.x + (kTagW - pSize.x) * 0.5f, b.Min.y + (b.Height() - pSize.y) * 0.5f },
                style.FontSize,
                { 1.0f, 1.0f, 1.0f, 1.0f }
            );
            textLeft = b.Min.x + kTagW + 6.0f;
        }

        const std::string text = std::vformat(Format, std::make_format_args(Value));
        const UIVec2 textSize = UILabel::MeasureText(text, nullptr, style.FontSize);
        const float textY = b.Min.y + (b.Height() - textSize.y) * 0.5f;
        drawList.AddText(text, nullptr, { textLeft, textY }, style.FontSize, style.TextColor);
    }
}
