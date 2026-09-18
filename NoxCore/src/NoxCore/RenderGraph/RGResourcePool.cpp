#include "RGResourcePool.h"

#include <algorithm>
#include <cstring>

#include "NRI/DescriptorHeap.h"
#include "NRI/Device.h"

namespace Nox
{
    RGResourcePool::~RGResourcePool()
    {
        ReleaseAll();
    }

    void RGResourcePool::Initialize(NRI::Device& device, NRI::DescriptorHeap& resourceHeap)
    {
        m_Device = &device;
        m_ResourceHeap = &resourceHeap;
    }

    void RGResourcePool::BeginFrame(uint64_t frameNumber)
    {
        m_Frame = frameNumber;

        auto released = std::remove_if(m_PendingReleases.begin(), m_PendingReleases.end(), [this](PendingRelease& release)
        {
            if (release.ReleaseFrame > m_Frame)
                return false;
            DestroyTexture(release.TextureResource);
            DestroyBuffer(release.BufferResource);
            return true;
        });
        m_PendingReleases.erase(released, m_PendingReleases.end());
    }

    void RGResourcePool::EndFrame()
    {
        const auto unused = [this](uint64_t lastUsedFrame) { return lastUsedFrame + UnusedKeepFrames < m_Frame; };

        for (size_t i = 0; i < m_Textures.size();)
        {
            if (unused(m_Textures[i]->LastUsedFrame))
            {
                ScheduleRelease(std::move(m_Textures[i]));
                m_Textures[i] = std::move(m_Textures.back());
                m_Textures.pop_back();
                continue;
            }
            ++i;
        }

        for (size_t i = 0; i < m_Buffers.size();)
        {
            if (unused(m_Buffers[i]->LastUsedFrame))
            {
                ScheduleRelease(std::move(m_Buffers[i]));
                m_Buffers[i] = std::move(m_Buffers.back());
                m_Buffers.pop_back();
                continue;
            }
            ++i;
        }

        for (auto it = m_TextureHistories.begin(); it != m_TextureHistories.end();)
        {
            if (!unused(it->second.LastUsedFrame))
            {
                ++it;
                continue;
            }
            for (std::unique_ptr<Texture>& texture : it->second.Textures)
                ScheduleRelease(std::move(texture));
            it = m_TextureHistories.erase(it);
        }

        for (auto it = m_BufferHistories.begin(); it != m_BufferHistories.end();)
        {
            if (!unused(it->second.LastUsedFrame))
            {
                ++it;
                continue;
            }
            for (std::unique_ptr<Buffer>& buffer : it->second.Buffers)
                ScheduleRelease(std::move(buffer));
            it = m_BufferHistories.erase(it);
        }
    }

    RGResourcePool::Texture& RGResourcePool::AcquireTransientTexture(const RGTextureKey& key, uint32_t firstPass, uint32_t lastPass)
    {
        // First fit in pool order: with an unchanged frame structure every transient gets the same entry as last frame.
        for (std::unique_ptr<Texture>& texture : m_Textures)
        {
            if (texture->Key != key)
                continue;
            if (texture->BusyFrame == m_Frame && texture->BusyUntilPass >= firstPass)
                continue;

            texture->BusyFrame = m_Frame;
            texture->BusyUntilPass = lastPass;
            texture->LastUsedFrame = m_Frame;
            return *texture;
        }

        Texture& texture = *m_Textures.emplace_back(CreateTexture(key));
        texture.BusyFrame = m_Frame;
        texture.BusyUntilPass = lastPass;
        texture.LastUsedFrame = m_Frame;
        return texture;
    }

    RGResourcePool::Buffer& RGResourcePool::AcquireTransientBuffer(const RGBufferKey& key, uint32_t firstPass, uint32_t lastPass)
    {
        for (std::unique_ptr<Buffer>& buffer : m_Buffers)
        {
            if (buffer->Key != key)
                continue;
            if (buffer->BusyFrame == m_Frame && buffer->BusyUntilPass >= firstPass)
                continue;

            buffer->BusyFrame = m_Frame;
            buffer->BusyUntilPass = lastPass;
            buffer->LastUsedFrame = m_Frame;
            return *buffer;
        }

        Buffer& buffer = *m_Buffers.emplace_back(CreateBuffer(key));
        buffer.BusyFrame = m_Frame;
        buffer.BusyUntilPass = lastPass;
        buffer.LastUsedFrame = m_Frame;
        return buffer;
    }

    RGResourcePool::TextureHistory& RGResourcePool::AcquireTextureHistory(const std::string& name, const RGTextureKey& key, uint32_t count, bool& outCreated)
    {
        TextureHistory& history = m_TextureHistories[name];
        outCreated = history.Key != key || history.Textures.size() != count;
        if (outCreated)
        {
            for (std::unique_ptr<Texture>& texture : history.Textures)
                ScheduleRelease(std::move(texture));
            history.Textures.clear();

            history.Key = key;
            for (uint32_t i = 0; i < count; ++i)
                history.Textures.push_back(CreateTexture(key));
        }
        history.LastUsedFrame = m_Frame;
        return history;
    }

