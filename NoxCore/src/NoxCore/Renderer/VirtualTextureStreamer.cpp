#include "VirtualTextureStreamer.h"

#include <algorithm>

#include "Renderer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/VirtualFileSystem.h"
#include "NoxCore/Profiling/Profiler.h"

namespace Nox
{
    namespace
    {
        constexpr uint32_t NoTile = UINT32_MAX;
        constexpr uint32_t NoSlot = UINT32_MAX;
    }

    VirtualTextureStreamer::VirtualTextureStreamer(Renderer& renderer) : m_Renderer(renderer) {}
    VirtualTextureStreamer::~VirtualTextureStreamer() = default;

    void VirtualTextureStreamer::tileCoord(const VirtualTextureLayout& layout, uint32_t tile, uint32_t mip, uint32_t& outX, uint32_t& outY)
    {
        const VirtualTextureLayout::Mip& tiles = layout.Mips[mip];
        const uint32_t local = tile - tiles.FirstTile;
        outX = local % tiles.TilesX;
        outY = local / tiles.TilesX;
    }

    uint32_t VirtualTextureStreamer::parentTile(const VirtualTextureLayout& layout, uint32_t tile, uint32_t mip)
    {
        if (mip + 1 >= layout.TiledMips)
            return NoTile;
        uint32_t x, y;
        tileCoord(layout, tile, mip, x, y);
        // Power-of-two sizes (ResolveVirtualTexture): a tile's texels are exactly those of a quarter of the tile above.
        const VirtualTextureLayout::Mip& parent = layout.Mips[mip + 1];
        return parent.FirstTile + std::min(y >> 1, parent.TilesY - 1) * parent.TilesX + std::min(x >> 1, parent.TilesX - 1);
    }

    void VirtualTextureStreamer::mapSubtree(VirtualTexture& texture, uint32_t tile, uint32_t mip, uint32_t entry)
    {
        const VirtualTextureLayout& layout = *texture.Header.Virtual;
        uint32_t x, y;
        tileCoord(layout, tile, mip, x, y);
        for (int32_t level = static_cast<int32_t>(mip); level >= 0; --level)
        {
            const uint32_t shift = mip - static_cast<uint32_t>(level);
            const VirtualTextureLayout::Mip& tiles = layout.Mips[level];
            const uint32_t x0 = std::min(x << shift, tiles.TilesX), x1 = std::min((x + 1) << shift, tiles.TilesX);
            const uint32_t y0 = std::min(y << shift, tiles.TilesY), y1 = std::min((y + 1) << shift, tiles.TilesY);
            for (uint32_t row = y0; row < y1; ++row)
            {
                auto first = texture.Entries.begin() + tiles.FirstTile + row * tiles.TilesX;
                std::fill(first + x0, first + x1, entry);
            }
        }
        texture.EntriesDirty = true;
    }

    bool VirtualTextureStreamer::Register(uint32_t slot, const CookedTextureHeader& header)
    {
        if (!header.Virtual)
            return false;
        const VirtualTextureLayout& layout = *header.Virtual;
        if (layout.TiledMips > shaderio::VirtualTextureMaxMips || layout.TileSize != shaderio::VirtualTileSize ||
            layout.Border != shaderio::VirtualTileBorder || slot >= shaderio::MipFeedbackSlots)
        {
            NOX_CORE_ERROR("Virtual textures: {} has a tiling the shaders do not read ({} tiled mips): it draws with its tail",
                           header.Path.filename().string(), layout.TiledMips);
            return false;
        }
        Unregister(slot);

        if (!m_PageTableAllocator)
            m_PageTableAllocator.emplace(shaderio::VirtualTileCapacity);
        const OffsetAllocator::Allocation range = m_PageTableAllocator->allocate(layout.TileCount);
        if (range.offset == OffsetAllocator::Allocation::NO_SPACE)
        {
            NOX_CORE_ERROR("Virtual textures: no page table room for {} ({} tiles): it draws with its tail", header.Path.filename().string(),
                           layout.TileCount);
            return false;
        }

        uint32_t id;
        if (!m_FreeIds.empty())
        {
            id = m_FreeIds.back();
            m_FreeIds.pop_back();
        }
        else
        {
            id = static_cast<uint32_t>(m_Textures.size());
            m_Textures.emplace_back();
        }
        VirtualTexture& texture = m_Textures[id];
        texture = VirtualTexture{};
        texture.Valid = true;
        texture.Slot = slot;
        texture.Header = header;
        texture.Pool = poolFor(header.Texture.Format);
        texture.PageTable = range;
        texture.Tiles.resize(layout.TileCount);
        for (uint32_t mip = 0; mip < layout.TiledMips; ++mip)
        {
            const VirtualTextureLayout::Mip& tiles = layout.Mips[mip];
            for (uint32_t tile = tiles.FirstTile; tile < tiles.FirstTile + tiles.TilesX * tiles.TilesY; ++tile)
                texture.Tiles[tile].Mip = static_cast<uint8_t>(mip);
        }
        // Its page table, in the frame that publishes it: every tile on the tail.
        texture.Entries.assign(layout.TileCount, shaderio::VirtualPageUnmapped);
        texture.EntriesDirty = true;

        m_BySlot[slot] = id;
        m_ByOffset[range.offset] = id;
        stageDescriptor(texture);
        ++m_Stats.Textures;
        m_Stats.Tiles += layout.TileCount;
        return true;
    }

