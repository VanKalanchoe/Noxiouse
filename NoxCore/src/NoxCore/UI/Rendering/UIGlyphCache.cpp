#include "UIGlyphCache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Renderer/Font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

namespace Nox::UI
{
    struct UIGlyphCache::Library
    {
        FT_Library Handle = nullptr;
        ~Library()
        {
            if (Handle)
                FT_Done_FreeType(Handle);
        }
    };

    struct UIGlyphCache::FontFace
    {
        std::vector<uint8_t> Bytes; // (FreeType reads the file in place: kept as long as the face)
        FT_Face Face = nullptr;
        uint32_t Index = 0;         // a small number for the glyph keys
        ~FontFace()
        {
            if (Face)
                FT_Done_Face(Face);
        }
    };

    UIGlyphCache& UIGlyphCache::Get()
    {
        static UIGlyphCache s_Cache;
        return s_Cache;
    }

    UIGlyphCache::UIGlyphCache()
        : m_Library(std::make_unique<Library>()), m_Pixels(static_cast<size_t>(kAtlasSize) * kAtlasSize, 0)
    {
        if (FT_Init_FreeType(&m_Library->Handle) != 0)
        {
            m_Library->Handle = nullptr;
            NOX_CORE_ERROR("UI text: FreeType could not be initialized; text is drawn from the distance field");
        }
    }

    UIGlyphCache::~UIGlyphCache()
    {
        m_Faces.clear(); // (faces before the library)
    }

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
        if (m_Library->Handle && !face->Bytes.empty() &&
            FT_New_Memory_Face(m_Library->Handle, face->Bytes.data(), static_cast<FT_Long>(face->Bytes.size()), 0, &face->Face) != 0)
            face->Face = nullptr;
        if (!face->Face || !FT_IS_SCALABLE(face->Face))
            NOX_CORE_WARN("UI text: a font could not be read as an outline font; its text is drawn from the distance field");
        FontFace* raw = face.get();
        m_Faces.emplace(font.GetCacheId(), std::move(face));
        return raw;
    }

    const UIGlyphCache::Glyph* UIGlyphCache::Find(const Font& font, float pixelSize, uint32_t codepoint, int phase)
    {
        if (m_TextureSlot == ~0u || pixelSize <= 0.0f || pixelSize > kMaxPixelSize)
            return nullptr;
        FontFace* face = FaceOf(font);
        if (!face || !face->Face || !FT_IS_SCALABLE(face->Face))
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

        // The size: `size` pixels from ascender to descender (the font's hhea metrics, as the distance-field layout uses), as an em
        // size in 26.6 fixed point at 72 dpi (1 point = 1 pixel). Unhinted (the layout's advances are unhinted too), the outline moved
        // right by the pen's phase before it is rendered. (FreeType's coverage is exact area coverage: against a 16x-supersampled
        // reference it is within 0.025 per pixel; stb_truetype, used first, was off by up to 0.25.)
        FT_Face ftFace = face->Face;
        const double heightUnits = static_cast<double>(ftFace->ascender) - static_cast<double>(ftFace->descender);
        if (heightUnits <= 0.0)
            return nullptr;
        const double emPixels = size * static_cast<double>(ftFace->units_per_EM) / heightUnits;
        if (FT_Set_Char_Size(ftFace, 0, static_cast<FT_F26Dot6>(std::lround(emPixels * 64.0)), 72, 72) != 0)
            return nullptr;
        const FT_UInt glyphIndex = FT_Get_Char_Index(ftFace, codepoint);
        if (FT_Load_Glyph(ftFace, glyphIndex, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0 || ftFace->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
            return nullptr;
        FT_Outline_Translate(&ftFace->glyph->outline, static_cast<FT_Pos>(phase * 64 / kSubpixelPhases), 0);
        if (FT_Render_Glyph(ftFace->glyph, FT_RENDER_MODE_NORMAL) != 0)
            return nullptr;
        const FT_Bitmap& bitmap = ftFace->glyph->bitmap;
        Glyph glyph;
        const uint32_t width = bitmap.width, height = bitmap.rows;
        if (width == 0 || height == 0 || bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
            return &m_Glyphs.emplace(key, glyph).first->second; // (nothing to draw: a space)

        // A place on the current shelf, or a new shelf below it; a texel of empty border around it (linear sampling never reaches
        // a neighbour).
        const uint32_t paddedW = width + 2, paddedH = height + 2;
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
        for (uint32_t row = 0; row < height; ++row)
            std::memcpy(&m_Pixels[static_cast<size_t>(ay + 1 + row) * kAtlasSize + ax + 1], bitmap.buffer + static_cast<ptrdiff_t>(row) * bitmap.pitch, width);
        if (!m_Pending.empty() && m_Pending.back().Y == ay && m_Pending.back().X + m_Pending.back().W == ax)
        {
            m_Pending.back().W += paddedW;
            m_Pending.back().H = std::max(m_Pending.back().H, paddedH);
        }
        else
            m_Pending.push_back({ ax, ay, paddedW, paddedH });

        const float texel = 1.0f / static_cast<float>(kAtlasSize);
        glyph.X0 = static_cast<int16_t>(ftFace->glyph->bitmap_left);
        glyph.Y0 = static_cast<int16_t>(-ftFace->glyph->bitmap_top);
        glyph.W = static_cast<uint16_t>(width);
        glyph.H = static_cast<uint16_t>(height);
        glyph.UV = glm::vec4(static_cast<float>(ax + 1) * texel, static_cast<float>(ay + 1) * texel,
                             static_cast<float>(ax + 1 + width) * texel, static_cast<float>(ay + 1 + height) * texel);
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
