#include "JobSystem.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <latch>
#include <string>

#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_thread.h>
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
        // The priority of the queued work this thread runs now (High: frame work, or no queued work).
        thread_local JobPriority t_JobPriority = JobPriority::High;

        // Index into AdmissionQueue::Pending / Running.
        uint32_t QueueIndex(JobPriority priority) { return static_cast<uint32_t>(priority); }

        // Sets the thread's priority for a scope.
        class ScopedJobPriority
        {
        public:
            explicit ScopedJobPriority(JobPriority priority) : m_Previous(t_JobPriority) { t_JobPriority = priority; }
            ~ScopedJobPriority() { t_JobPriority = m_Previous; }

        private:
            JobPriority m_Previous;
        };

        // Parallel-for chunks per worker: headroom for uneven chunk costs without flooding the queues.
        constexpr uint32_t ChunksPerWorker = 4;

        // Runs on each worker thread before it enters Taskflow's scheduling loop: assigns the thread's WorkerLocal
        // slot, names it for the profilers and lowers the OS priority of background workers.
        class NoxWorkerInterface final : public tf::WorkerInterface
        {
        public:
            NoxWorkerInterface(const char* namePrefix, uint32_t firstSlot, bool lowPriority = false)
                : m_NamePrefix(namePrefix), m_FirstSlot(firstSlot), m_LowPriority(lowPriority) {}

            void scheduler_prologue(tf::Worker& worker) override
            {
                t_ThreadSlot = m_FirstSlot + static_cast<uint32_t>(worker.id());
                const std::string name = std::format("{} {}", m_NamePrefix, worker.id());
                Profiler::Get().SetThreadName(name.c_str());
                if (m_LowPriority && !SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW))
                    NOX_CORE_WARN("JobSystem: {} keeps normal thread priority ({})", name, SDL_GetError());
            }

            void scheduler_epilogue(tf::Worker&, std::exception_ptr) override
            {
            }

        private:
            const char* m_NamePrefix;
            uint32_t m_FirstSlot;
            bool m_LowPriority;
        };
    }

    JobSystem::JobSystem()
    {
        NOX_CORE_ASSERT(!s_Instance, "JobSystem already exists");
        s_Instance = this;

        m_MainThreadId = std::this_thread::get_id();
        m_WorkerCount = static_cast<uint32_t>(std::max(1, SDL_GetNumLogicalCPUCores() - 1));
        // As many background workers as frame workers (at low OS priority they take what the frame leaves); Background work
        // leaves a quarter of them (at least one) free for streaming. Set before anything sized by GetThreadSlotCount.
        m_BackgroundWorkerCount = m_WorkerCount;
        m_BackgroundJobLimit = m_BackgroundWorkerCount > 1 ? m_BackgroundWorkerCount - std::max(1u, m_BackgroundWorkerCount / 4) : 1u;

        m_FrameArenas = std::make_unique<FrameArena[]>(GetThreadSlotCount());
        t_ThreadSlot = GetThreadSlotCount() - 1;

        m_ExclusiveKeys = std::make_unique<tf::Semaphore[]>(MaxExclusiveKeys);
        for (uint32_t key = 0; key < MaxExclusiveKeys; ++key)
            m_ExclusiveKeys[key].reset(1);

        m_Executor = std::make_unique<tf::Executor>(m_WorkerCount, std::make_shared<NoxWorkerInterface>("Nox Worker", 0));
        m_BackgroundExecutor = std::make_unique<tf::Executor>(m_BackgroundWorkerCount,
                                                              std::make_shared<NoxWorkerInterface>("Nox Background", m_WorkerCount, true));
        m_IoExecutor = std::make_unique<tf::Executor>(IoWorkerCount, std::make_shared<NoxWorkerInterface>("Nox IO", m_WorkerCount + m_BackgroundWorkerCount));

#if NOX_PROFILING_ENABLED
        // make_observer is not thread-safe: register before the first task is submitted.
        m_Executor->make_observer<ProfilerTaskObserver>();
        m_BackgroundExecutor->make_observer<ProfilerTaskObserver>();
        m_IoExecutor->make_observer<ProfilerTaskObserver>();
#endif

        NOX_CORE_INFO("JobSystem: {} frame workers, {} background workers (low priority, {} for Background jobs), {} IO workers",
                      m_WorkerCount, m_BackgroundWorkerCount, m_BackgroundJobLimit, IoWorkerCount);
    }

    JobSystem::~JobSystem()
    {
        // Queued work that has not started is dropped (its futures report a broken promise); executors wait for submitted
        // work and join their threads before the frame arenas go away.
        {
            std::scoped_lock lock(m_AdmissionMutex);
            m_ShuttingDown = true;
            for (AdmissionQueue* queue : { &m_CpuQueue, &m_IoQueue })
            {
                for (std::deque<AdmissionQueue::Job>& pending : queue->Pending)
                    pending.clear();
            }
        }
        m_IoExecutor.reset();
        m_BackgroundExecutor.reset();
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

        // Inside queued work: the same chunks (the layout stays deterministic), run by as many helpers as the work's priority
        // may still use beside the calling thread, which runs chunks too.
        if (const JobPriority priority = t_JobPriority; priority != JobPriority::High)
        {
            std::atomic<uint32_t> nextChunk = 0;
            auto drain = [&]()
            {
                for (uint32_t chunk = nextChunk.fetch_add(1, std::memory_order_relaxed); chunk < chunkCount;
                     chunk = nextChunk.fetch_add(1, std::memory_order_relaxed))
                {
                    const uint32_t begin = std::min(chunk * chunkSize, count);
                    body(chunk, begin, std::min(begin + chunkSize, count));
                }
            };
            const uint32_t helpers = AcquireSlots(priority, chunkCount - 1);
            if (helpers > 0)
            {
                std::latch done(helpers);
                for (uint32_t helper = 0; helper < helpers; ++helper)
                {
                    m_BackgroundExecutor->silent_async(taskName, [this, &drain, &done, priority]()
                    {
                        {
                            ScopedJobPriority scoped(priority);
                            drain();
                        }
                        ReleaseSlot(priority);
                        done.count_down(); // last: the caller's locals go away once every helper counted down
                    });
                }
                drain();
                // Blocking: the slots taken are background workers that were free, so the helpers get to run.
                done.wait();
            }
            else
                drain();
            return;
        }

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

    void JobSystem::SubmitAsync(std::string_view name, JobPriority priority, bool io, std::function<void()> work)
    {
        if (priority == JobPriority::High && !io)
        {
            m_Executor->silent_async(std::string(name), std::move(work));
            return;
        }

        std::vector<std::pair<JobPriority, AdmissionQueue::Job>> launch;
        {
            std::scoped_lock lock(m_AdmissionMutex);
            if (m_ShuttingDown)
                return;
            AdmissionQueue& queue = io ? m_IoQueue : m_CpuQueue;
            queue.Pending[QueueIndex(priority)].push_back({ std::string(name), std::move(work) });
            CollectAdmissible(queue, io, launch);
        }
        for (auto& [jobPriority, job] : launch)
            Launch(io, jobPriority, std::move(job));
    }

    void JobSystem::CollectAdmissible(AdmissionQueue& queue, bool io, std::vector<std::pair<JobPriority, AdmissionQueue::Job>>& out)
    {
        if (m_ShuttingDown)
            return;
        // The IO workers: all for High and Normal reads, one for Background reads. (Only IO queues hold High work.)
        const uint32_t limit = io ? IoWorkerCount : m_BackgroundWorkerCount;
        const uint32_t backgroundLimit = io ? 1u : m_BackgroundJobLimit;
        auto running = [&queue]() { return queue.Running[0] + queue.Running[1] + queue.Running[2]; };
        for (JobPriority priority : { JobPriority::High, JobPriority::Normal, JobPriority::Background })
        {
            std::deque<AdmissionQueue::Job>& pending = queue.Pending[QueueIndex(priority)];
            while (!pending.empty() && running() < limit && (priority != JobPriority::Background || queue.Running[2] < backgroundLimit))
            {
                out.emplace_back(priority, std::move(pending.front()));
                pending.pop_front();
                queue.Running[QueueIndex(priority)]++;
            }
            if (!pending.empty())
                return; // lower priorities wait while this one waits
        }
    }

    void JobSystem::Launch(bool io, JobPriority priority, AdmissionQueue::Job job)
    {
        tf::Executor& executor = io ? *m_IoExecutor : *m_BackgroundExecutor;
        executor.silent_async(job.Name, [this, io, priority, work = std::move(job.Work)]()
        {
            {
                ScopedJobPriority scoped(priority);
                work();
            }
            Admit(io, priority);
        });
    }

    void JobSystem::Admit(bool io, JobPriority finished)
    {
        std::vector<std::pair<JobPriority, AdmissionQueue::Job>> launch;
        {
            std::scoped_lock lock(m_AdmissionMutex);
            AdmissionQueue& queue = io ? m_IoQueue : m_CpuQueue;
            queue.Running[QueueIndex(finished)]--;
            CollectAdmissible(queue, io, launch);
        }
        for (auto& [jobPriority, job] : launch)
            Launch(io, jobPriority, std::move(job));
    }

    uint32_t JobSystem::AcquireSlots(JobPriority priority, uint32_t wanted)
    {
        std::scoped_lock lock(m_AdmissionMutex);
        AdmissionQueue& queue = m_CpuQueue;
        const uint32_t index = QueueIndex(priority);
        // Helpers never pass queued work of their priority or above.
        for (uint32_t above = 0; above <= index; ++above)
        {
            if (!queue.Pending[above].empty())
                return 0;
        }
        const uint32_t running = queue.Running[0] + queue.Running[1] + queue.Running[2];
        uint32_t available = running < m_BackgroundWorkerCount ? m_BackgroundWorkerCount - running : 0;
        if (priority == JobPriority::Background)
            available = std::min(available, queue.Running[2] < m_BackgroundJobLimit ? m_BackgroundJobLimit - queue.Running[2] : 0u);
        const uint32_t taken = std::min(wanted, available);
        queue.Running[index] += taken;
        return taken;
    }

    void JobSystem::ReleaseSlot(JobPriority priority)
    {
        Admit(false, priority);
    }

    uint32_t JobSystem::GetQueuedJobCount() const
    {
        std::scoped_lock lock(m_AdmissionMutex);
        size_t queued = 0;
        for (const AdmissionQueue* queue : { &m_CpuQueue, &m_IoQueue })
        {
            for (const std::deque<AdmissionQueue::Job>& pending : queue->Pending)
                queued += pending.size();
        }
        return static_cast<uint32_t>(queued);
    }

    void JobSystem::RunTaskflow(tf::Taskflow& taskflow)
    {
        if (m_Executor->this_worker_id() >= 0)
            m_Executor->corun(taskflow);
        else
            m_Executor->run(taskflow).get();
    }
}
