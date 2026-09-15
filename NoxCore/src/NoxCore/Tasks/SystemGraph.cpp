#include "SystemGraph.h"

#include <algorithm>
#include <fstream>

#include <taskflow/taskflow.hpp>

#include "JobSystem.h"
#include "NoxCore/Core/core.h"
#include "NoxCore/Core/Log.h"

namespace Nox
{
    namespace
    {
        bool Intersects(const std::vector<entt::id_type>& a, const std::vector<entt::id_type>& b)
        {
            for (entt::id_type id : a)
            {
                if (std::find(b.begin(), b.end(), id) != b.end())
                    return true;
            }
            return false;
        }
    }

    bool ComponentAccess::ConflictsWith(const ComponentAccess& other) const
    {
        return Intersects(m_Writes, other.m_Writes) || Intersects(m_Writes, other.m_Reads) || Intersects(other.m_Writes, m_Reads);
    }

    SystemGraph::SystemGraph(std::string name) : m_Name(std::move(name)), m_Taskflow(std::make_unique<tf::Taskflow>(m_Name))
    {
    }

    SystemGraph::~SystemGraph() = default;

    SystemGraph::SystemId SystemGraph::AddSystem(std::string name, ComponentAccess access, std::function<void()> run)
    {
        m_Systems.push_back({ std::move(name), std::move(access), std::move(run), {} });
        m_Dirty = true;
        return static_cast<SystemId>(m_Systems.size() - 1);
    }

    void SystemGraph::RunAfter(SystemId system, SystemId dependency)
    {
        NOX_CORE_ASSERT(system < m_Systems.size() && dependency < m_Systems.size() && dependency != system, "Invalid system ordering");
        m_Systems[system].RunAfter.push_back(dependency);
        m_Dirty = true;
    }

    void SystemGraph::Run()
    {
        if (m_Dirty)
            Build();
        JobSystem::Get().RunTaskflow(*m_Taskflow);
    }

    bool SystemGraph::DumpDot(const std::filesystem::path& path)
    {
        if (m_Dirty)
            Build();

        std::ofstream file(path, std::ios::trunc);
        m_Taskflow->dump(file);
        if (!file)
        {
            NOX_CORE_ERROR("SystemGraph '{}': could not write {}", m_Name, path.string());
            return false;
        }
        return true;
    }

    void SystemGraph::Build()
    {
        m_Taskflow->clear();

        std::vector<tf::Task> tasks;
        tasks.reserve(m_Systems.size());
        for (System& system : m_Systems)
            tasks.push_back(m_Taskflow->emplace([&system]() { system.Run(); }).name(system.Name));

        for (size_t index = 0; index < m_Systems.size(); ++index)
        {
            // Registration order decides which of two conflicting systems runs first.
            for (size_t earlier = 0; earlier < index; ++earlier)
            {
                if (m_Systems[index].Access.ConflictsWith(m_Systems[earlier].Access))
                    tasks[earlier].precede(tasks[index]);
            }
            for (SystemId dependency : m_Systems[index].RunAfter)
                tasks[dependency].precede(tasks[index]);
        }

        m_Dirty = false;
    }
}
