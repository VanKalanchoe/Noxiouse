#include "UICheckBox.h"

#include <algorithm>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Rendering/UIIcons.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UICheckBox::UICheckBox(std::string label, bool checked, std::function<void(bool)> onChanged)
        : Label(std::move(label)), Checked(checked), OnChanged(std::move(onChanged))
    {
        StyleClass = "CheckBox";
        DrawBackground = false;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
    }

    bool UICheckBox::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == 1 && IsEnabled() && GetBounds().Contains(e.Position))
        {
            Checked = !Checked;
            if (OnChanged)
                OnChanged(Checked);
            return true;
        }
        return UIElement::OnPointerUp(e);
    }

    UIVec2 UICheckBox::ComputeDesiredSize()
    {
        const UIStyleBox& style = GetResolvedStyle();
        const float boxSize = std::clamp(style.FontSize * 0.92f, 15.0f, 18.0f);
        const float labelGap = style.FontSize * 0.50f;
        float w = boxSize;
        float h = std::max(boxSize + 2.0f, style.FontSize * 1.15f);
        if (!Label.empty())
        {
            const UIVec2 textSize = UILabel::MeasureText(Label, nullptr, style.FontSize);
            w += labelGap + textSize.x;
            h = std::max(h, textSize.y);
        }
        return { w, h };
    }

    void UICheckBox::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        const UIStyleBox& style = GetResolvedStyle();
        const UIRect& bounds = GetBounds();
        const float boxSize = std::clamp(style.FontSize * 0.92f, 15.0f, 18.0f);
        const float labelGap = style.FontSize * 0.50f;
        const float boxY = bounds.Min.y + (bounds.Height() - boxSize) * 0.5f;
        const UIVec2 boxMin{ bounds.Min.x, boxY };
        const UIVec2 boxMax{ bounds.Min.x + boxSize, boxY + boxSize };

        const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        const bool hovered = HasState(GetState(), UIState::Hovered);
        const UIColor boxFill = Checked ? accent : style.FillColor;
        const UIColor border = Checked ? accent : (hovered ? theme.GetToken("AccentHover", accent) : style.BorderColor);

        drawList.AddRoundedRect(
            boxMin, boxMax,
            boxFill,
            style.CornerRadius.ToVec4(),
            std::max(1.0f, style.BorderThickness),
            border
        );

        if (Checked)
        {
            const UIColor checkCol = theme.GetToken("Text", { 1.0f, 1.0f, 1.0f, 0.98f });
            Icons::DrawCheckmark(drawList, boxMin, boxMax, checkCol, std::max(1.75f, boxSize * 0.13f));
        }

        if (!Label.empty())
        {
            const UIVec2 textSize = UILabel::MeasureText(Label, nullptr, style.FontSize);
            const float textY = bounds.Min.y + (bounds.Height() - textSize.y) * 0.5f;
            drawList.AddText(Label, nullptr, { boxMax.x + labelGap, textY }, style.FontSize, style.TextColor);
        }
    }
}
