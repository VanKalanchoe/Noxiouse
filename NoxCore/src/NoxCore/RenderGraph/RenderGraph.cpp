#include "RenderGraph.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "NRI/Device.h"
#include "NoxCore/Core/Log.h"

namespace Nox
{
    // ---------------------------------------------------------------------------------------------------------------
    // RGPassContext

    RGPassContext::RGPassContext(RenderGraph& graph, NRI::CommandBuffer& cmd, NRI::Extent2D renderArea, uint32_t passIndex)
        : m_Graph(graph), m_Cmd(cmd), m_RenderArea(renderArea), m_PassIndex(passIndex)
    {
    }

    NRI::Texture2D& RGPassContext::Texture(RGTexture texture) const
    {
        if (m_Graph.m_ValidationEnabled)
        {
            m_Graph.ValidateHandle(texture.Frame, m_PassIndex != RGInvalidIndex ? m_Graph.m_Passes[m_PassIndex].Name : "Texture Inspection");
            m_Graph.ValidateContextAccess(m_PassIndex, texture.Index, true);
        }
        NRI::Texture2D* resource = m_Graph.GetTexture(texture);
        NOX_CORE_ASSERT(resource, "RGPassContext::Texture: texture is invalid or was not allocated");
        return *resource;
    }

    NRI::Buffer& RGPassContext::Buffer(RGBuffer buffer) const
    {
        if (m_Graph.m_ValidationEnabled)
        {
            m_Graph.ValidateHandle(buffer.Frame, m_PassIndex != RGInvalidIndex ? m_Graph.m_Passes[m_PassIndex].Name : "Texture Inspection");
            m_Graph.ValidateContextAccess(m_PassIndex, buffer.Index, false);
        }
        NOX_CORE_ASSERT(buffer.IsValid() && buffer.Index < m_Graph.m_Buffers.size() && m_Graph.m_Buffers[buffer.Index].Buffer,
                        "RGPassContext::Buffer: buffer is invalid or was not allocated");
        return *m_Graph.m_Buffers[buffer.Index].Buffer;
    }

    uint32_t RGPassContext::Slot(RGTexture texture) const
    {
        return Texture(texture).GetDescriptorIndexSlot();
    }

    uint32_t RGPassContext::SlotOr(RGTexture texture, uint32_t fallback) const
    {
        if (m_Graph.m_ValidationEnabled && texture.IsValid() && m_Graph.GetTexture(texture))
            m_Graph.ValidateContextAccess(m_PassIndex, texture.Index, true);
        return m_Graph.GetSlotOr(texture, fallback);
    }

