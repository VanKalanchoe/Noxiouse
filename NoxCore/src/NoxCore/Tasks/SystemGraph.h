#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <entt/core/type_info.hpp>

namespace tf
{
    class Taskflow;
}

namespace Nox
{
    // What a system reads and writes (Decision D11: declared access). Any type can be named: ECS components, or empty
    // tag types for non-component resources a system fills (e.g. a renderer submission list).
    class ComponentAccess
    {
    public:
        template <typename... T>
        ComponentAccess& Read()
        {
            ((m_Reads.push_back(entt::type_hash<T>::value())), ...);
            return *this;
        }

        template <typename... T>
        ComponentAccess& Write()
        {
            ((m_Writes.push_back(entt::type_hash<T>::value())), ...);
            return *this;
        }

        // True when either side writes something the other reads or writes.
        bool ConflictsWith(const ComponentAccess& other) const;

    private:
        std::vector<entt::id_type> m_Reads;
        std::vector<entt::id_type> m_Writes;
    };

    // A frame graph of systems (§5.2.3, §5.3). Edges come from declared access: a system runs after every earlier
    // registered system it conflicts with; systems without conflicts run in parallel on the JobSystem workers.
    // Built once, run every frame. Systems must not make structural ECS changes (create/destroy entities, add/remove
    // components) - they record them into an EntityCommandBuffer instead.
    class SystemGraph
    {
    public:
        using SystemId = uint32_t;

        explicit SystemGraph(std::string name);
        ~SystemGraph();

        SystemGraph(const SystemGraph&) = delete;
        SystemGraph& operator=(const SystemGraph&) = delete;

        SystemId AddSystem(std::string name, ComponentAccess access, std::function<void()> run);
        // Ordering that is not a data conflict (e.g. a system consuming another system's side effect).
        void RunAfter(SystemId system, SystemId dependency);

        // Runs every system once and returns when all finished (cooperatively when called from a task).
        void Run();

        // GraphViz DOT of the built graph (tf::Taskflow::dump).
        bool DumpDot(const std::filesystem::path& path);
        const std::string& GetName() const { return m_Name; }

    private:
        struct System
        {
            std::string Name;
            ComponentAccess Access;
            std::function<void()> Run;
            std::vector<SystemId> RunAfter;
        };

        void Build();

    private:
        std::string m_Name;
        std::vector<System> m_Systems;
        std::unique_ptr<tf::Taskflow> m_Taskflow;
        bool m_Dirty = true;
    };
}