    void VirtualTextureStreamer::Unregister(uint32_t slot)
    {
        const auto found = m_BySlot.find(slot);
        if (found == m_BySlot.end())
            return;
        const uint32_t id = found->second;
        VirtualTexture& texture = m_Textures[id];

        for (Tile& tile : texture.Tiles)
        {
            switch (tile.State)
            {
            case TileState::Reading:
                // The read writes into its staging: that and the slot return once the read is done.
                m_Orphans.push_back({ tile.Staging, std::move(tile.Read), texture.Pool, tile.Slot });
                m_Pools[texture.Pool].Owners[tile.Slot] = { UINT32_MAX, 0 };
                --m_Stats.LoadingTiles;
                break;
            case TileState::Uploading:
                // Its copy is queued on the transfer queue, ahead of any later copy into the same slot.
                releaseSlot(texture.Pool, tile.Slot, MAX_FRAMES_IN_FLIGHT + 1);
                --m_Stats.LoadingTiles;
                break;
            case TileState::Resident:
                releaseSlot(texture.Pool, tile.Slot, MAX_FRAMES_IN_FLIGHT + 2);
                --m_Stats.ResidentTiles;
                break;
            case TileState::Absent:
                break;
            }
        }
        std::erase_if(m_InFlight, [id](const TileRef& ref) { return ref.Texture == id; });
        std::erase_if(m_Candidates, [id](const TileRef& ref) { return ref.Texture == id; });

        // Frames in flight still read its page table entries.
        m_ReleasingRanges.push_back({ texture.PageTable, m_Frame + MAX_FRAMES_IN_FLIGHT + 2 });
        m_ByOffset.erase(texture.PageTable.offset);
        m_BySlot.erase(found);
        --m_Stats.Textures;
        m_Stats.Tiles -= std::min<uint32_t>(m_Stats.Tiles, static_cast<uint32_t>(texture.Tiles.size()));
        texture = VirtualTexture{};
        m_FreeIds.push_back(id);
    }

    void VirtualTextureStreamer::ApplyFeedback(const shaderio::VirtualTextureFeedback& feedback)
    {
        m_FeedbackFrame = m_Frame;
        const uint32_t count = std::min(feedback.count, shaderio::VirtualTextureRequestCapacity);
        m_Stats.Requests = count;
        m_Stats.DroppedRequests = feedback.count - count;
        for (uint32_t index = 0; index < count; ++index)
        {
            const uint32_t global = feedback.requests[index] & ~shaderio::VirtualRequestResident;
            auto owner = m_ByOffset.upper_bound(global);
            if (owner == m_ByOffset.begin())
                continue;
            --owner;
            const uint32_t id = owner->second;
            VirtualTexture& texture = m_Textures[id];
            const uint32_t requested = global - owner->first;
            if (requested >= texture.Tiles.size())
                continue; // unloaded since

            // The tile and the tiles above it (residency is ancestor closed): wanted now, the missing ones asked for.
            for (uint32_t tile = requested; tile != NoTile;)
            {
                Tile& state = texture.Tiles[tile];
                if (state.LastWanted == m_Frame)
                    break; // and the tiles above it, by an earlier request of this readback
                state.LastWanted = m_Frame;
                if (state.State == TileState::Absent && !state.Failed)
                    m_Candidates.push_back({ id, tile });
                tile = parentTile(*texture.Header.Virtual, tile, state.Mip);
            }
        }
    }

