#pragma once
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Asset.h"
#include "TextureImporter.h"

namespace Nox
{
    class Renderer;

    // Texture streaming (§5.12): a streamed texture's image holds only its resident mips [FirstResident, last]. The
    // mip tail (mips no larger than TailSize) arrives with the load and always stays; the streamer moves each texture
    // to the residency it is wanted at by giving it a new image -- the kept mips copied over from the old one, new
    // ones read from the cooked file straight into staging -- which the asset manager swaps in once the copies have
    // completed (UE's texture reallocation: every shader, raster and ray traced, only ever sees resident mips).
    // Owned by the asset manager, main thread.
    class TextureStreamer
    {
    public:
        // Mips whose larger side is at most this many texels are always resident.
        static constexpr uint32_t TailSize = 64;
        // Reallocations started per frame (each creates an image; the upload budget bounds their bytes).
        static constexpr uint32_t StartsPerFrame = 32;
        // Readbacks a texture keeps its wanted mip after the pixels stopped asking for it (then it relaxes one mip):
        // looking away briefly does not drop what comes back into view.
        static constexpr uint32_t HoldReadbacks = 120;

        // What the streamed textures hold and want against the pool, for the stats and the editor.
        struct Stats
        {
            size_t Textures = 0;
            uint64_t ResidentBytes = 0;
            uint64_t WantedBytes = 0;
            uint64_t PoolBytes = 0;
        };

        TextureStreamer();
        ~TextureStreamer();

        TextureStreamer(const TextureStreamer&) = delete;
        TextureStreamer& operator=(const TextureStreamer&) = delete;

        // The texture's first tail mip; 0 for a texture that is all tail (not streamed).
        static uint32_t TailFirstMip(const TextureData& texture);

        // A loaded texture that streams: its cooked header (every mip), the mip its image starts at, and the image's slot.
        void Register(AssetHandle handle, const CookedTextureHeader& header, uint32_t firstResident, uint32_t slot);
        // The texture was unloaded or replaced: a reallocation in flight finishes and its image is released.
        void Unregister(AssetHandle handle);

        // Once per frame: takes the newest GPU mip feedback, plans every texture's residency within the pool, advances
        // reallocations and starts new ones within the upload budget (shared with the loads). outPublished: textures whose
        // new image is ready, for the asset manager to swap in.
        void Update(Renderer& renderer, uint64_t& uploadBudget, std::vector<std::pair<AssetHandle, Ref<Texture2D>>>& outPublished);
        // Pool size in bytes instead of the texture budget's share (0: the budget), to force pressure while testing.
        void SetPoolOverride(uint64_t bytes) { m_PoolOverride = bytes; }
        uint64_t GetPoolOverride() const { return m_PoolOverride; }
        const Stats& GetStats() const { return m_Stats; }
        // While the renderer still exists: waits for work in flight and drops it.
        void Shutdown();

        bool IsIdle() const { return m_InFlight == 0; }
        size_t GetStreamingCount() const { return m_InFlight; }
        // Bytes still to be uploaded by the reallocations in flight.
        uint64_t GetPendingUploadBytes() const;

        struct Reallocation;

    private:
        struct StreamedTexture
        {
            CookedTextureHeader Header;
            uint32_t FirstResident = 0;
            uint32_t TailFirst = 0;
            uint32_t WantedFirst = 0;
            uint32_t WantedAge = 0;          // readbacks since the pixels last asked for WantedFirst or finer
            uint32_t Requested = 0;          // the finest mip asked in the readback being applied
            uint64_t LastSeen = 0;           // feedback serial of the last readback that saw it
            uint32_t Target = 0;             // this frame's plan
            bool Failed = false; // its file could not be read: it keeps what it has
            std::unique_ptr<Reallocation> Pending;
        };

        // What the pixels asked of each streamed texture's image, as the finest mip each texture is wanted at.
        void applyFeedback(const std::vector<uint32_t>& feedback);
        // Every texture's residency target within the pool.
        void planTargets(uint64_t pool);
        static uint64_t residentBytes(const StreamedTexture& texture, uint32_t firstMip);
        // True once the reallocation is done (published or dropped).
        bool advance(Renderer& renderer, AssetHandle handle, Reallocation& reallocation, bool orphaned,
                     std::vector<std::pair<AssetHandle, Ref<Texture2D>>>& outPublished);
        std::unique_ptr<Reallocation> start(Renderer& renderer, AssetHandle handle, StreamedTexture& texture, uint64_t& uploadBudget);

        // An image slot of a streamed texture and the mip that image starts at. Feedback is a couple of frames old when
        // it arrives, so a replaced image keeps mapping back to its texture for a few more readbacks.
        struct SlotOwner
        {
            AssetHandle Handle;
            uint32_t FirstResident = 0;
            uint32_t ExpiresIn = 0; // readbacks left for a replaced image; 0 for the current one
        };
        static constexpr uint32_t ReplacedSlotReadbacks = 8;

        std::unordered_map<AssetHandle, StreamedTexture> m_Textures;
        std::unordered_map<uint32_t, SlotOwner> m_SlotOwners;
        uint64_t m_FeedbackSerial = 0;
        uint64_t m_PoolOverride = 0;
        Stats m_Stats;
        // Reallocations of textures unregistered meanwhile: they finish, then their image is released.
        std::vector<std::pair<AssetHandle, std::unique_ptr<Reallocation>>> m_Orphans;
        size_t m_InFlight = 0;
        // Images the streamer replaced that the deferred release still holds: their bytes and the frames until freed.
        struct ReleasingImage
        {
            uint64_t Bytes = 0;
            uint32_t FramesLeft = 0;
        };
        std::vector<ReleasingImage> m_Releasing;
    };
}
