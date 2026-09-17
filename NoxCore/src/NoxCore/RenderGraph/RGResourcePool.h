#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "NRI/Buffer.h"
#include "NRI/Texture.h"
#include "NoxCore/Core/Ref.h"

namespace NRI
{
    class Device;
    class DescriptorHeap;
}

namespace Nox
{
    // Physical description of a pooled texture (sizes already resolved).
    struct RGTextureKey
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        NRI::ImageFormat Format = NRI::ImageFormat::None;
        NRI::TextureUsage Usage = NRI::TextureUsage::ColorAttachment;
        uint32_t MipLevels = 1;

        bool operator==(const RGTextureKey&) const = default;
    };

    struct RGBufferKey
    {
        uint64_t Size = 0;
        NRI::BufferUsage Usage = NRI::BufferUsage::Storage;

        bool operator==(const RGBufferKey&) const = default;
    };

    // GPU memory behind the render graph's transient and history resources (§5.4.3). Transients are reused across frames
    // and aliased within a frame (same key, non-overlapping pass ranges); every entry keeps its bindless slots for its
    // whole life, so descriptors are not rewritten per frame. Entries unused for a while and history resources whose
    // description changed are released after a delay, once no in-flight frame can reference them.
    class RGResourcePool
    {
    public:
        struct Texture
        {
            RGTextureKey Key;
            Ref<NRI::Texture2D> Handle;
            std::vector<uint32_t> StorageSlots; // one storage (UAV) slot per mip for Storage usage
            uint64_t LastUsedFrame = 0;
            uint64_t BusyFrame = ~0ull;
            uint32_t BusyUntilPass = 0;
        };

        struct Buffer
        {
            RGBufferKey Key;
            std::unique_ptr<NRI::Buffer> Handle;
            uint64_t LastUsedFrame = 0;
            uint64_t BusyFrame = ~0ull;
            uint32_t BusyUntilPass = 0;
        };

        struct TextureHistory
        {
            RGTextureKey Key;
            std::vector<std::unique_ptr<Texture>> Textures;
            uint64_t LastUsedFrame = 0;
        };

        struct BufferHistory
        {
            RGBufferKey Key;
            std::vector<std::unique_ptr<Buffer>> Buffers;
            uint64_t LastUsedFrame = 0;
        };

        RGResourcePool() = default;
        ~RGResourcePool();

        RGResourcePool(const RGResourcePool&) = delete;
        RGResourcePool& operator=(const RGResourcePool&) = delete;

        void Initialize(NRI::Device& device, NRI::DescriptorHeap& resourceHeap);

        // Frame start: destroys resources whose release delay has passed.
        void BeginFrame(uint64_t frameNumber);
        // Frame end: schedules resources nobody used for a while for release.
        void EndFrame();

        // A pooled texture/buffer free for the pass range [firstPass, lastPass] of this frame (created when none is).
        Texture& AcquireTransientTexture(const RGTextureKey& key, uint32_t firstPass, uint32_t lastPass);
        Buffer& AcquireTransientBuffer(const RGBufferKey& key, uint32_t firstPass, uint32_t lastPass);

        // Persistent resources under a name; recreated (outCreated = true) when the key or count changed.
        TextureHistory& AcquireTextureHistory(const std::string& name, const RGTextureKey& key, uint32_t count, bool& outCreated);
        BufferHistory& AcquireBufferHistory(const std::string& name, const RGBufferKey& key, uint32_t count, bool& outCreated);

        // Destroys everything immediately. The GPU must be idle.
        void ReleaseAll();

        uint64_t GetTextureMemory() const { return m_TextureMemory; }
        uint64_t GetBufferMemory() const { return m_BufferMemory; }

        // Estimated size (all mips) of a pooled texture.
        static uint64_t EstimateTextureBytes(const RGTextureKey& key);

    private:
        std::unique_ptr<Texture> CreateTexture(const RGTextureKey& key);
        std::unique_ptr<Buffer> CreateBuffer(const RGBufferKey& key);
        void DestroyTexture(std::unique_ptr<Texture>& texture);
        void DestroyBuffer(std::unique_ptr<Buffer>& buffer);
        void ScheduleRelease(std::unique_ptr<Texture> texture);
        void ScheduleRelease(std::unique_ptr<Buffer> buffer);

    private:
        // Frames a released resource waits so no in-flight frame still references it.
        static constexpr uint64_t ReleaseDelayFrames = 4;
        // Frames an unused transient or history stays around (feature toggles don't churn allocations).
        static constexpr uint64_t UnusedKeepFrames = 120;

        struct PendingRelease
        {
            uint64_t ReleaseFrame = 0;
            std::unique_ptr<Texture> TextureResource;
            std::unique_ptr<Buffer> BufferResource;
        };

    private:
        NRI::Device* m_Device = nullptr;
        NRI::DescriptorHeap* m_ResourceHeap = nullptr;
        uint64_t m_Frame = 0;

        std::vector<std::unique_ptr<Texture>> m_Textures;
        std::vector<std::unique_ptr<Buffer>> m_Buffers;
        std::unordered_map<std::string, TextureHistory> m_TextureHistories;
        std::unordered_map<std::string, BufferHistory> m_BufferHistories;
        std::vector<PendingRelease> m_PendingReleases;

        uint64_t m_TextureMemory = 0; // estimated, for the render graph panel
        uint64_t m_BufferMemory = 0;
    };
}
