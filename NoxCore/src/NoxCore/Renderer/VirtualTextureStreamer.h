#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

#include "DataTypes.h" // shaderio, and OffsetAllocator through GeometryArena.h
#include "UploadManager.h"
#include "NoxCore/Asset/TextureImporter.h"
#include "NoxCore/Tasks/JobSystem.h"

namespace Nox
{
    class Renderer;

    // Streaming virtual texturing (§5.12.2, UE5's SVT): a virtual texture keeps its tail image (the mips no larger than a tile,
    // loaded with it); its other mips are 128x128-texel tiles (4-texel borders) read from the cooked file into slots of a physical
    // pool of its format as the tile feedback asks for them, and evicted, least recently wanted first, when the pool is full.
    //
    // - Residency is ancestor closed: a tile loads once the tile above it (its parent, a mip coarser) is resident or loading,
    //   becomes visible after it, and is evicted only when none of its children is resident or loading. The page table then maps
    //   every tile to the finest resident tile at or above it in one entry: a sample needs one lookup and never finds a hole.
    // - A tile's texels arrive on the transfer queue into a slot nothing samples yet; its page table entries change in a frame by
    //   that frame's own commands (Renderer's streaming patches, as geometry pages do), so every pass of a frame samples the same
    //   residency. An evicted tile's slot is reused only once the frames that may still sample it are done.
    // - Ray-traced and path-traced hits sample what the raster made resident (and the path tracer asks for what it sees directly).
    //
    // Owned by the renderer, main thread.
    class VirtualTextureStreamer
    {
    public:
        // Tile reads started per frame (the upload budget bounds their bytes too).
        static constexpr uint32_t StartsPerFrame = 256;
        // A tile is kept this long after the feedback last wanted it before it may be evicted for a coarser one or a new texture's.
        static constexpr uint64_t HoldFrames = 4 * shaderio::VirtualTileTouchInterval;
        // Pool slots per side: 48 x 136 texels = 6528² (2304 tiles; 41 MB of BC7, 20 MB of BC1 / BC4). One pool per format.
        static constexpr uint32_t DefaultPoolTilesPerSide = 48;
        static constexpr uint32_t MaxPoolTilesPerSide = 16384 / shaderio::VirtualTileStride;

        struct Stats
        {
            uint32_t Textures = 0;
            uint32_t Tiles = 0;          // tiles of all virtual textures
            uint32_t ResidentTiles = 0;
            uint32_t LoadingTiles = 0;
            uint32_t Pools = 0;
            uint32_t PoolTiles = 0;      // slots of all pools
            uint64_t PoolBytes = 0;
            uint32_t WantedTiles = 0;    // tiles the feedback wanted recently (resident or not)
            uint64_t TileLoads = 0;      // since start
            uint64_t TileEvictions = 0;
            uint32_t Requests = 0;       // in the newest feedback
            uint32_t DroppedRequests = 0;
            uint32_t WantedNotResident = 0; // requested tiles no pool had room for (last update)
        };

        explicit VirtualTextureStreamer(Renderer& renderer);
        ~VirtualTextureStreamer();
        VirtualTextureStreamer(const VirtualTextureStreamer&) = delete;
        VirtualTextureStreamer& operator=(const VirtualTextureStreamer&) = delete;

        // A loaded virtual texture: its tail image's slot (the material texture index) and its cooked header. False when it cannot
        // stream (no page table room, more tiled mips than the shaders handle): it draws with its tail alone.
        bool Register(uint32_t slot, const CookedTextureHeader& header);
        // Its tail image was unloaded: its tiles and page table entries go once frames are done with them.
        void Unregister(uint32_t slot);
        bool IsRegistered(uint32_t slot) const { return m_BySlot.contains(slot); }

        // The newest feedback readback (VirtualTextureFeedback).
        void ApplyFeedback(const shaderio::VirtualTextureFeedback& feedback);
        // Once per frame before the frame is recorded: finishes reads and copies, makes tiles visible, evicts and starts reads within
        // the upload budget (shared with the asset loads), and stages the page tables that changed.
        void Update(uint64_t& uploadBudget);

        // Whether textures cooked as virtual load as virtual textures (else they load mip streamed, their tiles put together into
        // whole mips): for the textures loaded after the change.
        void SetEnabled(bool enabled) { m_Enabled = enabled; }
        bool IsEnabled() const { return m_Enabled; }
        // Slots per pool side (each format's pool is made again at this size, its tiles streamed in again).
        void SetPoolTilesPerSide(uint32_t tiles);
        uint32_t GetPoolTilesPerSide() const { return m_PoolTilesPerSide; }
        const Stats& GetStats() const { return m_Stats; }
        bool IsIdle() const { return m_InFlight.empty() && m_Orphans.empty(); }
        // While the renderer still exists: waits for reads in flight and drops every tile and pool.
        void Shutdown();

    private:
        enum class TileState : uint8_t
        {
            Absent,
            Reading,   // the file read into staging is running
            Uploading, // the copy into its pool slot is queued on the transfer queue
            Resident
        };

