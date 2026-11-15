#include "UIDrawList.h"

#include <algorithm>
#include <cmath>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/MSDFData.h"
#include "NoxCore/Asset/TextureImporter.h"

namespace Nox::UI
{
    UIDrawList::UIDrawList()
    {
        Clear(glm::vec2(1.0f, 1.0f));
    }

    void UIDrawList::Clear(const glm::vec2& viewportSize)
    {
        m_ViewportSize = glm::max(viewportSize, glm::vec2(1.0f));
        m_Primitives.clear();
        m_ClipRects.clear();
        m_ClipStack.clear();

        shaderio::UIClipRect rootClip{};
        rootClip.rect = glm::vec4(0.0f, 0.0f, m_ViewportSize.x, m_ViewportSize.y);
        rootClip.cornerRadii = glm::vec4(0.0f);
        m_ClipRects.push_back(rootClip);
        m_ClipStack.push_back(0u);
    }

    void UIDrawList::PushClipRect(const glm::vec2& min, const glm::vec2& max, const glm::vec4& cornerRadii)
    {
        const shaderio::UIClipRect& parent = m_ClipRects[CurrentClipIndex()];
        glm::vec2 clampedMin = glm::max(min, glm::vec2(parent.rect.x, parent.rect.y));
        glm::vec2 clampedMax = glm::min(max, glm::vec2(parent.rect.z, parent.rect.w));
        clampedMax = glm::max(clampedMax, clampedMin);

        shaderio::UIClipRect clip{};
        clip.rect = glm::vec4(clampedMin.x, clampedMin.y, clampedMax.x, clampedMax.y);
        clip.cornerRadii = cornerRadii;

        const uint32_t index = static_cast<uint32_t>(m_ClipRects.size());
        m_ClipRects.push_back(clip);
        m_ClipStack.push_back(index);
    }

    void UIDrawList::PopClipRect()
    {
        if (m_ClipStack.size() <= 1)
        {
            NOX_CORE_WARN("UIDrawList::PopClipRect called with empty user clip stack");
            return;
        }
        m_ClipStack.pop_back();
    }

    void UIDrawList::AddRoundedRect(const glm::vec2& min, const glm::vec2& max, const glm::vec4& color,
                                    const glm::vec4& cornerRadii, float borderThickness, const glm::vec4& borderColor,
                                    const glm::vec4& shadowColor, const glm::vec2& shadowOffset, float shadowBlur)
    {
        if (max.x <= min.x || max.y <= min.y)
            return;

        shaderio::UIPrimitive prim{};
        prim.bounds = glm::vec4(min.x, min.y, max.x, max.y);
        prim.color0 = color;
        prim.color1 = color;
        prim.borderColor = borderColor;
        prim.shadowColor = shadowColor;
        prim.cornerRadii = cornerRadii;
        prim.shadowOffset = shadowOffset;
        prim.shadowBlur = std::max(0.0f, shadowBlur);
        prim.borderThickness = std::max(0.0f, borderThickness);
        prim.type = shaderio::UI_PRIM_ROUNDED_RECT;
        prim.clipIndex = CurrentClipIndex();
        prim.flags = 0u;
        m_Primitives.push_back(prim);
    }

    void UIDrawList::AddRoundedRectGradient(const glm::vec2& min, const glm::vec2& max,
                                            const glm::vec4& topColor, const glm::vec4& bottomColor,
                                            const glm::vec4& cornerRadii, float borderThickness, const glm::vec4& borderColor,
                                            const glm::vec4& shadowColor, const glm::vec2& shadowOffset, float shadowBlur)
    {
        if (max.x <= min.x || max.y <= min.y)
            return;

        shaderio::UIPrimitive prim{};
        prim.bounds = glm::vec4(min.x, min.y, max.x, max.y);
        prim.color0 = topColor;
        prim.color1 = bottomColor;
        prim.borderColor = borderColor;
        prim.shadowColor = shadowColor;
        prim.cornerRadii = cornerRadii;
        prim.shadowOffset = shadowOffset;
        prim.shadowBlur = std::max(0.0f, shadowBlur);
        prim.borderThickness = std::max(0.0f, borderThickness);
        prim.type = shaderio::UI_PRIM_ROUNDED_RECT;
        prim.clipIndex = CurrentClipIndex();
        prim.flags = 1u;
        m_Primitives.push_back(prim);
    }

    void UIDrawList::AddTexturedQuad(const glm::vec2& min, const glm::vec2& max, uint32_t textureDescriptorSlot,
                                     const glm::vec2& uvMin, const glm::vec2& uvMax,
                                     const glm::vec4& tint, const glm::vec4& cornerRadii)
    {
        if (max.x <= min.x || max.y <= min.y)
            return;

        shaderio::UIPrimitive prim{};
        prim.bounds = glm::vec4(min.x, min.y, max.x, max.y);
        prim.color0 = tint;
        prim.cornerRadii = cornerRadii;
        prim.uvOrP0P1 = glm::vec4(uvMin.x, uvMin.y, uvMax.x, uvMax.y);
        prim.type = shaderio::UI_PRIM_TEXTURED_QUAD;
        prim.clipIndex = CurrentClipIndex();
        prim.textureIndex = textureDescriptorSlot;
        m_Primitives.push_back(prim);
    }

