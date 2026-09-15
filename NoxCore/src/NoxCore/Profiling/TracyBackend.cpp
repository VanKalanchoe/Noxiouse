#include "TracyBackend.h"

#include "Profiler.h"

#if NOX_ENABLE_TRACY
#include <array>
#include <vector>

#include <tracy/TracyC.h>
#include <common/TracyQueue.hpp> // tracy::GpuContextType for ___tracy_gpu_new_context_data::type

namespace
{
    // Tracy keeps pointers to source locations, so entries never move. Each entry is written once, before its
    // scope id is returned to any caller, so other threads can read registered entries without a lock.
    std::array<___tracy_source_location_data, Nox::Profiler::MaxScopes> s_SourceLocations{};

    // Only the Nox GPU profiler emits GPU zones, so a single context id is enough.
    constexpr uint8_t GpuContextId = 0;

    thread_local std::vector<TracyCZoneCtx> t_OpenZones;
}

namespace Nox::TracyBackend
{
    void RegisterScope(uint32_t scopeId, const ProfileScopeInfo& info)
    {
        s_SourceLocations[scopeId] = { info.Name, info.Function, info.File, info.Line, 0 };
    }

    bool IsConnected()
    {
        return ___tracy_connected() != 0;
    }

    void BeginZone(uint32_t scopeId)
    {
        t_OpenZones.push_back(___tracy_emit_zone_begin(&s_SourceLocations[scopeId], 1));
    }

    void EndZone()
    {
        if (t_OpenZones.empty())
            return;
        ___tracy_emit_zone_end(t_OpenZones.back());
        t_OpenZones.pop_back();
    }

    void MarkFrame()
    {
        ___tracy_emit_frame_mark(nullptr);
    }

    void Plot(const char* name, double value)
    {
        ___tracy_emit_plot(name, value);
    }

    // Serial variants: Tracy's manual asks for them with multi-threaded APIs such as Vulkan, and they are
    // what TracyVulkan.hpp uses (static source location, not the per-zone allocated one).
    void CreateGpuContext(int64_t gpuTime, float periodNs)
    {
        ___tracy_emit_gpu_new_context_serial({
            .gpuTime = gpuTime,
            .period = periodNs,
            .context = GpuContextId,
            .flags = 0, // no CPU/GPU calibration (same as TracyVulkan without calibrated timestamps)
            .type = static_cast<uint8_t>(tracy::GpuContextType::Vulkan)
        });
    }

    void BeginGpuZone(uint32_t scopeId, uint32_t queryId)
    {
        ___tracy_emit_gpu_zone_begin_serial({
            .srcloc = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&s_SourceLocations[scopeId])),
            .queryId = static_cast<uint16_t>(queryId),
            .context = GpuContextId
        });
    }

    void EndGpuZone(uint32_t queryId)
    {
        ___tracy_emit_gpu_zone_end_serial({
            .queryId = static_cast<uint16_t>(queryId),
            .context = GpuContextId
        });
    }

    void SetGpuTime(uint32_t queryId, int64_t gpuTime)
    {
        ___tracy_emit_gpu_time_serial({
            .gpuTime = gpuTime,
            .queryId = static_cast<uint16_t>(queryId),
            .context = GpuContextId
        });
    }
}

#else

namespace Nox::TracyBackend
{
    void RegisterScope(uint32_t, const ProfileScopeInfo&) {}
    bool IsConnected() { return false; }
    void BeginZone(uint32_t) {}
    void EndZone() {}
    void MarkFrame() {}
    void Plot(const char*, double) {}
    void CreateGpuContext(int64_t, float) {}
    void BeginGpuZone(uint32_t, uint32_t) {}
    void EndGpuZone(uint32_t) {}
    void SetGpuTime(uint32_t, int64_t) {}
}

#endif
