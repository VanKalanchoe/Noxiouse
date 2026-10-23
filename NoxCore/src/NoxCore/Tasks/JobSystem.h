#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "FrameArena.h"

namespace tf
{
    class Executor;
    class Semaphore;
    class Taskflow;
}

namespace Nox
{
    // Shared cancellation flag between the submitter of async work and the work itself; the work polls it.
    class CancellationToken
    {
    public:
        CancellationToken() : m_Cancelled(std::make_shared<std::atomic<bool>>(false)) {}

        void Cancel() const { m_Cancelled->store(true, std::memory_order_relaxed); }
        bool IsCancelled() const { return m_Cancelled->load(std::memory_order_relaxed); }

    private:
        std::shared_ptr<std::atomic<bool>> m_Cancelled;
    };

    // Who waits for async work (§5.2.3: frame-critical > streaming > background cooking). Taskflow has no priorities, and a
    // worker waiting inside a frame task (corun) runs whatever task it finds, a long cook included. So, as Unreal's
    // background workers do, queued work runs on its own workers at low OS thread priority, which the frame workers never
    // pick up and the OS preempts for them; JobSystem admits it by priority (Normal before Background).
    enum class JobPriority : uint8_t
    {
        High,      // the frame waits for it (physics jobs: Async, started at once on the frame workers), or, on the IO workers, what
                   // is on screen now (texture streaming reads: AsyncIO, queued ahead of every other read)
        Normal,    // streaming and loading: queued, on the background workers
        Background // cooking and other one-off conversions: queued behind Normal, at most BackgroundJobLimit running
    };

    // Result of JobSystem::Async / AsyncIO. Poll IsReady() from the main thread; Get() blocks until the work is done
    // and rethrows its exception, so frame code only calls it once IsReady() is true.
    template <typename T>
    class TaskFuture
    {
    public:
        TaskFuture() = default;
        TaskFuture(std::future<T> future, CancellationToken token) : m_Future(std::move(future)), m_Token(std::move(token)) {}

        bool IsValid() const { return m_Future.valid(); }
        bool IsReady() const { return m_Future.valid() && m_Future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
        T Get() { return m_Future.get(); }
        // Blocks until the work is done, without taking its result (e.g. before freeing memory the work writes).
        void Wait() const { if (m_Future.valid()) m_Future.wait(); }
        void Cancel() const { m_Token.Cancel(); }
        const CancellationToken& GetToken() const { return m_Token; }

    private:
        std::future<T> m_Future;
        CancellationToken m_Token;
    };

    // The engine's only owner of threads (§5.2): one Taskflow executor for all CPU work (frame graph, parallel loops,
    // async jobs) and a small IO executor for blocking file IO. Every task on either executor is profiled through a
    // Taskflow observer. Created first by Application and destroyed last. Taskflow stays behind this module.
    class JobSystem
    {
    public:
        static constexpr uint32_t IoWorkerCount = 2;
        // Distinct exclusivity keys for RunTasks (bit indices of a mask).
        static constexpr uint32_t MaxExclusiveKeys = 32;

        JobSystem();
        ~JobSystem();

        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;

        static JobSystem& Get();

        uint32_t GetWorkerCount() const { return m_WorkerCount; }

        // Thread slots for WorkerLocal state: frame workers [0, workers), background workers next, IO workers next, the main
        // thread last.
        uint32_t GetThreadSlotCount() const { return m_WorkerCount + m_BackgroundWorkerCount + IoWorkerCount + 1; }
        uint32_t GetCurrentThreadSlot() const;

        // Deterministic split of [0, count): depends only on count, minBatchSize and the worker count, so data merged
        // by chunk index is identical to a serial loop.
        uint32_t GetChunkCount(uint32_t count, uint32_t minBatchSize) const;

        // Runs body(chunk, begin, end) for every chunk and returns when all are done. From a task the calling worker
        // keeps executing other tasks while it waits; from the main thread it blocks.
        void ParallelFor(std::string_view name, uint32_t count, uint32_t minBatchSize,
                         const std::function<void(uint32_t chunk, uint32_t begin, uint32_t end)>& body);

        // Runs task(index) for every index of exclusiveMasks and returns when all are done (from a task the calling worker
        // helps; from the main thread it blocks). Tasks whose masks share a bit never run at the same time, for work that
        // calls into libraries that are not thread-safe: a task waiting for a key is parked by the scheduler (tf::Semaphore),
        // no worker blocks on it.
        void RunTasks(std::string_view name, std::span<const uint32_t> exclusiveMasks, const std::function<void(uint32_t index)>& task);

