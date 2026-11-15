#pragma once

#include <string>

#include "NoxCore/Core/Ref.h"
#include "NoxCore/UI/Core/UIElement.h"

namespace Nox
{
    class Font;
}

namespace Nox::UI
{
    class UILabel : public UIElement
    {
    public:
        explicit UILabel(std::string text = {}, float fontSize = 0.0f);
        ~UILabel() override;

        std::string Text;
        float FontSizeOverride = 0.0f; // 0 = use theme style font_size
        bool UseColorOverride = false;
        UIColor ColorOverride{ 1.0f, 1.0f, 1.0f, 1.0f };
        Ref<Font> FontAsset;

        static UIVec2 MeasureText(std::string_view text, const Ref<Font>& fontAsset, float fontSize);

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;
    };
}
