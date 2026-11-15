#include "UITimeline.h"

#include <algorithm>
#include <cstdio>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    UITimeline::UITimeline(float duration, float currentTime)
        : m_Duration(std::max(0.1f, duration))
        , m_CurrentTime(std::clamp(currentTime, 0.0f, m_Duration))
    {
        StyleClass = "TextInput";
        DrawBackground = true;
        ClipChildren = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
        WidthMode = UISizeMode::Fill;
        HeightMode = UISizeMode::Fixed;
        FixedSize.y = 48.0f;
    }

    void UITimeline::SetDuration(float duration)
    {
        m_Duration = std::max(0.1f, duration);
        m_CurrentTime = std::clamp(m_CurrentTime, 0.0f, m_Duration);
    }

    void UITimeline::SetCurrentTime(float time, bool notify)
    {
        m_CurrentTime = std::clamp(time, 0.0f, m_Duration);
        if (notify && OnScrub)
            OnScrub(m_CurrentTime);
    }

    float UITimeline::TimeFromX(float x) const
    {
        const UIRect& b = GetBounds();
        const float pad = 10.0f;
        const float w = std::max(1.0f, b.Width() - pad * 2.0f);
        const float t = std::clamp((x - (b.Min.x + pad)) / w, 0.0f, 1.0f);
        return t * m_Duration;
    }

    bool UITimeline::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return false;
        m_Scrubbing = true;
        SetCurrentTime(TimeFromX(e.Position.x), true);
        return true;
    }

    bool UITimeline::OnPointerMove(const UIPointerEvent& e)
    {
        if (!m_Scrubbing)
            return false;
        SetCurrentTime(TimeFromX(e.Position.x), true);
        return true;
    }

    bool UITimeline::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == 1 && m_Scrubbing)
        {
            m_Scrubbing = false;
            return true;
        }
        return false;
    }

    UIVec2 UITimeline::ComputeDesiredSize()
    {
        return { 240.0f, FixedSize.y > 0.0f ? FixedSize.y : 48.0f };
    }

    void UITimeline::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIRect& b = GetBounds();
        const float pad = 10.0f;
        const float x0 = b.Min.x + pad;
        const float w = std::max(1.0f, b.Width() - pad * 2.0f);
        const Ref<Font> font = Font::GetDefault();

        // Ruler ticks
        constexpr int kTicks = 10;
        for (int i = 0; i <= kTicks; ++i)
        {
            const float tx = x0 + (static_cast<float>(i) / static_cast<float>(kTicks)) * w;
            const bool major = (i % 2 == 0);
            drawList.AddRoundedRect(
                { tx, b.Min.y + 4.0f },
                { tx + 1.0f, b.Min.y + (major ? 14.0f : 9.0f) },
                { 0.45f, 0.50f, 0.60f, 0.7f }
            );
        }

        // Keyframe track line
        const float trackY = b.Min.y + 30.0f;
        drawList.AddRoundedRect({ x0, trackY - 1.0f }, { x0 + w, trackY + 1.0f }, { 0.25f, 0.29f, 0.36f, 1.0f });

        // Keyframe markers
        for (float kf : m_Keyframes)
        {
            const float kx = x0 + std::clamp(kf / m_Duration, 0.0f, 1.0f) * w;
            drawList.AddRoundedRect(
                { kx - 4.0f, trackY - 4.0f },
                { kx + 4.0f, trackY + 4.0f },
                { 0.95f, 0.72f, 0.22f, 1.0f },
                glm::vec4(2.0f)
            );
        }

        // Playhead scrubber
        const float px = x0 + (m_CurrentTime / m_Duration) * w;
        drawList.AddRoundedRect({ px - 1.0f, b.Min.y + 2.0f }, { px + 1.0f, b.Max.y - 2.0f }, { 0.94f, 0.30f, 0.32f, 1.0f });
        drawList.AddRoundedRect({ px - 5.0f, b.Min.y + 2.0f }, { px + 5.0f, b.Min.y + 11.0f }, { 0.94f, 0.30f, 0.32f, 1.0f }, glm::vec4(2.0f));

        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%.2fs / %.2fs", m_CurrentTime, m_Duration);
        drawList.AddText(buf, font, { b.Max.x - 95.0f, b.Min.y + 4.0f }, 11.5f, { 0.78f, 0.84f, 0.92f, 0.9f });
    }
}
