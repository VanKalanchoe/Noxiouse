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

    std::string UILabel::WrapText(std::string_view text, const Ref<Font>& fontAsset, float fontSize, float maxWidth)
    {
        if (text.empty() || maxWidth <= 1.0f)
            return std::string(text);

        std::string result;
        result.reserve(text.size() + 16);

        size_t pos = 0;
        while (pos < text.size())
        {
            size_t lineEnd = text.find('\n', pos);
            const bool hasNewline = (lineEnd != std::string_view::npos);
            if (!hasNewline)
                lineEnd = text.size();

            std::string_view line = text.substr(pos, lineEnd - pos);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);

            std::string current;
            size_t wordStart = 0;
            while (wordStart < line.size())
            {
                const bool isSpace = (line[wordStart] == ' ' || line[wordStart] == '\t');
                size_t tokenEnd = wordStart + 1;
                if (!isSpace)
                {
                    while (tokenEnd < line.size() && line[tokenEnd] != ' ' && line[tokenEnd] != '\t')
                        ++tokenEnd;
                }
                else
                {
                    while (tokenEnd < line.size() && (line[tokenEnd] == ' ' || line[tokenEnd] == '\t'))
                        ++tokenEnd;
                }

                std::string_view token = line.substr(wordStart, tokenEnd - wordStart);
                if (current.empty())
                {
                    if (!isSpace)
                        current.assign(token);
                }
                else
                {
                    std::string candidate = current;
                    candidate.append(token);
                    if (MeasureText(candidate, fontAsset, fontSize).x <= maxWidth)
                    {
                        current = std::move(candidate);
                    }
                    else
                    {
                        if (!result.empty() && result.back() != '\n')
                            result.push_back('\n');
                        result.append(current);
                        current = isSpace ? std::string() : std::string(token);
                    }
                }
                wordStart = tokenEnd;
            }

            if (!current.empty())
            {
                if (!result.empty() && result.back() != '\n')
                    result.push_back('\n');
                result.append(current);
            }
            if (hasNewline)
                result.push_back('\n');
            pos = lineEnd + 1;
        }
        return result;
    }

    std::string UILabel::ElideText(std::string_view text, const Ref<Font>& fontAsset, float fontSize, float maxWidth, UITextElide mode)
    {
        if (mode == UITextElide::None || text.empty() || text.find('\n') != std::string_view::npos)
            return std::string(text);
        if (MeasureText(text, fontAsset, fontSize).x <= maxWidth + 0.5f)
            return std::string(text);

        static constexpr std::string_view kDots = "...";
        if (MeasureText(kDots, fontAsset, fontSize).x > maxWidth)
            return {}; // not even the dots fit: nothing is better than text outside the bounds

        // The most characters that still fit with the dots, by bisection (the width grows with the count).
        auto build = [&](size_t kept)
        {
            std::string out;
            out.reserve(kept + kDots.size());
            if (mode == UITextElide::Middle)
            {
                const size_t tail = kept / 2;
                const size_t head = kept - tail;
                out.append(text.substr(0, head));
                out.append(kDots);
                out.append(text.substr(text.size() - tail));
            }
            else
            {
                out.append(text.substr(0, kept));
                // No blank before the dots.
                while (!out.empty() && out.back() == ' ')
                    out.pop_back();
                out.append(kDots);
            }
            return out;
        };

        size_t low = 0;
        size_t high = text.size() - 1;
        while (low < high)
        {
            const size_t mid = (low + high + 1) / 2;
            if (MeasureText(build(mid), fontAsset, fontSize).x <= maxWidth + 0.5f)
                low = mid;
            else
                high = mid - 1;
        }
        return build(low);
    }

    float UILabel::ComputeHeightForWidth(float width)
    {
        if (HeightMode == UISizeMode::Fixed)
            return FixedSize.y;
        const float fontSize = FontSizeOverride > 0.0f ? FontSizeOverride : GetResolvedStyle().FontSize;
        if (WordWrap && width > 1.0f && !Text.empty())
            return MeasureText(WrapText(Text, FontAsset, fontSize, width), FontAsset, fontSize).y;
        return MeasureText(Text, FontAsset, fontSize).y;
    }

    UIVec2 UILabel::ComputeDesiredSize()
    {
        const float fontSize = FontSizeOverride > 0.0f ? FontSizeOverride : GetResolvedStyle().FontSize;
        if (WordWrap && WidthMode == UISizeMode::Fill)
            return { 0.0f, MeasureText(Text.empty() ? " " : Text, FontAsset, fontSize).y };
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

        const UIRect& b = GetBounds();
        const std::string wrapped = (WordWrap && b.Width() > 1.0f) ? WrapText(Text, FontAsset, fontSize, b.Width()) : std::string();
        std::string_view painted = (WordWrap && b.Width() > 1.0f) ? std::string_view(wrapped) : std::string_view(Text);

        // A line wider than the label is shortened: text never leaves the space its parent gave it.
        m_Elided = false;
        if (!WordWrap && Elide != UITextElide::None && b.Width() > 1.0f)
        {
            if (m_ElideSource != Text || m_ElideWidth != b.Width() || m_ElideFontSize != fontSize || m_ElideMode != Elide)
            {
                m_ElideSource = Text;
                m_ElideWidth = b.Width();
                m_ElideFontSize = fontSize;
                m_ElideMode = Elide;
                m_ElideResult = ElideText(Text, FontAsset, fontSize, b.Width(), Elide);
            }
            m_Elided = m_ElideResult.size() != Text.size() || m_ElideResult != Text;
            painted = m_ElideResult;
        }

        UIVec2 pos = b.Min;
        if (HAlign == UIAlignment::Center || HAlign == UIAlignment::End ||
            VAlign == UIAlignment::Center || VAlign == UIAlignment::End)
        {
            const UIVec2 textSize = MeasureText(painted, FontAsset, fontSize);
            if (HAlign == UIAlignment::Center)
                pos.x = b.Min.x + (b.Width() - textSize.x) * 0.5f;
            else if (HAlign == UIAlignment::End)
                pos.x = b.Max.x - textSize.x;

            if (VAlign == UIAlignment::Center)
                pos.y = b.Min.y + (b.Height() - textSize.y) * 0.5f;
            else if (VAlign == UIAlignment::End)
                pos.y = b.Max.y - textSize.y;
        }

        drawList.AddText(painted, FontAsset, pos, fontSize, color);
    }
}
