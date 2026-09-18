#include "TextureStreamer.h"

#include <algorithm>
#include <exception>

#include "AssetLoader.h"
#include "AssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Tasks/JobSystem.h"

namespace Nox
{
    // One texture moving to a new image: kept mips copied from the old image, new ones read from the file into staging.
    struct TextureStreamer::Reallocation
    {
        uint32_t NewFirst = 0;
        uint32_t SourceFirst = 0;  // the mip the old image starts at
        uint32_t TotalMips = 0;    // of the whole texture
        uint64_t Bytes = 0;        // read from the file
        uint64_t ImageBytes = 0;   // the new image's mips
        uint64_t SourceBytes = 0;  // the old image's mips
        TextureUpload Upload;
        Ref<Texture2D> Source;     // read by the copy until it completes
        TaskFuture<bool> Read;
        uint64_t CompleteValue = 0;
        bool Recorded = false;
        bool Failed = false;
    };

    // A replaced image counts until the deferred release (MAX_FRAMES_IN_FLIGHT frames) freed it and the next memory
    // sample saw it go.
    static constexpr uint32_t ReleaseFrames = MAX_FRAMES_IN_FLIGHT + 2;

    TextureStreamer::TextureStreamer() = default;

    TextureStreamer::~TextureStreamer()
    {
        // The reads write staging the reallocations own.
        for (auto& [handle, texture] : m_Textures)
        {
            if (texture.Pending)
                texture.Pending->Read.Wait();
        }
        for (auto& [handle, reallocation] : m_Orphans)
            reallocation->Read.Wait();
    }