    void UIDrawList::AddText(std::string_view text, const Ref<Font>& fontIn, const glm::vec2& topLeftPos,
                             float fontSizePx, const glm::vec4& color)
    {
        if (text.empty() || fontSizePx <= 0.0f)
            return;

        Ref<Font> font = fontIn ? fontIn : Font::GetDefault();
        if (!font || !font->IsValid() || !font->GetMSDFData())
            return;

        const Ref<Texture2D> atlas = font->GetAtlasTexture();
        if (!atlas)
            return;

        const auto& fontGeometry = font->GetMSDFData()->FontGeometry;
        const auto& metrics = fontGeometry.getMetrics();
        const double scale = static_cast<double>(fontSizePx) / (metrics.ascenderY - metrics.descenderY);

        const float texelW = 1.0f / static_cast<float>(std::max(atlas->GetWidth(), 1u));
        const float texelH = 1.0f / static_cast<float>(std::max(atlas->GetHeight(), 1u));
        const uint32_t atlasSlot = atlas->GetDescriptorIndexSlot();

        double x = static_cast<double>(topLeftPos.x);
        double baselineY = static_cast<double>(topLeftPos.y) + metrics.ascenderY * scale;
        const double lineHeight = metrics.lineHeight * scale;

        double spaceAdvance = 0.25;
        if (const auto* spaceGlyph = fontGeometry.getGlyph(' '))
            spaceAdvance = spaceGlyph->getAdvance();

        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c == '\r')
                continue;
            if (c == '\n')
            {
                x = static_cast<double>(topLeftPos.x);
                baselineY += lineHeight;
                continue;
            }
            if (c == ' ')
            {
                double adv = spaceAdvance;
                if (i + 1 < text.size())
                    fontGeometry.getAdvance(adv, c, text[i + 1]);
                x += adv * scale;
                continue;
            }
            if (c == '\t')
            {
                x += 4.0 * spaceAdvance * scale;
                continue;
            }

            const auto* glyph = fontGeometry.getGlyph(static_cast<uint32_t>(static_cast<unsigned char>(c)));
            if (!glyph)
                glyph = fontGeometry.getGlyph('?');
            if (!glyph)
                continue;

            double al = 0.0, ab = 0.0, ar = 0.0, at = 0.0;
            glyph->getQuadAtlasBounds(al, ab, ar, at);

            double pl = 0.0, pb = 0.0, pr = 0.0, pt = 0.0;
            glyph->getQuadPlaneBounds(pl, pb, pr, pt);

            if (pr > pl && pt > pb)
            {
                const float x0 = static_cast<float>(x + pl * scale);
                const float x1 = static_cast<float>(x + pr * scale);
                const float y0 = static_cast<float>(baselineY - pt * scale);
                const float y1 = static_cast<float>(baselineY - pb * scale);

                shaderio::UIPrimitive prim{};
                prim.bounds = glm::vec4(x0, y0, x1, y1);
                prim.color0 = color;
                prim.uvOrP0P1 = glm::vec4(static_cast<float>(al) * texelW, static_cast<float>(at) * texelH,
                                          static_cast<float>(ar) * texelW, static_cast<float>(ab) * texelH);
                prim.type = shaderio::UI_PRIM_MSDF_GLYPH;
                prim.clipIndex = CurrentClipIndex();
                prim.textureIndex = atlasSlot;
                m_Primitives.push_back(prim);
            }

            double advance = glyph->getAdvance();
            if (i + 1 < text.size())
                fontGeometry.getAdvance(advance, c, text[i + 1]);
            x += advance * scale;
        }
    }

    void UIDrawList::AddBezier(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec2& p3,
                               float thickness, const glm::vec4& color)
    {
        if (thickness <= 0.0f || color.a <= 0.0f)
            return;

        const glm::vec2 minP = glm::min(glm::min(p0, p1), glm::min(p2, p3));
        const glm::vec2 maxP = glm::max(glm::max(p0, p1), glm::max(p2, p3));

        shaderio::UIPrimitive prim{};
        prim.bounds = glm::vec4(minP.x, minP.y, maxP.x, maxP.y);
        prim.color0 = color;
        prim.uvOrP0P1 = glm::vec4(p0.x, p0.y, p1.x, p1.y);
        prim.extraOrP2P3 = glm::vec4(p2.x, p2.y, p3.x, p3.y);
        // Reuse extraOrP2P3? Wait: Bezier needs all 4 control points in uvOrP0P1 & extraOrP2P3,
        // so store stroke thickness in borderThickness!
        prim.borderThickness = thickness;
        prim.type = shaderio::UI_PRIM_CUBIC_BEZIER;
        prim.clipIndex = CurrentClipIndex();
        m_Primitives.push_back(prim);
    }
}
