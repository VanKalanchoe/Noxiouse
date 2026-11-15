#include "UIStarRating.h"

#include <algorithm>
#include <cmath>

#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    UIStarRating::UIStarRating(
        int maxRating,
        int rating,
        std::function<void(int)> onRatingChanged)
        : OnRatingChanged(std::move(onRatingChanged))
        , m_MaxRating(std::max(1, maxRating))
        , m_Rating(std::clamp(rating, 0, m_MaxRating))
    {
        DrawBackground = false;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
    }

    void UIStarRating::SetRating(int rating, bool notify)
    {
        const int clamped = std::clamp(rating, 0, m_MaxRating);
        if (m_Rating == clamped)
            return;
        m_Rating = clamped;
        if (notify && OnRatingChanged)
            OnRatingChanged(m_Rating);
    }

    int UIStarRating::HitIndex(const UIVec2& pos) const
    {
        const UIRect& b = GetBounds();
        if (!b.Contains(pos))
            return -1;
        const float cellW = std::max(18.0f, b.Width() / static_cast<float>(std::max(1, m_MaxRating)));
        const int idx = static_cast<int>((pos.x - b.Min.x) / cellW) + 1;
        return std::clamp(idx, 1, m_MaxRating);
    }

    bool UIStarRating::OnPointerMove(const UIPointerEvent& e)
    {
        m_HoveredIndex = HitIndex(e.Position);
        return false;
    }

    bool UIStarRating::OnPointerLeave(const UIPointerEvent&)
    {
        m_HoveredIndex = -1;
        return false;
    }

    bool UIStarRating::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return false;
        const int idx = HitIndex(e.Position);
        if (idx >= 1)
            SetRating(idx, true);
        return true;
    }

    UIVec2 UIStarRating::ComputeDesiredSize()
    {
        const UIStyleBox& style = GetResolvedStyle();
        const float cellW = std::max(18.0f, style.FontSize * 1.65f);
        return { static_cast<float>(m_MaxRating) * cellW, cellW };
    }

    void UIStarRating::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        const UIStyleBox& style = GetResolvedStyle();
        const UIRect& b = GetBounds();
        const float cellW = std::max(18.0f, b.Width() / static_cast<float>(std::max(1, m_MaxRating)));
        const int activeCount = (m_HoveredIndex >= 1) ? m_HoveredIndex : m_Rating;
        const float cy = (b.Min.y + b.Max.y) * 0.5f;
        const UIColor filledCol = theme.GetToken("Warning", { 0.851f, 0.616f, 0.224f, 1.0f });
        const UIColor emptyFill = theme.GetToken("Surface1", style.FillColor);
        const UIColor emptyBorder = theme.GetToken("Border", style.BorderColor);

        for (int i = 0; i < m_MaxRating; ++i)
        {
            const float cx = b.Min.x + static_cast<float>(i) * cellW + cellW * 0.5f;
            const bool filled = (i + 1) <= activeCount;
            const float r = std::min(cellW * 0.34f, filled ? style.FontSize * 0.55f : style.FontSize * 0.48f);

            drawList.AddRoundedRect(
                { cx - r, cy - r },
                { cx + r, cy + r },
                filled ? filledCol : emptyFill,
                glm::vec4(filled ? r * 0.5f : r),
                1.5f,
                filled ? filledCol : emptyBorder
            );
        }
    }
}
