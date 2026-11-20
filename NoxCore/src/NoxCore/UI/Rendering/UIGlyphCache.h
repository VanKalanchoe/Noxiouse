#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace Nox
{
    class Font;
}

namespace Nox::UI
{
    // Screen text at the size it is shown (Unreal Slate's font cache, Dear ImGui 1.92's dynamic fonts): each glyph is rasterized
    // once per pixel size and sub-pixel phase by FreeType into one 8-bit coverage atlas, and drawn texel for pixel. A multi-
    // channel distance field shrunk from a large atlas cannot do this: below its size the distance range covers less than a pixel
    // and edges go wavy, and nothing snaps to the pixel grid. Text above kMaxPixelSize keeps the distance field (it scales).
    //
    // Sizes are cached in steps of 1/2 px (a zoomed graph shows many sizes) and the pen's x position in steps of 1/4 px (the
    // glyph is rasterized with that offset, so letters keep their spacing while every quad sits on whole pixels); baselines are
    // snapped to whole pixels. Glyphs go into shelves of the atlas; the Renderer uploads the rectangles they changed before the
    // frame that draws them (TakePendingRegions). When the atlas is full, glyphs it cannot hold this frame are drawn from the distance
    // field and the atlas is emptied between frames, once the GPU is done with it (ResetRequested / Reset).
    //
    // Main thread only.
    class UIGlyphCache
    {
    public:
        static UIGlyphCache& Get();
        ~UIGlyphCache();

        static constexpr uint32_t kAtlasSize = 2048;   // texels per side (R8: 4 MB)
        static constexpr float kMaxPixelSize = 48.0f;  // larger text is drawn from the distance field
        static constexpr int kSubpixelPhases = 4;      // pen x positions per pixel

        struct Glyph
        {
            int16_t X0 = 0, Y0 = 0; // the bitmap's top-left from the snapped pen (x) and baseline (y), whole pixels (y down)
            uint16_t W = 0, H = 0;  // 0: nothing to draw (a space)
            glm::vec4 UV{ 0.0f };   // min u, min v, max u, max v
        };

        // The glyph of `codepoint` at `pixelSize` (the size the UI uses: ascender to descender) with the pen `phase` quarters of a
        // pixel right of a whole pixel; rasterized now when new. Null: not available this frame (no GPU atlas yet, the font has no
        // outlines, or the atlas is full until the frame ends) -- draw it from the distance field.
        const Glyph* Find(const Font& font, float pixelSize, uint32_t codepoint, int phase);
        // The size glyphs are rasterized at for a requested size (steps of 1/2 px).
        static float QuantizeSize(float pixelSize);

        // The atlas texture's bindless slot (set by the Renderer once it exists); ~0u before.
        uint32_t GetTextureSlot() const { return m_TextureSlot; }
        void SetTextureSlot(uint32_t slot) { m_TextureSlot = slot; }

        // For the Renderer: the rectangles of the atlas written since the last call, and the atlas's pixels (rows of kAtlasSize
        // bytes) to copy them from. Each is a run of glyphs placed side by side on one shelf: it holds only them and texels no glyph
        // uses, so copying it never touches what a frame still on the GPU reads.
        struct Region
        {
            uint32_t X = 0, Y = 0, W = 0, H = 0;
        };
        std::vector<Region> TakePendingRegions();
        const uint8_t* GetPixels() const { return m_Pixels.data(); }
        // The atlas ran out of room this frame: empty it between frames, once the GPU no longer reads it.
        bool ResetRequested() const { return m_ResetRequested; }
        void Reset();

    private:
        UIGlyphCache();
        struct Library;
        struct FontFace;
        FontFace* FaceOf(const Font& font);

        std::unique_ptr<Library> m_Library;
        std::unordered_map<uint64_t, std::unique_ptr<FontFace>> m_Faces; // by Font::GetCacheId
        std::unordered_map<uint64_t, Glyph> m_Glyphs;                    // by face, size step, phase, codepoint
        std::vector<uint8_t> m_Pixels;
        uint32_t m_ShelfY = 0, m_ShelfHeight = 0, m_CursorX = 0;
        std::vector<Region> m_Pending;
        bool m_ResetRequested = false;
        uint32_t m_TextureSlot = ~0u;
    };
}
