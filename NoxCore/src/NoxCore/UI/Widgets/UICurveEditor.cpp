#include "UICurveEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    namespace
    {
        constexpr double kDoubleClickSeconds = 0.35;
        constexpr float kKeyPickRadius = 8.0f;
        bool ByX(const UIVec2& a, const UIVec2& b) { return a.x < b.x; }
    }

    UICurveEditor::UICurveEditor(std::vector<UIVec2> points)
    {
        StyleClass = "TextInput";
        DrawBackground = true;
        ClipChildren = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
        WidthMode = UISizeMode::Fill;
        MinSize = { 200.0f, 130.0f };
        Padding = UIThickness::All(10.0f);
        SetPoints(std::move(points));
    }

    void UICurveEditor::SetPoints(std::vector<UIVec2> points)
    {
        m_Points = std::move(points);
        if (RangeMode == Range::Unit)
            for (auto& p : m_Points)
            {
                p.x = std::clamp(p.x, 0.0f, 1.0f);
                p.y = std::clamp(p.y, 0.0f, 1.0f);
            }
        std::sort(m_Points.begin(), m_Points.end(), ByX);
        m_DraggingIndex = -1;
    }

    std::vector<UIVec2>* UICurveEditor::Edited()
    {
        return Keys ? Keys() : &m_Points;
    }

    const std::vector<UIVec2>* UICurveEditor::Shown() const
    {
        return Keys ? Keys() : &m_Points;
    }

    const std::vector<UIVec2>& UICurveEditor::GetPoints() const
    {
        static const std::vector<UIVec2> s_None;
        const std::vector<UIVec2>* keys = Shown();
        return keys ? *keys : s_None;
    }

    float UICurveEditor::Evaluate(float t) const
    {
        if (Evaluator)
            return Evaluator(t);
        const std::vector<UIVec2>& points = GetPoints();
        if (points.empty())
            return 0.0f;
        if (RangeMode == Range::Unit)
            t = std::clamp(t, 0.0f, 1.0f);
        if (t <= points.front().x)
            return points.front().y;
        if (t >= points.back().x)
            return points.back().y;
        for (size_t i = 1; i < points.size(); ++i)
        {
            if (t <= points[i].x)
            {
                const float span = std::max(1e-5f, points[i].x - points[i - 1].x);
                const float u = (t - points[i - 1].x) / span;
                const float smooth = u * u * (3.0f - 2.0f * u);
                return points[i - 1].y + (points[i].y - points[i - 1].y) * smooth;
            }
        }
        return points.back().y;
    }

    void UICurveEditor::UpdateRange()
    {
        if (RangeMode == Range::Unit)
        {
            m_Low = UIVec2{ 0.0f };
            m_High = UIVec2{ 1.0f };
            return;
        }
        if (m_DraggingIndex >= 0)
            return; // (held still while a key moves: it would run away from the pointer)
        // The keys' span with a margin (one key, or all at one height: a unit around them).
        UIVec2 low{ 0.0f }, high{ 1.0f };
        const std::vector<UIVec2>& keys = GetPoints();
        if (!keys.empty())
        {
            low = high = keys.front();
            for (const UIVec2& key : keys)
            {
                low = glm::min(low, key);
                high = glm::max(high, key);
            }
        }
        for (int axis = 0; axis < 2; ++axis)
            if (high[axis] - low[axis] < 1e-4f)
            {
                low[axis] -= 0.5f;
                high[axis] += 0.5f;
            }
        const UIVec2 margin = (high - low) * 0.12f;
        m_Low = low - margin;
        m_High = high + margin;
    }

    UIRect UICurveEditor::GetPlotRect() const
    {
        return GetBounds().Inset(Padding);
    }

    UIVec2 UICurveEditor::ToScreen(const UIVec2& p, const UIRect& r) const
    {
        const UIVec2 t = (p - m_Low) / glm::max(m_High - m_Low, UIVec2(1e-6f));
        return { r.Min.x + t.x * r.Width(), r.Max.y - t.y * r.Height() };
    }

    UIVec2 UICurveEditor::ToCurve(const UIVec2& p, const UIRect& r) const
    {
        const UIVec2 t{ (p.x - r.Min.x) / std::max(1.0f, r.Width()), (r.Max.y - p.y) / std::max(1.0f, r.Height()) };
        return m_Low + glm::clamp(t, 0.0f, 1.0f) * (m_High - m_Low);
    }

    int UICurveEditor::KeyAt(const std::vector<UIVec2>& keys, const UIVec2& position, const UIRect& r) const
    {
        int hit = -1;
        float best = kKeyPickRadius;
        for (int i = 0; i < static_cast<int>(keys.size()); ++i)
        {
            const float distance = glm::length(ToScreen(keys[static_cast<size_t>(i)], r) - position);
            if (distance <= best)
            {
                best = distance;
                hit = i;
            }
        }
        return hit;
    }

    void UICurveEditor::Changed(bool countChanged)
    {
        // Copied first: a callback may rebuild what holds this editor.
        auto onChanging = OnChanging;
        auto onCount = OnKeyCountChanged;
        auto onCurve = OnCurveChanged;
        if (countChanged ? static_cast<bool>(onCount) : static_cast<bool>(onChanging))
            countChanged ? onCount() : onChanging();
        if (onCurve)
            onCurve(GetPoints());
    }

    bool UICurveEditor::OnPointerDown(const UIPointerEvent& e)
    {
        if (!IsEnabled() || !GetBounds().Contains(e.Position))
            return false;
        std::vector<UIVec2>* keys = Edited();
        if (!keys)
            return true;
        if (UIContext* ctx = UIContext::GetCurrent())
            ctx->CancelPendingDrag(); // (the curve's own drag: not a drag-and-drop of what it sits in)
        UpdateRange();
        const UIRect r = GetPlotRect();
        const int hit = KeyAt(*keys, e.Position, r);
        const bool unit = RangeMode == Range::Unit;

        if (e.Button == UIMouseButton::Right)
        {
            // Unit: the end keys stay (they pin the curve to 0 and 1).
            const bool removable = hit >= 0 && (!unit || (hit > 0 && hit < static_cast<int>(keys->size()) - 1 && keys->size() > 2));
            if (removable)
            {
                keys->erase(keys->begin() + hit);
                Changed(true);
            }
            return true;
        }
        if (e.Button != UIMouseButton::Left)
            return false;

        const bool quick = m_LastClickTime >= 0.0 && m_Time - m_LastClickTime < kDoubleClickSeconds;
        m_LastClickTime = m_Time;
        if (hit >= 0)
        {
            m_DraggingIndex = hit;
            return true;
        }
        if (AddOnDoubleClick && !quick)
            return true;
        const UIVec2 point = ToCurve(e.Position, r);
        const auto at = std::upper_bound(keys->begin(), keys->end(), point, ByX);
        const int index = static_cast<int>(at - keys->begin());
        keys->insert(at, point);
        // A click adds and keeps dragging the new key; a double-click only adds.
        m_DraggingIndex = AddOnDoubleClick ? -1 : index;
        Changed(true);
        return true;
    }

    bool UICurveEditor::OnPointerMove(const UIPointerEvent& e)
    {
        std::vector<UIVec2>* keys = Edited();
        if (m_DraggingIndex < 0 || !keys || m_DraggingIndex >= static_cast<int>(keys->size()))
            return false;
        const UIRect r = GetPlotRect();
        UIVec2 point = ToCurve(e.Position, r);
        const size_t i = static_cast<size_t>(m_DraggingIndex);
        if (RangeMode == Range::Unit)
        {
            if (i == 0)
                point.x = 0.0f;
            else if (i + 1 == keys->size())
                point.x = 1.0f;
            else
                point.x = std::clamp(point.x, (*keys)[i - 1].x + 0.01f, (*keys)[i + 1].x - 0.01f);
        }
        else
        {
            if (i > 0)
                point.x = std::max(point.x, (*keys)[i - 1].x);
            if (i + 1 < keys->size())
                point.x = std::min(point.x, (*keys)[i + 1].x);
        }
        (*keys)[i] = point;
        Changed(false);
        return true;
    }

    bool UICurveEditor::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == UIMouseButton::Left && m_DraggingIndex >= 0)
        {
            m_DraggingIndex = -1;
            return true;
        }
        return false;
    }

    UIVec2 UICurveEditor::ComputeDesiredSize()
    {
        if (HeightMode == UISizeMode::Fixed)
            return { MinSize.x, FixedSize.y };
        return MinSize;
    }

    void UICurveEditor::OnUpdate(float dt)
    {
        m_Time += static_cast<double>(dt);
    }

    void UICurveEditor::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);
        UpdateRange();
        const UIRect r = GetPlotRect();
        if (r.Width() <= 4.0f || r.Height() <= 4.0f)
            return;
        const std::vector<UIVec2>* keys = Shown();
        UIColor gridColor = theme.GetToken("Border", { 0.24f, 0.27f, 0.34f, 1.0f });
        gridColor.a *= 0.6f;
        const UIColor line = theme.GetToken("Accent", { 0.28f, 0.72f, 0.98f, 1.0f });
        const UIColor keyColor = theme.GetToken("CurveKey", { 1.0f, 0.82f, 0.35f, 1.0f });
        const UIColor keyBorder = theme.GetToken("Background", { 0.05f, 0.05f, 0.05f, 1.0f });

        if (RangeMode == Range::Unit)
        {
            for (int i = 1; i <= 3; ++i)
            {
                const float frac = static_cast<float>(i) * 0.25f;
                const float gx = r.Min.x + r.Width() * frac;
                const float gy = r.Min.y + r.Height() * frac;
                drawList.AddRoundedRect({ gx, r.Min.y }, { gx + 1.0f, r.Max.y }, gridColor);
                drawList.AddRoundedRect({ r.Min.x, gy }, { r.Max.x, gy + 1.0f }, gridColor);
            }
        }
        else
        {
            if (m_Low.y < 0.0f && m_High.y > 0.0f)
            {
                const float y = ToScreen({ 0.0f, 0.0f }, r).y;
                drawList.AddRoundedRect({ r.Min.x, y }, { r.Max.x, y + 1.0f }, gridColor);
            }
            if (m_Low.x < 0.0f && m_High.x > 0.0f)
            {
                const float x = ToScreen({ 0.0f, 0.0f }, r).x;
                drawList.AddRoundedRect({ x, r.Min.y }, { x + 1.0f, r.Max.y }, gridColor);
            }
        }
        if (!keys)
            return;

        drawList.PushClipRect(r.Min, r.Max);
        if (Evaluator || RangeMode == Range::FitKeys)
        {
            // Sampled across the view: what the curve's own interpolation gives.
            constexpr int kSamples = 96;
            UIVec2 previous = ToScreen({ m_Low.x, Evaluate(m_Low.x) }, r);
            for (int s = 1; s <= kSamples; ++s)
            {
                const float x = m_Low.x + (m_High.x - m_Low.x) * static_cast<float>(s) / kSamples;
                const UIVec2 point = ToScreen({ x, Evaluate(x) }, r);
                drawList.AddBezier(previous, previous, point, point, 2.0f, line);
                previous = point;
            }
        }
        else
        {
            // Smooth steps drawn as one Bezier per span (flat at both keys).
            for (size_t i = 1; i < keys->size(); ++i)
            {
                const UIVec2 p0 = ToScreen((*keys)[i - 1], r);
                const UIVec2 p3 = ToScreen((*keys)[i], r);
                const float midX = (p0.x + p3.x) * 0.5f;
                drawList.AddBezier(p0, { midX, p0.y }, { midX, p3.y }, p3, 2.2f, line);
            }
        }
        drawList.PopClipRect();

        for (int i = 0; i < static_cast<int>(keys->size()); ++i)
        {
            const UIVec2 p = ToScreen((*keys)[static_cast<size_t>(i)], r);
            const float rad = i == m_DraggingIndex ? 5.5f : 4.5f;
            drawList.AddRoundedRect(p - UIVec2{ rad }, p + UIVec2{ rad }, keyColor, glm::vec4(rad), 1.0f, keyBorder);
        }
        if (RangeMode == Range::FitKeys)
        {
            char text[96];
            std::snprintf(text, sizeof(text), "x %.3g .. %.3g   y %.3g .. %.3g", m_Low.x, m_High.x, m_Low.y, m_High.y);
            drawList.AddText(text, nullptr, { r.Min.x + 2.0f, r.Min.y + 1.0f }, 11.0f, theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f }));
        }
    }
}
