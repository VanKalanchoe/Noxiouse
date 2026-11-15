#pragma once

#include <string>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    // Represents the 3D scene viewport area inside a docked editor layout.
    class UIViewportSlot : public UIElement
    {
    public:
        UIViewportSlot();

        std::string BadgeText = "Perspective  |  Lit";

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;
    };
}
