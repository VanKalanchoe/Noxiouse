#include "UIPlotGraph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    UIPlotGraph::UIPlotGraph(std::string title, UIPlotMode mode)
        : Title(std::move(title))
        , m_Mode(mode)
    {
        StyleClass = "TextInput";
        DrawBackground = true;
        ClipChildren = true;
        HitTestMode = UIHitTestMode::Self;
        WidthMode = UISizeMode::Fill;
        MinSize = { 180.0f, 110.0f };
        Padding = UIThickness::All(10.0f);
    }

    void UIPlotGraph::SetValues(std::vector<float> values)
    {
        m_Values = std::move(values);
        if (m_Values.size() > MaxSamples && MaxSamples > 0)
            m_Values.erase(m_Values.begin(), m_Values.begin() + (m_Values.size() - MaxSamples));
        m_HoveredIndex = -1;
    }

    void UIPlotGraph::PushSample(float value)
    {
        m_Values.push_back(value);
        if (m_Values.size() > MaxSamples && MaxSamples > 0)
            m_Values.erase(m_Values.begin(), m_Values.begin() + (m_Values.size() - MaxSamples));
    }

    void UIPlotGraph::Clear()
    {
        m_Values.clear();
        m_HoveredIndex = -1;
    }

    bool UIPlotGraph::OnPointerMove(const UIPointerEvent& e)
    {
        const UIRect& b = GetBounds();
        if (m_Values.empty() || !b.Contains(e.Position))
        {
            m_HoveredIndex = -1;
            return false;
        }

        const float innerX0 = b.Min.x + Padding.Left;
        const float innerW = std::max(1.0f, b.Width() - Padding.Horizontal());
        const float t = std::clamp((e.Position.x - innerX0) / innerW, 0.0f, 1.0f);
        m_HoveredIndex = std::clamp(
            static_cast<int>(std::round(t * static_cast<float>(m_Values.size() - 1))),
            0,
            static_cast<int>(m_Values.size()) - 1
        );
        return false;
    }

    bool UIPlotGraph::OnPointerLeave(const UIPointerEvent&)
    {
        m_HoveredIndex = -1;
        return false;
    }

    UIVec2 UIPlotGraph::ComputeDesiredSize()
    {
        return { MinSize.x, MinSize.y };
    }

    void UIPlotGraph::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIRect& b = GetBounds();
        const Ref<Font> font = Font::GetDefault();
        const float topHeader = Title.empty() ? 0.0f : 18.0f;

        const UIRect plotRect{
            { b.Min.x + Padding.Left, b.Min.y + Padding.Top + topHeader },
            { b.Max.x - Padding.Right, b.Max.y - Padding.Bottom }
        };
        if (plotRect.Width() <= 4.0f || plotRect.Height() <= 4.0f)
            return;

        if (!Title.empty())
            drawList.AddText(Title, font, { b.Min.x + Padding.Left, b.Min.y + Padding.Top - 2.0f }, 12.5f, { 0.80f, 0.85f, 0.92f, 1.0f });

        // Subtle horizontal gridlines (25%, 50%, 75%)
        for (int g = 1; g <= 3; ++g)
        {
            const float y = plotRect.Min.y + plotRect.Height() * (static_cast<float>(g) * 0.25f);
            drawList.AddRoundedRect(
                { plotRect.Min.x, y },
                { plotRect.Max.x, y + 1.0f },
                { 0.24f, 0.27f, 0.34f, 0.45f }
            );
        }

        if (m_Values.empty())
            return;

        float vMin = MinValue;
        float vMax = MaxValue;
        if (AutoScale)
        {
            const auto [itMin, itMax] = std::minmax_element(m_Values.begin(), m_Values.end());
            vMin = *itMin;
            vMax = *itMax;
            if (std::abs(vMax - vMin) < 1e-4f)
            {
                vMin -= 1.0f;
                vMax += 1.0f;
            }
        }
        const float invRange = 1.0f / std::max(1e-5f, vMax - vMin);

        auto pointFor = [&](size_t i) -> UIVec2
        {
            const float tx = (m_Values.size() > 1)
                ? static_cast<float>(i) / static_cast<float>(m_Values.size() - 1)
                : 0.5f;
            const float ty = std::clamp((m_Values[i] - vMin) * invRange, 0.0f, 1.0f);
            return {
                plotRect.Min.x + tx * plotRect.Width(),
                plotRect.Max.y - ty * plotRect.Height()
            };
        };

        if (m_Mode == UIPlotMode::Histogram)
        {
            const float barW = std::max(2.0f, plotRect.Width() / static_cast<float>(m_Values.size()) - 1.5f);
            for (size_t i = 0; i < m_Values.size(); ++i)
            {
                const UIVec2 pt = pointFor(i);
                const float x0 = plotRect.Min.x + (static_cast<float>(i) / static_cast<float>(m_Values.size())) * plotRect.Width();
                const bool hovered = (static_cast<int>(i) == m_HoveredIndex);
                const UIColor col = hovered
                    ? UIColor{ 1.0f, 0.85f, 0.35f, 1.0f }
                    : LineColor;
                drawList.AddRoundedRect(
                    { x0, pt.y },
                    { std::min(plotRect.Max.x, x0 + barW), plotRect.Max.y },
                    col,
                    glm::vec4(2.0f)
                );
            }
        }
        else
        {
            for (size_t i = 1; i < m_Values.size(); ++i)
            {
                const UIVec2 p0 = pointFor(i - 1);
                const UIVec2 p1 = pointFor(i);
                const UIVec2 mid = (p0 + p1) * 0.5f;
                drawList.AddBezier(p0, mid, mid, p1, 2.0f, LineColor);
            }
        }

        if (m_HoveredIndex >= 0 && m_HoveredIndex < static_cast<int>(m_Values.size()))
        {
            const UIVec2 pt = pointFor(static_cast<size_t>(m_HoveredIndex));
            drawList.AddRoundedRect(
                { pt.x - 0.5f, plotRect.Min.y },
                { pt.x + 0.5f, plotRect.Max.y },
                { 0.90f, 0.92f, 0.96f, 0.35f }
            );
            drawList.AddRoundedRect(
                pt - UIVec2{ 3.5f, 3.5f },
                pt + UIVec2{ 3.5f, 3.5f },
                { 1.0f, 0.88f, 0.35f, 1.0f },
                glm::vec4(3.5f)
            );

            char buf[32]{};
            std::snprintf(buf, sizeof(buf), "%.2f", m_Values[m_HoveredIndex]);
            drawList.AddText(buf, font, { plotRect.Max.x - 48.0f, b.Min.y + Padding.Top - 2.0f }, 12.5f, { 1.0f, 0.88f, 0.35f, 1.0f });
        }
    }
}
