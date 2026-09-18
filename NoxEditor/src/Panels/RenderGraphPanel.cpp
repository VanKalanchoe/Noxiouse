#include "RenderGraphPanel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>

#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/RenderGraph/RGFrameReport.h"
#include "NoxCore/Renderer/Renderer.h"

namespace Nox
{
    namespace
    {
        const char* FormatName(NRI::ImageFormat format)
        {
            switch (format)
            {
            case NRI::ImageFormat::None: return "-";
            case NRI::ImageFormat::Surface: return "Surface";
            case NRI::ImageFormat::RGBA8: return "RGBA8";
            case NRI::ImageFormat::SRGBA8: return "SRGBA8";
            case NRI::ImageFormat::R16_SFLOAT: return "R16F";
            case NRI::ImageFormat::R10G10B10A2_UNORM: return "RGB10A2";
            case NRI::ImageFormat::R16G16: return "RG16";
            case NRI::ImageFormat::R32SINT: return "R32I";
            case NRI::ImageFormat::R32G32_UINT: return "RG32U";
            case NRI::ImageFormat::R32G32_SFLOAT: return "RG32F";
            case NRI::ImageFormat::R16G16_SFLOAT: return "RG16F";
            case NRI::ImageFormat::R16G16B16A16_SFLOAT: return "RGBA16F";
            case NRI::ImageFormat::R32G32B32A32_SFLOAT: return "RGBA32F";
            default: return "other";
            }
        }

        const char* KindName(RGResourceKind kind)
        {
            switch (kind)
            {
            case RGResourceKind::Imported: return "Imported";
            case RGResourceKind::Transient: return "Transient";
            case RGResourceKind::History: return "History";
            }
            return "";
        }

        // Per second, short: 12.34M, 1.25B.
        std::string FormatRate(double perSecond)
        {
            if (perSecond >= 1e12)
                return std::format("{:.2f}T", perSecond / 1e12);
            if (perSecond >= 1e9)
                return std::format("{:.2f}B", perSecond / 1e9);
            if (perSecond >= 1e6)
                return std::format("{:.2f}M", perSecond / 1e6);
            if (perSecond >= 1e3)
                return std::format("{:.2f}K", perSecond / 1e3);
            return std::format("{:.0f}", perSecond);
        }

        std::string FormatBytes(uint64_t bytes)
        {
            if (bytes >= 1024 * 1024)
                return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
            if (bytes >= 1024)
                return std::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
            return std::format("{} B", bytes);
        }

        // Average GPU time per scope name over the stats window (pass and group scopes share the render graph's names).
        std::unordered_map<std::string_view, double> CollectGpuTimes()
        {
            std::unordered_map<std::string_view, double> times;
#if NOX_PROFILE_STATS
            std::vector<ProfileScopeStats> scopes;
            Profiler::Get().GetGpuScopes(scopes);
            for (const ProfileScopeStats& scope : scopes)
                times[scope.Name] = scope.Timing.Avg;
#endif
            return times;
        }

        constexpr ImVec4 CulledColor{ 0.5f, 0.5f, 0.5f, 1.0f };
        constexpr ImVec4 GroupColor{ 0.55f, 0.75f, 1.0f, 1.0f };
        constexpr ImVec4 WarningColor{ 1.0f, 0.7f, 0.3f, 1.0f };
    }

