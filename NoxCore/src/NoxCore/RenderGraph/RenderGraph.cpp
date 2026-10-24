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
            if (HasFlag(flags, RGPassFlags::RayTracing)) return NRI::StageBits::RayTracing;
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
            case RGBufferAccess::AccelerationStructureBuildInput: return { NRI::AccessBits::AccelerationStructureRead,
                                                                           NRI::StageBits::AccelerationStructureBuild };
            case RGBufferAccess::AccelerationStructureRead: return { NRI::AccessBits::AccelerationStructureRead | NRI::AccessBits::ShaderRead, ShaderStages(flags) };
            case RGBufferAccess::CopySource: return { NRI::AccessBits::TransferRead, NRI::StageBits::Transfer };
            case RGBufferAccess::CopyDestination: return { NRI::AccessBits::TransferWrite, NRI::StageBits::Transfer };
            }
            return {};
        }

        // What a compute-only queue's barriers may name (VUID-vkCmdPipelineBarrier2-srcStageMask-03849: stages its family supports).
        constexpr uint32_t ComputeQueueStages = NRI::StageBits::Compute | NRI::StageBits::Transfer | NRI::StageBits::AccelerationStructureBuild |
                                                NRI::StageBits::Indirect | NRI::StageBits::RayTracing;
        constexpr uint32_t ComputeQueueAccess = NRI::AccessBits::ShaderRead | NRI::AccessBits::ShaderWrite | NRI::AccessBits::TransferRead |
                                                NRI::AccessBits::TransferWrite | NRI::AccessBits::AccelerationStructureRead |
                                                NRI::AccessBits::AccelerationStructureWrite | NRI::AccessBits::IndirectRead;

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

        m_Recorders.resize(static_cast<size_t>(MaxRecordedChunks) * framesInFlight);
        m_ChunkScratch.resize(MaxRecordedChunks);
        m_GpuScopeContexts.resize(MaxRecordedChunks);

        // Async compute: the graphics queue signals where a compute block may start, the compute queue where it is done.
        if (device.hasAsyncComputeQueue())
        {
            m_GraphicsTimeline = device.createTimelineSemaphore(0);
            m_ComputeTimeline = device.createTimelineSemaphore(0);
        }
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
        PlanQueues();
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

        auto physical = [&](size_t key) -> const void*
        {
            return key < textureCount ? static_cast<const void*>(m_Textures[key].Texture)
                                      : static_cast<const void*>(m_Buffers[key - textureCount].Buffer);
        };

        // The state is tracked per physical resource, not per graph resource: transients the pool aliases (one texture for two
        // whose lifetimes do not overlap) share it, so the second one's first access is ordered after the first one's last
        // (write after write / after read on the same memory).
        std::vector<uint32_t>& slotOf = m_PhysicalSlotScratch;
        std::vector<const void*>& slotResource = m_SlotResourceScratch;
        std::unordered_map<const void*, uint32_t>& slotByResource = m_SlotByResourceScratch;
        slotOf.resize(resourceCount);
        slotResource.clear();
        slotByResource.clear();
        for (size_t key = 0; key < resourceCount; ++key)
        {
            const void* resource = physical(key);
            if (!resource)
            {
                slotOf[key] = static_cast<uint32_t>(slotResource.size());
                slotResource.push_back(nullptr);
                continue;
            }
            auto [entry, added] = slotByResource.try_emplace(resource, static_cast<uint32_t>(slotResource.size()));
            if (added)
                slotResource.push_back(resource);
            slotOf[key] = entry->second;
        }
        const size_t slotCount = slotResource.size();

        std::vector<NRI::ResourceState>& lastWrite = m_LastWriteScratch;
        std::vector<NRI::ResourceState>& readsSinceWrite = m_ReadsSinceWriteScratch;
        std::vector<NRI::ResourceState>& visible = m_VisibleScratch;
        std::vector<bool>& accessed = m_AccessedScratch;
        std::vector<bool>& written = m_WrittenScratch;
        lastWrite.assign(slotCount, NRI::ResourceState{});
        readsSinceWrite.assign(slotCount, NRI::ResourceState{});
        visible.assign(slotCount, NRI::ResourceState{});
        accessed.assign(slotCount, false);
        written.assign(slotCount, false);

        // Start where the previous frame left each resource: its first access this frame is ordered after that.
        for (size_t slot = 0; slot < slotCount; ++slot)
        {
            const void* resource = slotResource[slot];
            auto carried = resource ? m_CarriedStates.find(resource) : m_CarriedStates.end();
            if (carried == m_CarriedStates.end())
                continue;
            lastWrite[slot] = carried->second.LastWrite;
            readsSinceWrite[slot] = carried->second.ReadsSinceWrite;
            visible[slot] = carried->second.Visible;
            written[slot] = carried->second.Written;
            accessed[slot] = true;
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
                const uint32_t slot = slotOf[key];
                auto addBarrier = [&](NRI::ResourceState before)
                {
                    // On the compute queue only what that queue did before is ordered by a barrier; the graphics queue's work it
                    // depends on is done before the block starts (the timeline wait at its fork), and the stages are not its own.
                    if (pass.Queue == NRI::QueueType::Compute)
                    {
                        before.stages &= ComputeQueueStages;
                        before.access &= ComputeQueueAccess;
                        if (before.stages == NRI::StageBits::None)
                            return;
                    }
                    if (key < textureCount)
                        m_TextureBarriers.push_back({ m_Textures[key].Texture, before, state });
                    else
                        m_BufferBarriers.push_back({ m_Buffers[key - textureCount].Buffer, before, state });
                };

                if (HasWriteAccess(state))
                {
                    if (accessed[slot])
                    {
                        NRI::ResourceState before = lastWrite[slot];
                        merge(before, readsSinceWrite[slot]);
                        addBarrier(before);
                    }
                    lastWrite[slot] = state;
                    readsSinceWrite[slot] = {};
                    visible[slot] = state;
                    written[slot] = true;
                }
                else
                {
                    if (written[slot] && !covers(visible[slot], state))
                    {
                        addBarrier(lastWrite[slot]);
                        merge(visible[slot], state);
                    }
                    merge(readsSinceWrite[slot], state);
                }
                accessed[slot] = true;
            }
            pass.TextureBarrierCount = static_cast<uint32_t>(m_TextureBarriers.size()) - pass.FirstTextureBarrier;
            pass.BufferBarrierCount = static_cast<uint32_t>(m_BufferBarriers.size()) - pass.FirstBufferBarrier;
        }

        m_CarriedStates.clear();
        for (size_t slot = 0; slot < slotCount; ++slot)
        {
            const void* resource = slotResource[slot];
            if (resource && accessed[slot])
                m_CarriedStates[resource] = CarriedState{ lastWrite[slot], readsSinceWrite[slot], visible[slot], written[slot] };
        }
    }

    void RenderGraph::PlanQueues()
    {
        m_AsyncBlocks.clear();
        m_AsyncComputePassCount = 0;
        std::vector<uint32_t>& executed = m_ExecutedScratch;
        executed.clear();
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            m_Passes[index].Queue = NRI::QueueType::Graphics;
            if (!m_Passes[index].Culled)
                executed.push_back(index);
        }
        // Per-queue barriers need Precise synchronization; the inspector records into the inspected pass's command buffer, which
        // may draw.
        if (!m_AsyncComputeEnabled || !m_GraphicsTimeline || m_Synchronization != RGSynchronization::Precise || m_InspectedTexture != RGInvalidIndex)
            return;

        const uint32_t count = static_cast<uint32_t>(executed.size());
        auto pass = [&](uint32_t position) -> Pass& { return m_Passes[executed[position]]; };
        bool anyAsync = false;
        for (uint32_t position = 0; position < count; ++position)
        {
            if (HasFlag(pass(position).Flags, RGPassFlags::AsyncCompute))
            {
                pass(position).Queue = NRI::QueueType::Compute;
                anyAsync = true;
            }
        }
        if (!anyAsync)
            return;

        const size_t textureCount = m_Textures.size();
        auto resourceKey = [textureCount](const ResourceAccess& access)
        {
            return access.IsTexture ? access.Resource : static_cast<uint32_t>(textureCount) + access.Resource;
        };
        std::vector<uint8_t>& blockAccess = m_BlockAccessScratch;

        // Blocks, fork and join. A block that cannot run beside graphics work (nothing graphics before it to start after, or none
        // after its fork to wait for it) goes back to the graphics queue, and the planning starts over: its passes are graphics
        // passes the other blocks may conflict with now.
        for (bool replan = true; replan;)
        {
            replan = false;
            m_AsyncBlocks.clear();
            for (uint32_t first = 0; first < count && !replan;)
            {
                if (pass(first).Queue != NRI::QueueType::Compute)
                {
                    ++first;
                    continue;
                }
                uint32_t end = first;
                while (end < count && pass(end).Queue == NRI::QueueType::Compute)
                    ++end;

                blockAccess.assign(textureCount + m_Buffers.size(), 0);
                for (uint32_t position = first; position < end; ++position)
                {
                    for (const ResourceAccess& access : pass(position).Accesses)
                        blockAccess[resourceKey(access)] |= access.IsWrite ? 2 : 1;
                }
                // A graphics pass conflicts with the block when one of them writes what the other accesses.
                auto conflicts = [&](const Pass& graphics)
                {
                    for (const ResourceAccess& access : graphics.Accesses)
                    {
                        const uint8_t blockUse = blockAccess[resourceKey(access)];
                        if (blockUse != 0 && (access.IsWrite || (blockUse & 2) != 0))
                            return true;
                    }
                    return false;
                };

                // Fork: the last earlier graphics pass it conflicts with, at least the frame's first graphics pass (the wait then
                // also orders it after the previous frame's graphics work, earlier on that queue), never before the previous
                // block's fork (the compute queue runs the blocks in order).
                uint32_t fork = RGInvalidIndex;
                uint32_t firstGraphics = RGInvalidIndex;
                for (uint32_t position = 0; position < first; ++position)
                {
                    if (pass(position).Queue == NRI::QueueType::Compute)
                        continue;
                    if (firstGraphics == RGInvalidIndex)
                        firstGraphics = position;
                    if (conflicts(pass(position)))
                        fork = position;
                }
                if (fork == RGInvalidIndex)
                    fork = firstGraphics;
                if (fork != RGInvalidIndex && !m_AsyncBlocks.empty())
                    fork = std::max(fork, m_AsyncBlocks.back().Fork);

                // Join: the first later graphics pass it conflicts with; none: the frame's last graphics submission waits.
                uint32_t join = RGInvalidIndex;
                bool graphicsAfterFork = false;
                for (uint32_t position = fork == RGInvalidIndex ? count : fork + 1; position < count; ++position)
                {
                    if (pass(position).Queue == NRI::QueueType::Compute)
                        continue;
                    graphicsAfterFork = true;
                    if (position >= end && join == RGInvalidIndex && conflicts(pass(position)))
                        join = position;
                }

                if (fork == RGInvalidIndex || !graphicsAfterFork)
                {
                    for (uint32_t position = first; position < end; ++position)
                        pass(position).Queue = NRI::QueueType::Graphics;
                    replan = true;
                    break;
                }
                m_AsyncBlocks.push_back({ first, end, fork, join });
                first = end;
            }
        }

        // Command buffers: the graphics segments between fork / join points, and one per block.
        std::vector<uint32_t> boundaries;
        for (const AsyncBlock& block : m_AsyncBlocks)
        {
            boundaries.push_back(block.Fork + 1);
            if (block.Join != RGInvalidIndex)
                boundaries.push_back(block.Join);
        }
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
        if (boundaries.size() + 1 + m_AsyncBlocks.size() > MaxRecordedChunks)
        {
            for (uint32_t position = 0; position < count; ++position)
                pass(position).Queue = NRI::QueueType::Graphics;
            m_AsyncBlocks.clear();
            return;
        }

        // While a block may run, the graphics work beside it runs too: what it accesses lives from its fork to its join, so no
        // transient of the graphics passes in between takes its memory.
        for (const AsyncBlock& block : m_AsyncBlocks)
        {
            const uint32_t forkPass = executed[block.Fork];
            const uint32_t joinPass = block.Join != RGInvalidIndex ? executed[block.Join] : m_PassCount - 1;
            for (uint32_t position = block.First; position < block.End; ++position)
            {
                for (const ResourceAccess& access : pass(position).Accesses)
                {
                    uint32_t& firstPass = access.IsTexture ? m_Textures[access.Resource].FirstPass : m_Buffers[access.Resource].FirstPass;
                    uint32_t& lastPass = access.IsTexture ? m_Textures[access.Resource].LastPass : m_Buffers[access.Resource].LastPass;
                    firstPass = std::min(firstPass, forkPass);
                    lastPass = std::max(lastPass, joinPass);
                }
            }
            m_AsyncComputePassCount += block.End - block.First;
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

    std::span<const RGSubmission> RenderGraph::Execute(uint32_t frameSlot)
    {
        NOX_CORE_ASSERT(m_Device && frameSlot < m_FramesInFlight, "RenderGraph::Execute: not initialized or invalid frame slot");

        PlanChunks();
        m_Units.clear();
        if (!m_AsyncBlocks.empty())
        {
            // The parallel split's chunk size, now within the queues' segments.
            float totalMs = 0.0f;
            for (const uint32_t index : m_ExecutedScratch)
                totalMs += GetEstimatedRecordMs(m_Passes[index]);
            PlanAsyncChunks(totalMs / static_cast<float>(std::max<size_t>(m_Chunks.size(), 1)));
        }
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

        // The submissions: one, or the units in order with their timeline points. A block's fork value is the graphics timeline
        // value its fork segment signals; its compute value is what its own submission signals and its joins wait for.
        m_Submissions.clear();
        if (m_Units.empty())
        {
            m_Submissions.push_back({ NRI::QueueType::Graphics, std::span<NRI::CommandBuffer* const>(m_SubmitList), {}, {} });
        }
        else
        {
            m_SubmissionWaits.resize(m_Units.size());
            m_SubmissionSignals.resize(m_Units.size());
            std::vector<uint64_t> forkValues(m_AsyncBlocks.size(), 0);
            std::vector<uint64_t> computeValues(m_AsyncBlocks.size(), 0);
            for (size_t unitIndex = 0; unitIndex < m_Units.size(); ++unitIndex)
            {
                const SubmissionUnit& unit = m_Units[unitIndex];
                std::vector<NRI::TimelinePoint>& waits = m_SubmissionWaits[unitIndex];
                std::vector<NRI::TimelinePoint>& signals = m_SubmissionSignals[unitIndex];
                waits.clear();
                signals.clear();
                if (unit.Queue == NRI::QueueType::Compute)
                {
                    waits.push_back({ m_GraphicsTimeline.get(), forkValues[unit.Block] });
                    computeValues[unit.Block] = ++m_ComputeTimelineValue;
                    signals.push_back({ m_ComputeTimeline.get(), computeValues[unit.Block] });
                }
                else
                {
                    for (const uint32_t block : unit.JoinBlocks)
                        waits.push_back({ m_ComputeTimeline.get(), computeValues[block] });
                    if (!unit.ForkBlocks.empty())
                    {
                        const uint64_t value = ++m_GraphicsTimelineValue;
                        for (const uint32_t block : unit.ForkBlocks)
                            forkValues[block] = value;
                        signals.push_back({ m_GraphicsTimeline.get(), value });
                    }
                }
                m_Submissions.push_back({ unit.Queue, std::span<NRI::CommandBuffer* const>(m_SubmitList.data() + unit.FirstChunk, unit.ChunkCount),
                                          std::span<const NRI::TimelinePoint>(waits), std::span<const NRI::TimelinePoint>(signals) });
            }
        }

        if (m_ReportEnabled)
            BuildReport();

        ReleaseCallbacks();
        m_Pool.EndFrame();
        return m_Submissions;
    }

    void RenderGraph::PlanAsyncChunks(float targetMs)
    {
        m_Chunks.clear();
        m_ChunkOpenGroups.clear();
        m_ChunkExclusiveMasks.clear();
        m_ChunkRecordMs.clear();
        m_Units.clear();

        m_PassEvent.assign(m_PassCount, 0);
        for (uint32_t eventIndex = 0; eventIndex < m_Events.size(); ++eventIndex)
        {
            if (m_Events[eventIndex].Type == EventType::Pass)
                m_PassEvent[m_Events[eventIndex].Index] = eventIndex;
        }

        const uint32_t count = static_cast<uint32_t>(m_ExecutedScratch.size());
        std::vector<uint32_t> boundaries{ 0, count };
        for (const AsyncBlock& block : m_AsyncBlocks)
        {
            boundaries.push_back(block.Fork + 1);
            if (block.Join != RGInvalidIndex)
                boundaries.push_back(block.Join);
        }
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

        std::vector<uint32_t> pendingJoins; // joins of segments with no graphics pass carry over to the next one
        size_t lastGraphicsUnit = 0;
        for (size_t segment = 0; segment + 1 < boundaries.size(); ++segment)
        {
            const uint32_t begin = boundaries[segment];
            const uint32_t end = boundaries[segment + 1];
            for (uint32_t block = 0; block < m_AsyncBlocks.size(); ++block)
            {
                if (m_AsyncBlocks[block].Join == begin)
                    pendingJoins.push_back(block);
            }
            bool hasGraphics = false;
            for (uint32_t position = begin; position < end && !hasGraphics; ++position)
                hasGraphics = m_Passes[m_ExecutedScratch[position]].Queue == NRI::QueueType::Graphics;
            if (!hasGraphics)
                continue;

            SubmissionUnit graphics;
            graphics.Queue = NRI::QueueType::Graphics;
            graphics.FirstChunk = static_cast<uint32_t>(m_Chunks.size());
            graphics.JoinBlocks = std::move(pendingJoins);
            pendingJoins.clear();
            AddChunk(NRI::QueueType::Graphics, begin, end, targetMs);
            graphics.ChunkCount = static_cast<uint32_t>(m_Chunks.size()) - graphics.FirstChunk;
            for (uint32_t block = 0; block < m_AsyncBlocks.size(); ++block)
            {
                if (m_AsyncBlocks[block].Fork + 1 == end)
                    graphics.ForkBlocks.push_back(block);
            }
            const std::vector<uint32_t> startedBlocks = graphics.ForkBlocks;
            lastGraphicsUnit = m_Units.size();
            m_Units.push_back(std::move(graphics));

            // The blocks this segment lets start, submitted right after it.
            for (const uint32_t block : startedBlocks)
            {
                SubmissionUnit compute;
                compute.Queue = NRI::QueueType::Compute;
                compute.FirstChunk = static_cast<uint32_t>(m_Chunks.size());
                compute.Block = block;
                AddChunk(NRI::QueueType::Compute, m_AsyncBlocks[block].First, m_AsyncBlocks[block].End, targetMs);
                compute.ChunkCount = static_cast<uint32_t>(m_Chunks.size()) - compute.FirstChunk;
                m_Units.push_back(std::move(compute));
            }
        }
        // Blocks nothing waits for: the frame's last graphics submission does (its fence then covers them).
        for (uint32_t block = 0; block < m_AsyncBlocks.size(); ++block)
        {
            if (m_AsyncBlocks[block].Join == RGInvalidIndex)
                m_Units[lastGraphicsUnit].JoinBlocks.push_back(block);
        }
    }

    void RenderGraph::AddChunk(NRI::QueueType queue, uint32_t firstPosition, uint32_t endPosition, float targetMs)
    {
        // The queue's passes in [firstPosition, endPosition), split by recording cost while chunks remain (each unit needs one).
        const uint32_t unitsLeft = static_cast<uint32_t>(m_AsyncBlocks.size()) * 3 + 1;
        bool open = false;
        uint32_t chunkPasses = 0;
        for (uint32_t position = firstPosition; position < endPosition; ++position)
        {
            Pass& pass = m_Passes[m_ExecutedScratch[position]];
            if (pass.Queue != queue)
                continue;
            const float passMs = GetEstimatedRecordMs(pass);
            const bool split = open && chunkPasses > 0 && m_ChunkRecordMs.back() + passMs * 0.5f > targetMs &&
                               m_Chunks.size() + unitsLeft < MaxRecordedChunks;
            if (!open || split)
            {
                Chunk chunk;
                chunk.FirstEvent = m_PassEvent[m_ExecutedScratch[position]];
                chunk.Queue = queue;
                // The groups open at its first event, outermost first.
                std::vector<uint32_t>& stack = m_PlanStackScratch;
                stack.clear();
                for (uint32_t eventIndex = 0; eventIndex < chunk.FirstEvent; ++eventIndex)
                {
                    if (m_Events[eventIndex].Type == EventType::GroupBegin)
                        stack.push_back(m_Events[eventIndex].Index);
                    else if (m_Events[eventIndex].Type == EventType::GroupEnd)
                        stack.pop_back();
                }
                chunk.OpenGroupsOffset = static_cast<uint32_t>(m_ChunkOpenGroups.size());
                chunk.OpenGroupsCount = static_cast<uint32_t>(stack.size());
                m_ChunkOpenGroups.insert(m_ChunkOpenGroups.end(), stack.begin(), stack.end());
                m_Chunks.push_back(chunk);
                m_ChunkExclusiveMasks.push_back(0);
                m_ChunkRecordMs.push_back(0.0f);
                open = true;
                chunkPasses = 0;
            }
            pass.Chunk = static_cast<uint32_t>(m_Chunks.size() - 1);
            m_Chunks.back().EndEvent = m_PassEvent[m_ExecutedScratch[position]] + 1;
            m_ChunkExclusiveMasks.back() |= pass.ExclusiveMask;
            m_ChunkRecordMs.back() += passMs;
            ++chunkPasses;
        }
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

        // Command buffers are created on first use, again when the chunk moved to the other queue (the slot's previous
        // submission finished, so its old buffer is free); a chunk's pool is reset by its recording task.
        for (uint32_t chunkIndex = 0; chunkIndex < m_Chunks.size(); ++chunkIndex)
        {
            CommandRecorder& recorder = m_Recorders[static_cast<size_t>(chunkIndex) * m_FramesInFlight + frameSlot];
            if (!recorder.Allocator || recorder.Queue != m_Chunks[chunkIndex].Queue)
            {
                recorder.CommandBuffer.reset();
                recorder.Allocator = m_Device->createCommandAllocator(NRI::CommandBufferReset::WithAllocator, m_Chunks[chunkIndex].Queue);
                recorder.CommandBuffer = recorder.Allocator->allocateCommandBuffer(1);
                recorder.Queue = m_Chunks[chunkIndex].Queue;
            }
            m_GpuScopeContexts[chunkIndex].SetAsyncCompute(m_Chunks[chunkIndex].Queue == NRI::QueueType::Compute);
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
            m_CommandBufferSetup(cmd, chunk.Queue);

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
                // (With async compute a chunk's event range also holds passes of the other queue's chunks.)
                if (pass.Culled || pass.Chunk != chunkIndex)
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
