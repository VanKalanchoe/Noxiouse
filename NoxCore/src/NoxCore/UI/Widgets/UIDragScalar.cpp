#include "UIDragScalar.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIDragScalar::UIDragScalar(float value, float speed, std::function<void(float)> onChanged)
        : Value(value), Speed(speed), OnChanged(std::move(onChanged))
    {
        StyleClass = "DragScalar";
        DrawBackground = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
    }

    void UIDragScalar::BeginEdit()
    {
        m_Editing = true;
        m_Dragging = false;
        m_SelectAll = true;
        m_BlinkTimer = 0.0f;
        m_EditText = std::vformat(Format, std::make_format_args(Value));
        m_Cursor = m_EditText.size();
    }

    void UIDragScalar::CommitEdit()
    {
        if (!m_Editing)
            return;
        m_Editing = false;
        m_SelectAll = false;
        if (!m_EditText.empty())
        {
            char* endPtr = nullptr;
            const float parsed = std::strtof(m_EditText.c_str(), &endPtr);
            if (endPtr != m_EditText.c_str())
            {
                float next = parsed;
                if (Max > Min)
                    next = std::clamp(next, Min, Max);
                if (next != Value)
                {
                    Value = next;
                    if (OnChanged)
                        OnChanged(Value);
                }
            }
        }
    }

    void UIDragScalar::OnUpdate(float dt)
    {
        if (m_Editing)
        {
            m_BlinkTimer += dt;
            if (!HasState(GetState(), UIState::Focused))
                CommitEdit();
        }
    }

    bool UIDragScalar::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button == 1 && IsEnabled())
        {
            const SDL_Keymod mods = SDL_GetModState();
            const uint64_t now = SDL_GetTicks();
            const bool doubleClick = (now - m_LastClickTicks) < 350;
            m_LastClickTicks = now;

            if (m_Editing)
                return true;

            if ((mods & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL)) != 0 || doubleClick)
            {
                BeginEdit();
                return true;
            }

            m_Dragging = true;
            return true;
        }
        return UIElement::OnPointerDown(e);
    }

    bool UIDragScalar::OnPointerMove(const UIPointerEvent& e)
    {
        if (m_Dragging && !m_Editing && IsEnabled())
        {
            float next = Value + e.Delta.x * Speed;
            if (Max > Min)
                next = std::clamp(next, Min, Max);
            if (next != Value)
            {
                Value = next;
                if (OnChanged)
                    OnChanged(Value);
            }
            return true;
        }
        return UIElement::OnPointerMove(e);
    }

    bool UIDragScalar::OnPointerUp(const UIPointerEvent& e)
    {
        if (m_Dragging && e.Button == 1)
        {
            m_Dragging = false;
            return true;
        }
        return UIElement::OnPointerUp(e);
    }

    bool UIDragScalar::OnKeyDown(const UIKeyEvent& e)
    {
        if (!m_Editing || !IsEnabled())
            return false;

        m_Cursor = std::min(m_Cursor, m_EditText.size());
        m_BlinkTimer = 0.0f;

        if (e.KeyCode == SDL_SCANCODE_A && (SDL_GetModState() & SDL_KMOD_CTRL) != 0)
        {
            m_SelectAll = true;
            m_Cursor = m_EditText.size();
            return true;
        }

        switch (e.KeyCode)
        {
        case SDL_SCANCODE_LEFT:
            if (m_SelectAll) { m_Cursor = 0; m_SelectAll = false; }
            else if (m_Cursor > 0) --m_Cursor;
            return true;
        case SDL_SCANCODE_RIGHT:
            if (m_SelectAll) { m_Cursor = m_EditText.size(); m_SelectAll = false; }
            else if (m_Cursor < m_EditText.size()) ++m_Cursor;
            return true;
        case SDL_SCANCODE_HOME:
            m_SelectAll = false;
            m_Cursor = 0;
            return true;
        case SDL_SCANCODE_END:
            m_SelectAll = false;
            m_Cursor = m_EditText.size();
            return true;
        case SDL_SCANCODE_BACKSPACE:
            if (m_SelectAll)
            {
                m_EditText.clear();
                m_Cursor = 0;
                m_SelectAll = false;
            }
            else if (m_Cursor > 0)
            {
                m_EditText.erase(m_Cursor - 1, 1);
                --m_Cursor;
            }
            return true;
        case SDL_SCANCODE_DELETE:
            if (m_SelectAll)
            {
                m_EditText.clear();
                m_Cursor = 0;
                m_SelectAll = false;
            }
            else if (m_Cursor < m_EditText.size())
                m_EditText.erase(m_Cursor, 1);
            return true;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            CommitEdit();
            return true;
        case SDL_SCANCODE_ESCAPE:
            m_Editing = false;
            m_SelectAll = false;
            return true;
        default:
            return false;
        }
    }

    bool UIDragScalar::OnTextInput(std::string_view text)
    {
        if (!m_Editing || !IsEnabled())
            return false;

        m_Cursor = std::min(m_Cursor, m_EditText.size());
        for (char c : text)
        {
            if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')
            {
                if (m_SelectAll)
                {
                    m_EditText.clear();
                    m_Cursor = 0;
                    m_SelectAll = false;
                }
                m_EditText.insert(m_Cursor, 1, c);
                ++m_Cursor;
                m_BlinkTimer = 0.0f;
            }
        }
        return true;
    }

    UIVec2 UIDragScalar::ComputeDesiredSize()
    {
        const UIStyleBox& style = GetResolvedStyle();
        float tagW = 0.0f;
        if (!Prefix.empty())
        {
            const UIVec2 pSize = UILabel::MeasureText(Prefix, nullptr, style.FontSize);
            tagW = std::max(style.FontSize * 1.25f, pSize.x + style.Padding.Left * 1.4f) + 6.0f;
        }
        return {
            std::max(MinSize.x, tagW + style.FontSize * 2.2f + style.Padding.Horizontal()),
            style.FontSize + style.Padding.Vertical()
        };
    }

    void UIDragScalar::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIStyleBox& style = GetResolvedStyle();
        const UIRect& b = GetBounds();
        float textLeft = b.Min.x + style.Padding.Left;

        if (!Prefix.empty())
        {
            const UIVec2 pSize = UILabel::MeasureText(Prefix, nullptr, style.FontSize);
            const float tagW = std::max(style.FontSize * 1.25f, pSize.x + style.Padding.Left * 1.4f);
            drawList.AddRoundedRect(
                b.Min,
                { b.Min.x + tagW, b.Max.y },
                PrefixColor,
                { style.CornerRadius.TopLeft, 0.0f, 0.0f, style.CornerRadius.BottomLeft }
            );
            drawList.AddText(
                Prefix, nullptr,
                { b.Min.x + (tagW - pSize.x) * 0.5f, b.Min.y + (b.Height() - pSize.y) * 0.5f },
                style.FontSize,
                { 1.0f, 1.0f, 1.0f, 1.0f }
            );
            textLeft = b.Min.x + tagW + 6.0f;
        }

        drawList.PushClipRect({ textLeft - 2.0f, b.Min.y + 1.0f }, { b.Max.x - 2.0f, b.Max.y - 1.0f }, style.CornerRadius.ToVec4());

        const std::string display = m_Editing ? m_EditText : std::vformat(Format, std::make_format_args(Value));
        const UIVec2 textSize = UILabel::MeasureText(display, nullptr, style.FontSize);
        const float textY = b.Min.y + (b.Height() - textSize.y) * 0.5f;
        const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });

        if (m_Editing && m_SelectAll && !display.empty())
        {
            drawList.AddRoundedRect(
                { textLeft - 2.0f, b.Min.y + 3.0f },
                { textLeft + textSize.x + 2.0f, b.Max.y - 3.0f },
                { accent.r, accent.g, accent.b, 0.35f },
                glm::vec4(2.0f)
            );
        }

        drawList.AddText(display, nullptr, { textLeft, textY }, style.FontSize, style.TextColor);

        if (m_Editing && std::fmod(m_BlinkTimer, 1.0f) < 0.55f)
        {
            const size_t clampedCursor = std::min(m_Cursor, m_EditText.size());
            const float caretOffset = UILabel::MeasureText(std::string_view(m_EditText).substr(0, clampedCursor), nullptr, style.FontSize).x;
            const float caretX = textLeft + caretOffset;
            drawList.AddRoundedRect(
                { caretX, b.Min.y + 4.0f },
                { caretX + 1.5f, b.Max.y - 4.0f },
                accent
            );
        }

        drawList.PopClipRect();
    }
}
