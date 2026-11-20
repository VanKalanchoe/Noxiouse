#include "RenderGraphV2Panel.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>

#include "NoxCore/UI/Widgets/UIPropertyRows.h"
#include "Panels/TableViewV2.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/RenderGraph/RGFrameReport.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Layout/UITabPanel.h"
#include "NoxCore/UI/Widgets/UIImage.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"

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
            case NRI::ImageFormat::R32G32B32A32_UINT: return "RGBA32U";
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
        std::string FormatRate(double perSecond)
        {
            if (perSecond >= 1e12) return std::format("{:.2f}T", perSecond / 1e12);
            if (perSecond >= 1e9) return std::format("{:.2f}B", perSecond / 1e9);
            if (perSecond >= 1e6) return std::format("{:.2f}M", perSecond / 1e6);
            if (perSecond >= 1e3) return std::format("{:.2f}K", perSecond / 1e3);
            return std::format("{:.0f}", perSecond);
        }
        std::string FormatBytes(uint64_t bytes)
        {
            if (bytes >= 1024 * 1024) return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
            if (bytes >= 1024) return std::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
            return std::format("{} B", bytes);
        }
        constexpr glm::vec4 kCulled{ 0.5f, 0.5f, 0.5f, 1.0f };
        constexpr glm::vec4 kGroup{ 0.55f, 0.75f, 1.0f, 1.0f };
        constexpr glm::vec4 kWarning{ 1.0f, 0.7f, 0.3f, 1.0f };
    }

    void RenderGraphV2Panel::Update(bool shown)
    {
        if (!m_Renderer)
            return;
        m_Renderer->getRenderGraph().SetReportEnabled(shown);
        if (!shown)
        {
            if (m_WasShown && !m_Renderer->getTextureInspection().Name.empty())
                m_Renderer->setTextureInspection({}); // (closed: what it showed stops being copied)
            m_WasShown = false;
            return;
        }
        m_WasShown = true;
    }

    const Ref<UI::UIElement>& RenderGraphV2Panel::BuildUI(UI::UIContext* context)
    {
        auto root = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 6.0f);
        root->WidthMode = UI::UISizeMode::Fill;
        root->HeightMode = UI::UISizeMode::Fill;
        root->StyleClass = "Surface";
        root->DrawBackground = true;
        root->Padding = UI::UIThickness::All(8.0f);
        m_Root = root;
        Renderer* r = m_Renderer;
        if (!r)
            return m_Root;
        RenderGraph* graph = &r->getRenderGraph();

        // ---- Header: switches and the frame's counts ----
        {
            UI::UIPropertyRows rows(root, context);
            UI::UIPropertyRows header = rows.Section("Graph, Culling and LOD", true, nullptr);
            header.Check("Validation", [graph]() { return graph->IsValidationEnabled(); }, [graph](bool v) { graph->SetValidationEnabled(v); });
            header.Check("Parallel Recording", [r]() { return r->isParallelCommandRecording(); }, [r](bool v) { r->setParallelCommandRecording(v); });
            header.Choice("Synchronization", { "Blanket", "Precise" }, [graph]() { return graph->GetSynchronization() == RGSynchronization::Precise ? 1 : 0; },
                          [graph](int v) { graph->SetSynchronization(v == 1 ? RGSynchronization::Precise : RGSynchronization::Blanket); },
                          "Blanket: one memory barrier after a pass whose writes a later pass uses. Precise: per-resource barriers before the consuming pass.");
            header.Button("Export .dot", [graph]() { WriteRenderGraphDot(graph->GetReport(), "RenderGraph.dot"); }, "Writes RenderGraph.dot next to the editor (GraphViz)");
            header.Check("Instance Culling", [r]() { return r->isInstanceCullingEnabled(); }, [r](bool v) { r->setInstanceCullingEnabled(v); });
            header.Check("Occlusion Culling", [r]() { return r->isOcclusionCullingEnabled(); }, [r](bool v) { r->setOcclusionCullingEnabled(v); },
                         "Hi-Z: instances hidden behind closer geometry, re-tested in phase 2 (off while the culling view is frozen)");
            header.Check("Meshlet Frustum", [r]() { return (r->getMeshletCulling() & shaderio::MESHLET_CULL_FRUSTUM) != 0; },
                         [r](bool v) { r->setMeshletCulling(v ? shaderio::MESHLET_CULL_FRUSTUM : 0u); },
                         "Also tests each meshlet of a visible instance against the frustum");
            header.Check("Freeze Culling View", [r]() { return r->getFrozen(); }, [r](bool v) { r->setFrozen(v); });
            header.Slider("LOD Error (px)", [r]() { return r->getLodErrorPixels(); }, [r](float v) { r->setLodErrorPixels(v); }, 0.0f, 8.0f, "{:.2f}");
            header.Check("Full Detail", [r]() { return r->getLodFullDetail(); }, [r](bool v) { r->setLodFullDetail(v); }, "Draws only the original clusters (no LOD), for comparison");
            Ref<UI::UIElement> statistics = header.Check("Pipeline Statistics", []() { return Profiler::Get().IsPipelineStatisticsEnabled(); },
                [](bool v) { Profiler::Get().SetPipelineStatisticsEnabled(v); }, "Fragments and task / mesh shader invocations per raster pass (queries cost GPU time)");
            header.EnableWhen(statistics, []() { return Profiler::Get().IsPipelineStatisticsSupported(); },
                              []() { return std::string("needs pipeline statistics and mesh shader queries (and a profiling build)"); });
            rows.Text([r, graph]()
            {
                const RGFrameReport& report = graph->GetReport();
                uint32_t executed = 0;
                for (const RGFrameReport::Pass& pass : report.Passes)
                    executed += pass.Culled ? 0 : 1;
                std::string text = std::format("Frame {} | {} of {} passes executed | {} command buffer(s) | pool: textures {}, buffers {}\n"
                                               "Visible {} (+{} of {} re-tested) / {} | drawn {} clusters, {} of {} triangles",
                                               report.FrameNumber, executed, report.Passes.size(), report.CommandBuffers, FormatBytes(report.PoolTextureBytes),
                                               FormatBytes(report.PoolBufferBytes), r->getVisibleInstanceCount(), r->getLateDrawnCount(), r->getLateCandidateCount(),
                                               r->getDrawListSize(), r->getDrawnClusterCount(), r->getDrawnTriangleCount(), r->getVisibleTriangleCount());
                if (const double frameMs = Profiler::Get().GetFrameTime().Avg; frameMs > 0.0)
                {
                    const double draws = static_cast<double>(r->getVisibleInstanceCount() + r->getLateDrawnCount());
                    text += std::format(" | {} draws/s | {} tris/s", FormatRate(draws * 1000.0 / frameMs), FormatRate(static_cast<double>(r->getVisibleTriangleCount()) * 1000.0 / frameMs));
                }
                return text;
            }, { 0.80f, 0.83f, 0.89f, 1.0f });
            rows.Text([graph]()
            {
                const RGFrameReport& report = graph->GetReport();
                if (!report.ResetAllHistory && report.ResetHistoryKeys.empty())
                    return std::string();
                std::string resets = report.ResetAllHistory ? "all" : "";
                for (const std::string& key : report.ResetHistoryKeys)
                    resets += (resets.empty() ? "" : ", ") + key;
                return "History reset this frame: " + resets;
            }, kWarning);
        }

        Ref<UI::UITabPanel> tabs(new UI::UITabPanel());
        tabs->WidthMode = UI::UISizeMode::Fill;
        tabs->HeightMode = UI::UISizeMode::Fill;
        root->AddChild(tabs);

        // ---- Passes ----
        {
            auto page = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 4.0f);
            page->WidthMode = UI::UISizeMode::Fill;
            page->HeightMode = UI::UISizeMode::Fill;
            Ref<TableViewV2> table(new TableViewV2());
            table->Columns = { { "Pass", 3.0f }, { "Command Buffer", 1.0f }, { "CPU ms", 0.8f }, { "GPU ms", 0.8f }, { "Mesh inv.", 0.9f }, { "Flags", 2.5f } };
            table->Fill = [this, graph](std::vector<TableViewV2::Row>& out)
            {
                const RGFrameReport& report = graph->GetReport();
                std::unordered_map<std::string_view, double> gpuTimes;
#if NOX_PROFILE_STATS
                std::vector<ProfileScopeStats> scopes;
                Profiler::Get().GetGpuScopes(scopes);
                for (const ProfileScopeStats& scope : scopes)
                    gpuTimes[scope.Name] = scope.Timing.Avg;
#endif
                std::vector<ProfilePipelineStatistics> statistics;
                Profiler::Get().GetPipelineStatistics(statistics);
                const auto gpuTime = [&](const char* name) { const auto found = gpuTimes.find(name); return found != gpuTimes.end() ? std::format("{:.2f}", found->second) : std::string("-"); };
                const auto mesh = [&](const char* name)
                {
                    for (const ProfilePipelineStatistics& pass : statistics)
                        if (std::string_view(pass.Name) == name)
                            return std::format("{}", pass.MeshInvocations);
                    return std::string("-");
                };
                for (const RGFrameReport::Row& row : report.Rows)
                {
                    TableViewV2::Row line;
                    line.Indent = static_cast<float>(row.Depth) * 14.0f;
                    if (row.Pass == RGInvalidIndex)
                    {
                        line.Cells = { row.Name ? row.Name : "", "", "", gpuTime(row.Name), "", "" };
                        line.Color = kGroup;
                        line.Id = ~0ull;
                        out.push_back(std::move(line));
                        continue;
                    }
                    const RGFrameReport::Pass& pass = report.Passes[row.Pass];
                    std::string flags;
                    if (pass.Raster) flags += "Raster ";
                    if (pass.Presents) flags += "Present ";
                    if (pass.SynchronizeAfter) flags += "Sync ";
                    if (pass.Barriers > 0) flags += std::format("{} barrier(s) ", pass.Barriers);
                    for (uint32_t key = 0; key < report.ExclusiveKeys.size(); ++key)
                        if ((pass.ExclusiveMask & (1u << key)) != 0)
                            flags += std::format("[{}] ", report.ExclusiveKeys[key]);
                    line.Cells = { pass.Name, pass.Culled ? std::string("culled") : std::to_string(pass.Chunk), pass.Culled ? std::string() : std::format("{:.3f}", pass.RecordMs),
                                   pass.Culled ? std::string() : gpuTime(pass.Name), !pass.Culled && pass.Raster ? mesh(pass.Name) : std::string(), flags };
                    line.Color = pass.Culled ? kCulled : line.Color;
                    line.Id = row.Pass;
                    line.Highlight = m_SelectedPass == row.Pass;
                    line.Tooltip = "Click: what it reads and writes";
                    out.push_back(std::move(line));
                }
            };
            table->OnRowClick = [this](const TableViewV2::Row& row, int) { if (row.Id != ~0ull) m_SelectedPass = m_SelectedPass == row.Id ? ~0u : static_cast<uint32_t>(row.Id); };
            page->AddChild(table);
            auto accesses = UI::Label("", 13.0f);
            Ref<UI::UILabel> accessesRef = accesses;
            accessesRef->WordWrap = true;
            accessesRef->WidthMode = UI::UISizeMode::Fill;
            page->AddChild(accessesRef);
            UI::AppendSync(*page, [this, graph, accessesRef]()
            {
                const RGFrameReport& report = graph->GetReport();
                if (m_SelectedPass >= report.Passes.size())
                {
                    accessesRef->Text = "Click a pass for what it reads and writes.";
                    return;
                }
                const RGFrameReport::Pass& pass = report.Passes[m_SelectedPass];
                std::string text = std::string(pass.Name) + ":";
                for (uint32_t index = pass.FirstAccess; index < pass.FirstAccess + pass.AccessCount; ++index)
                {
                    const RGFrameReport::Access& access = report.Accesses[index];
                    const std::vector<RGFrameReport::Resource>& resources = access.IsTexture ? report.Textures : report.Buffers;
                    if (access.Resource < resources.size())
                        text += std::format("\n  {}  {}  {}", access.IsWrite ? "Write" : "Read ", access.IsTexture ? "texture" : "buffer ", resources[access.Resource].Name);
                }
                accessesRef->Text = text;
            });
            tabs->AddTab("Passes", page);
        }

        // ---- Resources ----
        {
            auto page = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 4.0f);
            page->WidthMode = UI::UISizeMode::Fill;
            page->HeightMode = UI::UISizeMode::Fill;
            UI::UIPropertyRows rows(page, context);
            rows.Check("Show resources unused this frame", [this]() { return m_ShowUnusedResources; }, [this](bool v) { m_ShowUnusedResources = v; });
            Ref<TableViewV2> table(new TableViewV2());
            table->Columns = { { "Resource", 3.0f }, { "Kind", 1.2f }, { "Size", 1.4f }, { "Format", 1.0f }, { "Memory", 1.0f }, { "Lifetime", 2.2f }, { "GPU Object", 0.9f }, { "", 0.9f } };
            // Ids: textures 2i, buffers 2i+1 (in report order); names with their occurrence for the inspector.
            auto names = std::make_shared<std::unordered_map<uint64_t, std::pair<std::string, uint32_t>>>();
            table->Fill = [this, graph, r, names](std::vector<TableViewV2::Row>& out)
            {
                const RGFrameReport& report = graph->GetReport();
                const float passCount = static_cast<float>(std::max<size_t>(report.Passes.size(), 1));
                const Renderer::TextureInspection& inspection = r->getTextureInspection();
                names->clear();
                const auto rowsOf = [&](const std::vector<RGFrameReport::Resource>& resources, bool textures)
                {
                    std::unordered_map<std::string_view, uint32_t> occurrences;
                    for (uint32_t index = 0; index < resources.size(); ++index)
                    {
                        const RGFrameReport::Resource& resource = resources[index];
                        const uint32_t occurrence = occurrences[resource.Name]++;
                        const bool used = resource.FirstPass != RGInvalidIndex;
                        if (!used && !m_ShowUnusedResources)
                            continue;
                        TableViewV2::Row row;
                        row.Id = static_cast<uint64_t>(index) * 2 + (textures ? 0 : 1);
                        const std::string name = occurrence > 0 || resource.Kind == RGResourceKind::History ? std::format("{} [{}]", resource.Name, occurrence) : std::string(resource.Name);
                        const std::string size = textures ? (resource.Texture.MipLevels > 1 ? std::format("{}x{} ({} mips)", resource.Texture.Width, resource.Texture.Height, resource.Texture.MipLevels)
                                                                                           : std::format("{}x{}", resource.Texture.Width, resource.Texture.Height))
                                                          : (resource.Kind == RGResourceKind::Imported ? std::string("-") : FormatBytes(resource.Buffer.Size));
                        const bool inspected = textures && inspection.Name == resource.Name && inspection.Occurrence == occurrence;
                        row.Cells = { name, std::string(KindName(resource.Kind)) + (resource.ReadOnly ? " (RO)" : ""), size, textures ? FormatName(resource.Texture.Format) : "buffer",
                                      resource.Bytes > 0 ? FormatBytes(resource.Bytes) : std::string("-"), "", resource.PhysicalId != RGInvalidIndex ? std::format("#{}", resource.PhysicalId) : std::string(),
                                      textures ? (inspected ? "Inspecting" : "Inspect") : "" };
                        row.Color = used ? row.Color : kCulled;
                        row.Highlight = inspected;
                        if (used)
                        {
                            row.BarColumn = 5;
                            row.BarBegin = static_cast<float>(resource.FirstPass) / passCount;
                            row.BarEnd = static_cast<float>(resource.LastPass + 1) / passCount;
                            row.BarColor = resource.Kind == RGResourceKind::Transient ? glm::vec4(0.43f, 0.75f, 0.35f, 1.0f) : glm::vec4(0.86f, 0.59f, 0.27f, 1.0f);
                            if (resource.FirstPass < report.Passes.size() && resource.LastPass < report.Passes.size())
                                row.Tooltip = std::format("{} -> {}", report.Passes[resource.FirstPass].Name, report.Passes[resource.LastPass].Name);
                        }
                        if (textures)
                        {
                            row.Tooltip += (row.Tooltip.empty() ? "" : "\n") + std::string("Click Inspect (last column) to see it in the Inspector tab");
                            (*names)[row.Id] = { resource.Name, occurrence };
                        }
                        out.push_back(std::move(row));
                    }
                };
                rowsOf(report.Textures, true);
                rowsOf(report.Buffers, false);
            };
            table->OnRowClick = [r, names, tabs](const TableViewV2::Row& row, int column)
            {
                const auto found = names->find(row.Id);
                if (column != 7 || found == names->end())
                    return;
                Renderer::TextureInspection inspection = r->getTextureInspection();
                inspection.Name = found->second.first;
                inspection.Occurrence = found->second.second;
                inspection.Mip = 0;
                r->setTextureInspection(inspection);
                tabs->SetActiveTab(3); // (the Inspector)
            };
            page->AddChild(table);
            tabs->AddTab("Resources", page);
        }

        // ---- Validation ----
        {
            auto scroll = UI::MakeUI<UI::UIScrollView>();
            scroll->WidthMode = UI::UISizeMode::Fill;
            scroll->HeightMode = UI::UISizeMode::Fill;
            auto label = UI::Label("", 13.0f);
            Ref<UI::UILabel> labelRef = label;
            labelRef->WordWrap = true;
            labelRef->WidthMode = UI::UISizeMode::Fill;
            scroll->AddChild(labelRef);
            UI::AppendSync(*scroll, [graph, labelRef]()
            {
                const RGFrameReport& report = graph->GetReport();
                std::string text = !graph->IsValidationEnabled() ? "Validation is off." : report.ValidationMessages.empty() ? "No issues this frame." : "";
                for (const std::string& message : report.ValidationMessages)
                    text += (text.empty() ? "" : "\n") + message;
                if (!report.UnusedWrites.empty())
                {
                    text += "\n\nWrites no later pass reads (informational):";
                    for (const RGFrameReport::UnusedWrite& unused : report.UnusedWrites)
                    {
                        const std::vector<RGFrameReport::Resource>& resources = unused.IsTexture ? report.Textures : report.Buffers;
                        if (unused.Pass < report.Passes.size() && unused.Resource < resources.size())
                            text += std::format("\n  {} -> {}", report.Passes[unused.Pass].Name, resources[unused.Resource].Name);
                    }
                }
                labelRef->Text = text;
            });
            tabs->AddTab("Validation", scroll);
        }

        // ---- Inspector ----
        {
            auto page = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 4.0f);
            page->WidthMode = UI::UISizeMode::Fill;
            page->HeightMode = UI::UISizeMode::Fill;
            UI::UIPropertyRows rows(page, context);
            rows.Text([r]()
            {
                const Renderer::TextureInspection& inspection = r->getTextureInspection();
                if (inspection.Name.empty())
                    return std::string("Pick a texture with Inspect in the Resources tab.");
                const RGTextureKey& key = r->getInspectedTextureKey();
                return std::format("{} [{}]  {}x{} {}", inspection.Name, inspection.Occurrence, key.Width, key.Height, FormatName(key.Format));
            }, { 0.86f, 0.88f, 0.92f, 1.0f });
            const auto inspecting = [r]() { return !r->getTextureInspection().Name.empty(); };
            const auto edit = [r](auto change) { Renderer::TextureInspection inspection = r->getTextureInspection(); change(inspection); r->setTextureInspection(inspection); };
            Ref<UI::UIElement> stop = rows.Button("Stop", [r]() { r->setTextureInspection({}); });
            rows.ShowWhen(stop, inspecting);
            Ref<UI::UIElement> mip = rows.Int("Mip", [r]() { return static_cast<int>(r->getTextureInspection().Mip); },
                [edit](int v) { edit([v](Renderer::TextureInspection& i) { i.Mip = static_cast<uint32_t>(v); }); }, 0, 15);
            rows.ShowWhen(mip, [r, inspecting]() { return inspecting() && r->getInspectedTextureKey().MipLevels > 1; });
            const auto floatFormat = [r]()
            {
                const RGTextureKey& key = r->getInspectedTextureKey();
                return key.Format != NRI::ImageFormat::R32SINT && key.Format != NRI::ImageFormat::R32G32_UINT && key.Format != NRI::ImageFormat::R32G32B32A32_UINT &&
                       key.Usage != NRI::TextureUsage::DepthStencilAttachment;
            };
            UI::UIPropertyRows floats = rows.Group();
            floats.Check("Depth Curve", [r]() { return r->getTextureInspection().DepthCurve; }, [edit](bool v) { edit([v](Renderer::TextureInspection& i) { i.DepthCurve = v; }); },
                         "Reverse-Z curve: depth kept in a colour format (the Hi-Z pyramid) reads as black otherwise");
            floats.Slider("Exposure (stops)", [this]() { return m_ExposureStops; },
                          [this, edit](float v) { m_ExposureStops = v; edit([v](Renderer::TextureInspection& i) { i.Exposure = std::pow(2.0f, v); }); }, -10.0f, 10.0f, "{:.1f}");
            static constexpr const char* kChannels[4] = { "R", "G", "B", "A" };
            for (uint32_t channel = 0; channel < 4; ++channel)
                floats.Check(kChannels[channel], [r, channel]() { return (r->getTextureInspection().ChannelMask & (1u << channel)) != 0; },
                             [edit, channel](bool v) { edit([v, channel](Renderer::TextureInspection& i) { i.ChannelMask = v ? i.ChannelMask | (1u << channel) : i.ChannelMask & ~(1u << channel); }); });
            rows.ShowWhen(floats.GetBody(), [inspecting, floatFormat]() { return inspecting() && floatFormat(); });

            // The image, with the value under the pointer (the renderer reads back the probed texel).
            class InspectImage final : public UI::UIImage
            {
            public:
                Renderer* Render = nullptr;
                bool OnPointerMove(const UI::UIPointerEvent& e) override { m_Pointer = e.Position; m_In = true; return true; }
                bool OnPointerLeave(const UI::UIPointerEvent&) override { m_In = false; return false; }
            protected:
                void OnUpdate(float) override
                {
                    if (!Render)
                        return;
                    Renderer::TextureInspection inspection = Render->getTextureInspection();
                    inspection.ProbeX = inspection.ProbeY = -1;
                    Texture2D* image = Render->getInspectionImage();
                    TextureDescriptorSlot = image ? image->GetDescriptorIndexSlot() : ~0u;
                    Visible = image != nullptr;
                    const RGTextureKey& key = Render->getInspectedTextureKey();
                    const uint32_t width = std::max(key.Width >> inspection.Mip, 1u), height = std::max(key.Height >> inspection.Mip, 1u);
                    const UI::UIRect b = GetBounds();
                    const float scale = b.Width() / static_cast<float>(width);
                    FixedSize = { 0.0f, static_cast<float>(height) * std::max(scale, 0.01f) };
                    if (image && m_In && b.Contains(m_Pointer) && scale > 0.0f)
                    {
                        inspection.ProbeX = std::clamp(static_cast<int32_t>((m_Pointer.x - b.Min.x) / scale), 0, static_cast<int32_t>(width) - 1);
                        inspection.ProbeY = std::clamp(static_cast<int32_t>((m_Pointer.y - b.Min.y) / scale), 0, static_cast<int32_t>(height) - 1);
                        glm::vec4 value;
                        Tooltip = Render->getInspectionProbe(value) ? std::format("({}, {})\n{:.5g}  {:.5g}  {:.5g}  {:.5g}", inspection.ProbeX, inspection.ProbeY, value.r, value.g, value.b, value.a)
                                                                    : std::string();
                    }
                    if (!inspection.Name.empty())
                        Render->setTextureInspection(inspection);
                }
            private:
                UI::UIVec2 m_Pointer{ 0.0f };
                bool m_In = false;
            };
            Ref<InspectImage> image(new InspectImage());
            image->Render = r;
            image->WidthMode = UI::UISizeMode::Fill;
            image->HeightMode = UI::UISizeMode::Fixed;
            image->HitTestMode = UI::UIHitTestMode::Self;
            page->AddChild(image);
            rows.Text([r, inspecting]() { return inspecting() && !r->getInspectionImage() ? std::string("Not available this frame (culled or not produced by the current pipeline).") : std::string(); }, kWarning);
            tabs->AddTab("Inspector", page);
        }
        UI::AppendSync(*root, [tabs, graph]()
        {
            const std::string title = std::format("Validation ({})", graph->GetReport().ValidationMessages.size());
            if (tabs->GetTabCount() > 2 && tabs->GetTabTitle(2) != title)
                tabs->SetTabTitle(2, title);
        });
        return m_Root;
    }
}
