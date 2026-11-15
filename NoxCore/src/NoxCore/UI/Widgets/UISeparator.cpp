#include "UISeparator.h"

namespace Nox::UI
{
    UISeparator::UISeparator(UIOrientation orientation, float thickness)
    {
        StyleClass = "Separator";
        DrawBackground = true;
        HitTestMode = UIHitTestMode::None;

        if (orientation == UIOrientation::Horizontal)
        {
            WidthMode = UISizeMode::Fill;
            HeightMode = UISizeMode::Fixed;
            FixedSize.y = thickness;
            Margin = UIThickness::Symmetric(0.0f, 4.0f);
        }
        else
        {
            WidthMode = UISizeMode::Fixed;
            HeightMode = UISizeMode::Fill;
            FixedSize.x = thickness;
            Margin = UIThickness::Symmetric(4.0f, 0.0f);
        }
    }
}