    RGResourcePool::BufferHistory& RGResourcePool::AcquireBufferHistory(const std::string& name, const RGBufferKey& key, uint32_t count, bool& outCreated)
    {
        BufferHistory& history = m_BufferHistories[name];
        outCreated = history.Key != key || history.Buffers.size() != count;
        if (outCreated)
        {
            for (std::unique_ptr<Buffer>& buffer : history.Buffers)
                ScheduleRelease(std::move(buffer));
            history.Buffers.clear();

            history.Key = key;
            for (uint32_t i = 0; i < count; ++i)
                history.Buffers.push_back(CreateBuffer(key));
        }
        history.LastUsedFrame = m_Frame;
        return history;
    }

    void RGResourcePool::ReleaseAll()
    {
        for (PendingRelease& release : m_PendingReleases)
        {
            DestroyTexture(release.TextureResource);
            DestroyBuffer(release.BufferResource);
        }
        m_PendingReleases.clear();

        for (std::unique_ptr<Texture>& texture : m_Textures)
            DestroyTexture(texture);
        m_Textures.clear();

        for (std::unique_ptr<Buffer>& buffer : m_Buffers)
            DestroyBuffer(buffer);
        m_Buffers.clear();

        for (auto& [name, history] : m_TextureHistories)
        {
            for (std::unique_ptr<Texture>& texture : history.Textures)
                DestroyTexture(texture);
        }
        m_TextureHistories.clear();

        for (auto& [name, history] : m_BufferHistories)
        {
            for (std::unique_ptr<Buffer>& buffer : history.Buffers)
                DestroyBuffer(buffer);
        }
        m_BufferHistories.clear();
    }

    std::unique_ptr<RGResourcePool::Texture> RGResourcePool::CreateTexture(const RGTextureKey& key)
    {
        auto texture = std::make_unique<Texture>();
        texture->Key = key;

        NRI::TextureDesc desc{};
        desc.width = key.Width;
        desc.height = key.Height;
        desc.mipLevels = key.MipLevels;
        desc.sampleCount = 1;
        desc.usage = key.Usage;
        desc.format = key.Format;
        desc.directFormat = UINT32_MAX;
        texture->Handle = m_Device->createTexture(desc);

        if (key.Usage == NRI::TextureUsage::Storage)
        {
            // Storage usage includes sampling: a sampled slot for reads plus a storage slot per mip for writes.
            m_ResourceHeap->registerTexture(*texture->Handle, NRI::TextureUsage::ShaderResource);
            for (uint32_t mip = 0; mip < key.MipLevels; ++mip)
                texture->StorageSlots.push_back(m_ResourceHeap->registerStorageTextureMip(*texture->Handle, mip));
        }
        else
        {
            m_ResourceHeap->registerTexture(*texture->Handle);
        }

        m_TextureMemory += EstimateTextureBytes(key);
        return texture;
    }

    std::unique_ptr<RGResourcePool::Buffer> RGResourcePool::CreateBuffer(const RGBufferKey& key)
    {
        auto buffer = std::make_unique<Buffer>();
        buffer->Key = key;
        buffer->Handle = m_Device->createBuffer(NRI::BufferDesc{ .size = key.Size, .usage = key.Usage });

        // History consumers (reservoirs) expect zeroed contents on (re)creation.
        if (key.Usage == NRI::BufferUsage::Storage)
        {
            void* mapped = buffer->Handle->map(0, key.Size);
            std::memset(mapped, 0, key.Size);
            buffer->Handle->unmap();
        }

        m_BufferMemory += key.Size;
        return buffer;
    }

    void RGResourcePool::DestroyTexture(std::unique_ptr<Texture>& texture)
    {
        if (!texture)
            return;

        // The sampled slot is released by the texture itself; the per-mip storage slots are ours.
        for (uint32_t slot : texture->StorageSlots)
            m_ResourceHeap->unregisterTexture(slot);

        m_TextureMemory -= EstimateTextureBytes(texture->Key);
        texture.reset();
    }

    void RGResourcePool::DestroyBuffer(std::unique_ptr<Buffer>& buffer)
    {
        if (!buffer)
            return;

        m_BufferMemory -= buffer->Key.Size;
        buffer.reset();
    }

    void RGResourcePool::ScheduleRelease(std::unique_ptr<Texture> texture)
    {
        if (texture)
            m_PendingReleases.push_back({ m_Frame + ReleaseDelayFrames, std::move(texture), nullptr });
    }

    void RGResourcePool::ScheduleRelease(std::unique_ptr<Buffer> buffer)
    {
        if (buffer)
            m_PendingReleases.push_back({ m_Frame + ReleaseDelayFrames, nullptr, std::move(buffer) });
    }

    uint64_t RGResourcePool::EstimateTextureBytes(const RGTextureKey& key)
    {
        return NRI::estimateTextureBytes(NRI::TextureDesc{
            .width = key.Width,
            .height = key.Height,
            .mipLevels = key.MipLevels,
            .usage = key.Usage,
            .format = key.Format
        });
    }
}
