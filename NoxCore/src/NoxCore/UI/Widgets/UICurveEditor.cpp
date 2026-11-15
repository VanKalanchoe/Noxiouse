#include "UICurveEditor.h"

#include <algorithm>
#include <cmath>

#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
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
        for (auto& p : m_Points)
        {
            p.x = std::clamp(p.x, 0.0f, 1.0f);
            p.y = std::clamp(p.y, 0.0f, 1.0f);
        }
        std::sort(m_Points.begin(), m_Points.end(), [](const UIVec2& a, const UIVec2& b) { return a.x < b.x; });
        m_DraggingIndex = -1;
    }

    float UICurveEditor::Evaluate(float t) const
    {
        if (m_Points.empty())
            return 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        if (t <= m_Points.front().x)
            return m_Points.front().y;
        if (t >= m_Points.back().x)
            return m_Points.back().y;

        for (size_t i = 1; i < m_Points.size(); ++i)
        {
            if (t <= m_Points[i].x)
            {
                const float span = std::max(1e-5f, m_Points[i].x - m_Points[i - 1].x);
                const float u = (t - m_Points[i - 1].x) / span;
                const float smooth = u * u * (3.0f - 2.0f * u);
                return m_Points[i - 1].y + (m_Points[i].y - m_Points[i - 1].y) * smooth;
            }
        }
        return m_Points.back().y;
    }

    UIRect UICurveEditor::GetPlotRect() const
    {
        return GetBounds().Inset(Padding);
    }

    UIVec2 UICurveEditor::NormToScreen(const UIVec2& p, const UIRect& r) const
    {
        return {
            r.Min.x + p.x * r.Width(),
            r.Max.y - p.y * r.Height()
        };
    }

    UIVec2 UICurveEditor::ScreenToNorm(const UIVec2& p, const UIRect& r) const
    {
        return {
            std::clamp((p.x - r.Min.x) / std::max(1.0f, r.Width()), 0.0f, 1.0f),
            std::clamp((r.Max.y - p.y) / std::max(1.0f, r.Height()), 0.0f, 1.0f)
        };
    }

    bool UICurveEditor::OnPointerDown(const UIPointerEvent& e)
    {
        if (!IsEnabled())
            return false;

        const UIRect r = GetPlotRect();
        if (!GetBounds().Contains(e.Position))
            return false;

        int hitIdx = -1;
        float bestDistSq = 100.0f; // 10px radius
        for (int i = 0; i < static_cast<int>(m_Points.size()); ++i)
        {
            const UIVec2 sp = NormToScreen(m_Points[i], r);
            const UIVec2 d = e.Position - sp;
            const float distSq = d.x * d.x + d.y * d.y;
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                hitIdx = i;
            }
        }

        if (e.Button == 3) // Right-click removes interior keyframe
        {
            if (hitIdx > 0 && hitIdx < static_cast<int>(m_Points.size()) - 1 && m_Points.size() > 2)
            {
                m_Points.erase(m_Points.begin() + hitIdx);
                if (OnCurveChanged)
                    OnCurveChanged(m_Points);
            }
            return true;
        }

        if (e.Button != 1)
            return false;

        if (hitIdx >= 0)
        {
            m_DraggingIndex = hitIdx;
        }
        else
        {
            // Left-click empty space adds a new keyframe and starts dragging it
            const UIVec2 np = ScreenToNorm(e.Position, r);
            m_Points.push_back(np);
            std::sort(m_Points.begin(), m_Points.end(), [](const UIVec2& a, const UIVec2& b) { return a.x < b.x; });
            for (int i = 0; i < static_cast<int>(m_Points.size()); ++i)
            {
                if (std::abs(m_Points[i].x - np.x) < 1e-4f && std::abs(m_Points[i].y - np.y) < 1e-4f)
                {
                    m_DraggingIndex = i;
                    break;
                }
            }
            if (OnCurveChanged)
                OnCurveChanged(m_Points);
        }
        return true;
    }

    bool UICurveEditor::OnPointerMove(const UIPointerEvent& e)
    {
        if (m_DraggingIndex < 0 || m_DraggingIndex >= static_cast<int>(m_Points.size()))
            return false;

        const UIRect r = GetPlotRect();
        UIVec2 np = ScreenToNorm(e.Position, r);
        if (m_DraggingIndex == 0)
            np.x = 0.0f;
        else if (m_DraggingIndex == static_cast<int>(m_Points.size()) - 1)
            np.x = 1.0f;
        else
            np.x = std::clamp(np.x, m_Points[m_DraggingIndex - 1].x + 0.01f, m_Points[m_DraggingIndex + 1].x - 0.01f);

        m_Points[m_DraggingIndex] = np;
        if (OnCurveChanged)
            OnCurveChanged(m_Points);
        return true;
    }

    bool UICurveEditor::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == 1 && m_DraggingIndex >= 0)
        {
            m_DraggingIndex = -1;
            return true;
        }
        return false;
    }

    UIVec2 UICurveEditor::ComputeDesiredSize()
    {
        return MinSize;
    }

    void UICurveEditor::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIRect r = GetPlotRect();
        if (r.Width() <= 4.0f || r.Height() <= 4.0f)
            return;

        // 4x4 grid
        for (int i = 1; i <= 3; ++i)
        {
            const float frac = static_cast<float>(i) * 0.25f;
            const float gx = r.Min.x + r.Width() * frac;
            const float gy = r.Min.y + r.Height() * frac;
            drawList.AddRoundedRect({ gx, r.Min.y }, { gx + 1.0f, r.Max.y }, { 0.24f, 0.27f, 0.34f, 0.35f });
            drawList.AddRoundedRect({ r.Min.x, gy }, { r.Max.x, gy + 1.0f }, { 0.24f, 0.27f, 0.34f, 0.35f });
        }

        // Smooth cubic Hermite-style Bezier segments
        for (size_t i = 1; i < m_Points.size(); ++i)
        {
            const UIVec2 p0 = NormToScreen(m_Points[i - 1], r);
            const UIVec2 p3 = NormToScreen(m_Points[i], r);
            const float midX = (p0.x + p3.x) * 0.5f;
            drawList.AddBezier(p0, { midX, p0.y }, { midX, p3.y }, p3, 2.2f, { 0.28f, 0.72f, 0.98f, 1.0f });
        }

        // Control point handles
        for (int i = 0; i < static_cast<int>(m_Points.size()); ++i)
        {
            const UIVec2 sp = NormToScreen(m_Points[i], r);
            const bool active = (i == m_DraggingIndex);
            const float rad = active ? 5.5f : 4.5f;
            drawList.AddRoundedRect(
                sp - UIVec2{ rad, rad },
                sp + UIVec2{ rad, rad },
                active ? UIColor{ 1.0f, 0.88f, 0.35f, 1.0f } : UIColor{ 0.95f, 0.97f, 1.0f, 1.0f },
                glm::vec4(rad),
                1.5f,
                { 0.24f, 0.56f, 0.96f, 1.0f }
            );
        }
    }
}