    void RenderGraphPanel::OnImGuiRender()
    {
        RenderGraph& graph = m_Renderer->getRenderGraph();
        graph.SetReportEnabled(m_Open);
        if (!m_Open)
        {
            StopInspection();
            return;
        }

        if (!ImGui::Begin("Render Graph", &m_Open))
        {
            ImGui::End();
            return;
        }

        const RGFrameReport& report = graph.GetReport();
        DrawHeader(report);

        if (ImGui::BeginTabBar("##RenderGraphTabs"))
        {
            if (ImGui::BeginTabItem("Passes"))
            {
                DrawPasses(report);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Resources"))
            {
                DrawResources(report);
                ImGui::EndTabItem();
            }
            const std::string validationTab = std::format("Validation ({})###Validation", report.ValidationMessages.size());
            if (ImGui::BeginTabItem(validationTab.c_str()))
            {
                DrawValidation(report);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Inspector"))
            {
                DrawInspector();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    void RenderGraphPanel::DrawHeader(const RGFrameReport& report)
    {
        RenderGraph& graph = m_Renderer->getRenderGraph();

        bool validation = graph.IsValidationEnabled();
        if (ImGui::Checkbox("Validation", &validation))
            graph.SetValidationEnabled(validation);

        ImGui::SameLine();
        bool parallel = m_Renderer->isParallelCommandRecording();
        if (ImGui::Checkbox("Parallel Recording", &parallel))
            m_Renderer->setParallelCommandRecording(parallel);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        int synchronization = graph.GetSynchronization() == RGSynchronization::Precise ? 1 : 0;
        if (ImGui::Combo("Sync", &synchronization, "Blanket\0Precise\0"))
            graph.SetSynchronization(synchronization == 1 ? RGSynchronization::Precise : RGSynchronization::Blanket);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Blanket: one memory barrier after a pass whose writes a later pass uses.\nPrecise: per-resource barriers before the consuming pass.");

        ImGui::SameLine();
        if (ImGui::Button("Export .dot"))
            WriteRenderGraphDot(report, "RenderGraph.dot");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Writes RenderGraph.dot next to the editor (GraphViz)");

        // GPU culling (§5.6): instance frustum culling, optional meshlet culling, frozen culling view (also key F).
        bool instanceCulling = m_Renderer->isInstanceCullingEnabled();
        if (ImGui::Checkbox("Instance Culling", &instanceCulling))
            m_Renderer->setInstanceCullingEnabled(instanceCulling);

        ImGui::SameLine();
        bool occlusionCulling = m_Renderer->isOcclusionCullingEnabled();
        if (ImGui::Checkbox("Occlusion Culling", &occlusionCulling))
            m_Renderer->setOcclusionCullingEnabled(occlusionCulling);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hi-Z: instances hidden behind closer geometry, re-tested in phase 2 (off while the culling view is frozen)");

        ImGui::SameLine();
        bool meshletFrustum = (m_Renderer->getMeshletCulling() & shaderio::MESHLET_CULL_FRUSTUM) != 0;
        if (ImGui::Checkbox("Meshlet Frustum", &meshletFrustum))
            m_Renderer->setMeshletCulling(meshletFrustum ? shaderio::MESHLET_CULL_FRUSTUM : 0u);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Also tests each meshlet of a visible instance against the frustum (the task shader fetches meshlet bounds)");

        ImGui::SameLine();
        bool frozen = m_Renderer->getFrozen();
        if (ImGui::Checkbox("Freeze Culling View", &frozen))
            m_Renderer->setFrozen(frozen);

        ImGui::SameLine();
        ImGui::Text("| visible %u (+%u of %u re-tested) / %u", m_Renderer->getVisibleInstanceCount(), m_Renderer->getLateDrawnCount(),
                    m_Renderer->getLateCandidateCount(), m_Renderer->getDrawListSize());

        // Rates of the newest frame, from what the culling pass submitted: throughput, not a score -- culling lowers
        // them by design, because the same image is drawn with fewer draws and triangles.
        const double frameMs = Profiler::Get().GetFrameTime().Avg;
        if (frameMs > 0.0)
        {
            const double draws = static_cast<double>(m_Renderer->getVisibleInstanceCount() + m_Renderer->getLateDrawnCount());
            std::string rates = std::format("| {} draws/s", FormatRate(draws * 1000.0 / frameMs));

            rates += std::format(" | {} tris/s", FormatRate(static_cast<double>(m_Renderer->getVisibleTriangleCount()) * 1000.0 / frameMs));

            ImGui::SameLine();
            ImGui::TextUnformatted(rates.c_str());
        }

        // Cluster LOD (§5.7): the DAG cut the task shader draws, and what the visibility passes drew with it against the
        // original triangles of the visible instances.
        float lodErrorPixels = m_Renderer->getLodErrorPixels();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat("LOD Error (px)", &lodErrorPixels, 0.0f, 8.0f, "%.2f"))
            m_Renderer->setLodErrorPixels(lodErrorPixels);
        ImGui::SameLine();
        bool lodFullDetail = m_Renderer->getLodFullDetail();
        if (ImGui::Checkbox("Full Detail", &lodFullDetail))
            m_Renderer->setLodFullDetail(lodFullDetail);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draws only the original clusters (no LOD), for comparison");
        ImGui::SameLine();
        ImGui::Text("| drawn %u clusters, %u of %u triangles", m_Renderer->getDrawnClusterCount(), m_Renderer->getDrawnTriangleCount(),
                    m_Renderer->getVisibleTriangleCount());

        // Vulkan pipeline statistics per raster pass (Triangles column, stats report). Costs GPU time: off for timings.
        Profiler& profiler = Profiler::Get();
        ImGui::BeginDisabled(!profiler.IsPipelineStatisticsSupported());
        bool statistics = profiler.IsPipelineStatisticsEnabled();
        if (ImGui::Checkbox("Pipeline Statistics", &statistics))
            profiler.SetPipelineStatisticsEnabled(statistics);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(profiler.IsPipelineStatisticsSupported()
                                  ? "Fragments and task/mesh shader invocations per raster pass (queries cost GPU time)"
                                  : "Needs pipeline statistics and mesh shader queries (and a profiling build)");

        uint32_t executed = 0;
        for (const RGFrameReport::Pass& pass : report.Passes)
            executed += pass.Culled ? 0 : 1;

        ImGui::Text("Frame %llu | %u of %zu passes executed | %u command buffer(s) | pool: textures %s, buffers %s",
                    static_cast<unsigned long long>(report.FrameNumber), executed, report.Passes.size(), report.CommandBuffers,
                    FormatBytes(report.PoolTextureBytes).c_str(), FormatBytes(report.PoolBufferBytes).c_str());

        if (report.ResetAllHistory || !report.ResetHistoryKeys.empty())
        {
            std::string resets = report.ResetAllHistory ? "all" : "";
            for (const std::string& key : report.ResetHistoryKeys)
                resets += (resets.empty() ? "" : ", ") + key;
            ImGui::TextColored(WarningColor, "History reset this frame: %s", resets.c_str());
        }
    }

    void RenderGraphPanel::DrawPasses(const RGFrameReport& report)
    {
        const std::unordered_map<std::string_view, double> gpuTimes = CollectGpuTimes();
        std::vector<ProfilePipelineStatistics> statistics;
        Profiler::Get().GetPipelineStatistics(statistics);
        auto meshInvocations = [&statistics](const char* name) -> std::string
        {
            for (const ProfilePipelineStatistics& pass : statistics)
            {
                if (std::string_view(pass.Name) == name)
                    return std::format("{}", pass.MeshInvocations);
            }
            return std::string("-");
        };
        auto gpuTime = [&gpuTimes](const char* name) -> std::string
        {
            const auto found = gpuTimes.find(name);
            return found != gpuTimes.end() ? std::format("{:.2f}", found->second) : std::string("-");
        };

        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        const float tableHeight = m_SelectedPass < report.Passes.size() ? ImGui::GetContentRegionAvail().y * 0.65f : 0.0f;
        if (ImGui::BeginTable("##Passes", 6, tableFlags, ImVec2(0.0f, tableHeight)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Command Buffer", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("CPU ms", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Mesh inv.", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const RGFrameReport::Row& row : report.Rows)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Indent(static_cast<float>(row.Depth) * 14.0f + 1.0f);

                if (row.Pass == RGInvalidIndex)
                {
                    ImGui::TextColored(GroupColor, "%s", row.Name);
                    ImGui::Unindent(static_cast<float>(row.Depth) * 14.0f + 1.0f);
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(gpuTime(row.Name).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    continue;
                }

                const RGFrameReport::Pass& pass = report.Passes[row.Pass];
                if (pass.Culled)
                    ImGui::PushStyleColor(ImGuiCol_Text, CulledColor);
                ImGui::PushID(static_cast<int>(row.Pass));
                if (ImGui::Selectable(pass.Name, m_SelectedPass == row.Pass, ImGuiSelectableFlags_SpanAllColumns))
                    m_SelectedPass = m_SelectedPass == row.Pass ? ~0u : row.Pass;
                ImGui::PopID();
                ImGui::Unindent(static_cast<float>(row.Depth) * 14.0f + 1.0f);

                ImGui::TableNextColumn();
                if (pass.Culled)
                    ImGui::TextUnformatted("culled");
                else
                    ImGui::Text("%u", pass.Chunk);
                ImGui::TableNextColumn();
                if (!pass.Culled)
                    ImGui::Text("%.3f", pass.RecordMs);
                ImGui::TableNextColumn();
                if (!pass.Culled)
                    ImGui::TextUnformatted(gpuTime(pass.Name).c_str());
                ImGui::TableNextColumn();
                if (!pass.Culled && pass.Raster)
                    ImGui::TextUnformatted(meshInvocations(pass.Name).c_str());
                ImGui::TableNextColumn();

                std::string flags;
                if (pass.Raster)
                    flags += "Raster ";
                if (pass.Presents)
                    flags += "Present ";
                if (pass.SynchronizeAfter)
                    flags += "Sync ";
                if (pass.Barriers > 0)
                    flags += std::format("{} barrier(s) ", pass.Barriers);
                for (uint32_t key = 0; key < report.ExclusiveKeys.size(); ++key)
                {
                    if ((pass.ExclusiveMask & (1u << key)) != 0)
                        flags += std::format("[{}] ", report.ExclusiveKeys[key]);
                }
                ImGui::TextUnformatted(flags.c_str());

                if (pass.Culled)
                    ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }

        if (m_SelectedPass >= report.Passes.size())
            return;

        const RGFrameReport::Pass& pass = report.Passes[m_SelectedPass];
        ImGui::SeparatorText(pass.Name);
        if (ImGui::BeginChild("##PassAccesses"))
        {
            for (uint32_t index = pass.FirstAccess; index < pass.FirstAccess + pass.AccessCount; ++index)
            {
                const RGFrameReport::Access& access = report.Accesses[index];
                const std::vector<RGFrameReport::Resource>& resources = access.IsTexture ? report.Textures : report.Buffers;
                if (access.Resource >= resources.size())
                    continue;
                ImGui::Text("%s  %s  %s", access.IsWrite ? "Write" : "Read ", access.IsTexture ? "texture" : "buffer ", resources[access.Resource].Name);
            }
        }
        ImGui::EndChild();
    }

    void RenderGraphPanel::DrawResources(const RGFrameReport& report)
    {
        ImGui::Checkbox("Show resources unused this frame", &m_ShowUnusedResources);

        const float passCount = static_cast<float>(std::max<size_t>(report.Passes.size(), 1));
        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if (!ImGui::BeginTable("##Resources", 8, tableFlags))
            return;

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Resource", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Lifetime", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("GPU Object", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();

        const Renderer::TextureInspection& inspection = m_Renderer->getTextureInspection();
        auto drawRows = [&](const std::vector<RGFrameReport::Resource>& resources, bool textures)
        {
            // Occurrence per name, in graph order: history slots share their key as name.
            std::unordered_map<std::string_view, uint32_t> occurrences;
            for (uint32_t index = 0; index < resources.size(); ++index)
            {
                const RGFrameReport::Resource& resource = resources[index];
                const uint32_t occurrence = occurrences[resource.Name]++;
                const bool used = resource.FirstPass != RGInvalidIndex;
                if (!used && !m_ShowUnusedResources)
                    continue;

                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(index) * 2 + (textures ? 0 : 1));
                if (!used)
                    ImGui::PushStyleColor(ImGuiCol_Text, CulledColor);

                ImGui::TableNextColumn();
                if (occurrence > 0 || (resource.Kind == RGResourceKind::History))
                    ImGui::Text("%s [%u]", resource.Name, occurrence);
                else
                    ImGui::TextUnformatted(resource.Name);

                ImGui::TableNextColumn();
                ImGui::Text("%s%s", KindName(resource.Kind), resource.ReadOnly ? " (RO)" : "");

                ImGui::TableNextColumn();
                if (textures)
                {
                    if (resource.Texture.MipLevels > 1)
                        ImGui::Text("%ux%u (%u mips)", resource.Texture.Width, resource.Texture.Height, resource.Texture.MipLevels);
                    else
                        ImGui::Text("%ux%u", resource.Texture.Width, resource.Texture.Height);
                }
                else
                {
                    ImGui::TextUnformatted(resource.Kind == RGResourceKind::Imported ? "-" : FormatBytes(resource.Buffer.Size).c_str());
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(textures ? FormatName(resource.Texture.Format) : "buffer");

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(resource.Bytes > 0 ? FormatBytes(resource.Bytes).c_str() : "-");

                // First to last executed pass over the whole pass list.
                ImGui::TableNextColumn();
                if (used)
                {
                    const ImVec2 cursor = ImGui::GetCursorScreenPos();
                    const float width = ImGui::GetContentRegionAvail().x;
                    const float height = ImGui::GetTextLineHeight();
                    const float begin = cursor.x + width * (static_cast<float>(resource.FirstPass) / passCount);
                    const float end = cursor.x + width * (static_cast<float>(resource.LastPass + 1) / passCount);
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->AddRectFilled(ImVec2(cursor.x, cursor.y + 2.0f), ImVec2(cursor.x + width, cursor.y + height - 2.0f), IM_COL32(60, 60, 60, 120));
                    drawList->AddRectFilled(ImVec2(begin, cursor.y + 2.0f), ImVec2(std::max(end, begin + 2.0f), cursor.y + height - 2.0f),
                                            resource.Kind == RGResourceKind::Transient ? IM_COL32(110, 190, 90, 255) : IM_COL32(220, 150, 70, 255));
                    ImGui::Dummy(ImVec2(width, height));
                    if (ImGui::IsItemHovered() && resource.FirstPass < report.Passes.size() && resource.LastPass < report.Passes.size())
                        ImGui::SetTooltip("%s -> %s", report.Passes[resource.FirstPass].Name, report.Passes[resource.LastPass].Name);
                }

                ImGui::TableNextColumn();
                if (resource.PhysicalId != RGInvalidIndex)
                    ImGui::Text("#%u", resource.PhysicalId);

                ImGui::TableNextColumn();
                if (textures)
                {
                    const bool inspected = inspection.Name == resource.Name && inspection.Occurrence == occurrence;
                    if (ImGui::SmallButton(inspected ? "Inspecting" : "Inspect"))
                        Inspect(resource.Name, occurrence);
                }

                if (!used)
                    ImGui::PopStyleColor();
                ImGui::PopID();
            }
        };

        drawRows(report.Textures, true);
        drawRows(report.Buffers, false);
        ImGui::EndTable();
    }

    void RenderGraphPanel::DrawValidation(const RGFrameReport& report)
    {
        if (!m_Renderer->getRenderGraph().IsValidationEnabled())
            ImGui::TextColored(WarningColor, "Validation is off.");
        else if (report.ValidationMessages.empty())
            ImGui::TextUnformatted("No issues this frame.");

        for (const std::string& message : report.ValidationMessages)
            ImGui::TextColored(WarningColor, "%s", message.c_str());

        if (report.UnusedWrites.empty())
            return;

        ImGui::SeparatorText("Writes no later pass reads (informational)");
        for (const RGFrameReport::UnusedWrite& unused : report.UnusedWrites)
        {
            const std::vector<RGFrameReport::Resource>& resources = unused.IsTexture ? report.Textures : report.Buffers;
            if (unused.Pass < report.Passes.size() && unused.Resource < resources.size())
                ImGui::Text("%s -> %s", report.Passes[unused.Pass].Name, resources[unused.Resource].Name);
        }
    }

    void RenderGraphPanel::DrawInspector()
    {
        Renderer::TextureInspection inspection = m_Renderer->getTextureInspection();
        if (inspection.Name.empty())
        {
            ImGui::TextUnformatted("Pick a texture with Inspect in the Resources tab.");
            return;
        }

        const RGTextureKey& key = m_Renderer->getInspectedTextureKey();
        ImGui::Text("%s [%u]  %ux%u %s", inspection.Name.c_str(), inspection.Occurrence, key.Width, key.Height, FormatName(key.Format));
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop"))
        {
            StopInspection();
            return;
        }

        if (key.MipLevels > 1)
        {
            int mip = static_cast<int>(inspection.Mip);
            if (ImGui::SliderInt("Mip", &mip, 0, static_cast<int>(key.MipLevels) - 1))
                inspection.Mip = static_cast<uint32_t>(mip);
        }

        const bool floatFormat = key.Format != NRI::ImageFormat::R32SINT && key.Format != NRI::ImageFormat::R32G32_UINT &&
                                 key.Usage != NRI::TextureUsage::DepthStencilAttachment;
        if (floatFormat)
        {
            ImGui::Checkbox("Depth curve", &inspection.DepthCurve);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reverse-Z curve: depth kept in a color format (the Hi-Z pyramid) reads as black otherwise");

            if (ImGui::SliderFloat("Exposure (stops)", &m_ExposureStops, -10.0f, 10.0f, "%.1f"))
                inspection.Exposure = std::pow(2.0f, m_ExposureStops);

            constexpr const char* channelNames[] = { "R", "G", "B", "A" };
            for (uint32_t channel = 0; channel < 4; ++channel)
            {
                bool shown = (inspection.ChannelMask & (1u << channel)) != 0;
                if (channel > 0)
                    ImGui::SameLine();
                if (ImGui::Checkbox(channelNames[channel], &shown))
                    inspection.ChannelMask = shown ? inspection.ChannelMask | (1u << channel) : inspection.ChannelMask & ~(1u << channel);
            }
        }

        inspection.ProbeX = -1;
        inspection.ProbeY = -1;
        if (Texture2D* image = m_Renderer->getInspectionImage())
        {
            const uint32_t mipWidth = std::max(key.Width >> inspection.Mip, 1u);
            const uint32_t mipHeight = std::max(key.Height >> inspection.Mip, 1u);
            const float availableWidth = ImGui::GetContentRegionAvail().x;
            const float scale = availableWidth / static_cast<float>(mipWidth);
            const ImVec2 imageSize(availableWidth, static_cast<float>(mipHeight) * scale);

            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Image(image->getImTextureID(), imageSize);
            if (ImGui::IsItemHovered())
            {
                const ImVec2 mouse = ImGui::GetMousePos();
                inspection.ProbeX = std::clamp(static_cast<int32_t>((mouse.x - origin.x) / scale), 0, static_cast<int32_t>(mipWidth) - 1);
                inspection.ProbeY = std::clamp(static_cast<int32_t>((mouse.y - origin.y) / scale), 0, static_cast<int32_t>(mipHeight) - 1);

                glm::vec4 value;
                if (m_Renderer->getInspectionProbe(value))
                    ImGui::SetTooltip("(%d, %d)\n%.5g  %.5g  %.5g  %.5g", inspection.ProbeX, inspection.ProbeY, value.r, value.g, value.b, value.a);
            }
        }
        else
        {
            ImGui::TextColored(WarningColor, "Not available this frame (culled or not produced by the current pipeline).");
        }

        m_Renderer->setTextureInspection(inspection);
    }

    void RenderGraphPanel::Inspect(const char* name, uint32_t occurrence)
    {
        Renderer::TextureInspection inspection = m_Renderer->getTextureInspection();
        inspection.Name = name;
        inspection.Occurrence = occurrence;
        inspection.Mip = 0;
        m_Renderer->setTextureInspection(inspection);
    }

    void RenderGraphPanel::StopInspection()
    {
        if (!m_Renderer->getTextureInspection().Name.empty())
            m_Renderer->setTextureInspection({});
    }
}
