// Render graph tooling (§5.4.7): validation, texture inspection placement and the frame report.
#include "RenderGraph.h"

#include <algorithm>
#include <format>

#include "NoxCore/Core/Log.h"

namespace Nox
{
    // ---------------------------------------------------------------------------------------------------------------
    // Validation

    void RenderGraph::ReportValidation(std::string message)
    {
        std::scoped_lock lock(m_ValidationMutex);
        if (std::find(m_ValidationMessages.begin(), m_ValidationMessages.end(), message) == m_ValidationMessages.end())
            m_ValidationMessages.push_back(message);
        if (m_LoggedValidationMessages.insert(message).second)
            NOX_CORE_WARN("[RenderGraph] {}", message);
    }

    void RenderGraph::ValidateHandle(uint32_t handleFrame, const char* passName)
    {
        if (handleFrame != m_FrameId)
            ReportValidation(std::format("'{}' uses a resource handle from an earlier frame", passName));
    }

    void RenderGraph::ValidateContextAccess(uint32_t passIndex, uint32_t resource, bool isTexture)
    {
        // Graph tooling (inspection) may use any resource.
        if (passIndex == RGInvalidIndex)
            return;

        const Pass& pass = m_Passes[passIndex];
        for (const ResourceAccess& access : pass.Accesses)
        {
            if (access.Resource == resource && access.IsTexture == isTexture)
                return;
        }

        const size_t count = isTexture ? m_Textures.size() : m_Buffers.size();
        const char* name = resource < count ? (isTexture ? m_Textures[resource].Name : m_Buffers[resource].Name) : "<invalid>";
        ReportValidation(std::format("Pass '{}' uses {} '{}' without declaring it", pass.Name, isTexture ? "texture" : "buffer", name));
    }

    void RenderGraph::ValidateCompiledFrame()
    {
        const size_t textureCount = m_Textures.size();
        auto resourceKey = [textureCount](const ResourceAccess& access)
        {
            return access.IsTexture ? access.Resource : static_cast<uint32_t>(textureCount) + access.Resource;
        };
        auto resourceOf = [this](const ResourceAccess& access, const char*& outName, RGResourceKind& outKind, bool& outReadOnly)
        {
            if (access.IsTexture)
            {
                const TextureEntry& entry = m_Textures[access.Resource];
                outName = entry.Name;
                outKind = entry.Kind;
                outReadOnly = entry.ReadOnly;
            }
            else
            {
                const BufferEntry& entry = m_Buffers[access.Resource];
                outName = entry.Name;
                outKind = entry.Kind;
                outReadOnly = entry.ReadOnly;
            }
        };

        std::vector<uint32_t> lastRead(textureCount + m_Buffers.size(), RGInvalidIndex);
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            if (m_Passes[index].Culled)
                continue;
            for (const ResourceAccess& access : m_Passes[index].Accesses)
            {
                if (!access.IsWrite)
                    lastRead[resourceKey(access)] = index;
            }
        }

        std::vector<bool> written(textureCount + m_Buffers.size(), false);
        bool presents = false;
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            const Pass& pass = m_Passes[index];
            if (pass.Culled)
                continue;
            presents = presents || pass.Swapchain != nullptr;

            const char* name = nullptr;
            RGResourceKind kind = RGResourceKind::Imported;
            bool readOnly = false;

            // Reads first: a pass that loads and writes a transient nobody wrote before reads undefined contents.
            for (const ResourceAccess& access : pass.Accesses)
            {
                resourceOf(access, name, kind, readOnly);
                if (!access.IsWrite && kind == RGResourceKind::Transient && !written[resourceKey(access)])
                    ReportValidation(std::format("Pass '{}' reads '{}' before any pass wrote it this frame", pass.Name, name));
            }