    void VirtualTextureStreamer::Update(uint64_t& uploadBudget)
    {
        NOX_PROFILE_SCOPE("Virtual Texture Streaming");
        ++m_Frame;

        std::erase_if(m_Orphans, [this](OrphanRead& orphan)
        {
            if (!orphan.Read.IsReady())
                return false;
            try { orphan.Read.Get(); } catch (...) {}
            m_Renderer.m_uploads.ReleaseStaging(orphan.Staging);
            if (orphan.Pool < m_Pools.size())
                releaseSlot(orphan.Pool, orphan.Slot, 1); // never copied to
            return true;
        });
        for (Pool& pool : m_Pools)
        {
            std::erase_if(pool.Releasing, [&](const std::pair<uint32_t, uint64_t>& released)
            {
                if (m_Frame < released.second)
                    return false;
                pool.FreeSlots.push_back(released.first);
                return true;
            });
        }
        std::erase_if(m_ReleasingRanges, [this](const std::pair<OffsetAllocator::Allocation, uint64_t>& released)
        {
            if (m_Frame < released.second)
                return false;
            m_PageTableAllocator->free(released.first);
            return true;
        });

        // Reads and copies in flight, coarser tiles first: a tile becomes visible no later than the tile above it.
        std::sort(m_InFlight.begin(), m_InFlight.end(), [this](const TileRef& a, const TileRef& b)
        {
            return m_Textures[a.Texture].Tiles[a.Tile].Mip > m_Textures[b.Texture].Tiles[b.Tile].Mip;
        });
        std::erase_if(m_InFlight, [this](const TileRef& ref) { return advance(ref); });

        // The missing tiles the feedback asked for, coarser first (a tile's parent before it; the coarse ones cover the most).
        m_Stats.WantedNotResident = 0;
        if (!m_Candidates.empty())
        {
            std::sort(m_Candidates.begin(), m_Candidates.end(), [this](const TileRef& a, const TileRef& b)
            {
                const uint8_t mipA = m_Textures[a.Texture].Tiles[a.Tile].Mip, mipB = m_Textures[b.Texture].Tiles[b.Tile].Mip;
                if (mipA != mipB)
                    return mipA > mipB;
                return a.Texture != b.Texture ? a.Texture < b.Texture : a.Tile < b.Tile;
            });
            m_Candidates.erase(std::unique(m_Candidates.begin(), m_Candidates.end(),
                                           [](const TileRef& a, const TileRef& b) { return a.Texture == b.Texture && a.Tile == b.Tile; }),
                               m_Candidates.end());

            m_Waiting.assign(m_Pools.size(), 0);
            m_Victims.assign(m_Pools.size(), {});
            m_VictimCursor.assign(m_Pools.size(), 0);
            m_VictimsGathered.assign(m_Pools.size(), false);
            uint32_t starts = 0;
            for (const TileRef& candidate : m_Candidates)
            {
                VirtualTexture& texture = m_Textures[candidate.Texture];
                if (!texture.Valid)
                    continue;
                Tile& tile = texture.Tiles[candidate.Tile];
                if (tile.State != TileState::Absent || tile.Failed)
                    continue;
                const uint32_t parent = parentTile(*texture.Header.Virtual, candidate.Tile, tile.Mip);
                if (parent != NoTile && texture.Tiles[parent].State == TileState::Absent)
                    continue; // its parent could not start (no room): it comes first
                if (starts >= StartsPerFrame || texture.Header.Virtual->TileBytes() > uploadBudget)
                    break;
                const uint32_t slot = takeSlot(texture.Pool, tile.Mip);
                if (slot == NoSlot)
                {
                    ++m_Stats.WantedNotResident;
                    continue; // another pool may have room
                }
                startLoad(candidate.Texture, candidate.Tile, slot, uploadBudget);
                if (tile.State == TileState::Absent)
                {
                    m_Pools[texture.Pool].FreeSlots.push_back(slot); // no staging room this frame
                    break;
                }
                ++starts;
            }
            m_Candidates.clear();
        }

        // The page tables that changed, in the next frame's commands.
        for (VirtualTexture& texture : m_Textures)
        {
            if (!texture.Valid || !texture.EntriesDirty)
                continue;
            m_Renderer.stageGeometryPatch(Renderer::GeometryPatchTarget::VirtualPages, sizeof(uint32_t) * uint64_t(texture.PageTable.offset),
                                          texture.Entries.data(), sizeof(uint32_t) * texture.Entries.size());
            texture.EntriesDirty = false;
        }

        // What the feedback wants now (resident tiles wanted within HoldFrames, missing ones asked for), every few frames.
        if (m_Frame % shaderio::VirtualTileTouchInterval == 0)
        {
            m_Stats.WantedTiles = 0;
            for (const VirtualTexture& texture : m_Textures)
            {
                if (!texture.Valid)
                    continue;
                for (const Tile& tile : texture.Tiles)
                    m_Stats.WantedTiles += tile.LastWanted != 0 && tile.LastWanted + HoldFrames >= m_FeedbackFrame && !tile.Failed;
            }
        }
        m_Stats.Pools = static_cast<uint32_t>(m_Pools.size());
        m_Stats.PoolTiles = 0;
        m_Stats.PoolBytes = 0;
        for (const Pool& pool : m_Pools)
        {
            m_Stats.PoolTiles += pool.TilesPerSide * pool.TilesPerSide;
            const uint32_t texels = pool.TilesPerSide * shaderio::VirtualTileStride;
            m_Stats.PoolBytes += NRI::estimateTextureBytes(NRI::TextureDesc{ .width = texels, .height = texels, .format = pool.Format });
        }
        NOX_PROFILE_COUNTER("Virtual Textures", m_Stats.Textures);
        NOX_PROFILE_COUNTER("Virtual Tiles Resident", m_Stats.ResidentTiles);
        NOX_PROFILE_COUNTER("Virtual Tiles Loading", m_Stats.LoadingTiles);
        NOX_PROFILE_COUNTER("Virtual Tiles Wanted", m_Stats.WantedTiles);
        NOX_PROFILE_COUNTER("Virtual Pool Tiles", m_Stats.PoolTiles);
        NOX_PROFILE_COUNTER("Virtual Pool MB", static_cast<int64_t>(m_Stats.PoolBytes >> 20));
        NOX_PROFILE_COUNTER("Virtual Tile Requests", m_Stats.Requests);
        NOX_PROFILE_COUNTER("Virtual Tiles Evicted", static_cast<int64_t>(m_Stats.TileEvictions));
    }