    uint32_t RGPassContext::StorageSlot(RGTexture texture, uint32_t mip) const
    {
        if (m_Graph.m_ValidationEnabled)
            m_Graph.ValidateContextAccess(m_PassIndex, texture.Index, true);
        NOX_CORE_ASSERT(texture.IsValid() && texture.Index < m_Graph.m_Textures.size(), "RGPassContext::StorageSlot: invalid texture");
        const std::vector<uint32_t>* slots = m_Graph.m_Textures[texture.Index].StorageSlots;
        NOX_CORE_ASSERT(slots && mip < slots->size(), "RGPassContext::StorageSlot: texture has no storage slot for this mip");
        return (*slots)[mip];
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Access states (Precise synchronization)

    namespace
    {
        uint32_t ShaderStages(RGPassFlags flags)
        {
            // Raster passes draw with task/mesh + fragment shaders (fullscreen passes included); others dispatch compute
            // or call libraries that do.
            return HasFlag(flags, RGPassFlags::Raster) ? NRI::StageBits::Task | NRI::StageBits::Mesh | NRI::StageBits::Fragment : NRI::StageBits::Compute;
        }

        NRI::ResourceState TextureState(RGTextureAccess access, RGPassFlags flags)
        {
            switch (access)
            {
            case RGTextureAccess::Sampled:
            case RGTextureAccess::StorageRead: return { NRI::AccessBits::ShaderRead, ShaderStages(flags) };
            case RGTextureAccess::StorageWrite: return { NRI::AccessBits::ShaderWrite, ShaderStages(flags) };
            case RGTextureAccess::ColorTarget: return { NRI::AccessBits::ColorAttachmentWrite, NRI::StageBits::ColorAttachmentOutput };
            case RGTextureAccess::DepthTarget: return { NRI::AccessBits::DepthStencilWrite, NRI::StageBits::FragmentTests };
            case RGTextureAccess::CopySource: return { NRI::AccessBits::TransferRead, NRI::StageBits::Transfer };
            case RGTextureAccess::CopyDestination: return { NRI::AccessBits::TransferWrite, NRI::StageBits::Transfer };
            }
            return {};
        }

        NRI::ResourceState BufferState(RGBufferAccess access, RGPassFlags flags)
        {
            switch (access)
            {
            case RGBufferAccess::Read: return { NRI::AccessBits::ShaderRead, ShaderStages(flags) };
            case RGBufferAccess::IndirectRead: return { NRI::AccessBits::IndirectRead, NRI::StageBits::Indirect };
            case RGBufferAccess::Write: return { NRI::AccessBits::ShaderWrite, ShaderStages(flags) };
            case RGBufferAccess::AccelerationStructureBuild: return { NRI::AccessBits::AccelerationStructureWrite, NRI::StageBits::AccelerationStructureBuild };
            case RGBufferAccess::AccelerationStructureRead: return { NRI::AccessBits::AccelerationStructureRead | NRI::AccessBits::ShaderRead, ShaderStages(flags) };
            case RGBufferAccess::CopySource: return { NRI::AccessBits::TransferRead, NRI::StageBits::Transfer };
            case RGBufferAccess::CopyDestination: return { NRI::AccessBits::TransferWrite, NRI::StageBits::Transfer };
            }
            return {};
        }

        bool HasWriteAccess(const NRI::ResourceState& state)
        {
            constexpr uint32_t writeBits = NRI::AccessBits::ShaderWrite | NRI::AccessBits::ColorAttachmentWrite | NRI::AccessBits::DepthStencilWrite |
                                           NRI::AccessBits::TransferWrite | NRI::AccessBits::AccelerationStructureWrite;
            return (state.access & writeBits) != 0;
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // RGBuilder

    RGTexture RGBuilder::Read(RGTexture texture, RGTextureAccess access)
    {
        if (m_Graph.m_ValidationEnabled)
            m_Graph.ValidateHandle(texture.Frame, m_Graph.m_Passes[m_PassIndex].Name);
        m_Graph.AddAccess(m_PassIndex, texture.Index, true, IsWrite(access), TextureState(access, m_Graph.m_Passes[m_PassIndex].Flags));
        return texture;
    }

    RGTexture RGBuilder::Write(RGTexture texture, RGTextureAccess access)
    {
        if (m_Graph.m_ValidationEnabled)
            m_Graph.ValidateHandle(texture.Frame, m_Graph.m_Passes[m_PassIndex].Name);
        m_Graph.AddAccess(m_PassIndex, texture.Index, true, true, TextureState(access, m_Graph.m_Passes[m_PassIndex].Flags));
        return texture;
    }

    RGBuffer RGBuilder::Read(RGBuffer buffer, RGBufferAccess access)
    {
        if (m_Graph.m_ValidationEnabled)
            m_Graph.ValidateHandle(buffer.Frame, m_Graph.m_Passes[m_PassIndex].Name);
        m_Graph.AddAccess(m_PassIndex, buffer.Index, false, IsWrite(access), BufferState(access, m_Graph.m_Passes[m_PassIndex].Flags));
        return buffer;
    }

    RGBuffer RGBuilder::Write(RGBuffer buffer, RGBufferAccess access)
    {
        if (m_Graph.m_ValidationEnabled)
            m_Graph.ValidateHandle(buffer.Frame, m_Graph.m_Passes[m_PassIndex].Name);
        m_Graph.AddAccess(m_PassIndex, buffer.Index, false, true, BufferState(access, m_Graph.m_Passes[m_PassIndex].Flags));
        return buffer;
    }

    void RGBuilder::ColorTarget(RGTexture texture, NRI::LoadOP load, NRI::StoreOP store, NRI::ClearColor clear)
    {
        if (m_Graph.m_ValidationEnabled)
            m_Graph.ValidateHandle(texture.Frame, m_Graph.m_Passes[m_PassIndex].Name);
        if (load == NRI::LoadOP::load)
            m_Graph.AddAccess(m_PassIndex, texture.Index, true, false, { NRI::AccessBits::ColorAttachmentRead, NRI::StageBits::ColorAttachmentOutput });
        m_Graph.AddAccess(m_PassIndex, texture.Index, true, true, { NRI::AccessBits::ColorAttachmentWrite, NRI::StageBits::ColorAttachmentOutput });

        RenderGraph::Attachment attachment;
        attachment.Texture = texture.Index;
        attachment.Load = load;
        attachment.Store = store;
        attachment.ClearColor = clear;
        m_Graph.m_Passes[m_PassIndex].ColorTargets.push_back(attachment);
    }

    void RGBuilder::DepthTarget(RGTexture texture, NRI::LoadOP load, NRI::StoreOP store, NRI::ClearDepth clear)
    {
        if (m_Graph.m_ValidationEnabled)
            m_Graph.ValidateHandle(texture.Frame, m_Graph.m_Passes[m_PassIndex].Name);
        if (load == NRI::LoadOP::load)
            m_Graph.AddAccess(m_PassIndex, texture.Index, true, false, { NRI::AccessBits::DepthStencilRead, NRI::StageBits::FragmentTests });
        m_Graph.AddAccess(m_PassIndex, texture.Index, true, true, { NRI::AccessBits::DepthStencilWrite, NRI::StageBits::FragmentTests });

        RenderGraph::Attachment& attachment = m_Graph.m_Passes[m_PassIndex].DepthTarget;
        attachment.Texture = texture.Index;
        attachment.Load = load;
        attachment.Store = store;
        attachment.ClearDepth = clear;
    }

    void RGBuilder::SwapchainTarget(NRI::Swapchain& swapchain, uint32_t imageIndex, NRI::LoadOP load, NRI::StoreOP store)
    {
        RenderGraph::Pass& pass = m_Graph.m_Passes[m_PassIndex];
        pass.Swapchain = &swapchain;
        pass.SwapchainImage = imageIndex;
        pass.SwapchainLoad = load;
        pass.SwapchainStore = store;
    }

    void RGBuilder::SetRenderArea(NRI::Extent2D area, RGViewport viewport)
    {
        RenderGraph::Pass& pass = m_Graph.m_Passes[m_PassIndex];
        pass.RenderArea = area;
        pass.Viewport = viewport;
    }

    void RGBuilder::RecordExclusive(const char* key)
    {
        m_Graph.m_Passes[m_PassIndex].ExclusiveMask |= 1u << m_Graph.GetExclusiveKeyBit(key);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // RenderGraph: frame and resources

    RenderGraph::~RenderGraph()
    {
        ReleaseCallbacks();
    }

    void RenderGraph::Initialize(NRI::Device& device, NRI::DescriptorHeap& resourceHeap, uint32_t framesInFlight)
    {
        m_Device = &device;
        m_FramesInFlight = framesInFlight;
        m_Pool.Initialize(device, resourceHeap);

        m_Recorders.resize(static_cast<size_t>(MaxChunks) * framesInFlight);
        m_ChunkScratch.resize(MaxChunks);
        m_GpuScopeContexts.resize(MaxChunks);
    }

    void RenderGraph::ReleaseResources()
    {
        m_Textures.clear();
        m_Buffers.clear();
        m_Pool.ReleaseAll();

        m_SubmitList.clear();
        for (CommandRecorder& recorder : m_Recorders)
        {
            recorder.CommandBuffer.reset();
            recorder.Allocator.reset();
        }
    }

    void RenderGraph::Reset(const RGFrameDesc& frame)
    {
        ReleaseCallbacks();

        m_Frame = frame;
        ++m_FrameId;
        m_Pool.BeginFrame(frame.FrameNumber);

        m_InspectedTexture = RGInvalidIndex;
        m_Inspector = nullptr;
        m_InspectionPass = RGInvalidIndex;
        m_InspectionActive = false;
        m_ValidationMessages.clear();
        m_UnusedWrites.clear();

        m_PassCount = 0;
        m_Events.clear();
        m_GroupNames.clear();
        m_OpenGroupDepth = 0;
        m_Textures.clear();
        m_Buffers.clear();
        m_ImportedTextures.clear();
        m_ImportedBuffers.clear();
        m_Blackboard.NextFrame();

        m_FrameHistoryResets.swap(m_PendingHistoryResets);
        m_PendingHistoryResets.clear();
        m_FrameResetAll = m_PendingResetAll;
        m_PendingResetAll = false;
    }

    RGTextureKey RenderGraph::ResolveKey(const RGTextureDesc& desc) const
    {
        RGTextureKey key;
        switch (desc.Size)
        {
        case RGSize::RenderResolution:
            key.Width = m_Frame.RenderExtent.width;
            key.Height = m_Frame.RenderExtent.height;
            break;
        case RGSize::OutputResolution:
            key.Width = m_Frame.OutputExtent.width;
            key.Height = m_Frame.OutputExtent.height;
            break;
        case RGSize::Absolute:
            key.Width = desc.Width;
            key.Height = desc.Height;
            break;
        }
        key.Format = desc.Format;
        key.Usage = desc.Usage;
        key.MipLevels = desc.MipLevels;
        return key;
    }

    RGTexture RenderGraph::CreateTexture(const char* name, const RGTextureDesc& desc)
    {
        TextureEntry entry;
        entry.Name = name;
        entry.Kind = RGResourceKind::Transient;
        entry.Key = ResolveKey(desc);
        m_Textures.push_back(entry);
        return { static_cast<uint32_t>(m_Textures.size() - 1), m_FrameId };
    }

    RGBuffer RenderGraph::CreateBuffer(const char* name, const RGBufferDesc& desc)
    {
        BufferEntry entry;
        entry.Name = name;
        entry.Kind = RGResourceKind::Transient;
        entry.Key = { desc.Size, desc.Usage };
        m_Buffers.push_back(entry);
        return { static_cast<uint32_t>(m_Buffers.size() - 1), m_FrameId };
    }

    RGTextureHistory RenderGraph::GetHistoryTexture(const char* key, const RGTextureDesc& desc, uint32_t count)
    {
        NOX_CORE_ASSERT(count > 0 && count <= RGMaxHistoryLength, "RenderGraph::GetHistoryTexture: invalid history length");

        bool created = false;
        RGResourcePool::TextureHistory& history = m_Pool.AcquireTextureHistory(key, ResolveKey(desc), count, created);

        RGTextureHistory result;
        result.Count = count;
        result.WasReset = created || WasHistoryReset(key);
        for (uint32_t i = 0; i < count; ++i)
        {
            TextureEntry entry;
            entry.Name = key;
            entry.Kind = RGResourceKind::History;
            entry.Key = history.Key;
            entry.Texture = history.Textures[i]->Handle.get();
            entry.StorageSlots = &history.Textures[i]->StorageSlots;
            m_Textures.push_back(entry);
            result.Textures[i] = { static_cast<uint32_t>(m_Textures.size() - 1), m_FrameId };
        }
        return result;
    }

    RGBufferHistory RenderGraph::GetHistoryBuffer(const char* key, const RGBufferDesc& desc, uint32_t count)
    {
        NOX_CORE_ASSERT(count > 0 && count <= RGMaxHistoryLength, "RenderGraph::GetHistoryBuffer: invalid history length");

        bool created = false;
        RGResourcePool::BufferHistory& history = m_Pool.AcquireBufferHistory(key, { desc.Size, desc.Usage }, count, created);

        RGBufferHistory result;
        result.Count = count;
        result.WasReset = created || WasHistoryReset(key);
        for (uint32_t i = 0; i < count; ++i)
        {
            BufferEntry entry;
            entry.Name = key;
            entry.Kind = RGResourceKind::History;
            entry.Key = history.Key;
            entry.Buffer = history.Buffers[i]->Handle.get();
            m_Buffers.push_back(entry);
            result.Buffers[i] = { static_cast<uint32_t>(m_Buffers.size() - 1), m_FrameId };
        }
        return result;
    }

    RGTexture RenderGraph::ImportTexture(const char* name, NRI::Texture2D* texture, RGImportAccess access)
    {
        NOX_CORE_ASSERT(texture, "RenderGraph::ImportTexture: null texture");
        auto [found, inserted] = m_ImportedTextures.try_emplace(texture, static_cast<uint32_t>(m_Textures.size()));
        if (inserted)
        {
            TextureEntry entry;
            entry.Name = name;
            entry.Kind = RGResourceKind::Imported;
            entry.ReadOnly = access == RGImportAccess::ReadOnly;
            entry.Key.Width = texture->GetWidth();
            entry.Key.Height = texture->GetHeight();
            entry.Texture = texture;
            m_Textures.push_back(entry);
        }
        return { found->second, m_FrameId };
    }

    RGBuffer RenderGraph::ImportBuffer(const char* name, NRI::Buffer* buffer, RGImportAccess access)
    {
        NOX_CORE_ASSERT(buffer, "RenderGraph::ImportBuffer: null buffer");
        auto [found, inserted] = m_ImportedBuffers.try_emplace(buffer, static_cast<uint32_t>(m_Buffers.size()));
        if (inserted)
        {
            BufferEntry entry;
            entry.Name = name;
            entry.Kind = RGResourceKind::Imported;
            entry.ReadOnly = access == RGImportAccess::ReadOnly;
            entry.Buffer = buffer;
            m_Buffers.push_back(entry);
        }
        return { found->second, m_FrameId };
    }

    void RenderGraph::ResetHistory(const char* key)
    {
        m_PendingHistoryResets.emplace(key);
    }

    void RenderGraph::ResetAllHistory()
    {
        m_PendingResetAll = true;
    }

    bool RenderGraph::WasHistoryReset(const char* key) const
    {
        return m_FrameResetAll || m_FrameHistoryResets.contains(key);
    }

    NRI::Texture2D* RenderGraph::GetTexture(RGTexture texture) const
    {
        if (!texture.IsValid() || texture.Index >= m_Textures.size())
            return nullptr;
        return m_Textures[texture.Index].Texture;
    }

    uint32_t RenderGraph::GetSlotOr(RGTexture texture, uint32_t fallback) const
    {
        const NRI::Texture2D* resource = GetTexture(texture);
        return resource ? resource->GetDescriptorIndexSlot() : fallback;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // RenderGraph: passes

    void RenderGraph::PushGroup(const char* name)
    {
        m_Events.push_back({ EventType::GroupBegin, static_cast<uint32_t>(m_GroupNames.size()) });
        m_GroupNames.push_back(name);
        ++m_OpenGroupDepth;
    }

    void RenderGraph::PopGroup()
    {
        NOX_CORE_ASSERT(m_OpenGroupDepth > 0, "RenderGraph::PopGroup without PushGroup");
        m_Events.push_back({ EventType::GroupEnd, 0 });
        --m_OpenGroupDepth;
    }

    uint32_t RenderGraph::BeginPass(const char* name, RGPassFlags flags)
    {
        if (m_PassCount == m_Passes.size())
            m_Passes.emplace_back();

        Pass& pass = m_Passes[m_PassCount];
        pass.Name = name;
        pass.Flags = flags;
        pass.Accesses.clear();
        pass.ColorTargets.clear();
        pass.DepthTarget = {};
        pass.Swapchain = nullptr;
        pass.SwapchainImage = 0;
        pass.RenderArea = {};
        pass.Viewport = RGViewport::FlippedY;
        pass.Payload = nullptr;
        pass.Invoke = nullptr;
        pass.Destroy = nullptr;
        pass.ExclusiveMask = 0;
        pass.RecordMs = 0.0f;
        pass.Culled = false;
        pass.SynchronizeAfter = false;
        pass.FirstTextureBarrier = 0;
        pass.TextureBarrierCount = 0;
        pass.FirstBufferBarrier = 0;
        pass.BufferBarrierCount = 0;

        m_Events.push_back({ EventType::Pass, m_PassCount });
        return m_PassCount++;
    }

    void RenderGraph::AddAccess(uint32_t passIndex, uint32_t resource, bool isTexture, bool isWrite, NRI::ResourceState state)
    {
        NOX_CORE_ASSERT(resource != RGInvalidIndex, "RenderGraph: pass declares an invalid resource handle");
        m_Passes[passIndex].Accesses.push_back({ resource, isTexture, isWrite, state });
    }

    uint32_t RenderGraph::GetExclusiveKeyBit(const char* key)
    {
        for (uint32_t bit = 0; bit < m_ExclusiveKeys.size(); ++bit)
        {
            if (std::strcmp(m_ExclusiveKeys[bit], key) == 0)
                return bit;
        }
        NOX_CORE_ASSERT(m_ExclusiveKeys.size() < JobSystem::MaxExclusiveKeys, "RenderGraph: too many exclusive recording keys");
        m_ExclusiveKeys.push_back(key);
        return static_cast<uint32_t>(m_ExclusiveKeys.size() - 1);
    }

    bool RenderGraph::HasSideEffects(const Pass& pass) const
    {
        if (HasFlag(pass.Flags, RGPassFlags::NeverCull) || pass.Swapchain)
            return true;

        // Writing a resource that outlives the frame (imported or history) is visible outside this frame's graph.
        for (const ResourceAccess& access : pass.Accesses)
        {
            if (!access.IsWrite)
                continue;
            const RGResourceKind kind = access.IsTexture ? m_Textures[access.Resource].Kind : m_Buffers[access.Resource].Kind;
            if (kind != RGResourceKind::Transient)
                return true;
        }
        return false;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // RenderGraph: compile

    void RenderGraph::Compile()
    {
        NOX_CORE_ASSERT(m_OpenGroupDepth == 0, "RenderGraph::Compile with an unclosed group");

        // Resource index space: textures first, then buffers.
        const size_t textureCount = m_Textures.size();
        auto resourceKey = [textureCount](const ResourceAccess& access)
        {
            return access.IsTexture ? access.Resource : static_cast<uint32_t>(textureCount) + access.Resource;
        };

        // 1. Culling, back to front: a pass is needed when it has side effects or writes something a later needed pass
        // reads. Needed passes make their own reads needed in turn.
        m_ResourceRead.assign(textureCount + m_Buffers.size(), false);
        for (uint32_t index = m_PassCount; index-- > 0;)
        {
            Pass& pass = m_Passes[index];
            bool needed = HasSideEffects(pass);
            for (size_t i = 0; !needed && i < pass.Accesses.size(); ++i)
                needed = pass.Accesses[i].IsWrite && m_ResourceRead[resourceKey(pass.Accesses[i])];

            pass.Culled = !needed;
            if (!needed)
                continue;

            for (const ResourceAccess& access : pass.Accesses)
            {
                if (!access.IsWrite)
                    m_ResourceRead[resourceKey(access)] = true;
            }

            if (HasFlag(pass.Flags, RGPassFlags::Raster) && pass.RenderArea.width == 0 && pass.RenderArea.height == 0)
                NOX_CORE_ERROR("RenderGraph: raster pass '{}' has no render area", pass.Name);
        }

        // 2. Lifetimes of the resources executed passes access, then transient allocation.
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            const Pass& pass = m_Passes[index];
            if (pass.Culled)
                continue;

            for (const ResourceAccess& access : pass.Accesses)
            {
                uint32_t& firstPass = access.IsTexture ? m_Textures[access.Resource].FirstPass : m_Buffers[access.Resource].FirstPass;
                uint32_t& lastPass = access.IsTexture ? m_Textures[access.Resource].LastPass : m_Buffers[access.Resource].LastPass;
                firstPass = std::min(firstPass, index);
                lastPass = std::max(lastPass, index);
            }
        }
        AllocateTransients();

        // 3. Synchronization (§5.4.5): where a later executed pass uses what an earlier one wrote.
        if (m_Synchronization == RGSynchronization::Blanket)
            PlanBlanketSynchronization();
        else
            PlanPreciseSynchronization();

        PlaceInspection();
        if (m_ValidationEnabled)
            ValidateCompiledFrame();
    }

    void RenderGraph::PlanBlanketSynchronization()
    {
        // Front to back: a pass synchronizes after itself when a later executed pass accesses something it wrote.
        const size_t textureCount = m_Textures.size();
        std::vector<uint32_t>& lastWriter = m_LastWriterScratch;
        lastWriter.assign(textureCount + m_Buffers.size(), RGInvalidIndex);
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            Pass& pass = m_Passes[index];
            if (pass.Culled)
                continue;

            for (const ResourceAccess& access : pass.Accesses)
            {
                const uint32_t writer = lastWriter[access.IsTexture ? access.Resource : textureCount + access.Resource];
                if (writer != RGInvalidIndex && writer != index)
                    m_Passes[writer].SynchronizeAfter = true;
            }
            for (const ResourceAccess& access : pass.Accesses)
            {
                if (access.IsWrite)
                    lastWriter[access.IsTexture ? access.Resource : textureCount + access.Resource] = index;
            }
        }
    }

    void RenderGraph::PlanPreciseSynchronization()
    {
        // Front to back, per resource. A write waits for the last write and for every read since it (write after read
        // needs all readers, not only the most recent one). A read waits for the last write unless that write was
        // already made visible to its stages: several readers after one write can each need their own barrier (the
        // Hi-Z build reads depth in compute, the previous-frame copy reads it as a transfer). Read after read with no
        // write between needs none. All of a pass's accesses to one resource merge into one state; the barriers are
        // recorded at the start of the consuming pass, in whichever command buffer records it.
        m_TextureBarriers.clear();
        m_BufferBarriers.clear();

        const size_t textureCount = m_Textures.size();
        const size_t resourceCount = textureCount + m_Buffers.size();
        std::vector<NRI::ResourceState>& lastWrite = m_LastWriteScratch;
        std::vector<NRI::ResourceState>& readsSinceWrite = m_ReadsSinceWriteScratch;
        std::vector<NRI::ResourceState>& visible = m_VisibleScratch;
        std::vector<bool>& accessed = m_AccessedScratch;
        std::vector<bool>& written = m_WrittenScratch;
        lastWrite.assign(resourceCount, NRI::ResourceState{});
        readsSinceWrite.assign(resourceCount, NRI::ResourceState{});
        visible.assign(resourceCount, NRI::ResourceState{});
        accessed.assign(resourceCount, false);
        written.assign(resourceCount, false);

        auto physical = [&](size_t key) -> const void*
        {
            return key < textureCount ? static_cast<const void*>(m_Textures[key].Texture)
                                      : static_cast<const void*>(m_Buffers[key - textureCount].Buffer);
        };

        // Start where the previous frame left each resource: its first access this frame is ordered after that.
        for (size_t key = 0; key < resourceCount; ++key)
        {
            const void* resource = physical(key);
            auto carried = resource ? m_CarriedStates.find(resource) : m_CarriedStates.end();
            if (carried == m_CarriedStates.end())
                continue;
            lastWrite[key] = carried->second.LastWrite;
            readsSinceWrite[key] = carried->second.ReadsSinceWrite;
            visible[key] = carried->second.Visible;
            written[key] = carried->second.Written;
            accessed[key] = true;
        }

        auto merge = [](NRI::ResourceState& into, const NRI::ResourceState& state)
        {
            into.access |= state.access;
            into.stages |= state.stages;
        };
        auto covers = [](const NRI::ResourceState& covered, const NRI::ResourceState& state)
        {
            return (covered.access & state.access) == state.access && (covered.stages & state.stages) == state.stages;
        };

        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            Pass& pass = m_Passes[index];
            if (pass.Culled)
                continue;

            std::vector<std::pair<uint32_t, NRI::ResourceState>>& passStates = m_PassStatesScratch;
            passStates.clear();
            for (const ResourceAccess& access : pass.Accesses)
            {
                const uint32_t key = access.IsTexture ? access.Resource : static_cast<uint32_t>(textureCount) + access.Resource;
                auto merged = std::find_if(passStates.begin(), passStates.end(), [key](const auto& entry) { return entry.first == key; });
                if (merged == passStates.end())
                {
                    passStates.emplace_back(key, access.State);
                    continue;
                }
                merged->second.access |= access.State.access;
                merged->second.stages |= access.State.stages;
            }

            pass.FirstTextureBarrier = static_cast<uint32_t>(m_TextureBarriers.size());
            pass.FirstBufferBarrier = static_cast<uint32_t>(m_BufferBarriers.size());
            for (const auto& [key, state] : passStates)
            {
                auto addBarrier = [&](const NRI::ResourceState& before)
                {
                    if (key < textureCount)
                        m_TextureBarriers.push_back({ m_Textures[key].Texture, before, state });
                    else
                        m_BufferBarriers.push_back({ m_Buffers[key - textureCount].Buffer, before, state });
                };

                if (HasWriteAccess(state))
                {
                    if (accessed[key])
                    {
                        NRI::ResourceState before = lastWrite[key];
                        merge(before, readsSinceWrite[key]);
                        addBarrier(before);
                    }
                    lastWrite[key] = state;
                    readsSinceWrite[key] = {};
                    visible[key] = state;
                    written[key] = true;
                }
                else
                {
                    if (written[key] && !covers(visible[key], state))
                    {
                        addBarrier(lastWrite[key]);
                        merge(visible[key], state);
                    }
                    merge(readsSinceWrite[key], state);
                }
                accessed[key] = true;
            }
            pass.TextureBarrierCount = static_cast<uint32_t>(m_TextureBarriers.size()) - pass.FirstTextureBarrier;
            pass.BufferBarrierCount = static_cast<uint32_t>(m_BufferBarriers.size()) - pass.FirstBufferBarrier;
        }

        m_CarriedStates.clear();
        for (size_t key = 0; key < resourceCount; ++key)
        {
            const void* resource = physical(key);
            if (resource && accessed[key])
                m_CarriedStates[resource] = CarriedState{ lastWrite[key], readsSinceWrite[key], visible[key], written[key] };
        }
    }

    void RenderGraph::AllocateTransients()
    {
        // In order of first use, so a resource whose lifetime ended earlier this frame hands its memory to the next one
        // with the same key (aliasing).
        std::vector<uint32_t>& order = m_AllocationOrderScratch;

        order.clear();
        for (uint32_t index = 0; index < m_Textures.size(); ++index)
        {
            if (m_Textures[index].Kind == RGResourceKind::Transient && m_Textures[index].FirstPass != RGInvalidIndex)
                order.push_back(index);
        }
        std::sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b) { return m_Textures[a].FirstPass < m_Textures[b].FirstPass; });
        for (uint32_t index : order)
        {
            TextureEntry& entry = m_Textures[index];
            RGResourcePool::Texture& texture = m_Pool.AcquireTransientTexture(entry.Key, entry.FirstPass, entry.LastPass);
            entry.Texture = texture.Handle.get();
            entry.StorageSlots = &texture.StorageSlots;
        }

        order.clear();
        for (uint32_t index = 0; index < m_Buffers.size(); ++index)
        {
            if (m_Buffers[index].Kind == RGResourceKind::Transient && m_Buffers[index].FirstPass != RGInvalidIndex)
                order.push_back(index);
        }
        std::sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b) { return m_Buffers[a].FirstPass < m_Buffers[b].FirstPass; });
        for (uint32_t index : order)
        {
            BufferEntry& entry = m_Buffers[index];
            entry.Buffer = m_Pool.AcquireTransientBuffer(entry.Key, entry.FirstPass, entry.LastPass).Handle.get();
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // RenderGraph: execute

    std::span<NRI::CommandBuffer* const> RenderGraph::Execute(uint32_t frameSlot)
    {
        NOX_CORE_ASSERT(m_Device && frameSlot < m_FramesInFlight, "RenderGraph::Execute: not initialized or invalid frame slot");

        PlanChunks();
        PrepareRecording(frameSlot);

        const uint32_t chunkCount = static_cast<uint32_t>(m_Chunks.size());
#if NOX_PROFILING_ENABLED
        Profiler::Get().BeginGpuFrame(frameSlot);
#endif
        if (chunkCount == 1)
        {
            RecordChunk(0);
        }
        else
        {
            JobSystem::Get().RunTasks("Record Command Chunk", std::span<const uint32_t>(m_ChunkExclusiveMasks.data(), chunkCount),
                                      [this](uint32_t chunkIndex) { RecordChunk(chunkIndex); });
        }
#if NOX_PROFILING_ENABLED
        Profiler::Get().EndGpuFrame(std::span<GpuScopeContext>(m_GpuScopeContexts.data(), chunkCount));
#endif

        // Smoothed recording cost per pass name drives the next frame's chunking.
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            const Pass& pass = m_Passes[index];
            if (pass.Culled)
                continue;

            auto [found, inserted] = m_PassRecordMs.try_emplace(pass.Name, pass.RecordMs);
            if (!inserted)
                found->second += (pass.RecordMs - found->second) * 0.1f;
        }

        m_SubmitList.clear();
        for (uint32_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
            m_SubmitList.push_back(m_Recorders[static_cast<size_t>(chunkIndex) * m_FramesInFlight + frameSlot].CommandBuffer.get());

        if (m_ReportEnabled)
            BuildReport();

        ReleaseCallbacks();
        m_Pool.EndFrame();
        return m_SubmitList;
    }

    float RenderGraph::GetEstimatedRecordMs(const Pass& pass) const
    {
        const auto found = m_PassRecordMs.find(pass.Name);
        return found != m_PassRecordMs.end() ? found->second : DefaultPassRecordMs;
    }

    void RenderGraph::PlanChunks()
    {
        float totalMs = 0.0f;
        uint32_t executedPasses = 0;
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            if (!m_Passes[index].Culled)
            {
                totalMs += GetEstimatedRecordMs(m_Passes[index]);
                ++executedPasses;
            }
        }

        // The chunk count with the lowest estimated wall time wins; ties keep fewer chunks. Passes sharing an exclusive
        // key (e.g. NRD, spread over the whole frame) serialize their chunks, so splitting them only adds overhead.
        uint32_t bestCount = 1;
        if (m_ParallelRecording)
        {
            const uint32_t maxChunks = std::min({ MaxChunks, JobSystem::Get().GetWorkerCount(), std::max(executedPasses, 1u) });
            float bestMs = totalMs;
            for (uint32_t chunkCount = 2; chunkCount <= maxChunks; ++chunkCount)
            {
                const float estimatedMs = SplitIntoChunks(chunkCount, totalMs);
                if (estimatedMs < bestMs)
                {
                    bestMs = estimatedMs;
                    bestCount = chunkCount;
                }
            }
        }
        SplitIntoChunks(bestCount, totalMs);
    }

