#include "StatsReport.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_time.h>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Renderer/Renderer.h"
#include "Profiler.h"

#if !defined(NOX_BUILD_CONFIG)
    #define NOX_BUILD_CONFIG "Unknown"
#endif

namespace Nox
{
    namespace
    {
        constexpr double BytesPerMegabyte = 1024.0 * 1024.0;
        constexpr size_t DepthIndent = 2;
        constexpr size_t ValueWidth = 9;

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

        double ToMegabytes(uint64_t bytes)
        {
            return static_cast<double>(bytes) / BytesPerMegabyte;
        }

        std::string_view UpscaleModeName(NRI::UpscaleMode mode)
        {
            constexpr std::array<std::string_view, 6> Names = { "Off", "DLAA", "Quality", "Balanced", "Performance", "UltraPerformance" };
            const size_t index = static_cast<size_t>(mode);
            return index < Names.size() ? Names[index] : "Unknown";
        }

        const char* OnOff(bool value)
        {
            return value ? "ON" : "OFF";
        }

        std::string_view DenoiserName(uint8_t denoiser)
        {
            // NRI::NRDDiffuseDenoiser and NRI::NRDReflectionDenoiser share these values.
            constexpr std::array<std::string_view, 3> Names = { "Off", "NRD REBLUR", "NRD RELAX" };
            return denoiser < Names.size() ? Names[denoiser] : "Unknown";
        }

        template <typename... Args>
        void AppendSetting(std::string& out, std::format_string<Args...> format, Args&&... args)
        {
            out += "  ";
            std::format_to(std::back_inserter(out), format, std::forward<Args>(args)...);
            out += '\n';
        }

        // Only the settings that are active for the current pipeline, mirroring the editor's Settings panel.
        void AppendRendererSettings(std::string& out, Renderer& renderer)
        {
            const bool dlssActive = renderer.isDLSSEnabled() && renderer.getUpscaleMode() != NRI::UpscaleMode::Off;
            const bool rayReconstruction = dlssActive && renderer.isDLSSRayReconstructionEnabled();

            out += "Renderer\n";
            if (dlssActive)
                AppendSetting(out, "DLSS: {} | Ray Reconstruction {}", UpscaleModeName(renderer.getUpscaleMode()), OnOff(rayReconstruction));
            else
                AppendSetting(out, "DLSS: OFF");

            if (renderer.isPathTracingEnabled())
            {
                AppendSetting(out, "Pipeline: Path Tracing | accumulation {}", OnOff(renderer.isPathTracingAccumulation()));
                if (renderer.getReSTIRPTEnabled())
                {
                    AppendSetting(out, "ReSTIR PT: ON | initial samples {} | max bounces {} | NEE light samples {} | temporal {}",
                                  renderer.getReSTIRPTNumInitialSamples(), renderer.getReSTIRPTMaxBounceDepth(),
                                  renderer.getReSTIRPTNumNeeSamples(), OnOff(renderer.getReSTIRPTTemporalEnabled()));
                }
                else
                {
                    AppendSetting(out, "ReSTIR PT: OFF");
                }

                if (renderer.getPathTracerUsesRTXDI())
                {
                    AppendSetting(out, "ReSTIR DI/GI with path tracer: ON | direct {} | diffuse GI {}",
                                  renderer.getDirectLightingMode() == 1 ? "ReSTIR DI" : "Path Tracer NEE",
                                  renderer.getDiffuseGIMode() == 2 ? "ReSTIR GI" : "Off");
                }
                else
                {
                    AppendSetting(out, "ReSTIR DI/GI with path tracer: OFF");
                }

                AppendSetting(out, "Denoiser: {}", rayReconstruction ? "DLSS RR" : DenoiserName(static_cast<uint8_t>(renderer.getNRDPTDenoiser())));
            }
            else if (renderer.getRayTracingEnabled())
            {
                AppendSetting(out, "Pipeline: Hybrid Ray Tracing");

                if (renderer.getRayTracingShadows())
                    AppendSetting(out, "RT shadows: ON | NRD SIGMA {}", OnOff(renderer.getNRDShadowsEnabled()));
                else
                    AppendSetting(out, "RT shadows: OFF");

                if (renderer.getRayTracingReflections())
                    AppendSetting(out, "RT reflections: ON | denoiser {}", rayReconstruction ? "DLSS RR" : DenoiserName(static_cast<uint8_t>(renderer.getNRDReflectionDenoiser())));
                else
                    AppendSetting(out, "RT reflections: OFF");

                switch (renderer.getDiffuseGIMode())
                {
                case 1:
                    AppendSetting(out, "Diffuse GI: DDGI | probes {}x{}x{} | rays per probe {}", renderer.getDDGIProbeCountX(),
                                  renderer.getDDGIProbeCountY(), renderer.getDDGIProbeCountZ(), renderer.getDDGIRaysPerProbe());
                    break;
                case 2:
                    AppendSetting(out, "Diffuse GI: ReSTIR GI | spatial samples {} | max history {} | denoiser {}",
                                  renderer.getReSTIRGINumSpatialSamples(), renderer.getReSTIRGIMaxHistoryLength(),
                                  DenoiserName(static_cast<uint8_t>(renderer.getNRDGIDenoiser())));
                    break;
                default:
                    AppendSetting(out, "Diffuse GI: Off (IBL ambient)");
                    break;
                }

                if (renderer.getDirectLightingMode() == 1)
                {
                    AppendSetting(out, "Direct lighting: ReSTIR DI | local light samples {} | infinite light samples {} | spatial samples {} | max history {} | ReGIR {} | denoiser {}",
                                  renderer.getReSTIRDINumLocalLightSamples(), renderer.getReSTIRDINumInfiniteLightSamples(),
                                  renderer.getReSTIRDINumSpatialSamples(), renderer.getReSTIRDIMaxHistoryLength(),
                                  OnOff(renderer.getReGIREnabled()), DenoiserName(static_cast<uint8_t>(renderer.getNRDDIDenoiser())));
                }
                else
                {
                    AppendSetting(out, "Direct lighting: Brute-force analytic loop");
                }
            }
            else
            {
                AppendSetting(out, "Pipeline: Raster");
            }

            // Debug views replace the lit output and change what runs, so a report taken with one is not comparable.
            if (renderer.getDebugMode() != 0)
                AppendSetting(out, "Debug view: {} (not a normal lit frame)", renderer.getDebugMode());
        }

