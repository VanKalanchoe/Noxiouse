#include "UIScrollBar.h"

#include <algorithm>
#include <cmath>

#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    void UIScrollBar::SetGeometry(const UIRect& viewRect, float contentSize, float viewportSize)
    {
        m_View = viewRect;
        m_Content = std::max(0.0f, contentSize);
        m_Viewport = std::max(0.0f, viewportSize);
        if (!IsNeeded())
            m_Dragging = false;
    }

    float UIScrollBar::ThumbLength() const
    {
        const float track = TrackLength();
        if (m_Content <= 0.0f)
            return track;
        return std::clamp(track * (m_Viewport / m_Content), std::min(kMinThumb, track), track);
    }

    UIRect UIScrollBar::GetLaneRect() const
    {
        return { { m_View.Max.x - kLaneWidth, m_View.Min.y }, { m_View.Max.x, m_View.Max.y } };
    }

    UIRect UIScrollBar::GetThumbRect(float scrollOffset) const
    {
        const float maxScroll = GetMaxScroll();
        const float t = maxScroll > 0.0f ? std::clamp(scrollOffset / maxScroll, 0.0f, 1.0f) : 0.0f;
        const float thumb = ThumbLength();
        const float top = TrackTop() + t * (TrackLength() - thumb);
        return { { m_View.Max.x - kLaneWidth, top }, { m_View.Max.x, top + thumb } };
    }

    bool UIScrollBar::OnPointerDown(const UIVec2& pos, float& scrollOffset)
    {
        if (!Contains(pos))
            return false;

        const UIRect thumb = GetThumbRect(scrollOffset);
        m_Dragging = true;
        if (pos.y >= thumb.Min.y && pos.y <= thumb.Max.y)
        {
            // Grabbed where it was pressed: the content does not move until the pointer does.
            m_GrabOffset = pos.y - thumb.Min.y;
        }
        else
        {
            // Beside the thumb: its middle comes under the pointer, and the drag goes on from there.
            m_GrabOffset = thumb.Height() * 0.5f;
            OnPointerMove(pos, scrollOffset);
        }
        return true;
    }

    bool UIScrollBar::OnPointerMove(const UIVec2& pos, float& scrollOffset)
    {
        if (!m_Dragging)
            return false;

        const float usable = TrackLength() - ThumbLength();
        const float t = usable > 0.5f ? std::clamp((pos.y - m_GrabOffset - TrackTop()) / usable, 0.0f, 1.0f) : 0.0f;
        scrollOffset = t * GetMaxScroll();
        return true;
    }

    bool UIScrollBar::OnPointerUp()
    {
        if (!m_Dragging)
            return false;
        m_Dragging = false;
        m_Activity = 1.0f; // stays lit for a moment after the release
        return true;
    }

    void UIScrollBar::Update(float dt, bool hovered)
    {
        const float target = (hovered || m_Dragging) ? 1.0f : 0.0f;
        const float rate = 1.0f - std::exp(-std::max(0.0f, dt) / 0.06f);
        m_Hover += (target - m_Hover) * rate;
        if (std::abs(target - m_Hover) < 0.002f)
            m_Hover = target;
        // Held for a moment, then faded out.
        m_Activity = std::max(0.0f, m_Activity - std::max(0.0f, dt) / 0.9f);
    }

    void UIScrollBar::Paint(UIDrawList& drawList, const UITheme& theme, float scrollOffset) const
    {
        if (!IsNeeded())
            return;

        const UIColor idle = theme.GetToken("ScrollThumb", theme.GetToken("Border", { 0.20f, 0.235f, 0.32f, 1.0f }));
        const UIColor lit = theme.GetToken("ScrollThumbHover", theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f }));
        const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });

        // The lane shows itself while the pointer is on it, so the bar reads as something to grab.
        if (m_Hover > 0.01f)
        {
            const UIRect lane = GetLaneRect();
            const UIColor track = theme.GetToken("Background", { 0.063f, 0.075f, 0.106f, 1.0f });
            drawList.AddRoundedRect(
                { lane.Min.x + 1.0f, lane.Min.y + 1.0f }, { lane.Max.x - 1.0f, lane.Max.y - 1.0f },
                { track.r, track.g, track.b, 0.55f * m_Hover },
                glm::vec4((kLaneWidth - 2.0f) * 0.5f));
        }

        const float glow = std::clamp(std::max(m_Hover, std::min(1.0f, m_Activity * 1.6f)), 0.0f, 1.0f);
        UIColor color = glm::mix(idle, lit, glow);
        if (m_Dragging)
            color = accent;

        const UIRect thumb = GetThumbRect(scrollOffset);
        const float width = glm::mix(5.0f, 8.0f, m_Hover);
        const float right = thumb.Max.x - 2.0f;
        drawList.AddRoundedRect({ right - width, thumb.Min.y }, { right, thumb.Max.y }, color, glm::vec4(width * 0.5f));
    }
}