    uint32_t VirtualTextureStreamer::takeSlot(uint32_t poolIndex, uint32_t candidateMip)
    {
        Pool& pool = m_Pools[poolIndex];
        if (!pool.FreeSlots.empty())
        {
            const uint32_t slot = pool.FreeSlots.back();
            pool.FreeSlots.pop_back();
            return slot;
        }

        // Full: a tile goes to make room, its slot free once the frames that may sample it are done -- one per candidate waiting
        // beyond the slots already on their way back. Tiles not wanted any more first (least recently wanted first), then wanted
        // ones finer than the candidate (finest first): a coarse tile covers more and every finer one needs it.
        if (m_Waiting[poolIndex]++ < pool.Releasing.size())
            return NoSlot;
        if (!m_VictimsGathered[poolIndex])
        {
            m_VictimsGathered[poolIndex] = true;
            for (uint32_t slot = 0; slot < pool.Owners.size(); ++slot)
            {
                const auto [id, tile] = pool.Owners[slot];
                if (id == UINT32_MAX)
                    continue;
                const Tile& state = m_Textures[id].Tiles[tile];
                if (state.State == TileState::Resident && state.LoadingChildren == 0)
                    m_Victims[poolIndex].push_back({ state.LastWanted + HoldFrames >= m_FeedbackFrame, state.Mip, state.LastWanted, id, tile });
            }
            std::sort(m_Victims[poolIndex].begin(), m_Victims[poolIndex].end(), [](const Victim& a, const Victim& b)
            {
                if (a.Wanted != b.Wanted)
                    return !a.Wanted;
                if (a.Wanted && a.Mip != b.Mip)
                    return a.Mip < b.Mip;
                return a.LastWanted < b.LastWanted;
            });
        }
        std::vector<Victim>& victims = m_Victims[poolIndex];
        size_t& cursor = m_VictimCursor[poolIndex];
        while (cursor < victims.size())
        {
            const Victim& victim = victims[cursor];
            if (victim.Wanted && !(victim.Mip < candidateMip))
                return NoSlot; // the rest are at least as important
            ++cursor;
            const VirtualTexture& texture = m_Textures[victim.Texture];
            if (texture.Valid && victim.Tile < texture.Tiles.size() && texture.Tiles[victim.Tile].State == TileState::Resident &&
                texture.Tiles[victim.Tile].LoadingChildren == 0)
            {
                evict(victim.Texture, victim.Tile);
                return NoSlot;
            }
        }
        return NoSlot;
    }

