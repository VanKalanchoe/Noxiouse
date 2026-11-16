#pragma once

#include <cstdint>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    // Retained textured quad widget displaying a bindless descriptor slot with optional corner rounding & tint.
    class UIImage : public UIElement
    {
    public:
        explicit UIImage(uint32_t textureDescriptorSlot = ~0u, const UIVec2& desiredSize = { 64.0f, 64.0f });

        uint32_t TextureDescriptorSlot = ~0u;
        UIVec2 UVMin{ 0.0f, 0.0f };
        UIVec2 UVMax{ 1.0f, 1.0f };
        UIColor Tint{ 1.0f, 1.0f, 1.0f, 1.0f };
        UICornerRadius CornerRadius = UICornerRadius::All(4.0f);
        float AspectRatio = 0.0f; // > 0 preserves width / height ratio on auto-sizing and paint

    protected:
        float ComputeHeightForWidth(float width) override;
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        UIVec2 m_Desired{ 64.0f, 64.0f };
    };
}
