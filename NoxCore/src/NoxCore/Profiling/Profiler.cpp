#include "Profiler.h"

#include <algorithm>
#include <limits>

#include "NRI/GpuProfiler.h"
#include "NoxCore/Core/Log.h"
#include "TracyBackend.h"

namespace Nox
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr double NanosecondsPerMillisecond = 1'000'000.0;
        constexpr double BytesPerMegabyte = 1024.0 * 1024.0;
        constexpr uint32_t MaxTreeDepth = 64;

        struct OpenCpuScope
        {
            uint32_t ScopeId;
            Clock::time_point Start;
        };

        thread_local std::vector<OpenCpuScope> t_OpenCpuScopes;
    }

    Profiler& Profiler::Get()
    {
        static Profiler s_Profiler;
        return s_Profiler;
    }

    uint32_t Profiler::RegisterScope(const ProfileScopeInfo& info)
    {
        std::scoped_lock lock(m_RegistryMutex);
        return RegisterScopeLocked(info);
    }

    uint32_t Profiler::RegisterNamedScope(std::string_view name)
    {
        std::scoped_lock lock(m_RegistryMutex);

        std::string key(name);
        if (auto found = m_NamedScopeIds.find(key); found != m_NamedScopeIds.end())
            return found->second;

        NamedScope& scope = m_NamedScopes.emplace_back();
        scope.Name = std::move(key);
        scope.Info = { scope.Name.c_str(), "Task", "", 0 };

        const uint32_t scopeId = RegisterScopeLocked(scope.Info);
        m_NamedScopeIds.emplace(scope.Name, scopeId);
        return scopeId;
    }

    void Profiler::SetThreadName(const char* name)
    {
        TracyBackend::SetThreadName(name);
    }

    uint32_t Profiler::RegisterScopeLocked(const ProfileScopeInfo& info)
    {
        const uint32_t scopeId = m_ScopeCount.load(std::memory_order_relaxed);
        if (scopeId >= MaxScopes)
        {
            NOX_CORE_ERROR("[Profiler] More than {} profile scopes; '{}' is not recorded", MaxScopes, info.Name);
            return InvalidScopeId;
        }

        m_Scopes[scopeId] = &info;
        TracyBackend::RegisterScope(scopeId, info);
        m_ScopeCount.store(scopeId + 1, std::memory_order_relaxed);
        return scopeId;
    }

    void Profiler::BeginCpuScope(uint32_t scopeId)
    {
        if (scopeId == InvalidScopeId)
            return;

        TracyBackend::BeginZone(scopeId);
#if NOX_PROFILE_STATS
        t_OpenCpuScopes.push_back({ scopeId, Clock::now() });
#endif
    }

    void Profiler::EndCpuScope(uint32_t scopeId)
    {
        if (scopeId == InvalidScopeId)
            return;

        TracyBackend::EndZone();
#if NOX_PROFILE_STATS
        if (t_OpenCpuScopes.empty())
            return;

        const Clock::time_point end = Clock::now();
        const OpenCpuScope open = t_OpenCpuScopes.back();
        t_OpenCpuScopes.pop_back();
        const uint32_t parentId = t_OpenCpuScopes.empty() ? InvalidScopeId : t_OpenCpuScopes.back().ScopeId;

        ThreadScopeBuffer& buffer = GetThreadBuffer();
        std::scoped_lock lock(buffer.Mutex);
        buffer.Completed.push_back({ open.ScopeId, parentId, std::chrono::duration_cast<std::chrono::nanoseconds>(end - open.Start).count() });
#endif
    }

    void Profiler::EndFrame()
    {
#if NOX_PROFILE_STATS
        const Clock::time_point now = Clock::now();
        const bool hasPreviousFrame = m_LastFrameEnd != Clock::time_point{};
        const double frameMs = hasPreviousFrame ? std::chrono::duration<double, std::milli>(now - m_LastFrameEnd).count() : 0.0;
        m_LastFrameEnd = now;

        {
            std::scoped_lock lock(m_ThreadBuffersMutex);
            for (const std::unique_ptr<ThreadScopeBuffer>& buffer : m_ThreadBuffers)
            {
                std::scoped_lock bufferLock(buffer->Mutex);
                m_DrainScratch.insert(m_DrainScratch.end(), buffer->Completed.begin(), buffer->Completed.end());
                buffer->Completed.clear();
            }
        }

        const uint32_t scopeCount = m_ScopeCount.load(std::memory_order_relaxed);
        if (m_CpuHistories.size() < scopeCount)
        {
            m_CpuHistories.resize(scopeCount);
            m_CpuFrameMs.resize(scopeCount, -1.0);
        }

        // A scope entered several times in one frame contributes one summed sample.
        double waitMs = 0.0;
        for (const CompletedCpuScope& scope : m_DrainScratch)
        {
            const double scopeMs = static_cast<double>(scope.DurationNs) / NanosecondsPerMillisecond;
            double& scopeFrameMs = m_CpuFrameMs[scope.ScopeId];
            scopeFrameMs = std::max(scopeFrameMs, 0.0) + scopeMs;
            m_CpuHistories[scope.ScopeId].ParentId = scope.ParentId;
            if (m_Scopes[scope.ScopeId]->IsWait)
                waitMs += scopeMs;
        }
        for (const CompletedCpuScope& scope : m_DrainScratch)
        {
            double& scopeFrameMs = m_CpuFrameMs[scope.ScopeId];
            if (scopeFrameMs < 0.0)
                continue;
            PushSample(m_CpuHistories[scope.ScopeId].Timing, static_cast<float>(scopeFrameMs), m_FrameNumber);
            scopeFrameMs = -1.0;
        }
        m_DrainScratch.clear();

        if (hasPreviousFrame)
        {
            PushSample(m_FrameTime, static_cast<float>(frameMs), m_FrameNumber);
            PushSample(m_CpuFrameTime, static_cast<float>(std::max(frameMs - waitMs, 0.0)), m_FrameNumber);
        }
#endif

        TracyBackend::MarkFrame();
        ++m_FrameNumber;
    }

    void Profiler::SetGpuProfiler(NRI::GpuProfiler* gpuProfiler)
    {
        m_GpuProfiler = gpuProfiler;
        m_GpuFrames.clear();
        m_GpuCurrentSlot = NoGpuSlot;
    }

    void Profiler::BeginGpuFrame(uint32_t frameSlot)
    {
        m_GpuCurrentSlot = NoGpuSlot;
        if (!m_GpuProfiler || !m_GpuProfiler->isSupported())
            return;

        if (frameSlot >= m_GpuFrames.size())
            m_GpuFrames.resize(frameSlot + 1);

        m_GpuProfiler->beginFrame(frameSlot);

        GpuFrameRecord& frame = m_GpuFrames[frameSlot];
        ProcessGpuReadback(frame);

        frame.Scopes.clear();
        // Tracy's manual: read the connection state once and use the same value for a zone's begin and end
        // (and, for GPU zones, for its times).
        frame.TracyZonesEmitted = m_TracyGpuContextCreated && TracyBackend::IsConnected();

        static const ProfileScopeInfo s_GpuFrameScope{ "GPU Frame", __FUNCTION__, __FILE__, static_cast<uint32_t>(__LINE__) };
        if (m_GpuFrameScopeId == InvalidScopeId)
            m_GpuFrameScopeId = RegisterScope(s_GpuFrameScope);

        // The frame scope spans every command buffer: its timestamps go into the first and the last one, its Tracy zone
        // is opened and closed here on the main thread.
        const uint32_t query = m_GpuProfiler->allocateQueryPair();
        frame.Scopes.push_back({ m_GpuFrameScopeId, InvalidScopeId, query });
        if (frame.TracyZonesEmitted && query != NRI::GpuProfiler::InvalidQueryId)
            TracyBackend::BeginGpuZone(m_GpuFrameScopeId, query);

        m_GpuCurrentSlot = frameSlot;
    }

    void Profiler::RecordGpuFrameBegin(NRI::CommandBuffer& cmd)
    {
        if (m_GpuCurrentSlot == NoGpuSlot)
            return;

        m_GpuProfiler->resetQueries(cmd);
        m_GpuProfiler->writeTimestamp(cmd, m_GpuFrames[m_GpuCurrentSlot].Scopes.front().BeginQuery);
    }

    void Profiler::RecordGpuFrameEnd(NRI::CommandBuffer& cmd)
    {
        if (m_GpuCurrentSlot == NoGpuSlot)
            return;

        const uint32_t beginQuery = m_GpuFrames[m_GpuCurrentSlot].Scopes.front().BeginQuery;
        if (beginQuery != NRI::GpuProfiler::InvalidQueryId)
            m_GpuProfiler->writeTimestamp(cmd, beginQuery + 1);
    }

    void Profiler::BeginGpuScope(GpuScopeContext& context, NRI::CommandBuffer& cmd, uint32_t scopeId)
    {
        if (m_GpuCurrentSlot == NoGpuSlot)
            return;

        const uint32_t parentId = context.m_OpenScopes.empty() ? m_GpuFrameScopeId : context.m_Scopes[context.m_OpenScopes.back()].ScopeId;

        // Unregistered scopes still get a record so the matching EndGpuScope stays balanced.
        uint32_t beginQuery = NRI::GpuProfiler::InvalidQueryId;
        if (scopeId != InvalidScopeId)
        {
            beginQuery = m_GpuProfiler->allocateQueryPair();
            m_GpuProfiler->writeTimestamp(cmd, beginQuery);
        }

        context.m_OpenScopes.push_back(static_cast<uint32_t>(context.m_Scopes.size()));
        context.m_Scopes.push_back({ scopeId, parentId, beginQuery });

        if (m_GpuFrames[m_GpuCurrentSlot].TracyZonesEmitted && beginQuery != NRI::GpuProfiler::InvalidQueryId)
            TracyBackend::BeginGpuZone(scopeId, beginQuery);
    }

    void Profiler::EndGpuScope(GpuScopeContext& context, NRI::CommandBuffer& cmd)
    {
        if (m_GpuCurrentSlot == NoGpuSlot || context.m_OpenScopes.empty())
            return;

        const GpuScopeContext::Scope scope = context.m_Scopes[context.m_OpenScopes.back()];
        context.m_OpenScopes.pop_back();
        if (scope.ScopeId == InvalidScopeId)
            return;

        if (scope.BeginQuery != NRI::GpuProfiler::InvalidQueryId)
        {
            m_GpuProfiler->writeTimestamp(cmd, scope.BeginQuery + 1);
            if (m_GpuFrames[m_GpuCurrentSlot].TracyZonesEmitted)
                TracyBackend::EndGpuZone(scope.BeginQuery + 1);
        }
    }

    void Profiler::EndGpuFrame(std::span<GpuScopeContext> contexts)
    {
        if (m_GpuCurrentSlot != NoGpuSlot)
        {
            GpuFrameRecord& frame = m_GpuFrames[m_GpuCurrentSlot];
            for (const GpuScopeContext& context : contexts)
            {
                if (!context.m_OpenScopes.empty())
                    NOX_CORE_ERROR("[Profiler] Unbalanced GPU scopes at the end of a command buffer: {} open", context.m_OpenScopes.size());
                frame.Scopes.insert(frame.Scopes.end(), context.m_Scopes.begin(), context.m_Scopes.end());
            }

            const uint32_t frameQuery = frame.Scopes.front().BeginQuery;
            if (frame.TracyZonesEmitted && frameQuery != NRI::GpuProfiler::InvalidQueryId)
                TracyBackend::EndGpuZone(frameQuery + 1);
        }

        for (GpuScopeContext& context : contexts)
        {
            context.m_Scopes.clear();
            context.m_OpenScopes.clear();
        }
        m_GpuCurrentSlot = NoGpuSlot;
    }

    void Profiler::SetCounter(uint32_t scopeId, double value)
    {
        if (scopeId == InvalidScopeId)
            return;

        TracyBackend::Plot(m_Scopes[scopeId]->Name, value);
#if NOX_PROFILE_STATS
        if (m_Counters.size() <= scopeId)
            m_Counters.resize(scopeId + 1);
        m_Counters[scopeId] = { value, m_FrameNumber };
#endif
    }

    void Profiler::SubmitMemoryStats(std::span<const NRI::MemoryHeapStats> heaps, bool budgetFromDriver, const Platform::ProcessMemory& process)
    {
        uint64_t deviceLocalUsage = 0;
        for (const NRI::MemoryHeapStats& heap : heaps)
        {
            if (heap.deviceLocal)
                deviceLocalUsage += heap.usage;
        }
        TracyBackend::Plot("VRAM Usage (MB)", static_cast<double>(deviceLocalUsage) / BytesPerMegabyte);
        TracyBackend::Plot("Process Private Bytes (MB)", static_cast<double>(process.PrivateBytes) / BytesPerMegabyte);

#if NOX_PROFILE_STATS
        m_MemoryStats.Heaps.assign(heaps.begin(), heaps.end());
        m_MemoryStats.BudgetFromDriver = budgetFromDriver;
        m_MemoryStats.Process = process;
#endif
    }

    void Profiler::ResetStats()
    {
        m_FrameTime = History{};
        m_CpuFrameTime = History{};
        m_GpuFrameTime = History{};
        for (ScopeHistory& history : m_CpuHistories)
            history.Timing = History{};
        for (ScopeHistory& history : m_GpuHistories)
            history.Timing = History{};
    }

    bool Profiler::IsGpuTimingAvailable() const
    {
        return m_GpuProfiler && m_GpuProfiler->isSupported();
    }

    ProfileTiming Profiler::GetFrameTime() const
    {
        return Summarize(m_FrameTime);
    }

    ProfileTiming Profiler::GetCpuFrameTime() const
    {
        return Summarize(m_CpuFrameTime);
    }

    ProfileTiming Profiler::GetGpuFrameTime() const
    {
        return Summarize(m_GpuFrameTime);
    }

    void Profiler::GetCpuScopes(std::vector<ProfileScopeStats>& outScopes) const
    {
        BuildTreeOrder(m_CpuHistories, outScopes);
    }

    void Profiler::GetGpuScopes(std::vector<ProfileScopeStats>& outScopes) const
    {
        BuildTreeOrder(m_GpuHistories, outScopes);
    }

    void Profiler::GetCounters(std::vector<ProfileCounterStats>& outCounters) const
    {
        outCounters.clear();
        for (uint32_t scopeId = 0; scopeId < m_Counters.size(); ++scopeId)
        {
            if (IsRecent(m_Counters[scopeId].LastSetFrame))
                outCounters.push_back({ m_Scopes[scopeId]->Name, m_Counters[scopeId].Value });
        }
    }

    Profiler::ThreadScopeBuffer& Profiler::GetThreadBuffer()
    {
        thread_local ThreadScopeBuffer* t_Buffer = nullptr;
        if (!t_Buffer)
        {
            std::unique_ptr<ThreadScopeBuffer> buffer = std::make_unique<ThreadScopeBuffer>();
            t_Buffer = buffer.get();
            std::scoped_lock lock(m_ThreadBuffersMutex);
            m_ThreadBuffers.push_back(std::move(buffer));
        }
        return *t_Buffer;
    }

    void Profiler::ProcessGpuReadback(GpuFrameRecord& frame)
    {
        const std::span<const NRI::GpuTimestamp> timestamps = m_GpuProfiler->getReadbackTimestamps();

        if (frame.TracyZonesEmitted)
        {
            for (const NRI::GpuTimestamp& timestamp : timestamps)
                TracyBackend::SetGpuTime(timestamp.queryId, static_cast<int64_t>(timestamp.ticks));
        }

        // A Tracy GPU context needs an initial GPU time, so it is created from the first read-back timestamp;
        // zones are emitted from the next recorded frame on.
        if (!m_TracyGpuContextCreated && !timestamps.empty() && TracyBackend::IsConnected())
        {
            TracyBackend::CreateGpuContext(static_cast<int64_t>(timestamps.front().ticks), m_GpuProfiler->getTimestampPeriod());
            m_TracyGpuContextCreated = true;
        }

#if NOX_PROFILE_STATS
        if (frame.Scopes.empty() || timestamps.empty())
            return;

        const uint32_t scopeCount = m_ScopeCount.load(std::memory_order_relaxed);
        if (m_GpuHistories.size() < scopeCount)
        {
            m_GpuHistories.resize(scopeCount);
            m_GpuFrameMs.resize(scopeCount, -1.0);
        }

        // Timestamps arrive sorted by query id.
        auto findTicks = [&timestamps](uint32_t queryId, uint64_t& outTicks)
        {
            const auto it = std::lower_bound(timestamps.begin(), timestamps.end(), queryId,
                                             [](const NRI::GpuTimestamp& timestamp, uint32_t id) { return timestamp.queryId < id; });
            if (it == timestamps.end() || it->queryId != queryId)
                return false;
            outTicks = it->ticks;
            return true;
        };

        const uint32_t validBits = m_GpuProfiler->getTimestampValidBits();
        const uint64_t tickMask = validBits >= 64 ? std::numeric_limits<uint64_t>::max() : (uint64_t{1} << validBits) - 1;
        const double millisecondsPerTick = static_cast<double>(m_GpuProfiler->getTimestampPeriod()) / NanosecondsPerMillisecond;

        for (const GpuScopeContext::Scope& record : frame.Scopes)
        {
            uint64_t beginTicks = 0;
            uint64_t endTicks = 0;
            if (record.ScopeId == InvalidScopeId || record.BeginQuery == NRI::GpuProfiler::InvalidQueryId ||
                !findTicks(record.BeginQuery, beginTicks) || !findTicks(record.BeginQuery + 1, endTicks))
                continue;

            const double scopeMs = static_cast<double>((endTicks - beginTicks) & tickMask) * millisecondsPerTick;
            double& frameMs = m_GpuFrameMs[record.ScopeId];
            frameMs = std::max(frameMs, 0.0) + scopeMs;
            m_GpuHistories[record.ScopeId].ParentId = record.ParentId;
            if (record.ScopeId == m_GpuFrameScopeId)
                PushSample(m_GpuFrameTime, static_cast<float>(scopeMs), m_FrameNumber);
        }
        for (const GpuScopeContext::Scope& record : frame.Scopes)
        {
            if (record.ScopeId == InvalidScopeId)
                continue;
            double& frameMs = m_GpuFrameMs[record.ScopeId];
            if (frameMs < 0.0)
                continue;
            PushSample(m_GpuHistories[record.ScopeId].Timing, static_cast<float>(frameMs), m_FrameNumber);
            frameMs = -1.0;
        }
#endif
    }

    void Profiler::BuildTreeOrder(const std::vector<ScopeHistory>& histories, std::vector<ProfileScopeStats>& outScopes) const
    {
        outScopes.clear();

        const uint32_t count = static_cast<uint32_t>(histories.size());
        std::vector<std::vector<uint32_t>> children(count);
        std::vector<uint32_t> roots;
        for (uint32_t scopeId = 0; scopeId < count; ++scopeId)
        {
            if (!IsRecent(histories[scopeId].Timing.LastSampleFrame))
                continue;

            const uint32_t parentId = histories[scopeId].ParentId;
            if (parentId < count && parentId != scopeId && IsRecent(histories[parentId].Timing.LastSampleFrame))
                children[parentId].push_back(scopeId);
            else
                roots.push_back(scopeId);
        }

        std::vector<bool> listed(count, false);
        for (uint32_t rootId : roots)
            AppendSubtree(histories, children, rootId, 0, listed, outScopes);

        // Scopes whose most recent parents form a cycle (nesting changed between frames) have no root.
        for (uint32_t scopeId = 0; scopeId < count; ++scopeId)
        {
            if (!listed[scopeId] && IsRecent(histories[scopeId].Timing.LastSampleFrame))
                AppendSubtree(histories, children, scopeId, 0, listed, outScopes);
        }
    }

    void Profiler::AppendSubtree(const std::vector<ScopeHistory>& histories, const std::vector<std::vector<uint32_t>>& children,
                                 uint32_t scopeId, uint32_t depth, std::vector<bool>& listed, std::vector<ProfileScopeStats>& outScopes) const
    {
        if (listed[scopeId] || depth > MaxTreeDepth)
            return;

        listed[scopeId] = true;
        outScopes.push_back({ m_Scopes[scopeId]->Name, depth, Summarize(histories[scopeId].Timing) });
        for (uint32_t childId : children[scopeId])
            AppendSubtree(histories, children, childId, depth + 1, listed, outScopes);
    }

    bool Profiler::IsRecent(uint64_t lastSampleFrame) const
    {
        return lastSampleFrame != NeverSampled && m_FrameNumber - lastSampleFrame <= WindowFrames;
    }

    void Profiler::PushSample(History& history, float value, uint64_t frame)
    {
        history.Samples[history.Next] = value;
        history.Next = (history.Next + 1) % WindowFrames;
        history.Count = std::min(history.Count + 1, WindowFrames);
        history.Peak = std::max(history.Peak, value);
        history.LastSampleFrame = frame;
    }

    ProfileTiming Profiler::Summarize(const History& history)
    {
        ProfileTiming timing;
        if (history.Count == 0)
            return timing;

        // Until the window is full, the valid samples are [0, Count).
        float minValue = std::numeric_limits<float>::max();
        float maxValue = std::numeric_limits<float>::lowest();
        double sum = 0.0;
        for (uint32_t index = 0; index < history.Count; ++index)
        {
            const float value = history.Samples[index];
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
            sum += value;
        }

        timing.Last = history.Samples[(history.Next + WindowFrames - 1) % WindowFrames];
        timing.Min = minValue;
        timing.Max = maxValue;
        timing.Avg = sum / history.Count;
        timing.Peak = history.Peak;
        return timing;
    }
}
