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

    float UIImage::ComputeHeightForWidth(float width)
    {
        if (HeightMode == UISizeMode::Fixed)
            return FixedSize.y;
        if (AspectRatio > 0.0f && width > 0.0f)
            return width / AspectRatio;
        return m_Desired.y;
    }

    UIVec2 UIImage::ComputeDesiredSize()
    {
        if (AspectRatio > 0.0f && m_Desired.x > 0.0f)
            return { m_Desired.x, m_Desired.x / AspectRatio };
        return m_Desired;
    }

    void UIImage::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        UIRect b = GetBounds();
        if (AspectRatio > 0.0f && b.Width() > 0.0f && b.Height() > 0.0f)
        {
            const float boxW = b.Width();
            const float boxH = b.Height();
            const float boxAspect = boxW / boxH;
            if (boxAspect > AspectRatio)
            {
                const float fitW = boxH * AspectRatio;
                const float inset = (boxW - fitW) * 0.5f;
                b.Min.x += inset;
                b.Max.x -= inset;
            }
            else if (boxAspect < AspectRatio)
            {
                const float fitH = boxW / AspectRatio;
                const float inset = (boxH - fitH) * 0.5f;
                b.Min.y += inset;
                b.Max.y -= inset;
            }
        }
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