    void VirtualTextureStreamer::startLoad(uint32_t id, uint32_t index, uint32_t slot, uint64_t& uploadBudget)
    {
        VirtualTexture& texture = m_Textures[id];
        const VirtualTextureLayout& layout = *texture.Header.Virtual;
        const uint64_t bytes = layout.TileBytes();
        std::optional<StagingSpan> staging = m_Renderer.m_uploads.TryReserveStaging(bytes, 16);
        if (!staging)
            return;

        Tile& tile = texture.Tiles[index];
        uint32_t x, y;
        tileCoord(layout, index, tile.Mip, x, y);
        tile.State = TileState::Reading;
        tile.Slot = slot;
        tile.Staging = *staging;
        m_Pools[texture.Pool].Owners[slot] = { id, index };
        // From the cooked file straight into staging (a pack's entry as a loose file's range).
        tile.Read = JobSystem::Get().AsyncIO("Read Virtual Texture Tile", JobPriority::High,
            [file = texture.Header.Path, offset = layout.TileFileOffset(tile.Mip, x, y), bytes, destination = staging->data](const CancellationToken&)
            {
                return VFS::ReadRange(file, offset, bytes, destination);
            });
        const uint32_t parent = parentTile(layout, index, tile.Mip);
        if (parent != NoTile)
            ++texture.Tiles[parent].LoadingChildren;
        uploadBudget -= std::min(uploadBudget, bytes);
        ++m_Stats.LoadingTiles;
        m_InFlight.push_back({ id, index });
    }

    bool VirtualTextureStreamer::advance(const TileRef& ref)
    {
        VirtualTexture& texture = m_Textures[ref.Texture];
        Tile& tile = texture.Tiles[ref.Tile];
        const VirtualTextureLayout& layout = *texture.Header.Virtual;
        const uint32_t parent = parentTile(layout, ref.Tile, tile.Mip);
        if (tile.State == TileState::Reading)
        {
            if (!tile.Read.IsReady())
                return false;
            bool read = false;
            try { read = tile.Read.Get(); } catch (...) { read = false; }
            if (!read)
            {
                NOX_CORE_ERROR("Virtual textures: could not read a tile of {}", texture.Header.Path.generic_string());
                m_Renderer.m_uploads.ReleaseStaging(tile.Staging);
                releaseSlot(texture.Pool, tile.Slot, 1);
                tile.Staging = {};
                tile.State = TileState::Absent;
                tile.Failed = true;
                if (parent != NoTile)
                    --texture.Tiles[parent].LoadingChildren;
                --m_Stats.LoadingTiles;
                return true;
            }

            // The tile, borders included, into its pool slot.
            Pool& pool = m_Pools[texture.Pool];
            const uint32_t x = (tile.Slot % pool.TilesPerSide) * shaderio::VirtualTileStride;
            const uint32_t y = (tile.Slot / pool.TilesPerSide) * shaderio::VirtualTileStride;
            m_Renderer.m_uploads.CopyToTextureRegion(tile.Staging, 0, *pool.Image, x, y, shaderio::VirtualTileStride, shaderio::VirtualTileStride);
            tile.CopiesComplete = m_Renderer.m_uploads.Commit(tile.Staging, false);
            tile.Staging = {};
            tile.State = TileState::Uploading;
        }
        if (tile.State == TileState::Uploading && m_Renderer.GetCompletedUploadValue() >= tile.CopiesComplete &&
            (parent == NoTile || texture.Tiles[parent].State == TileState::Resident))
        {
            activate(texture, ref.Tile);
            return true;
        }
        return false;
    }

