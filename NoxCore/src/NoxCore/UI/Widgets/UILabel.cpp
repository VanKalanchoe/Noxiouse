#include "UILabel.h"

#include <algorithm>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/MSDFData.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    UILabel::UILabel(std::string text, float fontSize)
        : Text(std::move(text)), FontSizeOverride(fontSize)
    {
        StyleClass = "Label";
        HitTestMode = UIHitTestMode::PassThrough;
        DrawBackground = false;
    }

    UILabel::~UILabel() = default;

    UIVec2 UILabel::MeasureText(std::string_view text, const Ref<Font>& fontAsset, float fontSize)
    {
        if (text.empty())
            return { 0.0f, fontSize };

        Ref<Font> font = fontAsset ? fontAsset : Font::GetDefault();
        if (!font || !font->IsValid() || !font->GetMSDFData())
            return { static_cast<float>(text.size()) * fontSize * 0.55f, fontSize };

        const auto& fontGeometry = font->GetMSDFData()->FontGeometry;
        const auto& metrics = fontGeometry.getMetrics();
        const double scale = static_cast<double>(fontSize) / (metrics.ascenderY - metrics.descenderY);

        double spaceAdv = 0.25;
        if (const auto* sp = fontGeometry.getGlyph(' '))
            spaceAdv = sp->getAdvance();

        double maxW = 0.0;
        double lineW = 0.0;
        int lines = 1;
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c == '\r')
                continue;
            if (c == '\n')
            {
                maxW = std::max(maxW, lineW);
                lineW = 0.0;
                ++lines;
                continue;
            }
            if (c == ' ')
            {
                lineW += spaceAdv * scale;
                continue;
            }
            if (c == '\t')
            {
                lineW += 4.0 * spaceAdv * scale;
                continue;
            }
            const auto* glyph = fontGeometry.getGlyph(static_cast<uint32_t>(static_cast<unsigned char>(c)));
            if (!glyph)
                glyph = fontGeometry.getGlyph('?');
            if (glyph)
                lineW += glyph->getAdvance() * scale;
        }
        maxW = std::max(maxW, lineW);
        const double totalH = static_cast<double>(lines) * metrics.lineHeight * scale;
        return { static_cast<float>(maxW), static_cast<float>(totalH) };
    }

    UIVec2 UILabel::ComputeDesiredSize()
    {
        const float fontSize = FontSizeOverride > 0.0f ? FontSizeOverride : GetResolvedStyle().FontSize;
        return MeasureText(Text, FontAsset, fontSize);
    }

    void UILabel::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);
        if (Text.empty())
            return;

        const UIStyleBox& style = GetResolvedStyle();
        const float fontSize = FontSizeOverride > 0.0f ? FontSizeOverride : style.FontSize;
        const UIColor color = UseColorOverride ? ColorOverride : style.TextColor;

        drawList.AddText(Text, FontAsset, GetBounds().Min, fontSize, color);
    }
}
