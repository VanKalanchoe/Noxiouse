#include "UITextInput.h"

#include <algorithm>
#include <SDL3/SDL_scancode.h>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UITextInput::UITextInput(std::string text, std::string placeholder)
        : Text(std::move(text)), Placeholder(std::move(placeholder)), m_Cursor(Text.size())
    {
        StyleClass = "TextInput";
        DrawBackground = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
        ClipChildren = true;
    }

    bool UITextInput::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button == 1 && IsEnabled())
        {
            const UIStyleBox& style = GetResolvedStyle();
            const float localX = e.Position.x - (GetBounds().Min.x + style.Padding.Left);
            m_Cursor = Text.size();
            for (size_t i = 0; i <= Text.size(); ++i)
            {
                const float w = UILabel::MeasureText(std::string_view(Text).substr(0, i), nullptr, style.FontSize).x;
                if (w >= localX)
                {
                    m_Cursor = i;
                    break;
                }
            }
            return true;
        }
        return UIElement::OnPointerDown(e);
    }

    bool UITextInput::OnKeyDown(const UIKeyEvent& e)
    {
        if (!IsEnabled())
            return false;

        m_Cursor = std::min(m_Cursor, Text.size());

        switch (e.KeyCode)
        {
        case SDL_SCANCODE_LEFT:
            if (m_Cursor > 0) --m_Cursor;
            return true;
        case SDL_SCANCODE_RIGHT:
            if (m_Cursor < Text.size()) ++m_Cursor;
            return true;
        case SDL_SCANCODE_HOME:
            m_Cursor = 0;
            return true;
        case SDL_SCANCODE_END:
            m_Cursor = Text.size();
            return true;
        case SDL_SCANCODE_BACKSPACE:
            if (!ReadOnly && m_Cursor > 0)
            {
                Text.erase(m_Cursor - 1, 1);
                --m_Cursor;
                if (OnChanged) OnChanged(Text);
            }
            return true;
        case SDL_SCANCODE_DELETE:
            if (!ReadOnly && m_Cursor < Text.size())
            {
                Text.erase(m_Cursor, 1);
                if (OnChanged) OnChanged(Text);
            }
            return true;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            if (OnSubmit) OnSubmit(Text);
            return true;
        default:
            return false;
        }
    }

    bool UITextInput::OnTextInput(std::string_view text)
    {
        if (!IsEnabled() || ReadOnly || text.empty())
            return false;

        m_Cursor = std::min(m_Cursor, Text.size());
        Text.insert(m_Cursor, text);
        m_Cursor += text.size();
        if (OnChanged)
            OnChanged(Text);
        return true;
    }

    UIVec2 UITextInput::ComputeDesiredSize()
    {
        const UIStyleBox& style = GetResolvedStyle();
        return { 160.0f, std::max(28.0f, style.FontSize + style.Padding.Vertical()) };
    }

    void UITextInput::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIStyleBox& style = GetResolvedStyle();
        const UIRect& b = GetBounds();
        const float textX = b.Min.x + style.Padding.Left;

        drawList.PushClipRect(b.Min + UIVec2(2.0f, 2.0f), b.Max - UIVec2(2.0f, 2.0f), style.CornerRadius.ToVec4());

        if (Text.empty() && !Placeholder.empty())
        {
            const UIVec2 size = UILabel::MeasureText(Placeholder, nullptr, style.FontSize);
            const float y = b.Min.y + (b.Height() - size.y) * 0.5f;
            const UIColor muted = theme.GetToken("TextMuted", { 0.62f, 0.67f, 0.76f, 0.8f });
            drawList.AddText(Placeholder, nullptr, { textX, y }, style.FontSize, muted);
        }
        else if (!Text.empty())
        {
            const UIVec2 size = UILabel::MeasureText(Text, nullptr, style.FontSize);
            const float y = b.Min.y + (b.Height() - size.y) * 0.5f;
            drawList.AddText(Text, nullptr, { textX, y }, style.FontSize, style.TextColor);
        }

        if (HasState(GetState(), UIState::Focused) && !ReadOnly)
        {
            const size_t clampedCursor = std::min(m_Cursor, Text.size());
            const float caretOffset = UILabel::MeasureText(std::string_view(Text).substr(0, clampedCursor), nullptr, style.FontSize).x;
            const float caretX = textX + caretOffset;
            const float caretTop = b.Min.y + 5.0f;
            const float caretBot = b.Max.y - 5.0f;
            const UIColor accent = theme.GetToken("Accent", { 0.24f, 0.56f, 0.96f, 1.0f });
            drawList.AddRoundedRect({ caretX, caretTop }, { caretX + 1.5f, caretBot }, accent);
        }

        drawList.PopClipRect();
    }
}