    void VirtualTextureStreamer::activate(VirtualTexture& texture, uint32_t index)
    {
        // Every tile below it maps a tile above it (none of them is resident without it): they map it now.
        Tile& tile = texture.Tiles[index];
        tile.State = TileState::Resident;
        mapSubtree(texture, index, tile.Mip, encodeEntry(tile.Slot, tile.Mip));
        --m_Stats.LoadingTiles;
        ++m_Stats.ResidentTiles;
        ++m_Stats.TileLoads;
    }

    void VirtualTextureStreamer::evict(uint32_t id, uint32_t index)
    {
        // A leaf (no child resident or loading): it and the tiles below it map its parent again, or the tail.
        VirtualTexture& texture = m_Textures[id];
        Tile& tile = texture.Tiles[index];
        const uint32_t parent = parentTile(*texture.Header.Virtual, index, tile.Mip);
        mapSubtree(texture, index, tile.Mip, parent == NoTile ? shaderio::VirtualPageUnmapped : texture.Entries[parent]);
        releaseSlot(texture.Pool, tile.Slot, MAX_FRAMES_IN_FLIGHT + 2);
        tile.State = TileState::Absent;
        if (parent != NoTile)
            --texture.Tiles[parent].LoadingChildren;
        --m_Stats.ResidentTiles;
        ++m_Stats.TileEvictions;
    }

    void VirtualTextureStreamer::releaseSlot(uint32_t poolIndex, uint32_t slot, uint64_t frames)
    {
        Pool& pool = m_Pools[poolIndex];
        pool.Owners[slot] = { UINT32_MAX, 0 };
        pool.Releasing.push_back({ slot, m_Frame + frames });
    }

    uint32_t VirtualTextureStreamer::poolFor(NRI::ImageFormat format)
    {
        for (uint32_t index = 0; index < m_Pools.size(); ++index)
        {
            if (m_Pools[index].Format == format)
                return index;
        }
        Pool& pool = m_Pools.emplace_back();
        pool.Format = format;
        pool.TilesPerSide = m_PoolTilesPerSide;
        createPoolImage(pool);
        return static_cast<uint32_t>(m_Pools.size() - 1);
    }

    void VirtualTextureStreamer::createPoolImage(Pool& pool)
    {
        const uint32_t texels = pool.TilesPerSide * shaderio::VirtualTileStride;
        pool.Image = m_Renderer.m_device->createTexture(NRI::TextureDesc{
            .width = texels,
            .height = texels,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ShaderResource,
            .format = pool.Format,
            .sharedAcrossQueues = true
        });
        // Its first transition on the transfer queue, ahead of every tile copy into it; frames sample only slots tiles were copied to.
        m_Renderer.m_uploads.InitializeTexture(*pool.Image);
        m_Renderer.m_uploads.CommitCopies(true);
        m_Renderer.PublishTexture(*pool.Image);

        const uint32_t slots = pool.TilesPerSide * pool.TilesPerSide;
        pool.FreeSlots.resize(slots);
        for (uint32_t slot = 0; slot < slots; ++slot)
            pool.FreeSlots[slot] = slots - 1 - slot; // slot 0 first
        pool.Owners.assign(slots, { UINT32_MAX, 0 });
        pool.Releasing.clear();
        NOX_CORE_INFO("Virtual textures: pool of format {} with {}x{} tiles ({} MB)", static_cast<uint32_t>(pool.Format), pool.TilesPerSide, pool.TilesPerSide,
                      NRI::estimateTextureBytes(NRI::TextureDesc{ .width = texels, .height = texels, .format = pool.Format }) >> 20);
    }

