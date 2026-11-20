#include "UISlider.h"

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
    UISlider::UISlider(float value, float min, float max, std::function<void(float)> onChanged)
        : Value(value), Min(min), Max(max), OnChanged(std::move(onChanged))
    {
        StyleClass = "Slider";
        DrawBackground = false;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
    }

    void UISlider::UpdateFromPointerX(float x)
    {
        const UIStyleBox& style = GetResolvedStyle();
        const float thumbR = std::max(5.0f, style.FontSize * 0.5f);
        float valueW = 0.0f;
        if (ShowValue)
        {
            const std::string text = std::vformat(Format, std::make_format_args(Value));
            valueW = UILabel::MeasureText(text, nullptr, style.FontSize).x + thumbR + 8.0f;
        }
        const UIRect& b = GetBounds();
        const float trackLeft = b.Min.x + thumbR;
        const float trackRight = b.Max.x - thumbR - valueW;
        const float span = std::max(1.0f, trackRight - trackLeft);
        const float t = std::clamp((x - trackLeft) / span, 0.0f, 1.0f);

        float next = Min + t * (Max - Min);
        if (Step > 0.0f)
            next = Min + std::round((next - Min) / Step) * Step;
        next = std::clamp(next, std::min(Min, Max), std::max(Min, Max));

        if (next != Value)
        {
            Value = next;
            if (OnChanged)
                OnChanged(Value);
        }
    }

    bool UISlider::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button == 1 && IsEnabled())
        {
            const uint64_t now = SDL_GetTicks();
            const bool dblClick = (now - m_LastClickTicks) < 400;
            m_LastClickTicks = now;

            const bool modClick = (e.Modifiers & (UIMod::Shift | UIMod::Ctrl)) != 0;

            if (modClick || dblClick)
            {
                BeginEdit();
                return true;
            }

            if (m_Editing)
                CommitEdit(true);

            m_Dragging = true;
            UpdateFromPointerX(e.Position.x);
            return true;
        }
        return UIElement::OnPointerDown(e);
    }

    bool UISlider::OnPointerMove(const UIPointerEvent& e)
    {
        if (m_Dragging && IsEnabled())
        {
            UpdateFromPointerX(e.Position.x);
            return true;
        }
        return UIElement::OnPointerMove(e);
    }

    bool UISlider::OnPointerUp(const UIPointerEvent& e)
    {
        if (m_Dragging && e.Button == 1)
        {
            m_Dragging = false;
            return true;
        }
        return UIElement::OnPointerUp(e);
    }

    bool UISlider::OnKeyDown(const UIKeyEvent& e)
    {
        if (!m_Editing || !IsEnabled())
            return UIElement::OnKeyDown(e);

        m_BlinkTimer = 0.0f;
        if (e.KeyCode == SDL_SCANCODE_A && (e.Modifiers & UIMod::Ctrl) != 0)
        {
            m_SelectAll = true;
            m_Cursor = m_EditText.size();
            return true;
        }

        switch (e.KeyCode)
        {
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            CommitEdit(true);
            return true;
        case SDL_SCANCODE_ESCAPE:
            CommitEdit(false);
            return true;
        case SDL_SCANCODE_BACKSPACE:
            if (m_SelectAll)
            {
                m_EditText.clear();
                m_Cursor = 0;
                m_SelectAll = false;
            }
            else if (m_Cursor > 0 && !m_EditText.empty())
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
        default:
            return true;
        }
    }

    bool UISlider::OnTextInput(std::string_view text)
    {
        if (!m_Editing || !IsEnabled())
            return false;

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

    void UISlider::OnUpdate(float deltaTime)
    {
        if (m_Editing)
        {
            m_BlinkTimer += deltaTime;
            if (!HasState(GetState(), UIState::Focused))
                CommitEdit(true);
        }
        UIElement::OnUpdate(deltaTime);
    }

    void UISlider::BeginEdit()
    {
        m_Editing = true;
        m_Dragging = false;
        m_SelectAll = true;
        m_BlinkTimer = 0.0f;
        m_EditText = std::format("{:.4g}", Value);
        m_Cursor = m_EditText.size();
    }

    void UISlider::CommitEdit(bool apply)
    {
        if (!m_Editing)
            return;
        m_Editing = false;
        m_SelectAll = false;

        if (apply && !m_EditText.empty())
        {
            char* endPtr = nullptr;
            const float parsed = std::strtof(m_EditText.c_str(), &endPtr);
            if (endPtr != m_EditText.c_str() && std::isfinite(parsed))
            {
                float clamped = std::clamp(parsed, Min, Max);
                if (Step > 0.0f)
                {
                    clamped = std::round((clamped - Min) / Step) * Step + Min;
                    clamped = std::clamp(clamped, Min, Max);
                }
                if (std::abs(clamped - Value) > 1e-6f)
                {
                    Value = clamped;
                    if (OnChanged)
                        OnChanged(Value);
                }
            }
        }
    }

    UIVec2 UISlider::ComputeDesiredSize()
    {
        const UIStyleBox& style = GetResolvedStyle();
        const float h = std::max(18.0f, style.FontSize * 1.65f);
        const float valueW = ShowValue ? (style.FontSize * 4.0f + 12.0f) : 0.0f;
        return { std::max(MinSize.x, 96.0f + valueW), h };
    }

    void UISlider::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        const UIStyleBox& style = GetResolvedStyle();
        const float trackH = std::max(4.0f, style.FontSize * 0.42f);
        const float thumbR = std::max(5.0f, style.FontSize * 0.5f);
        const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        const UIRect& b = GetBounds();
        const float cy = b.Min.y + b.Height() * 0.5f;

        if (m_Editing && !ShowValue)
        {
            const UIColor inputBg = theme.GetToken("InputBg", style.FillColor);
            const glm::vec4 cornerVec = { style.CornerRadius.TopLeft, style.CornerRadius.TopRight, style.CornerRadius.BottomRight, style.CornerRadius.BottomLeft };
            drawList.AddRoundedRect(b.Min, b.Max, inputBg, cornerVec, 1.0f, accent);
            const UIVec2 textSize = UILabel::MeasureText(m_EditText, nullptr, style.FontSize);
            const float textX = b.Min.x + std::max(4.0f, (b.Width() - textSize.x) * 0.5f);
            const float textY = cy - textSize.y * 0.5f;
            if (m_SelectAll && !m_EditText.empty())
            {
                drawList.AddRoundedRect(
                    { textX - 2.0f, b.Min.y + 3.0f },
                    { textX + textSize.x + 2.0f, b.Max.y - 3.0f },
                    { accent.r, accent.g, accent.b, 0.35f },
                    glm::vec4(2.0f)
                );
            }
            drawList.AddText(m_EditText, nullptr, { textX, textY }, style.FontSize, style.TextColor);
            if (std::fmod(m_BlinkTimer, 1.0f) < 0.55f)
            {
                const std::string beforeCursor = m_EditText.substr(0, std::min(m_Cursor, m_EditText.size()));
                const float cx = textX + UILabel::MeasureText(beforeCursor, nullptr, style.FontSize).x;
                drawList.AddRoundedRect({ cx, b.Min.y + 3.0f }, { cx + 1.5f, b.Max.y - 3.0f }, accent, glm::vec4(0.0f));
            }
            return;
        }

        const std::string text = m_Editing
            ? m_EditText
            : (ShowValue ? std::vformat(Format, std::make_format_args(Value)) : std::string{});
        const UIVec2 textSize = ShowValue ? UILabel::MeasureText(text, nullptr, style.FontSize) : UIVec2{};
        const float valueBoxW = ShowValue ? std::max(style.FontSize * 4.0f, textSize.x + 8.0f) : 0.0f;
        const float valueW = ShowValue ? (valueBoxW + thumbR + 8.0f) : 0.0f;

        const float trackLeft = b.Min.x + thumbR;
        const float trackRight = std::max(trackLeft + 10.0f, b.Max.x - thumbR - valueW);

        const float range = (Max - Min);
        const float t = std::abs(range) > 1e-6f ? std::clamp((Value - Min) / range, 0.0f, 1.0f) : 0.0f;
        const float thumbX = trackLeft + t * (trackRight - trackLeft);

        // Track background
        drawList.AddRoundedRect(
            { trackLeft, cy - trackH * 0.5f },
            { trackRight, cy + trackH * 0.5f },
            style.FillColor,
            glm::vec4(trackH * 0.5f),
            1.0f,
            style.BorderColor
        );

        // Filled active portion
        if (thumbX > trackLeft + 1.0f)
        {
            drawList.AddRoundedRect(
                { trackLeft, cy - trackH * 0.5f },
                { thumbX, cy + trackH * 0.5f },
                accent,
                glm::vec4(trackH * 0.5f)
            );
        }

        // Thumb
        const UIColor thumbColor = HasState(GetState(), UIState::Hovered) || m_Dragging
            ? UIColor{ 1.0f, 1.0f, 1.0f, 1.0f }
            : theme.GetToken("Text", { 0.918f, 0.929f, 0.953f, 1.0f });
        drawList.AddRoundedRect(
            { thumbX - thumbR, cy - thumbR },
            { thumbX + thumbR, cy + thumbR },
            thumbColor,
            glm::vec4(thumbR),
            1.5f,
            accent
        );

        if (ShowValue)
        {
            const float boxLeft = trackRight + thumbR + 6.0f;
            const float textY = cy - textSize.y * 0.5f;
            if (m_Editing)
            {
                const UIColor inputBg = theme.GetToken("InputBg", style.FillColor);
                const glm::vec4 cornerVec = { style.CornerRadius.TopLeft, style.CornerRadius.TopRight, style.CornerRadius.BottomRight, style.CornerRadius.BottomLeft };
                drawList.AddRoundedRect(
                    { boxLeft, b.Min.y + 1.0f },
                    { b.Max.x, b.Max.y - 1.0f },
                    inputBg,
                    cornerVec,
                    1.0f,
                    accent
                );
                const float textX = boxLeft + 4.0f;
                if (m_SelectAll && !text.empty())
                {
                    drawList.AddRoundedRect(
                        { textX - 2.0f, b.Min.y + 3.0f },
                        { textX + textSize.x + 2.0f, b.Max.y - 3.0f },
                        { accent.r, accent.g, accent.b, 0.35f },
                        glm::vec4(2.0f)
                    );
                }
                drawList.AddText(text, nullptr, { textX, textY }, style.FontSize, style.TextColor);
                if (std::fmod(m_BlinkTimer, 1.0f) < 0.55f)
                {
                    const std::string beforeCursor = text.substr(0, std::min(m_Cursor, text.size()));
                    const float cx = textX + UILabel::MeasureText(beforeCursor, nullptr, style.FontSize).x;
                    drawList.AddRoundedRect({ cx, b.Min.y + 3.0f }, { cx + 1.5f, b.Max.y - 3.0f }, accent, glm::vec4(0.0f));
                }
            }
            else
            {
                drawList.AddText(text, nullptr, { boxLeft, textY }, style.FontSize, style.TextColor);
            }
        }
    }
}