    float RenderGraph::SplitIntoChunks(uint32_t chunkCount, float totalMs)
    {
        m_Chunks.clear();
        m_ChunkOpenGroups.clear();
        m_ChunkExclusiveMasks.clear();
        m_ChunkRecordMs.clear();

        const float targetMs = totalMs / static_cast<float>(chunkCount);

        // Consecutive events, split before a pass once the current chunk reached its share of the cost. A split lands in
        // front of the group begins that lead into that pass (the previous chunk keeps its trailing group ends), so every
        // chunk starts with the groups still open from before (m_ChunkOpenGroups) and opens the rest itself.
        std::vector<uint32_t>& stack = m_PlanStackScratch;
        stack.clear();
        m_Chunks.push_back({ 0, 0, 0, 0 });
        m_ChunkExclusiveMasks.push_back(0);
        m_ChunkRecordMs.push_back(0.0f);

        uint32_t chunkPasses = 0;
        uint32_t splitEvent = RGInvalidIndex; // first event after the last executed pass that is not a group end
        uint32_t splitStackSize = 0;
        for (uint32_t eventIndex = 0; eventIndex < m_Events.size(); ++eventIndex)
        {
            const Event& event = m_Events[eventIndex];
            if (event.Type == EventType::GroupEnd)
            {
                stack.pop_back();
                continue;
            }
            if (event.Type == EventType::Pass && m_Passes[event.Index].Culled)
                continue;

            if (splitEvent == RGInvalidIndex)
            {
                splitEvent = eventIndex;
                splitStackSize = static_cast<uint32_t>(stack.size());
            }

            if (event.Type == EventType::GroupBegin)
            {
                stack.push_back(event.Index);
                continue;
            }

            const Pass& pass = m_Passes[event.Index];
            const float passMs = GetEstimatedRecordMs(pass);
            if (chunkPasses > 0 && m_Chunks.size() < chunkCount && m_ChunkRecordMs.back() + passMs * 0.5f > targetMs)
            {
                m_Chunks.back().EndEvent = splitEvent;
                Chunk next;
                next.FirstEvent = splitEvent;
                next.OpenGroupsOffset = static_cast<uint32_t>(m_ChunkOpenGroups.size());
                next.OpenGroupsCount = splitStackSize;
                m_ChunkOpenGroups.insert(m_ChunkOpenGroups.end(), stack.begin(), stack.begin() + splitStackSize);
                m_Chunks.push_back(next);
                m_ChunkExclusiveMasks.push_back(0);
                m_ChunkRecordMs.push_back(0.0f);
                chunkPasses = 0;
            }

            m_Passes[event.Index].Chunk = static_cast<uint32_t>(m_Chunks.size() - 1);
            m_ChunkRecordMs.back() += passMs;
            ++chunkPasses;
            m_ChunkExclusiveMasks.back() |= pass.ExclusiveMask;
            splitEvent = RGInvalidIndex;
        }
        m_Chunks.back().EndEvent = static_cast<uint32_t>(m_Events.size());

        // Wall time: the longest chunk, or the chunks holding one exclusive key back to back.
        float wallMs = 0.0f;
        for (float chunkMs : m_ChunkRecordMs)
            wallMs = std::max(wallMs, chunkMs);
        for (uint32_t key = 0; key < m_ExclusiveKeys.size(); ++key)
        {
            float serialMs = 0.0f;
            for (uint32_t chunkIndex = 0; chunkIndex < m_Chunks.size(); ++chunkIndex)
            {
                if ((m_ChunkExclusiveMasks[chunkIndex] & (1u << key)) != 0)
                    serialMs += m_ChunkRecordMs[chunkIndex];
            }
            wallMs = std::max(wallMs, serialMs);
        }
        return wallMs + ChunkOverheadMs * static_cast<float>(m_Chunks.size() - 1);
    }