        size_t NameColumnWidth(const std::vector<ProfileScopeStats>& scopes, size_t minimum)
        {
            size_t width = minimum;
            for (const ProfileScopeStats& scope : scopes)
                width = std::max(width, scope.Depth * DepthIndent + std::string_view(scope.Name).size());
            return width + 2;
        }

        void AppendTimingHeader(std::string& out, std::string_view title, size_t nameWidth)
        {
            std::format_to(std::back_inserter(out), "{:<{}}{:>{}}{:>{}}{:>{}}{:>{}}{:>{}}\n", title, nameWidth,
                           "last", ValueWidth, "avg", ValueWidth, "min", ValueWidth, "max", ValueWidth, "peak", ValueWidth);
        }

        void AppendTimingRow(std::string& out, std::string_view name, uint32_t depth, const ProfileTiming& timing, size_t nameWidth)
        {
            const size_t indent = depth * DepthIndent;
            std::format_to(std::back_inserter(out), "{:{}}{:<{}}{:>{}.2f}{:>{}.2f}{:>{}.2f}{:>{}.2f}{:>{}.2f}\n", "", indent, name, nameWidth - indent,
                           timing.Last, ValueWidth, timing.Avg, ValueWidth, timing.Min, ValueWidth, timing.Max, ValueWidth, timing.Peak, ValueWidth);
        }

        void AppendScopeTable(std::string& out, std::string_view title, const std::vector<ProfileScopeStats>& scopes)
        {
            const size_t nameWidth = NameColumnWidth(scopes, title.size());
            AppendTimingHeader(out, title, nameWidth);
            for (const ProfileScopeStats& scope : scopes)
                AppendTimingRow(out, scope.Name, scope.Depth, scope.Timing, nameWidth);
        }
    }

