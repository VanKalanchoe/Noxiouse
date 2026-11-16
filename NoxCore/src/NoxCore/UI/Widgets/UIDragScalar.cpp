#include "UIDragScalar.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    namespace
    {
        constexpr float kScrubThreshold = 3.0f; // pixels the pointer moves before a press is a drag and not a click
    }

    UIDragScalar::UIDragScalar(float value, float speed, std::function<void(float)> onChanged)
        : Value(value), Speed(speed), OnChanged(std::move(onChanged))
    {
        StyleClass = "DragScalar";
        DrawBackground = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
    }

    void UIDragScalar::SetValue(float next)
    {
        if (Step > 0.0f)
            next = std::round(next / Step) * Step;
        if (HasRange())
            next = std::clamp(next, Min, Max);
        if (next != Value)
        {
            Value = next;
            if (OnChanged)
                OnChanged(Value);
        }
    }

    std::string UIDragScalar::DisplayText() const
    {
        std::string text = std::vformat(Format, std::make_format_args(Value));
        if (TrimZeros)
        {
            const size_t dot = text.find('.');
            if (dot != std::string::npos && text.find_first_of("eE") == std::string::npos)
            {
                size_t end = text.size();
                while (end > dot + 2 && text[end - 1] == '0')
                    --end;
                text.resize(end);
            }
            if (text == "-0.0")
                text = "0.0";
        }
        return text;
    }

    float UIDragScalar::TextSize() const
    {
        return FontSize > 0.0f ? FontSize : GetResolvedStyle().FontSize;
    }

    float UIDragScalar::TextLeft() const
    {
        const UIStyleBox& style = GetResolvedStyle();
        const UIRect& b = GetBounds();
        if (Prefix.empty())
            return b.Min.x + style.Padding.Left + (Stripe.a > 0.0f ? 3.0f : 0.0f);
        const UIVec2 pSize = UILabel::MeasureText(Prefix, nullptr, TextSize());
        const float tagW = std::max(TextSize() * 1.25f, pSize.x + style.Padding.Left * 1.4f);
        return b.Min.x + tagW + 6.0f;
    }

    void UIDragScalar::BeginEdit()
    {
        m_Editing = true;
        m_Pressed = false;
        m_Scrubbing = false;
        m_SelectAll = true;
        m_BlinkTimer = 0.0f;
        // The value itself, not its rounded display: what is typed over is what the field really holds.
        m_EditText = Step >= 1.0f ? std::format("{:.0f}", Value) : std::format("{}", Value);
        m_Cursor = m_EditText.size();
    }

    void UIDragScalar::CommitEdit(bool apply)
    {
        if (!m_Editing)
            return;
        m_Editing = false;
        m_SelectAll = false;
        if (!apply || m_EditText.empty())
            return;

        char* endPtr = nullptr;
        const float parsed = std::strtof(m_EditText.c_str(), &endPtr);
        if (endPtr != m_EditText.c_str() && std::isfinite(parsed))
            SetValue(parsed);
    }

    void UIDragScalar::OnUpdate(float dt)
    {
        if (m_Editing)
        {
            m_BlinkTimer += dt;
            if (!HasState(GetState(), UIState::Focused))
                CommitEdit();
        }
        // The button was released somewhere the field did not hear of (the pointer capture was taken away).
        if (m_Pressed && !HasState(GetState(), UIState::Pressed))
        {
            m_Pressed = false;
            m_Scrubbing = false;
        }
    }

    void UIDragScalar::OnFocusChanged(bool focused)
    {
        // Leaving the field applies what was typed, right away (its popup may be gone by the next frame).
        if (!focused)
            CommitEdit(true);
    }

    UICursorShape UIDragScalar::GetCursorShape(const UIVec2&) const
    {
        return (m_Editing || !IsEnabled()) ? UICursorShape::Default : UICursorShape::ResizeEW;
    }

    bool UIDragScalar::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return UIElement::OnPointerDown(e);

        if (m_Editing)
        {
            // A click in the text puts the caret between the two nearest characters.
            const UIStyleBox& style = GetResolvedStyle();
            const float x = e.Position.x - TextLeft();
            size_t best = 0;
            float bestDist = std::abs(x);
            for (size_t i = 1; i <= m_EditText.size(); ++i)
            {
                const float w = UILabel::MeasureText(std::string_view(m_EditText).substr(0, i), nullptr, TextSize()).x;
                if (std::abs(x - w) < bestDist)
                {
                    bestDist = std::abs(x - w);
                    best = i;
                }
            }
            m_Cursor = best;
            m_SelectAll = false;
            m_BlinkTimer = 0.0f;
            return true;
        }

        m_Pressed = true;
        m_Scrubbing = false;
        m_PressX = e.Position.x;
        m_ScrubValue = Value;
        // The field's own drag: not a drag-and-drop of whatever it sits in.
        if (UIContext* ctx = UIContext::GetCurrent())
            ctx->CancelPendingDrag();
        return true;
    }

    bool UIDragScalar::OnPointerMove(const UIPointerEvent& e)
    {
        if (!m_Pressed || m_Editing || !IsEnabled())
            return UIElement::OnPointerMove(e);

        if (!m_Scrubbing)
        {
            if (std::abs(e.Position.x - m_PressX) < kScrubThreshold)
                return true;
            m_Scrubbing = true; // from here on; the threshold's pixels are not applied, so the value does not jump
            return true;
        }

        // With a fill bar the field's width is the range; without, Speed per pixel.
        float perPixel = Speed;
        if (ShowFill && HasRange())
            perPixel = (Max - Min) / std::max(60.0f, GetBounds().Width());
        if ((SDL_GetModState() & SDL_KMOD_SHIFT) != 0)
            perPixel *= 0.1f;

        m_ScrubValue += e.Delta.x * perPixel;
        if (HasRange())
            m_ScrubValue = std::clamp(m_ScrubValue, Min, Max);
        SetValue(m_ScrubValue);
        return true;
    }

    bool UIDragScalar::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !m_Pressed)
            return UIElement::OnPointerUp(e);

        const bool clicked = !m_Scrubbing && GetBounds().Contains(e.Position);
        m_Pressed = false;
        m_Scrubbing = false;
        if (clicked && IsEnabled())
            BeginEdit();
        return true;
    }

    bool UIDragScalar::OnKeyDown(const UIKeyEvent& e)
    {
        if (!IsEnabled())
            return false;

        if (!m_Editing)
        {
            if (e.KeyCode == SDL_SCANCODE_RETURN || e.KeyCode == SDL_SCANCODE_KP_ENTER || e.KeyCode == SDL_SCANCODE_F2)
            {
                BeginEdit();
                return true;
            }
            return false;
        }

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
            CommitEdit(true);
            return true;
        case SDL_SCANCODE_ESCAPE:
            CommitEdit(false);
            return true;
        default:
            return false;
        }
    }

    bool UIDragScalar::OnTextInput(std::string_view text)
    {
        if (!IsEnabled())
            return false;

        auto accepted = [](char c)
        {
            return (c >= '0' && c <= '9') || c == '.' || c == ',' || c == '-' || c == '+' || c == 'e' || c == 'E';
        };

        if (!m_Editing)
        {
            // Focused by Tab and a number is typed: that starts the edit (and replaces the value).
            const bool startsNumber = !text.empty() && ((text[0] >= '0' && text[0] <= '9') || text[0] == '-' || text[0] == '.' || text[0] == ',');
            if (!startsNumber || !HasState(GetState(), UIState::Focused))
                return false;
            BeginEdit();
        }

        m_Cursor = std::min(m_Cursor, m_EditText.size());
        for (char c : text)
        {
            if (!accepted(c))
                continue;
            if (c == ',')
                c = '.'; // a keyboard whose decimal key types a comma
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
        return true;
    }

    UIVec2 UIDragScalar::ComputeDesiredSize()
    {
        const UIStyleBox& style = GetResolvedStyle();
        float tagW = 0.0f;
        if (!Prefix.empty())
        {
            const UIVec2 pSize = UILabel::MeasureText(Prefix, nullptr, TextSize());
            tagW = std::max(TextSize() * 1.25f, pSize.x + style.Padding.Left * 1.4f) + 6.0f;
        }
        return {
            std::max(MinSize.x, tagW + TextSize() * 2.2f + style.Padding.Horizontal()),
            TextSize() + style.Padding.Vertical()
        };
    }

    void UIDragScalar::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIStyleBox& style = GetResolvedStyle();
        const UIRect& b = GetBounds();
        const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        const glm::vec4 radii = style.CornerRadius.ToVec4();
        float tagRight = b.Min.x;

        if (!Prefix.empty())
        {
            const UIVec2 pSize = UILabel::MeasureText(Prefix, nullptr, TextSize());
            const float tagW = std::max(TextSize() * 1.25f, pSize.x + style.Padding.Left * 1.4f);
            drawList.AddRoundedRect(
                b.Min,
                { b.Min.x + tagW, b.Max.y },
                PrefixColor,
                { style.CornerRadius.TopLeft, 0.0f, 0.0f, style.CornerRadius.BottomLeft }
            );
            drawList.AddText(
                Prefix, nullptr,
                { b.Min.x + (tagW - pSize.x) * 0.5f, b.Min.y + (b.Height() - pSize.y) * 0.5f },
                TextSize(),
                { 1.0f, 1.0f, 1.0f, 1.0f }
            );
            tagRight = b.Min.x + tagW;
        }
        const float textLeft = TextLeft();

        if (Stripe.a > 0.0f && Prefix.empty())
            drawList.AddRoundedRect({ b.Min.x + 1.0f, b.Min.y + 1.0f }, { b.Min.x + 4.0f, b.Max.y - 1.0f }, Stripe, { radii.x, 0.0f, 0.0f, radii.w });

        // The slider in the field: its background filled up to the value.
        if (ShowFill && HasRange() && !m_Editing)
        {
            const float t = std::clamp((Value - Min) / (Max - Min), 0.0f, 1.0f);
            const float left = tagRight + (Prefix.empty() ? 1.0f : 0.0f);
            const float right = left + (b.Max.x - 1.0f - left) * t;
            if (right > left + 0.5f)
            {
                const bool lit = m_Scrubbing || HasState(GetState(), UIState::Hovered);
                const bool full = t > 0.995f;
                drawList.AddRoundedRect(
                    { left, b.Min.y + 1.0f }, { right, b.Max.y - 1.0f },
                    { accent.r, accent.g, accent.b, lit ? 0.50f : 0.36f },
                    { Prefix.empty() ? radii.x : 0.0f, full ? radii.y : 0.0f, full ? radii.z : 0.0f, Prefix.empty() ? radii.w : 0.0f });
                // The value's edge, so the end of the fill reads as a handle.
                if (!full)
                    drawList.AddRoundedRect({ right - 1.5f, b.Min.y + 2.0f }, { right, b.Max.y - 2.0f }, { accent.r, accent.g, accent.b, lit ? 1.0f : 0.85f });
            }
        }

        if (m_Editing)
            drawList.AddRoundedRect(b.Min, b.Max, { 0.0f, 0.0f, 0.0f, 0.0f }, radii, 1.5f, accent);

        drawList.PushClipRect({ textLeft - 2.0f, b.Min.y + 1.0f }, { b.Max.x - 2.0f, b.Max.y - 1.0f }, radii);

        std::string display = m_Editing ? m_EditText : DisplayText();
        if (!m_Editing)
        {
            // Too wide for the field (a narrow inspector): fewer decimals until it fits -- 123.456 -> 123.46 -> 123.5 -> 123 --
            // so the whole number stays readable; typing into the field shows every digit.
            const float available = (b.Max.x - 2.0f) - textLeft;
            const size_t dot = display.find('.');
            if (dot != std::string::npos && display.find_first_of("eE") == std::string::npos &&
                UILabel::MeasureText(display, nullptr, TextSize()).x > available)
            {
                for (int decimals = static_cast<int>(display.size() - dot - 1) - 1; decimals >= 0; --decimals)
                {
                    std::string shorter = std::vformat("{:." + std::to_string(decimals) + "f}", std::make_format_args(Value));
                    if (shorter == "-0" || shorter == "-0.0")
                        shorter.erase(0, 1);
                    display = std::move(shorter);
                    if (UILabel::MeasureText(display, nullptr, TextSize()).x <= available)
                        break;
                }
            }
        }
        const UIVec2 textSize = UILabel::MeasureText(display, nullptr, TextSize());
        const float textY = b.Min.y + (b.Height() - textSize.y) * 0.5f;

        if (m_Editing && m_SelectAll && !display.empty())
        {
            drawList.AddRoundedRect(
                { textLeft - 2.0f, b.Min.y + 3.0f },
                { textLeft + textSize.x + 2.0f, b.Max.y - 3.0f },
                { accent.r, accent.g, accent.b, 0.35f },
                glm::vec4(2.0f)
            );
        }

        drawList.AddText(display, nullptr, { textLeft, textY }, TextSize(), style.TextColor);

        if (m_Editing && std::fmod(m_BlinkTimer, 1.0f) < 0.55f)
        {
            const size_t clampedCursor = std::min(m_Cursor, m_EditText.size());
            const float caretOffset = UILabel::MeasureText(std::string_view(m_EditText).substr(0, clampedCursor), nullptr, TextSize()).x;
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
