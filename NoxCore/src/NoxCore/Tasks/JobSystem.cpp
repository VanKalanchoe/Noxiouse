#include "JobSystem.h"

#include <algorithm>
#include <format>
#include <string>

#include <SDL3/SDL_cpuinfo.h>
#include <taskflow/taskflow.hpp>

#include "NoxCore/Core/core.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Profiling/Profiler.h"
#include "TaskObserver.h"

namespace Nox
{
    namespace
    {
        JobSystem* s_Instance = nullptr;

        constexpr uint32_t NoThreadSlot = ~0u;
        thread_local uint32_t t_ThreadSlot = NoThreadSlot;

        // Parallel-for chunks per worker: headroom for uneven chunk costs without flooding the queues.
        constexpr uint32_t ChunksPerWorker = 4;

        // Runs on each worker thread before it enters Taskflow's scheduling loop: assigns the thread's WorkerLocal
        // slot and names it for Tracy.
        class NoxWorkerInterface final : public tf::WorkerInterface
        {
        public:
            NoxWorkerInterface(const char* namePrefix, uint32_t firstSlot) : m_NamePrefix(namePrefix), m_FirstSlot(firstSlot) {}

            void scheduler_prologue(tf::Worker& worker) override
            {
                t_ThreadSlot = m_FirstSlot + static_cast<uint32_t>(worker.id());
                const std::string name = std::format("{} {}", m_NamePrefix, worker.id());
                Profiler::Get().SetThreadName(name.c_str());
            }

            void scheduler_epilogue(tf::Worker&, std::exception_ptr) override
            {
            }

        private:
            const char* m_NamePrefix;
            uint32_t m_FirstSlot;
        };
    }

    JobSystem::JobSystem()
    {
        NOX_CORE_ASSERT(!s_Instance, "JobSystem already exists");
        s_Instance = this;

        m_MainThreadId = std::this_thread::get_id();
        m_WorkerCount = static_cast<uint32_t>(std::max(1, SDL_GetNumLogicalCPUCores() - 1));
        m_FrameArenas = std::make_unique<FrameArena[]>(GetThreadSlotCount());
        t_ThreadSlot = GetThreadSlotCount() - 1;

        m_ExclusiveKeys = std::make_unique<tf::Semaphore[]>(MaxExclusiveKeys);
        for (uint32_t key = 0; key < MaxExclusiveKeys; ++key)
            m_ExclusiveKeys[key].reset(1);

        m_Executor = std::make_unique<tf::Executor>(m_WorkerCount, std::make_shared<NoxWorkerInterface>("Nox Worker", 0));
        m_IoExecutor = std::make_unique<tf::Executor>(IoWorkerCount, std::make_shared<NoxWorkerInterface>("Nox IO", m_WorkerCount));

#if NOX_PROFILING_ENABLED
        // make_observer is not thread-safe: register before the first task is submitted.
        m_Executor->make_observer<ProfilerTaskObserver>();
        m_IoExecutor->make_observer<ProfilerTaskObserver>();
#endif

        NOX_CORE_INFO("JobSystem: {} workers, {} IO workers", m_WorkerCount, IoWorkerCount);
    }

    JobSystem::~JobSystem()
    {
        // Executors wait for submitted work and join their threads before the frame arenas go away.
        m_IoExecutor.reset();
        m_Executor.reset();
        s_Instance = nullptr;
    }

    JobSystem& JobSystem::Get()
    {
        NOX_CORE_ASSERT(s_Instance, "JobSystem does not exist");
        return *s_Instance;
    }

    uint32_t JobSystem::GetCurrentThreadSlot() const
    {
        // Threads the JobSystem does not own (e.g. a library's internal thread) have no slot.
        NOX_CORE_ASSERT(t_ThreadSlot != NoThreadSlot, "WorkerLocal state used from a thread the JobSystem does not own");
        return t_ThreadSlot;
    }

    uint32_t JobSystem::GetChunkCount(uint32_t count, uint32_t minBatchSize) const
    {
        if (count == 0)
            return 0;
        return std::clamp(count / std::max(minBatchSize, 1u), 1u, m_WorkerCount * ChunksPerWorker);
    }

    void JobSystem::ParallelFor(std::string_view name, uint32_t count, uint32_t minBatchSize,
                                const std::function<void(uint32_t chunk, uint32_t begin, uint32_t end)>& body)
    {
        const uint32_t chunkCount = GetChunkCount(count, minBatchSize);
        if (chunkCount == 0)
            return;
        if (chunkCount == 1)
        {
            body(0, 0, count);
            return;
        }

        const uint32_t chunkSize = (count + chunkCount - 1) / chunkCount;
        const std::string taskName(name);
        auto dispatch = [&]()
        {
            tf::TaskGroup group = m_Executor->task_group();
            for (uint32_t chunk = 0; chunk < chunkCount; ++chunk)
            {
                const uint32_t begin = std::min(chunk * chunkSize, count);
                const uint32_t end = std::min(begin + chunkSize, count);
                group.silent_async(taskName, [&body, chunk, begin, end]() { body(chunk, begin, end); });
            }
            // The calling worker executes chunks too while it waits.
            group.corun();
        };

        if (m_Executor->this_worker_id() >= 0)
            dispatch();
        else
            m_Executor->async(taskName + " Dispatch", dispatch).get(); // task groups can only be created on a worker
    }

    void JobSystem::RunTasks(std::string_view name, std::span<const uint32_t> exclusiveMasks, const std::function<void(uint32_t index)>& task)
    {
        tf::Taskflow taskflow;
        const std::string taskName(name);
        for (uint32_t index = 0; index < exclusiveMasks.size(); ++index)
        {
            tf::Task node = taskflow.emplace([&task, index]() { task(index); }).name(taskName);
            for (uint32_t key = 0; key < MaxExclusiveKeys; ++key)
            {
                if ((exclusiveMasks[index] & (1u << key)) != 0)
                {
                    // Taskflow acquires all of a task's semaphores at once or none (no lock-order deadlock).
                    node.acquire(m_ExclusiveKeys[key]);
                    node.release(m_ExclusiveKeys[key]);
                }
            }
        }
        RunTaskflow(taskflow);
    }

    FrameArena& JobSystem::GetFrameArena()
    {
        return m_FrameArenas[GetCurrentThreadSlot()];
    }

    void JobSystem::ResetFrameArenas()
    {
        NOX_CORE_ASSERT(std::this_thread::get_id() == m_MainThreadId, "Frame arenas are reset on the main thread");
        for (uint32_t slot = 0; slot < GetThreadSlotCount(); ++slot)
            m_FrameArenas[slot].Reset();
    }

    void JobSystem::SubmitAsync(std::string_view name, bool io, std::function<void()> work)
    {
        tf::Executor& executor = io ? *m_IoExecutor : *m_Executor;
        executor.silent_async(std::string(name), std::move(work));
    }

    void JobSystem::RunTaskflow(tf::Taskflow& taskflow)
    {
        if (m_Executor->this_worker_id() >= 0)
            m_Executor->corun(taskflow);
        else
            m_Executor->run(taskflow).get();
    }
}
