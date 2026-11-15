#include "UITextArea.h"

#include <algorithm>
#include <SDL3/SDL_scancode.h>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    namespace
    {
        constexpr float kLineHeight = 24.0f;
        constexpr float kFontSize = 18.0f;
        constexpr float kGutterWidth = 44.0f;
    }

    UITextArea::UITextArea(std::string text, std::string placeholder)
        : Text(std::move(text))
        , Placeholder(std::move(placeholder))
        , m_Cursor(Text.size())
    {
        StyleClass = "TextInput";
        DrawBackground = true;
        ClipChildren = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
        WidthMode = UISizeMode::Fill;
        MinSize = { 220.0f, 120.0f };
        Padding = UIThickness::All(8.0f);
    }

    std::vector<std::pair<size_t, size_t>> UITextArea::SplitLines() const
    {
        std::vector<std::pair<size_t, size_t>> lines;
        size_t start = 0;
        for (size_t i = 0; i < Text.size(); ++i)
        {
            if (Text[i] == '\n')
            {
                lines.emplace_back(start, i - start);
                start = i + 1;
            }
        }
        lines.emplace_back(start, Text.size() - start);
        return lines;
    }

    void UITextArea::CursorToLineCol(const std::vector<std::pair<size_t, size_t>>& lines, size_t& outLine, size_t& outCol) const
    {
        outLine = 0;
        outCol = 0;
        const size_t c = std::min(m_Cursor, Text.size());
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const auto [start, len] = lines[i];
            if (c <= start + len)
            {
                outLine = i;
                outCol = c - start;
                return;
            }
        }
        if (!lines.empty())
        {
            outLine = lines.size() - 1;
            outCol = lines.back().second;
        }
    }

    bool UITextArea::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return false;

        const UIRect& b = GetBounds();
        const float gutter = ShowLineNumbers ? kGutterWidth : 0.0f;
        const float relY = (e.Position.y - (b.Min.y + Padding.Top)) + m_ScrollY;
        const float relX = e.Position.x - (b.Min.x + Padding.Left + gutter);

        const auto lines = SplitLines();
        const size_t lineIdx = lines.empty()
            ? 0
            : std::min(lines.size() - 1, static_cast<size_t>(std::max(0.0f, relY) / kLineHeight));

        if (!lines.empty())
        {
            const auto [start, len] = lines[lineIdx];
            const std::string_view lineView(Text.data() + start, len);
            size_t bestCol = len;
            for (size_t col = 0; col <= len; ++col)
            {
                if (UILabel::MeasureText(lineView.substr(0, col), nullptr, kFontSize).x >= relX)
                {
                    bestCol = col;
                    break;
                }
            }
            m_Cursor = start + bestCol;
        }
        return true;
    }

    bool UITextArea::OnMouseWheel(const UIPointerEvent& e)
    {
        const auto lines = SplitLines();
        const float viewH = std::max(0.0f, GetBounds().Height() - Padding.Vertical());
        const float totalH = static_cast<float>(lines.size()) * kLineHeight;
        const float maxScroll = std::max(0.0f, totalH - viewH);
        m_ScrollY = std::clamp(m_ScrollY - e.ScrollDelta.y * 36.0f, 0.0f, maxScroll);
        return true;
    }

    bool UITextArea::OnKeyDown(const UIKeyEvent& e)
    {
        if (!IsEnabled())
            return false;

        m_Cursor = std::min(m_Cursor, Text.size());
        const auto lines = SplitLines();
        size_t line = 0, col = 0;
        CursorToLineCol(lines, line, col);

        switch (e.KeyCode)
        {
        case SDL_SCANCODE_LEFT:
            if (m_Cursor > 0) --m_Cursor;
            return true;
        case SDL_SCANCODE_RIGHT:
            if (m_Cursor < Text.size()) ++m_Cursor;
            return true;
        case SDL_SCANCODE_UP:
            if (line > 0)
            {
                const auto [pStart, pLen] = lines[line - 1];
                m_Cursor = pStart + std::min(col, pLen);
            }
            return true;
        case SDL_SCANCODE_DOWN:
            if (line + 1 < lines.size())
            {
                const auto [nStart, nLen] = lines[line + 1];
                m_Cursor = nStart + std::min(col, nLen);
            }
            return true;
        case SDL_SCANCODE_HOME:
            m_Cursor = lines[line].first;
            return true;
        case SDL_SCANCODE_END:
            m_Cursor = lines[line].first + lines[line].second;
            return true;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            Text.insert(m_Cursor, 1, '\n');
            ++m_Cursor;
            if (OnChanged) OnChanged(Text);
            return true;
        case SDL_SCANCODE_BACKSPACE:
            if (m_Cursor > 0)
            {
                Text.erase(m_Cursor - 1, 1);
                --m_Cursor;
                if (OnChanged) OnChanged(Text);
            }
            return true;
        case SDL_SCANCODE_DELETE:
            if (m_Cursor < Text.size())
            {
                Text.erase(m_Cursor, 1);
                if (OnChanged) OnChanged(Text);
            }
            return true;
        default:
            return false;
        }
    }

    bool UITextArea::OnTextInput(std::string_view text)
    {
        if (!IsEnabled() || text.empty())
            return false;
        m_Cursor = std::min(m_Cursor, Text.size());
        Text.insert(m_Cursor, text);
        m_Cursor += text.size();
        if (OnChanged)
            OnChanged(Text);
        return true;
    }

    UIVec2 UITextArea::ComputeDesiredSize()
    {
        return MinSize;
    }

    void UITextArea::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIRect& b = GetBounds();
        const Ref<Font> font = Font::GetDefault();
        const float gutter = ShowLineNumbers ? kGutterWidth : 0.0f;

        if (ShowLineNumbers)
        {
            drawList.AddRoundedRect(
                { b.Min.x + 1.0f, b.Min.y + 1.0f },
                { b.Min.x + gutter, b.Max.y - 1.0f },
                { 0.08f, 0.09f, 0.13f, 0.8f },
                { 4.0f, 0.0f, 0.0f, 4.0f }
            );
        }

        const auto lines = SplitLines();
        const float viewH = std::max(0.0f, b.Height() - Padding.Vertical());
        const float totalH = static_cast<float>(lines.size()) * kLineHeight;
        const float maxScroll = std::max(0.0f, totalH - viewH);
        m_ScrollY = std::clamp(m_ScrollY, 0.0f, maxScroll);

        drawList.PushClipRect(b.Min, b.Max);

        if (Text.empty() && !Placeholder.empty())
        {
            drawList.AddText(
                Placeholder,
                font,
                { b.Min.x + Padding.Left + gutter, b.Min.y + Padding.Top },
                kFontSize,
                { 0.48f, 0.53f, 0.62f, 0.85f }
            );
        }

        size_t curLine = 0, curCol = 0;
        CursorToLineCol(lines, curLine, curCol);

        const int first = std::max(0, static_cast<int>(m_ScrollY / kLineHeight));
        const int last = std::min(
            static_cast<int>(lines.size()) - 1,
            static_cast<int>((m_ScrollY + viewH) / kLineHeight) + 1
        );

        for (int i = first; i <= last; ++i)
        {
            const float y = b.Min.y + Padding.Top + static_cast<float>(i) * kLineHeight - m_ScrollY;
            if (ShowLineNumbers)
            {
                drawList.AddText(
                    std::to_string(i + 1),
                    font,
                    { b.Min.x + 8.0f, y + 1.0f },
                    11.5f,
                    (static_cast<size_t>(i) == curLine) ? UIColor{ 0.82f, 0.88f, 0.96f, 1.0f } : UIColor{ 0.44f, 0.49f, 0.58f, 0.9f }
                );
            }

            const auto [start, len] = lines[i];
            if (len > 0)
            {
                drawList.AddText(
                    std::string_view(Text.data() + start, len),
                    font,
                    { b.Min.x + Padding.Left + gutter, y + 1.0f },
                    kFontSize,
                    { 0.90f, 0.93f, 0.98f, 1.0f }
                );
            }

            if (HasState(GetState(), UIState::Focused) && static_cast<size_t>(i) == curLine)
            {
                const float cx = b.Min.x + Padding.Left + gutter +
                    UILabel::MeasureText(std::string_view(Text.data() + start, curCol), font, kFontSize).x;
                drawList.AddRoundedRect(
                    { cx, y + 1.0f },
                    { cx + 1.5f, y + kLineHeight - 2.0f },
                    { 0.30f, 0.68f, 1.0f, 1.0f }
                );
            }
        }

        drawList.PopClipRect();
    }
}
