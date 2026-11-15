#include "UICheckBox.h"

#include <algorithm>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
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
        const float boxSize = std::max(12.0f, style.FontSize * 1.25f);
        const float labelGap = style.FontSize * 0.55f;
        float w = boxSize;
        float h = boxSize;
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
        const float boxSize = std::max(12.0f, style.FontSize * 1.25f);
        const float labelGap = style.FontSize * 0.55f;
        const float boxY = bounds.Min.y + (bounds.Height() - boxSize) * 0.5f;
        const UIVec2 boxMin{ bounds.Min.x, boxY };
        const UIVec2 boxMax{ bounds.Min.x + boxSize, boxY + boxSize };

        const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        const UIColor boxFill = Checked ? accent : style.FillColor;
        const UIColor border = Checked ? accent : style.BorderColor;

        drawList.AddRoundedRect(
            boxMin, boxMax,
            boxFill,
            style.CornerRadius.ToVec4(),
            std::max(1.0f, style.BorderThickness),
            border
        );

        if (Checked)
        {
            const float inset = boxSize * 0.25f;
            const UIVec2 innerMin = boxMin + UIVec2(inset, inset);
            const UIVec2 innerMax = boxMax - UIVec2(inset, inset);
            drawList.AddRoundedRect(
                innerMin, innerMax,
                theme.GetToken("Text", { 1.0f, 1.0f, 1.0f, 0.95f }),
                glm::vec4(std::max(1.5f, inset * 0.5f))
            );
        }

        if (!Label.empty())
        {
            const UIVec2 textSize = UILabel::MeasureText(Label, nullptr, style.FontSize);
            const float textY = bounds.Min.y + (bounds.Height() - textSize.y) * 0.5f;
            drawList.AddText(Label, nullptr, { boxMax.x + labelGap, textY }, style.FontSize, style.TextColor);
        }
    }
}
