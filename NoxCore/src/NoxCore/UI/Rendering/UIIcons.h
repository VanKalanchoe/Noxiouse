#pragma once

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI::Icons
{
    inline void DrawSegment(UIDrawList& drawList, const glm::vec2& a, const glm::vec2& b,
                            float thickness, const glm::vec4& color)
    {
        drawList.AddBezier(a, glm::mix(a, b, 1.0f / 3.0f), glm::mix(a, b, 2.0f / 3.0f), b, thickness, color);
    }

    // Crisp vector chevron: down when expanded == true, right when expanded == false.
    inline void DrawChevron(UIDrawList& drawList, const glm::vec2& center, float size,
                            bool down, const glm::vec4& color, float thickness = 1.6f)
    {
        const float r = std::max(3.0f, size * 0.5f);
        if (down)
        {
            const glm::vec2 left{ center.x - r, center.y - r * 0.42f };
            const glm::vec2 apex{ center.x, center.y + r * 0.52f };
            const glm::vec2 right{ center.x + r, center.y - r * 0.42f };
            DrawSegment(drawList, left, apex, thickness, color);
            DrawSegment(drawList, apex, right, thickness, color);
        }
        else
        {
            const glm::vec2 top{ center.x - r * 0.42f, center.y - r };
            const glm::vec2 apex{ center.x + r * 0.52f, center.y };
            const glm::vec2 bot{ center.x - r * 0.42f, center.y + r };
            DrawSegment(drawList, top, apex, thickness, color);
            DrawSegment(drawList, apex, bot, thickness, color);
        }
    }

    // Crisp vector checkmark centered inside [boxMin, boxMax].
    inline void DrawCheckmark(UIDrawList& drawList, const glm::vec2& boxMin, const glm::vec2& boxMax,
                              const glm::vec4& color, float thickness = 1.8f)
    {
        const float w = boxMax.x - boxMin.x;
        const float h = boxMax.y - boxMin.y;
        const glm::vec2 p0{ boxMin.x + w * 0.22f, boxMin.y + h * 0.53f };
        const glm::vec2 p1{ boxMin.x + w * 0.43f, boxMin.y + h * 0.74f };
        const glm::vec2 p2{ boxMin.x + w * 0.79f, boxMin.y + h * 0.28f };
        DrawSegment(drawList, p0, p1, thickness, color);
        DrawSegment(drawList, p1, p2, thickness, color);
    }

    // Three evenly spaced dots for component/panel overflow menus.
    inline void DrawMoreDots(UIDrawList& drawList, const glm::vec2& center, float span,
                             const glm::vec4& color, bool vertical = true)
    {
        const float r = std::max(1.3f, span * 0.13f);
        const float step = span * 0.34f;
        for (int i = -1; i <= 1; ++i)
        {
            const glm::vec2 d = vertical
                ? glm::vec2{ center.x, center.y + static_cast<float>(i) * step }
                : glm::vec2{ center.x + static_cast<float>(i) * step, center.y };
            drawList.AddRoundedRect(d - glm::vec2(r), d + glm::vec2(r), color, glm::vec4(r));
        }
    }

    // Clean power symbol for entity active state toggle.
    inline void DrawPowerGlyph(UIDrawList& drawList, const glm::vec2& center, float radius,
                               const glm::vec4& color, float thickness = 1.6f)
    {
        const float r = std::max(4.0f, radius);
        // Open top ring drawn with 3 smooth bezier arcs (~280 degrees from top-right to top-left)
        const float aStart = -0.95f; // rad from +Y? Use standard angle from +X: -pi/2 is top
        constexpr float kPi = 3.14159265f;
        const float gapHalf = 0.58f;
        const float startAng = -kPi * 0.5f + gapHalf;
        const float endAng = kPi * 1.5f - gapHalf;
        constexpr int kSegs = 6;
        const float step = (endAng - startAng) / static_cast<float>(kSegs);
        for (int i = 0; i < kSegs; ++i)
        {
            const float a0 = startAng + static_cast<float>(i) * step;
            const float a1 = a0 + step;
            const glm::vec2 p0 = center + glm::vec2{ std::cos(a0), std::sin(a0) } * r;
            const glm::vec2 p1 = center + glm::vec2{ std::cos(a1), std::sin(a1) } * r;
            DrawSegment(drawList, p0, p1, thickness, color);
        }
        DrawSegment(drawList, { center.x, center.y - r * 1.15f }, { center.x, center.y - r * 0.1f }, thickness, color);
        (void)aStart;
    }
}