    void VirtualTextureStreamer::releasePoolImage(Pool& pool)
    {
        if (pool.Image)
            Renderer::DeferAssetRelease(Ref<Asset>(pool.Image));
        pool.Image = nullptr;
    }

    void VirtualTextureStreamer::stageDescriptor(const VirtualTexture& texture)
    {
        const VirtualTextureLayout& layout = *texture.Header.Virtual;
        const Pool& pool = m_Pools[texture.Pool];
        shaderio::GpuVirtualTexture descriptor{};
        descriptor.width = texture.Header.Texture.Width;
        descriptor.height = texture.Header.Texture.Height;
        descriptor.mipLevels = texture.Header.Texture.MipLevels;
        descriptor.tiledMips = layout.TiledMips;
        descriptor.pageTableOffset = texture.PageTable.offset;
        descriptor.poolTextureIndex = pool.Image->GetDescriptorIndexSlot();
        descriptor.poolTilesPerRow = pool.TilesPerSide;
        descriptor.poolInvSize = 1.0f / float(pool.TilesPerSide * shaderio::VirtualTileStride);
        for (uint32_t mip = 0; mip < layout.TiledMips; ++mip)
            descriptor.firstTile[mip] = layout.Mips[mip].FirstTile;
        m_Renderer.stageGeometryPatch(Renderer::GeometryPatchTarget::VirtualTextures, sizeof(descriptor) * uint64_t(texture.Slot), &descriptor,
                                      sizeof(descriptor));
    }

    void VirtualTextureStreamer::dropAllTiles()
    {
        for (VirtualTexture& texture : m_Textures)
        {
            if (!texture.Valid)
                continue;
            for (Tile& tile : texture.Tiles)
            {
                if (tile.State == TileState::Reading)
                {
                    tile.Read.Wait();
                    m_Renderer.m_uploads.ReleaseStaging(tile.Staging);
                }
                tile.State = TileState::Absent;
                tile.LoadingChildren = 0;
                tile.Staging = {};
                tile.Read = {};
            }
            std::fill(texture.Entries.begin(), texture.Entries.end(), shaderio::VirtualPageUnmapped);
            texture.EntriesDirty = true;
        }
        for (OrphanRead& orphan : m_Orphans)
        {
            orphan.Read.Wait();
            m_Renderer.m_uploads.ReleaseStaging(orphan.Staging);
        }
        m_Orphans.clear();
        m_InFlight.clear();
        m_Candidates.clear();
        m_Stats.ResidentTiles = 0;
        m_Stats.LoadingTiles = 0;
    }

    void VirtualTextureStreamer::SetPoolTilesPerSide(uint32_t tiles)
    {
        tiles = std::clamp(tiles, 4u, MaxPoolTilesPerSide);
        if (tiles == m_PoolTilesPerSide)
            return;
        m_PoolTilesPerSide = tiles;
        if (m_Pools.empty())
            return;

        // A debugging action: everything the pools hold goes, after the GPU (every queue) is done with them.
        m_Renderer.m_device->waitIdle();
        dropAllTiles();
        for (Pool& pool : m_Pools)
        {
            releasePoolImage(pool);
            pool.TilesPerSide = tiles;
            createPoolImage(pool);
        }
        for (const VirtualTexture& texture : m_Textures)
        {
            if (texture.Valid)
                stageDescriptor(texture);
        }
    }

    void VirtualTextureStreamer::Shutdown()
    {
        for (VirtualTexture& texture : m_Textures)
            for (Tile& tile : texture.Tiles)
                tile.Read.Wait();
        for (OrphanRead& orphan : m_Orphans)
            orphan.Read.Wait();
        m_Orphans.clear();
        m_InFlight.clear();
        m_Candidates.clear();
        m_Textures.clear();
        m_FreeIds.clear();
        m_BySlot.clear();
        m_ByOffset.clear();
        for (Pool& pool : m_Pools)
            pool.Image = nullptr;
        m_Pools.clear();
        m_ReleasingRanges.clear();
        m_PageTableAllocator.reset();
    }
}
