#pragma once
#include <cstdint>
#include <string>

#include <taskflow/taskflow.hpp>

// Internal to the Tasks module: only Tasks/*.cpp include this header (Taskflow stays behind the module).
namespace Nox
{
    // Taskflow's observer hook (tf::ObserverInterface, the mechanism its own TFProf profiler uses): every task any
    // worker runs - graph tasks, parallel-for chunks, async and subflow tasks - becomes a profiler scope named after
    // the task, visible in Nox Stats and as a zone on the worker's thread in Tracy.
    class ProfilerTaskObserver final : public tf::ObserverInterface
    {
    public:
        void set_up(size_t workerCount) override;
        void on_entry(tf::WorkerView worker, tf::TaskView task) override;
        void on_exit(tf::WorkerView worker, tf::TaskView task) override;

    private:
        static uint32_t GetScopeId(const std::string& taskName);
    };
}
