#include "UIGlyphCache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Renderer/Font.h"

// stb_truetype (the copy Dear ImGui ships), compiled static into this file: ImGui compiles its own static copy, so neither sees the other.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

namespace Nox::UI
{
    struct UIGlyphCache::FontFace
    {
        std::vector<uint8_t> Bytes; // (stb_truetype reads the file in place: kept as long as the face)
        stbtt_fontinfo Info{};
        bool Valid = false;
        uint32_t Index = 0;         // a small number for the glyph keys
    };

    UIGlyphCache& UIGlyphCache::Get()
    {
        static UIGlyphCache s_Cache;
        return s_Cache;
    }

    UIGlyphCache::UIGlyphCache()
        : m_Pixels(static_cast<size_t>(kAtlasSize) * kAtlasSize, 0)
    {
    }

    UIGlyphCache::~UIGlyphCache() = default;

    float UIGlyphCache::QuantizeSize(float pixelSize)
    {
        return std::max(1.0f, std::round(pixelSize * 2.0f) * 0.5f);
    }

    UIGlyphCache::FontFace* UIGlyphCache::FaceOf(const Font& font)
    {
        auto found = m_Faces.find(font.GetCacheId());
        if (found != m_Faces.end())
            return found->second.get();
        auto face = std::make_unique<FontFace>();
        face->Bytes = font.GetFontBytes();
        face->Index = static_cast<uint32_t>(m_Faces.size());
        if (!face->Bytes.empty())
        {
            const int offset = stbtt_GetFontOffsetForIndex(face->Bytes.data(), 0);
            face->Valid = offset >= 0 && stbtt_InitFont(&face->Info, face->Bytes.data(), offset) != 0;
        }
        if (!face->Valid)
            NOX_CORE_WARN("UI text: a font could not be read by stb_truetype; its text is drawn from the distance field");
        FontFace* raw = face.get();
        m_Faces.emplace(font.GetCacheId(), std::move(face));
        return raw;
    }

    const UIGlyphCache::Glyph* UIGlyphCache::Find(const Font& font, float pixelSize, uint32_t codepoint, int phase)
    {
        if (m_TextureSlot == ~0u || pixelSize <= 0.0f || pixelSize > kMaxPixelSize)
            return nullptr;
        FontFace* face = FaceOf(font);
        if (!face || !face->Valid)
            return nullptr;
        const float size = QuantizeSize(pixelSize);
        const uint32_t sizeStep = static_cast<uint32_t>(size * 2.0f);
        phase = std::clamp(phase, 0, kSubpixelPhases - 1);
        const uint64_t key = (static_cast<uint64_t>(face->Index) << 40) | (static_cast<uint64_t>(sizeStep) << 24) |
                             (static_cast<uint64_t>(phase) << 21) | (codepoint & 0x1FFFFFu);
        if (const auto found = m_Glyphs.find(key); found != m_Glyphs.end())
            return &found->second;
        if (m_ResetRequested)
            return nullptr; // (full: nothing new until the atlas is emptied between frames)

        // Rasterized now: the font's scale for this pixel height (ascender to descender, as the distance-field layout uses), the
        // pen offset as a sub-pixel shift.
        const int glyphIndex = stbtt_FindGlyphIndex(&face->Info, static_cast<int>(codepoint));
        const float scale = stbtt_ScaleForPixelHeight(&face->Info, size);
        const float shift = static_cast<float>(phase) / static_cast<float>(kSubpixelPhases);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(&face->Info, glyphIndex, scale, scale, shift, 0.0f, &x0, &y0, &x1, &y1);
        Glyph glyph;
        const int width = x1 - x0, height = y1 - y0;
        if (width <= 0 || height <= 0)
            return &m_Glyphs.emplace(key, glyph).first->second; // (nothing to draw: a space)

        // A place on the current shelf, or a new shelf below it; a texel of empty border around it (linear sampling never reaches
        // a neighbour).
        const uint32_t paddedW = static_cast<uint32_t>(width) + 2, paddedH = static_cast<uint32_t>(height) + 2;
        if (paddedW > kAtlasSize || paddedH > kAtlasSize)
            return nullptr;
        if (m_CursorX + paddedW > kAtlasSize)
        {
            m_ShelfY += m_ShelfHeight;
            m_ShelfHeight = 0;
            m_CursorX = 0;
        }
        if (m_ShelfY + paddedH > kAtlasSize)
        {
            m_ResetRequested = true;
            NOX_CORE_INFO("UI text: the glyph atlas is full; it is emptied after this frame");
            return nullptr;
        }
        const uint32_t ax = m_CursorX, ay = m_ShelfY;
        m_CursorX += paddedW;
        m_ShelfHeight = std::max(m_ShelfHeight, paddedH);

        for (uint32_t row = 0; row < paddedH; ++row)
            std::memset(&m_Pixels[static_cast<size_t>(ay + row) * kAtlasSize + ax], 0, paddedW);
        stbtt_MakeGlyphBitmapSubpixel(&face->Info, &m_Pixels[static_cast<size_t>(ay + 1) * kAtlasSize + ax + 1], width, height,
                                      static_cast<int>(kAtlasSize), scale, scale, shift, 0.0f, glyphIndex);
        if (!m_Pending.empty() && m_Pending.back().Y == ay && m_Pending.back().X + m_Pending.back().W == ax)
        {
            m_Pending.back().W += paddedW;
            m_Pending.back().H = std::max(m_Pending.back().H, paddedH);
        }
        else
            m_Pending.push_back({ ax, ay, paddedW, paddedH });

        const float texel = 1.0f / static_cast<float>(kAtlasSize);
        glyph.X0 = static_cast<int16_t>(x0);
        glyph.Y0 = static_cast<int16_t>(y0);
        glyph.W = static_cast<uint16_t>(width);
        glyph.H = static_cast<uint16_t>(height);
        glyph.UV = { static_cast<float>(ax + 1) * texel, static_cast<float>(ay + 1) * texel,
                     static_cast<float>(ax + 1 + static_cast<uint32_t>(width)) * texel, static_cast<float>(ay + 1 + static_cast<uint32_t>(height)) * texel };
        return &m_Glyphs.emplace(key, glyph).first->second;
    }

    std::vector<UIGlyphCache::Region> UIGlyphCache::TakePendingRegions()
    {
        std::vector<Region> regions = std::move(m_Pending);
        m_Pending.clear();
        return regions;
    }

    void UIGlyphCache::Reset()
    {
        m_Glyphs.clear();
        m_ShelfY = m_ShelfHeight = m_CursorX = 0;
        m_Pending.clear();
        m_ResetRequested = false;
    }
}
