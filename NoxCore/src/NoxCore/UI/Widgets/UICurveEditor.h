#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    // A curve (x -> y) to see and shape by its keys: drag a key (it stays between its neighbours), add one on empty space (a click,
    // or a double-click with AddOnDoubleClick), right-click a key to remove it.
    //
    // Range::Unit (the default): x and y in 0..1, the first and last key pinned to x = 0 and 1 and never removed, a 4 x 4 grid.
    // Range::FitKeys: any values; the view spans the keys with a margin (held still while a key is dragged), the x = 0 and y = 0
    // lines drawn, the range written in a corner; every key can be removed.
    //
    // The keys are the editor's own (SetPoints / GetPoints) or someone else's (Keys: a function giving the vector to edit, read
    // each time -- null: nothing to edit). The curve is drawn by sampling Evaluator when set (the curve's own interpolation),
    // else Evaluate (smooth steps between keys). OnChanging: a key dragged (each step). OnKeyCountChanged: a key added or removed.
    // OnCurveChanged: after either, with the keys.
    class UICurveEditor : public UIElement
    {
    public:
        enum class Range : uint8_t { Unit, FitKeys };

        explicit UICurveEditor(std::vector<UIVec2> points = { { 0.0f, 0.1f }, { 0.35f, 0.85f }, { 0.7f, 0.45f }, { 1.0f, 0.95f } });

        void SetPoints(std::vector<UIVec2> points);
        const std::vector<UIVec2>& GetPoints() const;
        float Evaluate(float t) const;

        Range RangeMode = Range::Unit;
        bool AddOnDoubleClick = false;
        std::function<std::vector<UIVec2>*()> Keys;
        std::function<float(float)> Evaluator;

        std::function<void()> OnChanging;
        std::function<void()> OnKeyCountChanged;
        std::function<void(const std::vector<UIVec2>&)> OnCurveChanged;

        // The span shown (FitKeys: follows the keys; Unit: 0..1).
        UIVec2 GetViewMin() const { return m_Low; }
        UIVec2 GetViewMax() const { return m_High; }
        int GetDraggedKey() const { return m_DraggingIndex; }

        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnUpdate(float dt) override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        std::vector<UIVec2>* Edited();
        const std::vector<UIVec2>* Shown() const;
        void UpdateRange();
        UIRect GetPlotRect() const;
        UIVec2 ToScreen(const UIVec2& p, const UIRect& r) const;
        UIVec2 ToCurve(const UIVec2& p, const UIRect& r) const;
        int KeyAt(const std::vector<UIVec2>& keys, const UIVec2& position, const UIRect& r) const;
        void Changed(bool countChanged);

        std::vector<UIVec2> m_Points;
        int m_DraggingIndex = -1;
        UIVec2 m_Low{ 0.0f }, m_High{ 1.0f };
        double m_LastClickTime = -1.0;
        double m_Time = 0.0;
    };
}
