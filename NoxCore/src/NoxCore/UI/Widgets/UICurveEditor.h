#pragma once

#include <functional>
#include <vector>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    // Interactive 2D [0..1] curve editor with draggable keyframes, add/remove, and Evaluate(t).
    class UICurveEditor : public UIElement
    {
    public:
        explicit UICurveEditor(std::vector<UIVec2> points = { { 0.0f, 0.1f }, { 0.35f, 0.85f }, { 0.7f, 0.45f }, { 1.0f, 0.95f } });

        void SetPoints(std::vector<UIVec2> points);
        const std::vector<UIVec2>& GetPoints() const { return m_Points; }
        float Evaluate(float t) const;

        std::function<void(const std::vector<UIVec2>&)> OnCurveChanged;

        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        UIRect GetPlotRect() const;
        UIVec2 NormToScreen(const UIVec2& p, const UIRect& r) const;
        UIVec2 ScreenToNorm(const UIVec2& p, const UIRect& r) const;

        std::vector<UIVec2> m_Points;
        int m_DraggingIndex = -1;
    };
}
