#pragma once
#include <cstdint>

namespace NRI
{
    struct Extent2D
    {
        uint32_t width;
        uint32_t height;
    };
    
    struct ViewportBounds
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
    };
    
    enum class CompareOp : uint8_t
    {
        Never = 0,
        Less,
        Equal,
        LessOrEqual,
        Greater,
        NotEqual,
        GreaterOrEqual,
        Always
    };
}