        // Work outside the frame graph (physics jobs, loading, cooking). The work receives the future's cancellation token.
        // A ParallelFor inside Normal or Background work runs on at most the workers its priority may still use.
        template <typename F>
        auto Async(std::string_view name, JobPriority priority, F&& work)
        {
            return MakeAsync(name, priority, std::forward<F>(work), false);
        }

        // Only blocking IO that cannot use non-blocking APIs; CPU-heavy decode continues through Async (§5.2.2). The IO
        // workers take queued reads by priority: a High read (texture streaming) waits for the reads already running only.
        // (Every read is queued here: a task a worker submits goes to that worker's own queue, ahead of one submitted
        // from outside, so a High read handed to Taskflow directly would wait for every read the workers chain.)
        template <typename F>
        auto AsyncIO(std::string_view name, JobPriority priority, F&& work)
        {
            return MakeAsync(name, priority, std::forward<F>(work), true);
        }

        // The workers queued (Normal and Background) work runs on, and how many of them Background work may use at once.
        uint32_t GetBackgroundWorkerCount() const { return m_BackgroundWorkerCount; }
        uint32_t GetBackgroundJobLimit() const { return m_BackgroundJobLimit; }
        // Queued work not started yet (CPU and IO), e.g. for a loading indicator.
        uint32_t GetQueuedJobCount() const;

        // Frame arena of the calling thread's slot.
        FrameArena& GetFrameArena();
        // Frame sync point, main thread, while no frame graph task runs.
        void ResetFrameArenas();

    private:
        friend class SystemGraph;

        template <typename F>
        auto MakeAsync(std::string_view name, JobPriority priority, F&& work, bool io)
        {
            using Result = std::invoke_result_t<F, const CancellationToken&>;

            CancellationToken token;
            auto promise = std::make_shared<std::promise<Result>>();
            TaskFuture<Result> future(promise->get_future(), token);

            // shared_ptr: std::function needs a copyable callable, the work itself may be move-only.
            auto callable = std::make_shared<std::decay_t<F>>(std::forward<F>(work));
            SubmitAsync(name, priority, io, [promise, token, callable]()
            {
                try
                {
                    if constexpr (std::is_void_v<Result>)
                    {
                        (*callable)(token);
                        promise->set_value();
                    }
                    else
                    {
                        promise->set_value((*callable)(token));
                    }
                }
                catch (...)
                {
                    promise->set_exception(std::current_exception());
                }
            });
            return future;
        }

        // Queued work of one executor, by priority (High: IO reads only, Normal, Background), and how many of each run.
        struct AdmissionQueue
        {
            struct Job
            {
                std::string Name;
                std::function<void()> Work;
            };

            std::deque<Job> Pending[3];
            uint32_t Running[3] = {};
        };

        void SubmitAsync(std::string_view name, JobPriority priority, bool io, std::function<void()> work);
        // Starts queued work while the limits allow (lock held); the jobs to launch are collected and launched unlocked.
        void CollectAdmissible(AdmissionQueue& queue, bool io, std::vector<std::pair<JobPriority, AdmissionQueue::Job>>& out);
        void Launch(bool io, JobPriority priority, AdmissionQueue::Job job);
        // A queued job (or slot) of `finished` priority is done: the next queued work starts.
        void Admit(bool io, JobPriority finished);
        // Takes up to `wanted` background worker slots of a priority without queueing (ParallelFor helpers), and gives them back.
        uint32_t AcquireSlots(JobPriority priority, uint32_t wanted);
        void ReleaseSlot(JobPriority priority);
        // Runs a taskflow to completion: cooperative wait on a worker, blocking on any other thread.
        void RunTaskflow(tf::Taskflow& taskflow);

    private:
        uint32_t m_WorkerCount = 0;
        std::thread::id m_MainThreadId;
        std::unique_ptr<FrameArena[]> m_FrameArenas;
        std::unique_ptr<tf::Executor> m_Executor;
        std::unique_ptr<tf::Executor> m_BackgroundExecutor;
        std::unique_ptr<tf::Executor> m_IoExecutor;
        std::unique_ptr<tf::Semaphore[]> m_ExclusiveKeys; // one binary semaphore per key, see RunTasks

        mutable std::mutex m_AdmissionMutex;
        AdmissionQueue m_CpuQueue;
        AdmissionQueue m_IoQueue;
        uint32_t m_BackgroundWorkerCount = 1;
        uint32_t m_BackgroundJobLimit = 1;
        bool m_ShuttingDown = false;
    };
}