            for (const ResourceAccess& access : pass.Accesses)
            {
                if (!access.IsWrite)
                    continue;

                resourceOf(access, name, kind, readOnly);
                const uint32_t key = resourceKey(access);
                if (readOnly)
                    ReportValidation(std::format("Pass '{}' writes read-only import '{}'", pass.Name, name));

                if (kind == RGResourceKind::Transient && (lastRead[key] == RGInvalidIndex || lastRead[key] <= index))
                {
                    const RGFrameReport::UnusedWrite unused{ index, access.Resource, access.IsTexture };
                    const bool listed = std::any_of(m_UnusedWrites.begin(), m_UnusedWrites.end(), [&unused](const RGFrameReport::UnusedWrite& other)
                    {
                        return other.Pass == unused.Pass && other.Resource == unused.Resource && other.IsTexture == unused.IsTexture;
                    });
                    if (!listed)
                        m_UnusedWrites.push_back(unused);
                }
                written[key] = true;
            }
        }

        if (!presents)
            ReportValidation("No executed pass presents to the swapchain");
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Texture inspection

    RGTexture RenderGraph::FindTexture(std::string_view name, uint32_t occurrence) const
    {
        uint32_t seen = 0;
        for (uint32_t index = 0; index < m_Textures.size(); ++index)
        {
            if (m_Textures[index].Name && name == m_Textures[index].Name && seen++ == occurrence)
                return { index, m_FrameId };
        }
        return {};
    }

    const RGTextureKey& RenderGraph::GetTextureKey(RGTexture texture) const
    {
        NOX_CORE_ASSERT(texture.IsValid() && texture.Index < m_Textures.size(), "RenderGraph::GetTextureKey: invalid texture");
        return m_Textures[texture.Index].Key;
    }

    void RenderGraph::InspectTexture(RGTexture texture, std::function<void(RGPassContext& context)> inspector)
    {
        NOX_CORE_ASSERT(texture.IsValid() && texture.Index < m_Textures.size(), "RenderGraph::InspectTexture: invalid texture");
        m_InspectedTexture = texture.Index;
        m_Inspector = std::move(inspector);
    }

    void RenderGraph::PlaceInspection()
    {
        m_InspectionActive = false;
        m_InspectionPass = RGInvalidIndex;
        if (m_InspectedTexture == RGInvalidIndex || !m_Inspector)
            return;

        uint32_t lastWriter = RGInvalidIndex;
        uint32_t lastAccess = RGInvalidIndex;
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            if (m_Passes[index].Culled)
                continue;
            for (const ResourceAccess& access : m_Passes[index].Accesses)
            {
                if (!access.IsTexture || access.Resource != m_InspectedTexture)
                    continue;
                lastAccess = index;
                if (access.IsWrite)
                    lastWriter = index;
            }
        }

        // A transient no executed pass uses has no memory this frame.
        if (m_Textures[m_InspectedTexture].Kind == RGResourceKind::Transient && lastAccess == RGInvalidIndex)
            return;

        m_InspectionPass = lastWriter != RGInvalidIndex ? lastWriter : lastAccess;
        m_InspectionActive = true;
    }

    void RenderGraph::RecordInspection(NRI::CommandBuffer& cmd)
    {
        // The inspected contents must be visible to the inspector, and its output to the editor's ImGui pass.
        cmd.executionBarrier();
        cmd.beginDebugLabel("Texture Inspection");
        RGPassContext context(*this, cmd, {}, RGInvalidIndex);
        m_Inspector(context);
        cmd.endDebugLabel();
        cmd.executionBarrier();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Report

    void RenderGraph::BuildReport()
    {
        RGFrameReport& report = m_Report;
        report.FrameNumber = m_Frame.FrameNumber;
        report.CommandBuffers = static_cast<uint32_t>(m_Chunks.size());
        report.Synchronization = m_Synchronization;
        report.PoolTextureBytes = m_Pool.GetTextureMemory();
        report.PoolBufferBytes = m_Pool.GetBufferMemory();

        report.Rows.clear();
        uint32_t depth = 0;
        for (const Event& event : m_Events)
        {
            switch (event.Type)
            {
            case EventType::GroupBegin:
                report.Rows.push_back({ m_GroupNames[event.Index], depth, RGInvalidIndex });
                ++depth;
                break;
            case EventType::GroupEnd:
                --depth;
                break;
            case EventType::Pass:
                report.Rows.push_back({ m_Passes[event.Index].Name, depth, event.Index });
                break;
            }
        }

        report.Passes.clear();
        report.Accesses.clear();
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            const Pass& pass = m_Passes[index];
            RGFrameReport::Pass& entry = report.Passes.emplace_back();
            entry.Name = pass.Name;
            entry.Culled = pass.Culled;
            entry.Raster = HasFlag(pass.Flags, RGPassFlags::Raster);
            entry.Presents = pass.Swapchain != nullptr;
            entry.SynchronizeAfter = pass.SynchronizeAfter;
            entry.Barriers = pass.TextureBarrierCount + pass.BufferBarrierCount;
            entry.Chunk = pass.Chunk;
            entry.RecordMs = pass.Culled ? 0.0f : pass.RecordMs;
            entry.ExclusiveMask = pass.ExclusiveMask;
            entry.FirstAccess = static_cast<uint32_t>(report.Accesses.size());
            entry.AccessCount = static_cast<uint32_t>(pass.Accesses.size());
            for (const ResourceAccess& access : pass.Accesses)
                report.Accesses.push_back({ access.Resource, access.IsTexture, access.IsWrite });
        }

        // Same GPU object, same id: aliased transients and repeated imports share one.
        std::unordered_map<const void*, uint32_t> physicalIds;
        auto physicalId = [&physicalIds](const void* object)
        {
            if (!object)
                return RGInvalidIndex;
            return physicalIds.try_emplace(object, static_cast<uint32_t>(physicalIds.size())).first->second;
        };

        report.Textures.clear();
        for (const TextureEntry& texture : m_Textures)
        {
            RGFrameReport::Resource& entry = report.Textures.emplace_back();
            entry.Name = texture.Name;
            entry.Kind = texture.Kind;
            entry.ReadOnly = texture.ReadOnly;
            entry.Texture = texture.Key;
            entry.FirstPass = texture.FirstPass;
            entry.LastPass = texture.LastPass;
            entry.PhysicalId = physicalId(texture.Texture);
            entry.Bytes = texture.Kind != RGResourceKind::Imported && texture.Texture ? RGResourcePool::EstimateTextureBytes(texture.Key) : 0;
        }

        report.Buffers.clear();
        for (const BufferEntry& buffer : m_Buffers)
        {
            RGFrameReport::Resource& entry = report.Buffers.emplace_back();
            entry.Name = buffer.Name;
            entry.Kind = buffer.Kind;
            entry.ReadOnly = buffer.ReadOnly;
            entry.Buffer = buffer.Key;
            entry.FirstPass = buffer.FirstPass;
            entry.LastPass = buffer.LastPass;
            entry.PhysicalId = physicalId(buffer.Buffer);
            entry.Bytes = buffer.Kind != RGResourceKind::Imported && buffer.Buffer ? buffer.Key.Size : 0;
        }

        report.ExclusiveKeys = m_ExclusiveKeys;
        report.ResetHistoryKeys.assign(m_FrameHistoryResets.begin(), m_FrameHistoryResets.end());
        std::sort(report.ResetHistoryKeys.begin(), report.ResetHistoryKeys.end());
        report.ResetAllHistory = m_FrameResetAll;
        report.ValidationMessages = m_ValidationMessages;
        report.UnusedWrites = m_UnusedWrites;
    }
}
