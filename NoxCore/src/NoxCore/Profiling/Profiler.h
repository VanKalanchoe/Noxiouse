#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "NRI/NRITypes.h"
#include "NoxCore/Utils/PlatformUtils.h"

// Engine instrumentation layer (docs/Engine_Architecture_Plan_2026.md 5.1). Engine code only uses the
// NOX_PROFILE_* macros below; they feed the in-engine Nox Stats (NOX_PROFILE_STATS) and Tracy
// (NOX_ENABLE_TRACY). Neither Tracy nor Vulkan headers are included here.

#if !defined(NOX_PROFILE_STATS)
    #define NOX_PROFILE_STATS 0
#endif
#if !defined(NOX_ENABLE_TRACY)
    #define NOX_ENABLE_TRACY 0
#endif
#define NOX_PROFILING_ENABLED (NOX_PROFILE_STATS || NOX_ENABLE_TRACY)

namespace NRI
{
    class CommandBuffer;
    class GpuProfiler;
}

namespace Nox
{
    // Static description of one instrumented call site (one per macro expansion, never destroyed).
    struct ProfileScopeInfo
    {
        const char* Name;
        const char* Function;
        const char* File;
        uint32_t Line;
        bool IsWait = false; // time blocked on the GPU or display: excluded from the CPU frame time
    };

    // Milliseconds (or counter units) over the stats window; Peak is the maximum since the last reset.
    struct ProfileTiming
    {
        double Last = 0.0;
        double Min = 0.0;
        double Max = 0.0;
        double Avg = 0.0;
        double Peak = 0.0;
    };

    struct ProfileScopeStats
    {
        const char* Name = nullptr;
        uint32_t Depth = 0;
        ProfileTiming Timing;
    };

    struct ProfileCounterStats
    {
        const char* Name = nullptr;
        double Value = 0.0;
    };

    struct ProfileMemoryStats
    {
        std::vector<NRI::MemoryHeapStats> Heaps;
        bool BudgetFromDriver = false; // false: the allocator estimates usage/budget itself
        Platform::ProcessMemory Process;
    };

    // GPU scope state of one command buffer while it is recorded (Profiler::BeginGpuScope). A frame's command buffers can
    // be recorded on different threads, each with its own context; a context is used by one thread at a time and keeps
    // its capacity across frames.
    class alignas(64) GpuScopeContext
    {
    private:
        friend class Profiler;

        struct Scope
        {
            uint32_t ScopeId;
            uint32_t ParentId;
            uint32_t BeginQuery; // the end timestamp is BeginQuery + 1
        };

    private:
        std::vector<Scope> m_Scopes;
        std::vector<uint32_t> m_OpenScopes; // indices into m_Scopes
    };

    class Profiler
    {
    public:
        static constexpr uint32_t WindowFrames = 120;
        // Fixed so per-scope tables never reallocate while other threads read them.
        static constexpr uint32_t MaxScopes = 4096;

        static Profiler& Get();

        Profiler(const Profiler&) = delete;
        Profiler& operator=(const Profiler&) = delete;

        // Called once per call site (static initialization inside the macros).
        uint32_t RegisterScope(const ProfileScopeInfo& info);
        // Scopes known only by a runtime name (task system tasks): one id per distinct name, the name is copied.
        uint32_t RegisterNamedScope(std::string_view name);
        // Names the calling thread in Tracy (worker threads of the task system).
        void SetThreadName(const char* name);

        // CPU scopes: any thread. Aggregated per frame on the main thread in EndFrame().
        void BeginCpuScope(uint32_t scopeId);
        void EndCpuScope(uint32_t scopeId);
        void EndFrame();

        // GPU scopes over the frame's command buffers (submitted together, possibly recorded in parallel).
        void SetGpuProfiler(NRI::GpuProfiler* gpuProfiler);
        // Main thread, before recording: collects the slot's previous timings and opens the root "GPU Frame" scope
        // (GetGpuFrameTime).
        void BeginGpuFrame(uint32_t frameSlot);
        // Recorded first into the first submitted command buffer (outside rendering) / last into the last one.
        void RecordGpuFrameBegin(NRI::CommandBuffer& cmd);
        void RecordGpuFrameEnd(NRI::CommandBuffer& cmd);
        // By the thread recording the context's command buffer; scopes nest within a context under "GPU Frame".
        void BeginGpuScope(GpuScopeContext& context, NRI::CommandBuffer& cmd, uint32_t scopeId);
        void EndGpuScope(GpuScopeContext& context, NRI::CommandBuffer& cmd);
        // Main thread, after recording: takes the contexts' scopes in submission order and closes the frame.
        void EndGpuFrame(std::span<GpuScopeContext> contexts);

