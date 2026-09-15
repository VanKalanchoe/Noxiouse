#include "TaskObserver.h"

#include <unordered_map>

#include "NoxCore/Profiling/Profiler.h"

namespace Nox
{
    void ProfilerTaskObserver::set_up(size_t)
    {
    }

    void ProfilerTaskObserver::on_entry(tf::WorkerView, tf::TaskView task)
    {
        Profiler::Get().BeginCpuScope(GetScopeId(task.name()));
    }

    void ProfilerTaskObserver::on_exit(tf::WorkerView, tf::TaskView task)
    {
        Profiler::Get().EndCpuScope(GetScopeId(task.name()));
    }

    uint32_t ProfilerTaskObserver::GetScopeId(const std::string& taskName)
    {
        // Per-thread cache: after the first run of a task name on a worker, entering a task costs one hash lookup and
        // no lock. The profiler registry hands out one id per distinct name.
        thread_local std::unordered_map<std::string, uint32_t> t_ScopeIds;

        static const std::string UnnamedTask = "Unnamed Task";
        const std::string& name = taskName.empty() ? UnnamedTask : taskName;

        if (auto found = t_ScopeIds.find(name); found != t_ScopeIds.end())
            return found->second;

        const uint32_t scopeId = Profiler::Get().RegisterNamedScope(name);
        t_ScopeIds.emplace(name, scopeId);
        return scopeId;
    }
}