    void RenderGraph::PrepareRecording(uint32_t frameSlot)
    {
        m_FrameSlot = frameSlot;

        // Command buffers are created on first use; a chunk's pool is reset by its recording task.
        for (uint32_t chunkIndex = 0; chunkIndex < m_Chunks.size(); ++chunkIndex)
        {
            CommandRecorder& recorder = m_Recorders[static_cast<size_t>(chunkIndex) * m_FramesInFlight + frameSlot];
            if (!recorder.Allocator)
            {
                recorder.Allocator = m_Device->createCommandAllocator(NRI::CommandBufferReset::WithAllocator);
                recorder.CommandBuffer = recorder.Allocator->allocateCommandBuffer(1);
            }
        }

        // Profiler scope ids are registered here: recording threads only read them.
        m_GroupScopeIds.resize(m_GroupNames.size());
        for (uint32_t group = 0; group < m_GroupNames.size(); ++group)
            m_GroupScopeIds[group] = GetScopeId(m_GroupNames[group]);
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            if (!m_Passes[index].Culled)
                m_Passes[index].ScopeId = GetScopeId(m_Passes[index].Name);
        }
    }

    void RenderGraph::RecordChunk(uint32_t chunkIndex)
    {
        const Chunk& chunk = m_Chunks[chunkIndex];
        [[maybe_unused]] const bool firstChunk = chunkIndex == 0;
        [[maybe_unused]] const bool lastChunk = chunkIndex + 1 == m_Chunks.size();
        CommandRecorder& recorder = m_Recorders[static_cast<size_t>(chunkIndex) * m_FramesInFlight + m_FrameSlot];
        ChunkScratch& scratch = m_ChunkScratch[chunkIndex];
        GpuScopeContext& gpuScopes = m_GpuScopeContexts[chunkIndex];

        // The slot's previous submission finished (the caller waited for its fence): recycle the whole pool at once.
        recorder.Allocator->reset();
        NRI::CommandBuffer& cmd = *recorder.CommandBuffer;
        cmd.begin(0, true);

#if NOX_PROFILING_ENABLED
        if (firstChunk)
            Profiler::Get().RecordGpuFrameBegin(cmd);
#endif
        if (m_CommandBufferSetup)
            m_CommandBufferSetup(cmd);

        // Groups open lazily with their chunk's first executed pass, so a group whose passes were all culled leaves no scope,
        // and a group spanning chunks gets one scope per chunk (the profiler sums them).
        std::vector<uint32_t>& groupStack = scratch.GroupStack;
        groupStack.assign(m_ChunkOpenGroups.begin() + chunk.OpenGroupsOffset,
                          m_ChunkOpenGroups.begin() + chunk.OpenGroupsOffset + chunk.OpenGroupsCount);
        uint32_t openedDepth = 0;

        auto closeOpenedGroup = [&]()
        {
#if NOX_PROFILING_ENABLED
            Profiler::Get().EndGpuScope(gpuScopes, cmd);
            Profiler::Get().EndCpuScope(m_GroupScopeIds[groupStack[openedDepth - 1]]);
#endif
            cmd.endDebugLabel();
            --openedDepth;
        };

        for (uint32_t eventIndex = chunk.FirstEvent; eventIndex < chunk.EndEvent; ++eventIndex)
        {
            const Event& event = m_Events[eventIndex];
            switch (event.Type)
            {
            case EventType::GroupBegin:
                groupStack.push_back(event.Index);
                break;

            case EventType::GroupEnd:
                if (openedDepth == groupStack.size())
                    closeOpenedGroup();
                groupStack.pop_back();
                break;

            case EventType::Pass:
            {
                Pass& pass = m_Passes[event.Index];
                if (pass.Culled)
                    break;

                while (openedDepth < groupStack.size())
                {
                    cmd.beginDebugLabel(m_GroupNames[groupStack[openedDepth]]);
#if NOX_PROFILING_ENABLED
                    const uint32_t scopeId = m_GroupScopeIds[groupStack[openedDepth]];
                    Profiler::Get().BeginCpuScope(scopeId);
                    Profiler::Get().BeginGpuScope(gpuScopes, cmd, scopeId);
#endif
                    ++openedDepth;
                }

                ExecutePass(pass, cmd, scratch, gpuScopes);
                break;
            }
            }
        }

        // Groups that continue in the next chunk close here and reopen there.
        while (openedDepth > 0)
            closeOpenedGroup();

        if (lastChunk && m_InspectionActive && m_InspectionPass == RGInvalidIndex)
            RecordInspection(cmd);

#if NOX_PROFILING_ENABLED
        if (lastChunk)
            Profiler::Get().RecordGpuFrameEnd(cmd);
#endif
        cmd.end(0);
    }

    void RenderGraph::ExecutePass(Pass& pass, NRI::CommandBuffer& cmd, ChunkScratch& scratch, GpuScopeContext& gpuScopes)
    {
        const auto start = std::chrono::steady_clock::now();
        const uint32_t passIndex = static_cast<uint32_t>(&pass - m_Passes.data());
        cmd.beginDebugLabel(pass.Name);
#if NOX_PROFILING_ENABLED
        Profiler::Get().BeginCpuScope(pass.ScopeId);
        Profiler::Get().BeginGpuScope(gpuScopes, cmd, pass.ScopeId);
        // Draw statistics (triangles, fragments, task/mesh shader invocations) of raster passes, when enabled.
        if (HasFlag(pass.Flags, RGPassFlags::Raster))
            Profiler::Get().BeginGpuStatistics(gpuScopes, cmd, pass.ScopeId);
#endif

        if (pass.TextureBarrierCount > 0 || pass.BufferBarrierCount > 0)
        {
            cmd.resourceBarriers(std::span<const NRI::TextureBarrierDesc>(m_TextureBarriers.data() + pass.FirstTextureBarrier, pass.TextureBarrierCount),
                                 std::span<const NRI::BufferBarrierDesc>(m_BufferBarriers.data() + pass.FirstBufferBarrier, pass.BufferBarrierCount));
        }

        // The swapchain image is the only image that changes layout (§5.4.4): ready for rendering, then for present.
        if (pass.Swapchain)
            cmd.transitionSwapchainLayout(*pass.Swapchain, pass.SwapchainImage, NRI::TextureLayout::Undefined, NRI::TextureLayout::ColorAttachment);

        const bool raster = HasFlag(pass.Flags, RGPassFlags::Raster);
        if (raster)
            BeginRendering(pass, cmd, scratch.RenderDesc);

        RGPassContext context(*this, cmd, pass.RenderArea, passIndex);
        pass.Invoke(pass.Payload, context);

        if (raster)
            cmd.endRendering();

        if (pass.Swapchain)
            cmd.transitionSwapchainLayout(*pass.Swapchain, pass.SwapchainImage, NRI::TextureLayout::ColorAttachment, NRI::TextureLayout::Present);

#if NOX_PROFILING_ENABLED
        Profiler::Get().EndGpuStatistics(gpuScopes, cmd);
        Profiler::Get().EndGpuScope(gpuScopes, cmd);
        Profiler::Get().EndCpuScope(pass.ScopeId);
#endif
        cmd.endDebugLabel();

        if (pass.SynchronizeAfter)
            cmd.executionBarrier();

        if (m_InspectionActive && m_InspectionPass == passIndex)
            RecordInspection(cmd);

        pass.RecordMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void RenderGraph::BeginRendering(const Pass& pass, NRI::CommandBuffer& cmd, NRI::RenderDesc& desc)
    {
        desc.renderArea = pass.RenderArea;
        desc.colorAttachments.clear();
        desc.depthAttachment = {};
        desc.isSwapchainPass = false;

        if (pass.Swapchain)
        {
            NRI::RenderAttachDesc attachment{};
            attachment.attachmentSwapchain = pass.Swapchain;
            attachment.resolveImageIndex = pass.SwapchainImage;
            attachment.loadOP = pass.SwapchainLoad;
            attachment.storeOP = pass.SwapchainStore;
            desc.colorAttachments.push_back(attachment);
        }

        for (const Attachment& target : pass.ColorTargets)
        {
            NRI::RenderAttachDesc attachment{};
            attachment.attachment = m_Textures[target.Texture].Texture;
            attachment.loadOP = target.Load;
            attachment.storeOP = target.Store;
            attachment.clearColor = target.ClearColor;
            desc.colorAttachments.push_back(attachment);
        }

        if (pass.DepthTarget.Texture != RGInvalidIndex)
        {
            desc.depthAttachment.attachment = m_Textures[pass.DepthTarget.Texture].Texture;
            desc.depthAttachment.loadOP = pass.DepthTarget.Load;
            desc.depthAttachment.storeOP = pass.DepthTarget.Store;
            desc.depthAttachment.clearDepth = pass.DepthTarget.ClearDepth;
        }

        cmd.beginRendering(desc);

        if (pass.Viewport == RGViewport::FlippedY)
        {
            const float width = static_cast<float>(pass.RenderArea.width);
            const float height = static_cast<float>(pass.RenderArea.height);
            cmd.setViewportWithCount({ 0.0f, height, width, -height }, 0.0f, 1.0f);
            cmd.setScissorWithCount(pass.RenderArea);
        }
    }

    void RenderGraph::ReleaseCallbacks()
    {
        // Callback captures live in the frame arena; only their destructors run here.
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            Pass& pass = m_Passes[index];
            if (pass.Destroy)
                pass.Destroy(pass.Payload);
            pass.Payload = nullptr;
            pass.Invoke = nullptr;
            pass.Destroy = nullptr;
        }
    }

    uint32_t RenderGraph::GetScopeId(const char* name)
    {
        auto [found, inserted] = m_ScopeIds.try_emplace(name, 0u);
        if (inserted)
            found->second = Profiler::Get().RegisterNamedScope(name);
        return found->second;
    }
}