        // Main thread.
        void SetCounter(uint32_t scopeId, double value);
        void SubmitMemoryStats(std::span<const NRI::MemoryHeapStats> heaps, bool budgetFromDriver, const Platform::ProcessMemory& process);
        void ResetStats();

        // Nox Stats readers (main thread). Scopes come in tree order: parents before their children.
        bool IsGpuTimingAvailable() const;
        ProfileTiming GetFrameTime() const;    // wall time between frames
        ProfileTiming GetCpuFrameTime() const; // frame time minus wait scopes (GPU fence, present)
        ProfileTiming GetGpuFrameTime() const; // "GPU Frame" timestamps
        void GetCpuScopes(std::vector<ProfileScopeStats>& outScopes) const;
        void GetGpuScopes(std::vector<ProfileScopeStats>& outScopes) const;
        void GetCounters(std::vector<ProfileCounterStats>& outCounters) const;
        const ProfileMemoryStats& GetMemoryStats() const { return m_MemoryStats; }

    private:
        struct History;
        struct ScopeHistory;
        struct ThreadScopeBuffer;
        struct GpuFrameRecord;

        Profiler() = default;

        uint32_t RegisterScopeLocked(const ProfileScopeInfo& info);
        ThreadScopeBuffer& GetThreadBuffer();
        void ProcessGpuReadback(GpuFrameRecord& frame);
        void BuildTreeOrder(const std::vector<ScopeHistory>& histories, std::vector<ProfileScopeStats>& outScopes) const;
        void AppendSubtree(const std::vector<ScopeHistory>& histories, const std::vector<std::vector<uint32_t>>& children,
                           uint32_t scopeId, uint32_t depth, std::vector<bool>& listed, std::vector<ProfileScopeStats>& outScopes) const;
        bool IsRecent(uint64_t lastSampleFrame) const;

        static void PushSample(History& history, float value, uint64_t frame);
        static ProfileTiming Summarize(const History& history);

    private:
        static constexpr uint32_t InvalidScopeId = ~0u;
        static constexpr uint32_t NoGpuSlot = ~0u;
        static constexpr uint64_t NeverSampled = ~0ull;

        struct History
        {
            std::array<float, WindowFrames> Samples{};
            uint32_t Count = 0;
            uint32_t Next = 0;
            uint64_t LastSampleFrame = NeverSampled;
            float Peak = 0.0f;
        };

        struct ScopeHistory
        {
            History Timing;
            uint32_t ParentId = InvalidScopeId; // from the most recent sample
        };

        struct CompletedCpuScope
        {
            uint32_t ScopeId;
            uint32_t ParentId;
            int64_t DurationNs;
        };

        struct ThreadScopeBuffer
        {
            std::mutex Mutex; // uncontended: the owning thread appends, EndFrame() swaps once per frame
            std::vector<CompletedCpuScope> Completed;
        };

        struct GpuFrameRecord
        {
            std::vector<GpuScopeContext::Scope> Scopes; // "GPU Frame" first, then the contexts in submission order
            bool TracyZonesEmitted = false;              // zone begins were sent to Tracy, so their times must be too
        };

        struct NamedScope
        {
            std::string Name;
            ProfileScopeInfo Info;
        };

        struct CounterValue
        {
            double Value = 0.0;
            uint64_t LastSetFrame = NeverSampled;
        };

        // Registry: ids are dense and never reused; an entry is written before its id is published.
        std::mutex m_RegistryMutex;
        std::array<const ProfileScopeInfo*, MaxScopes> m_Scopes{};
        std::atomic<uint32_t> m_ScopeCount = 0;
        std::deque<NamedScope> m_NamedScopes; // deque: registered ProfileScopeInfo/name storage never moves
        std::unordered_map<std::string, uint32_t> m_NamedScopeIds;

