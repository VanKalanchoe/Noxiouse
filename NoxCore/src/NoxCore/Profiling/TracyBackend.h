#pragma once
#include <cstdint>

// Internal to the profiler: the only bridge to Tracy. TracyBackend.cpp is the only file that includes
// Tracy headers; every function is a no-op unless NOX_ENABLE_TRACY is set.
namespace Nox
{
    struct ProfileScopeInfo;
}

namespace Nox::TracyBackend
{
    // Called once per scope id (< Profiler::MaxScopes) under the profiler registry lock.
    void RegisterScope(uint32_t scopeId, const ProfileScopeInfo& info);

    bool IsConnected();

    void BeginZone(uint32_t scopeId);
    void EndZone();
    void MarkFrame();
    void Plot(const char* name, double value);

    // GPU zones through Tracy's C GPU API, in the same order TracyVulkan.hpp emits them:
    // context once, zone begin/end while recording, times when the queries are read back.
    void CreateGpuContext(int64_t gpuTime, float periodNs);
    void BeginGpuZone(uint32_t scopeId, uint32_t queryId);
    void EndGpuZone(uint32_t queryId);
    void SetGpuTime(uint32_t queryId, int64_t gpuTime);
}
