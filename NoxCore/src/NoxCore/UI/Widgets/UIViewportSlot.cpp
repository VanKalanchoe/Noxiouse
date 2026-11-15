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

    void UIViewportSlot::ArrangeChildren(const UIRect& contentRect)
    {
        for (const auto& child : GetChildren())
        {
            if (child->Visible)
                child->Arrange(contentRect);
        }
    }

    void UIViewportSlot::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIRect& b = GetBounds();
        if (TextureSlot != 0 && b.Width() > 2.0f && b.Height() > 2.0f)
        {
            drawList.AddTexturedQuad(
                b.Min, b.Max, TextureSlot, UVMin, UVMax,
                { 1.0f, 1.0f, 1.0f, 1.0f },
                GetResolvedStyle().CornerRadius.ToVec4()
            );
        }

        if (BadgeText.empty())
            return;

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
