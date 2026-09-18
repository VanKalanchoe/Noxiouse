#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

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

        // Thread slots for WorkerLocal state: CPU workers [0, workers), IO workers next, the main thread last.
        uint32_t GetThreadSlotCount() const { return m_WorkerCount + IoWorkerCount + 1; }
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

        // Work outside the frame graph (loading, cooking). The work receives the future's cancellation token.
        template <typename F>
        auto Async(std::string_view name, F&& work)
        {
            return MakeAsync(name, std::forward<F>(work), false);
        }

        // Only blocking IO that cannot use non-blocking APIs; CPU-heavy decode continues through Async (§5.2.2).
        template <typename F>
        auto AsyncIO(std::string_view name, F&& work)
        {
            return MakeAsync(name, std::forward<F>(work), true);
        }

        // Frame arena of the calling thread's slot.
        FrameArena& GetFrameArena();
        // Frame sync point, main thread, while no frame graph task runs.
        void ResetFrameArenas();

    private:
        friend class SystemGraph;

        template <typename F>
        auto MakeAsync(std::string_view name, F&& work, bool io)
        {
            using Result = std::invoke_result_t<F, const CancellationToken&>;

            CancellationToken token;
            auto promise = std::make_shared<std::promise<Result>>();
            TaskFuture<Result> future(promise->get_future(), token);

            // shared_ptr: std::function needs a copyable callable, the work itself may be move-only.
            auto callable = std::make_shared<std::decay_t<F>>(std::forward<F>(work));
            SubmitAsync(name, io, [promise, token, callable]()
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

        void SubmitAsync(std::string_view name, bool io, std::function<void()> work);
        // Runs a taskflow to completion: cooperative wait on a worker, blocking on any other thread.
        void RunTaskflow(tf::Taskflow& taskflow);

    private:
        uint32_t m_WorkerCount = 0;
        std::thread::id m_MainThreadId;
        std::unique_ptr<FrameArena[]> m_FrameArenas;
        std::unique_ptr<tf::Executor> m_Executor;
        std::unique_ptr<tf::Executor> m_IoExecutor;
        std::unique_ptr<tf::Semaphore[]> m_ExclusiveKeys; // one binary semaphore per key, see RunTasks
    };
}