        struct Tile
        {
            TileState State = TileState::Absent;
            bool Failed = false;           // its read failed: not asked for again, the tiles above stand in
            uint8_t Mip = 0;
            uint8_t LoadingChildren = 0;   // children resident or loading (it is evicted only without any)
            uint32_t Slot = 0;             // its pool slot while not Absent
            uint64_t LastWanted = 0;       // streamer frame of the newest feedback that wanted it
            StagingSpan Staging;
            TaskFuture<bool> Read;
            uint64_t CopiesComplete = 0;
        };

        struct VirtualTexture
        {
            bool Valid = false;
            uint32_t Slot = 0;             // its tail image's
            CookedTextureHeader Header;
            uint32_t Pool = 0;
            OffsetAllocator::Allocation PageTable;
            std::vector<Tile> Tiles;       // page table order: mip by mip, row by row
            std::vector<uint32_t> Entries; // the page table as the GPU gets it
            bool EntriesDirty = false;
        };

        struct Pool
        {
            NRI::ImageFormat Format = NRI::ImageFormat::None;
            Ref<Texture2D> Image;
            uint32_t TilesPerSide = 0;
            std::vector<uint32_t> FreeSlots;
            // Slots of evicted tiles, free once the frames that may sample them are done: (slot, streamer frame they are free at).
            std::vector<std::pair<uint32_t, uint64_t>> Releasing;
            // Per slot, the tile in it: (texture id, tile index), UINT32_MAX texture when free.
            std::vector<std::pair<uint32_t, uint32_t>> Owners;
        };

        // A tile whose texture went while it was read: its staging and slot return once the read is done.
        struct OrphanRead
        {
            StagingSpan Staging;
            TaskFuture<bool> Read;
            uint32_t Pool = 0;
            uint32_t Slot = 0;
        };

        struct TileRef
        {
            uint32_t Texture = 0;
            uint32_t Tile = 0;
        };

        // Position of a tile in its mip: (mip, column, row).
        static void tileCoord(const VirtualTextureLayout& layout, uint32_t tile, uint32_t mip, uint32_t& outX, uint32_t& outY);
        // The tile above (a mip coarser), UINT32_MAX for the coarsest tiled mip's.
        static uint32_t parentTile(const VirtualTextureLayout& layout, uint32_t tile, uint32_t mip);
        // Sets the page table entries of a tile and every tile below it to `entry`.
        static void mapSubtree(VirtualTexture& texture, uint32_t tile, uint32_t mip, uint32_t entry);
        static uint32_t encodeEntry(uint32_t slot, uint32_t mip) { return slot | (mip << shaderio::VirtualPageMipShift); }

        uint32_t poolFor(NRI::ImageFormat format);
        void createPoolImage(Pool& pool);
        void releasePoolImage(Pool& pool);
        void stageDescriptor(const VirtualTexture& texture);
        // An eviction candidate (takeSlot): tiles no longer wanted first, least recently wanted first, then wanted ones, finest first.
        struct Victim
        {
            bool Wanted = false;
            uint8_t Mip = 0;
            uint64_t LastWanted = 0;
            uint32_t Texture = 0;
            uint32_t Tile = 0;
        };
        // A free slot of the pool; when it is full, evicts what the candidate (its mip) may replace, its slot free a few frames later.
        // UINT32_MAX when no slot is free now.
        uint32_t takeSlot(uint32_t poolIndex, uint32_t candidateMip);
        void startLoad(uint32_t id, uint32_t tile, uint32_t slot, uint64_t& uploadBudget);
        // True when the tile left the in-flight list (resident, failed, or gone).
        bool advance(const TileRef& ref);
        void activate(VirtualTexture& texture, uint32_t tile);
        void evict(uint32_t id, uint32_t tile);
        void releaseSlot(uint32_t poolIndex, uint32_t slot, uint64_t frames);
        // Drops every tile of every texture (a pool changed): their page tables back to the tail.
        void dropAllTiles();

        Renderer& m_Renderer;
        std::vector<VirtualTexture> m_Textures;
        std::vector<uint32_t> m_FreeIds;
        std::map<uint32_t, uint32_t> m_BySlot;   // tail slot -> texture id
        std::map<uint32_t, uint32_t> m_ByOffset; // page table offset -> texture id (the feedback's tiles)
        std::vector<Pool> m_Pools;
        std::optional<OffsetAllocator::Allocator> m_PageTableAllocator;
        // Page table ranges of unregistered textures, free once frames are done with them: (allocation, streamer frame).
        std::vector<std::pair<OffsetAllocator::Allocation, uint64_t>> m_ReleasingRanges;
        std::vector<TileRef> m_Candidates; // missing tiles the newest feedback asked for (and their missing ancestors)
        std::vector<TileRef> m_InFlight;   // tiles Reading or Uploading
        std::vector<OrphanRead> m_Orphans;
        // Per pool, this update's candidates waiting for a slot and its eviction candidates (gathered once needed).
        std::vector<uint32_t> m_Waiting;
        std::vector<std::vector<Victim>> m_Victims;
        std::vector<size_t> m_VictimCursor;
        std::vector<bool> m_VictimsGathered;
        uint64_t m_Frame = 0;          // the streamer's clock (Update calls)
        uint64_t m_FeedbackFrame = 0;  // Update call of the newest feedback
        uint32_t m_PoolTilesPerSide = DefaultPoolTilesPerSide;
        bool m_Enabled = true;
        Stats m_Stats;
    };
}
