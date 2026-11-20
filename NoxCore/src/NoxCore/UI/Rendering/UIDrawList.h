#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>

namespace shaderio
{
    using namespace glm;
    #include "shaderIO.h"
}

#include "NoxCore/Core/Ref.h"

namespace Nox
{
    class Font;
}

namespace Nox::UI
{
    class UIGlyphCache;

    // Collects resolution-independent 2D SDF UI primitives for one viewport frame.
    // Coordinates are in output pixels with origin at top-left (x right, y down).
    class UIDrawList
    {
    public:
        UIDrawList();

        void Clear(const glm::vec2& viewportSize);

        void PushClipRect(const glm::vec2& min, const glm::vec2& max, const glm::vec4& cornerRadii = glm::vec4(0.0f));
        void PopClipRect();

        void AddRoundedRect(const glm::vec2& min, const glm::vec2& max, const glm::vec4& color,
                            const glm::vec4& cornerRadii = glm::vec4(0.0f),
                            float borderThickness = 0.0f, const glm::vec4& borderColor = glm::vec4(0.0f),
                            const glm::vec4& shadowColor = glm::vec4(0.0f),
                            const glm::vec2& shadowOffset = glm::vec2(0.0f), float shadowBlur = 0.0f);

        void AddRoundedRectGradient(const glm::vec2& min, const glm::vec2& max,
                                    const glm::vec4& topColor, const glm::vec4& bottomColor,
                                    const glm::vec4& cornerRadii = glm::vec4(0.0f),
                                    float borderThickness = 0.0f, const glm::vec4& borderColor = glm::vec4(0.0f),
                                    const glm::vec4& shadowColor = glm::vec4(0.0f),
                                    const glm::vec2& shadowOffset = glm::vec2(0.0f), float shadowBlur = 0.0f);

        void AddTexturedQuad(const glm::vec2& min, const glm::vec2& max, uint32_t textureDescriptorSlot,
                             const glm::vec2& uvMin = glm::vec2(0.0f, 0.0f),
                             const glm::vec2& uvMax = glm::vec2(1.0f, 1.0f),
                             const glm::vec4& tint = glm::vec4(1.0f),
                             const glm::vec4& cornerRadii = glm::vec4(0.0f));

        void AddText(std::string_view text, const Ref<Font>& font, const glm::vec2& topLeftPos,
                     float fontSizePx, const glm::vec4& color);

        void AddBezier(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec2& p3,
                       float thickness, const glm::vec4& color);

        const std::vector<shaderio::UIPrimitive>& GetPrimitives() const { return m_Primitives; }
        const std::vector<shaderio::UIClipRect>& GetClipRects() const { return m_ClipRects; }
        bool IsEmpty() const { return m_Primitives.empty(); }

    private:
        uint32_t CurrentClipIndex() const { return m_ClipStack.empty() ? 0u : m_ClipStack.back(); }
        enum class GlyphCacheResult : uint8_t { Drawn, Missing };
        // One glyph from the glyph cache at the pen (snapped to the pixel grid); Missing: draw it from the distance field.
        GlyphCacheResult DrawCachedGlyph(UIGlyphCache& cache, const Font& font, float fontSizePx, uint32_t codepoint, double penX,
                                         double baselineY, const glm::vec4& color);

        std::vector<shaderio::UIPrimitive> m_Primitives;
        std::vector<shaderio::UIClipRect> m_ClipRects;
        std::vector<uint32_t> m_ClipStack;
        glm::vec2 m_ViewportSize{ 1.0f, 1.0f };
    };
}
