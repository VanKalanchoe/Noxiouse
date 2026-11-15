#include "UITimeline.h"

#include <algorithm>
#include <cstdio>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

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

        const UIStyleBox& style = GetResolvedStyle();
        const UIRect& b = GetBounds();
        const float pad = 10.0f;
        const float x0 = b.Min.x + pad;
        const float w = std::max(1.0f, b.Width() - pad * 2.0f);
        const Ref<Font> font = Font::GetDefault();
        const float fontSize = std::max(10.0f, style.FontSize * 0.82f);

        const UIColor tickCol = theme.GetToken("Border", { 0.35f, 0.40f, 0.50f, 0.7f });
        const UIColor trackCol = theme.GetToken("Surface2", { 0.184f, 0.194f, 0.227f, 1.0f });
        const UIColor kfCol = theme.GetToken("Warning", { 0.851f, 0.616f, 0.224f, 1.0f });
        const UIColor playheadCol = theme.GetToken("Danger", { 0.835f, 0.310f, 0.333f, 1.0f });
        const UIColor textCol = theme.GetToken("TextMuted", { 0.580f, 0.616f, 0.686f, 1.0f });

        // Ruler ticks
        constexpr int kTicks = 10;
        for (int i = 0; i <= kTicks; ++i)
        {
            const float tx = x0 + (static_cast<float>(i) / static_cast<float>(kTicks)) * w;
            const bool major = (i % 2 == 0);
            drawList.AddRoundedRect(
                { tx, b.Min.y + 4.0f },
                { tx + 1.0f, b.Min.y + (major ? 14.0f : 9.0f) },
                tickCol
            );
        }

        // Keyframe track line
        const float trackY = b.Min.y + b.Height() * 0.62f;
        drawList.AddRoundedRect({ x0, trackY - 1.0f }, { x0 + w, trackY + 1.0f }, trackCol);

        // Keyframe markers
        for (float kf : m_Keyframes)
        {
            const float kx = x0 + std::clamp(kf / m_Duration, 0.0f, 1.0f) * w;
            drawList.AddRoundedRect(
                { kx - 4.0f, trackY - 4.0f },
                { kx + 4.0f, trackY + 4.0f },
                kfCol,
                glm::vec4(2.0f)
            );
        }

        // Playhead scrubber
        const float px = x0 + (m_CurrentTime / m_Duration) * w;
        drawList.AddRoundedRect({ px - 1.0f, b.Min.y + 2.0f }, { px + 1.0f, b.Max.y - 2.0f }, playheadCol);
        drawList.AddRoundedRect({ px - 5.0f, b.Min.y + 2.0f }, { px + 5.0f, b.Min.y + 11.0f }, playheadCol, glm::vec4(2.0f));

        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%.2fs / %.2fs", m_CurrentTime, m_Duration);
        const UIVec2 tSz = UILabel::MeasureText(buf, font, fontSize);
        drawList.AddText(buf, font, { b.Max.x - pad - tSz.x, b.Min.y + 4.0f }, fontSize, textCol);
    }
}
