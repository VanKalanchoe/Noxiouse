// The UI glyph cache's atlas on the GPU (UI::UIGlyphCache): created on first use, the rectangles of new glyphs copied into it before the
// frame that draws them, emptied between frames when it ran full.
#include "Renderer.h"

#include <cstring>

#include "NoxCore/UI/Rendering/UIGlyphCache.h"

namespace Nox
{
    bool Renderer::uploadGlyphAtlas()
    {
        if (!m_device || !m_resourceHeap)
            return false;
        UI::UIGlyphCache& cache = UI::UIGlyphCache::Get();
        constexpr uint32_t size = UI::UIGlyphCache::kAtlasSize;
        if (!m_glyphAtlas)
        {
            m_glyphAtlas = m_device->createTexture(NRI::TextureDesc{
                .width = size,
                .height = size,
                .mipLevels = 1,
                .sampleCount = 1,
                .usage = NRI::TextureUsage::ShaderResource,
                .format = NRI::ImageFormat::R8_UNORM,
                .sharedAcrossQueues = true
            });
            if (!m_glyphAtlas)
                return false;
            // Its first transition on the transfer queue, ahead of every glyph copy; frames sample only rectangles copied to.
            m_uploads.InitializeTexture(*m_glyphAtlas);
            m_uploads.CommitCopies(true);
            PublishTexture(*m_glyphAtlas);
            cache.SetTextureSlot(m_glyphAtlas->GetDescriptorIndexSlot()); // (from now on small UI text comes from the cache)
        }

        const std::vector<UI::UIGlyphCache::Region> regions = cache.TakePendingRegions();
        if (regions.empty())
            return false;
        // Every rectangle's rows at a 256-byte pitch, each rectangle at a 512-byte boundary: D3D12's placement rules, and the pitch is
        // given to the copy, so Vulkan reads the same layout.
        const auto align = [](uint64_t value, uint64_t to) { return (value + to - 1) / to * to; };
        uint64_t total = 0;
        for (const UI::UIGlyphCache::Region& region : regions)
            total = align(total, 512) + align(region.W, 256) * region.H;
        const StagingSpan span = m_uploads.ReserveStaging(total, 512);
        const uint8_t* pixels = cache.GetPixels();
        uint64_t offset = 0;
        for (const UI::UIGlyphCache::Region& region : regions)
        {
            offset = align(offset, 512);
            const uint32_t pitch = static_cast<uint32_t>(align(region.W, 256));
            for (uint32_t row = 0; row < region.H; ++row)
                std::memcpy(span.data + offset + static_cast<uint64_t>(row) * pitch, pixels + static_cast<size_t>(region.Y + row) * size + region.X, region.W);
            m_uploads.CopyToTextureRegion(span, offset, *m_glyphAtlas, region.X, region.Y, region.W, region.H, pitch);
            offset += static_cast<uint64_t>(pitch) * region.H;
        }
        m_uploads.Commit(span, true); // (the next frame waits for these copies)
        return true;
    }

    void Renderer::resetGlyphAtlasIfFull()
    {
        UI::UIGlyphCache& cache = UI::UIGlyphCache::Get();
        if (!cache.ResetRequested())
            return;
        // Rare (2048^2 texels of glyphs): the frames in flight still sample the atlas, so the GPU finishes them before its space is
        // given out again. Glyphs it could not hold were drawn from the distance field meanwhile.
        m_device->waitIdle();
        cache.Reset();
    }
}
