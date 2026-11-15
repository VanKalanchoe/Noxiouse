#include "UIViewportSlot.h"

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIViewportSlot::UIViewportSlot()
    {
        StyleClass = "ViewportSlot";
        DrawBackground = true;
        HitTestMode = UIHitTestMode::Self;
    }

    UIVec2 UIViewportSlot::ComputeDesiredSize()
    {
        return { 320.0f, 200.0f };
    }

    void UIViewportSlot::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        if (BadgeText.empty())
            return;

        const UIRect& b = GetBounds();
        const UIVec2 textSize = UILabel::MeasureText(BadgeText, nullptr, 12.5f);
        const UIVec2 pillMin{ b.Min.x + 10.0f, b.Min.y + 10.0f };
        const UIVec2 pillMax{ pillMin.x + textSize.x + 18.0f, pillMin.y + textSize.y + 10.0f };

        drawList.AddRoundedRect(
            pillMin, pillMax,
            { 0.08f, 0.10f, 0.14f, 0.85f },
            glm::vec4(6.0f),
            1.0f,
            theme.GetToken("Border", { 0.22f, 0.25f, 0.33f, 0.9f })
        );
        drawList.AddText(
            BadgeText, nullptr,
            { pillMin.x + 9.0f, pillMin.y + 5.0f },
            12.5f,
            theme.GetToken("Text", { 0.92f, 0.94f, 0.98f, 1.0f })
        );
    }
}
