#pragma once

#include <algorithm>
#include <cstdint>

#include <glm/glm.hpp>

namespace Nox::UI
{
    using UIVec2 = glm::vec2;
    using UIColor = glm::vec4;

    struct UIThickness
    {
        float Left = 0.0f;
        float Top = 0.0f;
        float Right = 0.0f;
        float Bottom = 0.0f;

        static constexpr UIThickness All(float v) { return { v, v, v, v }; }
        static constexpr UIThickness Symmetric(float horiz, float vert) { return { horiz, vert, horiz, vert }; }

        constexpr float Horizontal() const { return Left + Right; }
        constexpr float Vertical() const { return Top + Bottom; }
    };

    struct UICornerRadius
    {
        float TopLeft = 0.0f;
        float TopRight = 0.0f;
        float BottomRight = 0.0f;
        float BottomLeft = 0.0f;

        static constexpr UICornerRadius All(float r) { return { r, r, r, r }; }
        glm::vec4 ToVec4() const { return { TopLeft, TopRight, BottomRight, BottomLeft }; }
    };

    struct UIRect
    {
        UIVec2 Min{ 0.0f, 0.0f };
        UIVec2 Max{ 0.0f, 0.0f };

        float Width() const { return std::max(0.0f, Max.x - Min.x); }
        float Height() const { return std::max(0.0f, Max.y - Min.y); }
        UIVec2 Size() const { return { Width(), Height() }; }
        UIVec2 Center() const { return (Min + Max) * 0.5f; }

        bool Contains(const UIVec2& p) const
        {
            return p.x >= Min.x && p.x <= Max.x && p.y >= Min.y && p.y <= Max.y;
        }

        UIRect Inset(const UIThickness& t) const
        {
            UIRect r;
            r.Min = { Min.x + t.Left, Min.y + t.Top };
            r.Max = { std::max(r.Min.x, Max.x - t.Right), std::max(r.Min.y, Max.y - t.Bottom) };
            return r;
        }
    };

    enum class UISizeMode : uint8_t
    {
        Auto = 0,  // Sizes to content (ComputeDesiredSize)
        Fixed,     // Uses FixedSize
        Fill       // Expands to fill available parent space (weighted by FlexWeight)
    };

    enum class UIAlignment : uint8_t
    {
        Start = 0,
        Center,
        End,
        Stretch
    };

    enum class UIHitTestMode : uint8_t
    {
        Self = 0,    // Can be hit and consumes pointer events
        PassThrough, // Transparent to pointer, but children can be hit
        None         // Ignored by hit-testing (and children too)
    };

    enum class UIState : uint8_t
    {
        Normal   = 0,
        Hovered  = 1 << 0,
        Pressed  = 1 << 1,
        Focused  = 1 << 2,
        Disabled = 1 << 3
    };

    inline UIState operator|(UIState a, UIState b)
    {
        return static_cast<UIState>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }
    inline UIState operator&(UIState a, UIState b)
    {
        return static_cast<UIState>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
    }
    inline bool HasState(UIState mask, UIState flag)
    {
        return (static_cast<uint8_t>(mask) & static_cast<uint8_t>(flag)) != 0;
    }

    struct UIPointerEvent
    {
        UIVec2 Position{ 0.0f, 0.0f };
        UIVec2 Delta{ 0.0f, 0.0f };
        UIVec2 ScrollDelta{ 0.0f, 0.0f };
        int Button = 0; // 0 = Left, 1 = Right, 2 = Middle
        bool Handled = false;
    };

    struct UIKeyEvent
    {
        int KeyCode = 0;
        uint16_t Modifiers = 0;
        bool Repeat = false;
        bool Handled = false;
    };
}