    std::filesystem::path SaveStatsReport(Renderer& renderer)
    {
        const Profiler& profiler = Profiler::Get();
        std::string report;

        SDL_DateTime now{};
        SDL_Time ticks = 0;
        if (!SDL_GetCurrentTime(&ticks) || !SDL_TimeToDateTime(ticks, &now, true))
            NOX_CORE_WARN("SaveStatsReport: could not read the local time: {}", SDL_GetError());

        // Header: everything that changes the numbers below, so a report is comparable without extra context.
        const NRI::Extent2D outputSize = renderer.getOutputSize();
        const NRI::Extent2D renderSize = renderer.getRenderSize();
        std::format_to(std::back_inserter(report), "Nox Stats report {:04}-{:02}-{:02} {:02}:{:02}:{:02}\n", now.year, now.month, now.day, now.hour, now.minute, now.second);
        std::format_to(std::back_inserter(report), "Build {} | VSync {} | Parallel recording {} | Sync {} | Output {}x{} | Render {}x{}\n", NOX_BUILD_CONFIG,
                       OnOff(renderer.getVSync()), OnOff(renderer.isParallelCommandRecording()),
                       renderer.getRenderGraph().GetSynchronization() == RGSynchronization::Precise ? "Precise" : "Blanket",
                       outputSize.width, outputSize.height, renderSize.width, renderSize.height);
        if (std::string_view(NOX_BUILD_CONFIG) == "Debug")
            report += "Debug build: not for performance numbers\n";
        std::format_to(std::back_inserter(report), "Times in ms over the last {} frames; peak = maximum since the last stats reset\n\n", Profiler::WindowFrames);
        AppendRendererSettings(report, renderer);

        const ProfileTiming frame = profiler.GetFrameTime();
        std::format_to(std::back_inserter(report), "\nFPS {:.0f}\n", frame.Avg > 0.0 ? 1000.0 / frame.Avg : 0.0);
        constexpr size_t FrameNameWidth = 12;
        AppendTimingHeader(report, "Frame", FrameNameWidth);
        AppendTimingRow(report, "Wall", 0, frame, FrameNameWidth);
        AppendTimingRow(report, "CPU", 0, profiler.GetCpuFrameTime(), FrameNameWidth);
        if (profiler.IsGpuTimingAvailable())
            AppendTimingRow(report, "GPU", 0, profiler.GetGpuFrameTime(), FrameNameWidth);

        const ProfileMemoryStats& memory = profiler.GetMemoryStats();
        std::format_to(std::back_inserter(report), "\nRAM process: working set {:.0f} MB, private {:.0f} MB / installed {} MB\n",
                       ToMegabytes(memory.Process.WorkingSetBytes), ToMegabytes(memory.Process.PrivateBytes), SDL_GetSystemRAM());
        for (size_t heapIndex = 0; heapIndex < memory.Heaps.size(); ++heapIndex)
        {
            const NRI::MemoryHeapStats& heap = memory.Heaps[heapIndex];
            std::format_to(std::back_inserter(report), "{} heap {}: {:.0f} / {:.0f} MB, {} allocations\n", heap.deviceLocal ? "VRAM" : "Host", heapIndex,
                           ToMegabytes(heap.usage), ToMegabytes(heap.budget), heap.allocationCount);
        }
        if (!memory.Heaps.empty() && !memory.BudgetFromDriver)
            report += "VK_EXT_memory_budget unavailable: usage and budget are estimated\n";

        std::vector<ProfileScopeStats> scopes;
        report += '\n';
        if (profiler.IsGpuTimingAvailable())
        {
            profiler.GetGpuScopes(scopes);
            AppendScopeTable(report, "GPU passes", scopes);
        }
        else
        {
            report += "GPU timestamp queries are not supported on this queue\n";
        }

        std::vector<ProfilePipelineStatistics> statistics;
        profiler.GetPipelineStatistics(statistics);
        if (!statistics.empty())
        {

            report += "\nGPU pipeline statistics (last frame)       fragments  task invocations  mesh invocations\n";
            for (const ProfilePipelineStatistics& pass : statistics)
                std::format_to(std::back_inserter(report), "  {:<34} {:>13} {:>17} {:>17}\n", pass.Name, pass.Fragments,
                               pass.TaskInvocations, pass.MeshInvocations);
        }

        report += '\n';
        profiler.GetCpuScopes(scopes);
        AppendScopeTable(report, "CPU scopes", scopes);

        std::vector<ProfileCounterStats> counters;
        profiler.GetCounters(counters);
        {
            // Draws/s: the indirect draws the GPU executed for the camera view (phase 1 plus the phase 2 candidates).
            const double frameMs = profiler.GetFrameTime().Avg;
            double draws = 0.0;
            double triangles = 0.0;
            for (const ProfileCounterStats& counter : counters)
            {
                if (std::string_view(counter.Name) == "Visible Instances" || std::string_view(counter.Name) == "Phase 2 Drawn")
                    draws += counter.Value;
                else if (std::string_view(counter.Name) == "Visible Triangles")
                    triangles = counter.Value;
            }
            if (frameMs > 0.0 && draws > 0.0)
            {
                std::format_to(std::back_inserter(report), "\nDraws/s: {}\n", FormatRate(draws * 1000.0 / frameMs));
                std::format_to(std::back_inserter(report), "Triangles/s: {}\n", FormatRate(triangles * 1000.0 / frameMs));
            }
        }
        if (!counters.empty())
        {
            report += "\nCounters\n";
            for (const ProfileCounterStats& counter : counters)
                std::format_to(std::back_inserter(report), "{}: {:.0f}\n", counter.Name, counter.Value);
        }

        // Nox.log is opened relative to the working directory, so this lands next to it.
        std::error_code ec;
        const std::filesystem::path path = std::filesystem::absolute(
            std::format("NoxStats_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}.txt", now.year, now.month, now.day, now.hour, now.minute, now.second), ec);

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(report.data(), static_cast<std::streamsize>(report.size()));
        if (ec || !file)
        {
            NOX_CORE_ERROR("SaveStatsReport: could not write {}", path.string());
            return {};
        }

        NOX_CORE_INFO("Nox Stats report saved: {}", path.string());
        return path;
    }
}