    uint32_t TextureStreamer::TailFirstMip(const TextureData& texture)
    {
        const uint32_t mipLevels = std::max(texture.MipLevels, 1u);
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            if (std::max(texture.Width >> mip, texture.Height >> mip) <= TailSize)
                return mip;
        }
        return mipLevels - 1;
    }

    void TextureStreamer::Register(AssetHandle handle, const CookedTextureHeader& header, uint32_t firstResident, uint32_t slot)
    {
        Unregister(handle);
        StreamedTexture& texture = m_Textures[handle];
        texture.Header = header;
        texture.FirstResident = firstResident;
        texture.TailFirst = TailFirstMip(header.Texture);
        texture.WantedFirst = firstResident; // the tail, until a pixel asks for more
        m_SlotOwners[slot] = { handle, firstResident, 0 };
    }

    void TextureStreamer::Unregister(AssetHandle handle)
    {
        auto found = m_Textures.find(handle);
        if (found == m_Textures.end())
            return;
        if (found->second.Pending)
            m_Orphans.emplace_back(handle, std::move(found->second.Pending));
        m_Textures.erase(found);
        std::erase_if(m_SlotOwners, [&](const auto& owner) { return owner.second.Handle == handle; });
    }

    void TextureStreamer::Update(Renderer& renderer, uint64_t& uploadBudget, std::vector<std::pair<AssetHandle, Ref<Texture2D>>>& outPublished)
    {
        std::erase_if(m_Releasing, [](ReleasingImage& image) { return image.FramesLeft-- == 0; });
        if (m_Textures.empty() && m_Orphans.empty())
            return;
        NOX_PROFILE_SCOPE("Texture Streaming");

        uint64_t feedbackSerial = 0;
        const std::vector<uint32_t>& feedback = renderer.GetMipFeedback(feedbackSerial);
        if (feedbackSerial != m_FeedbackSerial)
        {
            m_FeedbackSerial = feedbackSerial;
            applyFeedback(feedback);
        }

        // The pool: the Textures budget minus what other images (the renderer's own, unstreamed textures) hold of it.
        // The streamer's own images in transit -- new images of reallocations in flight, replaced ones not released
        // yet -- are not others: counted, every reallocation would shrink the pool, and the shrinks that forces would
        // shrink it further.
        uint64_t streamedResident = 0;
        uint64_t streamedWanted = 0;
        uint64_t inTransit = 0;
        for (const auto& [handle, texture] : m_Textures)
        {
            streamedResident += residentBytes(texture, texture.FirstResident);
            streamedWanted += residentBytes(texture, texture.WantedFirst);
            if (texture.Pending)
                inTransit += texture.Pending->ImageBytes;
        }
        for (const auto& [handle, reallocation] : m_Orphans)
            inTransit += reallocation->ImageBytes;
        for (const ReleasingImage& image : m_Releasing)
            inTransit += image.Bytes;
        uint64_t pool = m_PoolOverride;
        if (pool == 0)
        {
            const MemoryCategoryStats& textures = renderer.GetMemoryCategory(MemoryCategory::Textures);
            const uint64_t streamer = streamedResident + inTransit;
            const uint64_t others = textures.Committed > streamer ? textures.Committed - streamer : 0;
            pool = textures.Budget > others ? textures.Budget - others : 0;
            if (textures.Budget == 0)
                pool = UINT64_MAX; // no budget sampled yet
        }
        planTargets(pool);

        m_Stats = { m_Textures.size(), streamedResident, streamedWanted, pool };
        NOX_PROFILE_COUNTER("Streamed Textures", m_Stats.Textures);
        NOX_PROFILE_COUNTER("Streamed Resident MB", streamedResident / (1024 * 1024));
        NOX_PROFILE_COUNTER("Streamed Wanted MB", streamedWanted / (1024 * 1024));
        NOX_PROFILE_COUNTER("Texture Pool MB", pool == UINT64_MAX ? 0 : pool / (1024 * 1024));
        NOX_PROFILE_COUNTER("Texture Reallocations", m_InFlight);

        // Shrinks first (they free memory and upload nothing), then the largest shortfalls.
        std::vector<std::pair<AssetHandle, StreamedTexture*>> changes;
        for (auto& [handle, texture] : m_Textures)
        {
            if (!texture.Pending && !texture.Failed && texture.Target != texture.FirstResident)
                changes.emplace_back(handle, &texture);
        }
        std::sort(changes.begin(), changes.end(), [](const auto& a, const auto& b)
        {
            const bool aShrinks = a.second->Target > a.second->FirstResident;
            const bool bShrinks = b.second->Target > b.second->FirstResident;
            if (aShrinks != bShrinks)
                return aShrinks;
            return static_cast<int>(a.second->FirstResident) - static_cast<int>(a.second->Target) >
                   static_cast<int>(b.second->FirstResident) - static_cast<int>(b.second->Target);
        });

        uint32_t starts = StartsPerFrame;
        for (auto& [handle, texture] : changes)
        {
            if (starts == 0)
                break;
            texture->Pending = start(renderer, handle, *texture, uploadBudget);
            if (texture->Pending)
            {
                ++m_InFlight;
                --starts;
            }
        }

        // Then the reallocations in flight, after the starts: the asset manager swaps a completed one's image in only
        // after this returns, so a texture must not start again in the frame its reallocation completed (it would copy
        // from the old image, taking it for one that starts at the new first mip).
        std::erase_if(m_Orphans, [&](auto& orphan)
        {
            const bool done = advance(renderer, orphan.first, *orphan.second, true, outPublished);
            if (done)
                --m_InFlight;
            return done;
        });

        for (auto& [handle, texture] : m_Textures)
        {
            if (texture.Pending && advance(renderer, handle, *texture.Pending, false, outPublished))
            {
                if (texture.Pending->Failed)
                    texture.Failed = true;
                else
                    texture.FirstResident = texture.Pending->NewFirst;
                texture.Pending.reset();
                --m_InFlight;
            }
        }
    }

    uint64_t TextureStreamer::residentBytes(const StreamedTexture& texture, uint32_t firstMip)
    {
        const std::vector<size_t>& offsets = texture.Header.Texture.MipOffsets;
        return texture.Header.DataSize - (firstMip < offsets.size() ? offsets[firstMip] : 0);
    }

    void TextureStreamer::planTargets(uint64_t pool)
    {
        // 1. The pool is a cache: what is resident stays, what is wanted is added.
        uint64_t total = 0;
        for (auto& [handle, texture] : m_Textures)
        {
            texture.Target = std::min(texture.FirstResident, texture.WantedFirst);
            total += residentBytes(texture, texture.Target);
        }
        if (total <= pool)
            return;

        // Over the pool: textures out of view give first (the longest unseen first), then the ones in view, larger ones
        // first. The order must hold still while the view does: feedback samples one pixel per tile per frame, so the
        // exact readback a texture was last seen in changes from frame to frame, and cutting by it would pick other
        // textures every frame (shrinking some while growing others back, without end).
        std::vector<std::pair<AssetHandle, StreamedTexture*>> order;
        order.reserve(m_Textures.size());
        for (auto& [handle, texture] : m_Textures)
            order.emplace_back(handle, &texture);
        const auto inView = [this](const StreamedTexture& texture)
        {
            return texture.LastSeen != 0 && m_FeedbackSerial - texture.LastSeen < HoldReadbacks;
        };
        std::sort(order.begin(), order.end(), [&](const auto& a, const auto& b)
        {
            const bool aInView = inView(*a.second);
            const bool bInView = inView(*b.second);
            if (aInView != bInView)
                return !aInView;
            if (!aInView && a.second->LastSeen != b.second->LastSeen)
                return a.second->LastSeen < b.second->LastSeen;
            if (a.second->Header.DataSize != b.second->Header.DataSize)
                return a.second->Header.DataSize > b.second->Header.DataSize;
            return a.first < b.first;
        });

        // 2. Mips nothing wants any more.
        for (auto& [handle, texture] : order)
        {
            if (texture->Target < texture->WantedFirst)
            {
                total -= residentBytes(*texture, texture->Target) - residentBytes(*texture, texture->WantedFirst);
                texture->Target = texture->WantedFirst;
                if (total <= pool)
                    return;
            }
        }

        // 3. Still over: one mip at a time off every texture, round after round, never below the tail.
        bool reduced = true;
        while (total > pool && reduced)
        {
            reduced = false;
            for (auto& [handle, texture] : order)
            {
                if (texture->Target >= texture->TailFirst)
                    continue;
                total -= residentBytes(*texture, texture->Target) - residentBytes(*texture, texture->Target + 1);
                ++texture->Target;
                reduced = true;
                if (total <= pool)
                    return;
            }
        }
    }

    void TextureStreamer::Shutdown()
    {
        auto drop = [](Reallocation& reallocation)
        {
            reallocation.Read.Wait();
            // Copies recorded may still run on the transfer queue: both images leave after frames in flight.
            if (reallocation.Recorded)
            {
                Renderer::DeferAssetRelease(Ref<Asset>(reallocation.Upload.Texture));
                Renderer::DeferAssetRelease(Ref<Asset>(reallocation.Source));
            }
        };
        for (auto& [handle, texture] : m_Textures)
        {
            if (texture.Pending)
                drop(*texture.Pending);
        }
        for (auto& [handle, reallocation] : m_Orphans)
            drop(*reallocation);
        m_Textures.clear();
        m_Orphans.clear();
        m_Releasing.clear();
        m_InFlight = 0;
    }

    uint64_t TextureStreamer::GetPendingUploadBytes() const
    {
        uint64_t bytes = 0;
        for (const auto& [handle, texture] : m_Textures)
        {
            if (texture.Pending)
                bytes += texture.Pending->Bytes;
        }
        return bytes;
    }

    void TextureStreamer::applyFeedback(const std::vector<uint32_t>& feedback)
    {
        for (auto& [handle, texture] : m_Textures)
            texture.Requested = UINT32_MAX;

        for (auto owner = m_SlotOwners.begin(); owner != m_SlotOwners.end();)
        {
            const uint32_t value = owner->first < feedback.size() ? feedback[owner->first] : shaderio::MipFeedbackNone;
            if (value != shaderio::MipFeedbackNone)
            {
                auto texture = m_Textures.find(owner->second.Handle);
                if (texture != m_Textures.end())
                {
                    // The value is relative to the image the pixel sampled: its first mip turns it into the texture's.
                    const int wanted = static_cast<int>(value) - static_cast<int>(shaderio::MipFeedbackBias) + static_cast<int>(owner->second.FirstResident);
                    const uint32_t wantedFirst = static_cast<uint32_t>(std::clamp(wanted, 0, static_cast<int>(texture->second.TailFirst)));
                    texture->second.Requested = std::min(texture->second.Requested, wantedFirst);
                }
            }

            if (owner->second.ExpiresIn > 0 && --owner->second.ExpiresIn == 0)
                owner = m_SlotOwners.erase(owner);
            else
                ++owner;
        }

        // Finer requests take effect at once; a texture asked for less (or not seen) keeps its wanted mip for
        // HoldReadbacks, then relaxes one mip at a time towards what it is asked for (or the tail).
        for (auto& [handle, texture] : m_Textures)
        {
            if (texture.Requested != UINT32_MAX)
                texture.LastSeen = m_FeedbackSerial;
            if (texture.Requested <= texture.WantedFirst)
            {
                texture.WantedFirst = texture.Requested;
                texture.WantedAge = 0;
            }
            else if (++texture.WantedAge >= HoldReadbacks)
            {
                texture.WantedFirst = std::min(texture.WantedFirst + 1, std::min(texture.Requested, texture.TailFirst));
                texture.WantedAge = 0;
            }
        }
    }

    std::unique_ptr<TextureStreamer::Reallocation> TextureStreamer::start(Renderer& renderer, AssetHandle handle, StreamedTexture& texture, uint64_t& uploadBudget)
    {
        Texture2D* current = AssetManager::FindLoadedAsset<Texture2D>(handle);
        if (!current)
            return nullptr;

        const TextureData& header = texture.Header.Texture;
        const uint32_t newFirst = std::min(texture.Target, texture.TailFirst);
        const bool grow = newFirst < texture.FirstResident;
        const uint32_t uploadMipCount = grow ? texture.FirstResident - newFirst : 0;
        const uint64_t bytes = grow ? header.MipOffsets[texture.FirstResident] - header.MipOffsets[newFirst] : 0;

        // Same rule as the loads: a frame with its budget untouched takes one larger upload.
        if (bytes > uploadBudget && uploadBudget != AssetLoader::UploadBytesPerFrame)
            return nullptr;
        std::optional<TextureUpload> upload = renderer.BeginTextureUpload(header, newFirst, uploadMipCount, texture.Header.DataSize, false);
        if (!upload)
            return nullptr;
        uploadBudget -= std::min(uploadBudget, bytes);

        auto reallocation = std::make_unique<Reallocation>();
        reallocation->NewFirst = newFirst;
        reallocation->SourceFirst = texture.FirstResident;
        reallocation->TotalMips = std::max(header.MipLevels, 1u);
        reallocation->Bytes = bytes;
        reallocation->ImageBytes = residentBytes(texture, newFirst);
        reallocation->SourceBytes = residentBytes(texture, texture.FirstResident);
        reallocation->Source = Ref<Texture2D>(current);
        reallocation->Upload = std::move(*upload);
        if (uploadMipCount > 0)
        {
            // From the file straight into staging, like a load.
            reallocation->Read = JobSystem::Get().AsyncIO("Read Texture Mips",
                [header = texture.Header, newFirst, uploadMipCount, destination = reallocation->Upload.Staging.data](const CancellationToken&)
                {
                    return TextureImporter::ReadCookedTextureMips(header, newFirst, uploadMipCount, destination);
                });
        }
        return reallocation;
    }

    bool TextureStreamer::advance(Renderer& renderer, AssetHandle handle, Reallocation& reallocation, bool orphaned,
                                  std::vector<std::pair<AssetHandle, Ref<Texture2D>>>& outPublished)
    {
        if (!reallocation.Recorded)
        {
            if (reallocation.Read.IsValid() && !reallocation.Read.IsReady())
                return false;

            bool read = true;
            if (reallocation.Read.IsValid())
            {
                try
                {
                    read = reallocation.Read.Get();
                }
                catch (const std::exception& exception)
                {
                    NOX_CORE_ERROR("TextureStreamer: {}", exception.what());
                    read = false;
                }
            }
            // Nothing on the GPU reads the new image or its staging yet: both go now.
            if (!read || orphaned)
            {
                if (!read)
                    NOX_CORE_ERROR("TextureStreamer: could not read the mips of texture {}", static_cast<uint64_t>(handle));
                renderer.AbandonUpload(reallocation.Upload.Staging);
                reallocation.Failed = !read;
                return true;
            }

            // The new image: its uploaded mips first, then the kept ones copied from the old image.
            const uint32_t keptFirst = std::max(reallocation.NewFirst, reallocation.SourceFirst);
            reallocation.CompleteValue = renderer.EndTextureUpload(reallocation.Upload, false, reallocation.Source.get(),
                                                                   keptFirst - reallocation.SourceFirst, reallocation.TotalMips - keptFirst);
            reallocation.Recorded = true;
            return false;
        }

        if (renderer.GetCompletedUploadValue() < reallocation.CompleteValue)
            return false;

        if (orphaned)
        {
            // Unloaded meanwhile: never published, nothing but the finished copy used it.
            Renderer::DeferAssetRelease(Ref<Asset>(reallocation.Upload.Texture));
            m_Releasing.push_back({ reallocation.ImageBytes, ReleaseFrames });
        }
        else
        {
            renderer.PublishTexture(*reallocation.Upload.Texture);
            outPublished.emplace_back(handle, reallocation.Upload.Texture);
            // Feedback for the old image still arrives for a few readbacks.
            if (auto old = m_SlotOwners.find(reallocation.Source->GetDescriptorIndexSlot()); old != m_SlotOwners.end() && old->second.Handle == handle)
                old->second.ExpiresIn = ReplacedSlotReadbacks;
            m_SlotOwners[reallocation.Upload.Texture->GetDescriptorIndexSlot()] = { handle, reallocation.NewFirst, 0 };
            // The asset manager hands the old image to the deferred release.
            m_Releasing.push_back({ reallocation.SourceBytes, ReleaseFrames });
        }
        reallocation.Source = {};
        return true;
    }
}
