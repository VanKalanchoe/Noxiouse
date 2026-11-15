#include "UIImage.h"

#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    UIImage::UIImage(uint32_t textureDescriptorSlot, const UIVec2& desiredSize)
        : TextureDescriptorSlot(textureDescriptorSlot)
        , m_Desired(desiredSize)
    {
        StyleClass = "Panel";
        DrawBackground = false;
        HitTestMode = UIHitTestMode::None;
    }

    UIVec2 UIImage::ComputeDesiredSize()
    {
        return m_Desired;
    }

    void UIImage::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIRect& b = GetBounds();
        if (TextureDescriptorSlot != ~0u)
        {
            drawList.AddTexturedQuad(
                b.Min,
                b.Max,
                TextureDescriptorSlot,
                UVMin,
                UVMax,
                Tint,
                { CornerRadius.TopLeft, CornerRadius.TopRight, CornerRadius.BottomRight, CornerRadius.BottomLeft }
            );
        }
        else
        {
            drawList.AddRoundedRect(
                b.Min,
                b.Max,
                { 0.14f, 0.16f, 0.21f, 1.0f },
                { CornerRadius.TopLeft, CornerRadius.TopRight, CornerRadius.BottomRight, CornerRadius.BottomLeft },
                1.0f,
                { 0.28f, 0.32f, 0.40f, 1.0f }
            );
        }
    }
}
