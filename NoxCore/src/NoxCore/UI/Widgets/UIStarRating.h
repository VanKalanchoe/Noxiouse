#pragma once

#include <functional>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    // Interactive 1..N star/dot rating bar with hover preview and click selection.
    class UIStarRating : public UIElement
    {
    public:
        explicit UIStarRating(
            int maxRating = 5,
            int rating = 3,
            std::function<void(int)> onRatingChanged = {});

        void SetRating(int rating, bool notify = false);
        int GetRating() const { return m_Rating; }

        std::function<void(int)> OnRatingChanged;

        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerLeave(const UIPointerEvent& e) override;
        bool OnPointerDown(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        int HitIndex(const UIVec2& pos) const;

        int m_MaxRating = 5;
        int m_Rating = 3;
        int m_HoveredIndex = -1;
    };
}
