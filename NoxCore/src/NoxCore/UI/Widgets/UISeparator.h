#pragma once

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    // Horizontal or vertical 1px hairline divider using the theme's "Separator" style.
    class UISeparator : public UIElement
    {
    public:
        explicit UISeparator(UIOrientation orientation = UIOrientation::Horizontal, float thickness = 1.0f);
    };
}