        // CPU
        std::mutex m_ThreadBuffersMutex;
        std::vector<std::unique_ptr<ThreadScopeBuffer>> m_ThreadBuffers;
        std::vector<CompletedCpuScope> m_DrainScratch;
        std::vector<double> m_CpuFrameMs;
        std::vector<ScopeHistory> m_CpuHistories;
        History m_FrameTime;
        History m_CpuFrameTime;
        std::chrono::steady_clock::time_point m_LastFrameEnd{};
        uint64_t m_FrameNumber = 0;

        // GPU
        NRI::GpuProfiler* m_GpuProfiler = nullptr;
        std::vector<GpuFrameRecord> m_GpuFrames;
        std::vector<double> m_GpuFrameMs;
        std::vector<ScopeHistory> m_GpuHistories;
        History m_GpuFrameTime;
        uint32_t m_GpuFrameScopeId = InvalidScopeId;
        uint32_t m_GpuCurrentSlot = NoGpuSlot; // written on the main thread outside recording, read by recording threads
        bool m_TracyGpuContextCreated = false;

        // Counters + memory
        std::vector<CounterValue> m_Counters;
        ProfileMemoryStats m_MemoryStats;
    };

    class CpuProfileScope
    {
    public:
        explicit CpuProfileScope(uint32_t scopeId) : m_ScopeId(scopeId) { Profiler::Get().BeginCpuScope(scopeId); }
        ~CpuProfileScope() { Profiler::Get().EndCpuScope(m_ScopeId); }

        CpuProfileScope(const CpuProfileScope&) = delete;
        CpuProfileScope& operator=(const CpuProfileScope&) = delete;

    private:
        uint32_t m_ScopeId;
    };

}

#if NOX_PROFILING_ENABLED
    #define NOX_PROFILE_DECLARE_SITE_EX(var, name, isWait) \
        static const ::Nox::ProfileScopeInfo var##Info{ name, __FUNCTION__, __FILE__, static_cast<uint32_t>(__LINE__), isWait }; \
        static const uint32_t var##Id = ::Nox::Profiler::Get().RegisterScope(var##Info)
    #define NOX_PROFILE_DECLARE_SITE(var, name) NOX_PROFILE_DECLARE_SITE_EX(var, name, false)

    #define NOX_PROFILE_SCOPE_LINE2(name, line) NOX_PROFILE_DECLARE_SITE(noxProfileSite##line, name); ::Nox::CpuProfileScope noxProfileScope##line(noxProfileSite##line##Id)
    #define NOX_PROFILE_SCOPE_LINE(name, line) NOX_PROFILE_SCOPE_LINE2(name, line)
    #define NOX_PROFILE_SCOPE(name) NOX_PROFILE_SCOPE_LINE(name, __LINE__)
    #define NOX_PROFILE_FUNCTION() NOX_PROFILE_SCOPE(__FUNCTION__)

    #define NOX_PROFILE_WAIT_SCOPE_LINE2(name, line) NOX_PROFILE_DECLARE_SITE_EX(noxProfileSite##line, name, true); ::Nox::CpuProfileScope noxProfileScope##line(noxProfileSite##line##Id)
    #define NOX_PROFILE_WAIT_SCOPE_LINE(name, line) NOX_PROFILE_WAIT_SCOPE_LINE2(name, line)
    // CPU scope for time spent blocked on the GPU or display; excluded from the CPU frame time.
    #define NOX_PROFILE_WAIT_SCOPE(name) NOX_PROFILE_WAIT_SCOPE_LINE(name, __LINE__)

    #define NOX_PROFILE_COUNTER(name, value) do { NOX_PROFILE_DECLARE_SITE(noxProfileSite, name); ::Nox::Profiler::Get().SetCounter(noxProfileSiteId, static_cast<double>(value)); } while (false)
    #define NOX_PROFILE_FRAME() ::Nox::Profiler::Get().EndFrame()
#else
    #define NOX_PROFILE_SCOPE(name)
    #define NOX_PROFILE_FUNCTION()
    #define NOX_PROFILE_WAIT_SCOPE(name)
    #define NOX_PROFILE_COUNTER(name, value)
    #define NOX_PROFILE_FRAME()
#endif
