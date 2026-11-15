#pragma once

#include <cstdint>
#include <string>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    // Represents the 3D scene viewport area inside a docked editor layout.
    class UIViewportSlot : public UIElement
    {
    public:
        UIViewportSlot();

        uint32_t TextureSlot = ~0u;
        UIVec2 UVMin{ 0.0f, 0.0f };
        UIVec2 UVMax{ 1.0f, 1.0f };
        std::string BadgeText = "Perspective  |  Lit";

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;
    };
}
