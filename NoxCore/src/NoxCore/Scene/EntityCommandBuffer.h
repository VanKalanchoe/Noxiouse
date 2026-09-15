#pragma once
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "NoxCore/Core/UUID.h"
#include "NoxCore/Tasks/JobSystem.h"

namespace Nox
{
    class Scene;

    // Deferred structural ECS changes (§5.2.5): systems running as tasks record entity creation/destruction and
    // component add/remove here instead of touching the registry, which is not safe while other tasks iterate it.
    // One buffer per thread slot (Scene keeps them in a WorkerLocal); the Scene applies them on the main thread in
    // slot order at its sync point. Commands live in the recording thread's frame arena until applied.
    // Recording component commands needs the complete Scene/Entity types: include "Entity.h" where they are recorded.
    class EntityCommandBuffer
    {
    public:
        EntityCommandBuffer() = default;
        ~EntityCommandBuffer();

        EntityCommandBuffer(const EntityCommandBuffer&) = delete;
        EntityCommandBuffer& operator=(const EntityCommandBuffer&) = delete;

        void CreateEntity(UUID uuid, std::string name);
        void DestroyEntity(UUID uuid);

        template <typename T>
        void AddOrReplaceComponent(UUID uuid, T component)
        {
            // Generic lambda: the Scene/Entity calls resolve where the command is applied (Scene is complete there).
            Record([uuid, component = std::move(component)](auto& scene) mutable
            {
                auto entity = scene.GetEntityByUUID(uuid);
                if (entity)
                    entity.template AddOrReplaceComponent<T>(std::move(component));
            });
        }

        template <typename T>
        void RemoveComponent(UUID uuid)
        {
            Record([uuid](auto& scene)
            {
                auto entity = scene.GetEntityByUUID(uuid);
                if (entity && entity.template HasComponent<T>())
                    entity.template RemoveComponent<T>();
            });
        }

        // Main thread, sync point: runs the commands in recording order, then drops them.
        void Apply(Scene& scene);
        bool IsEmpty() const { return m_Commands.empty(); }

    private:
        template <typename F>
        void Record(F&& function)
        {
            using Function = std::decay_t<F>;
            Command command;
            command.Payload = JobSystem::Get().GetFrameArena().New<Function>(std::forward<F>(function));
            command.Invoke = [](void* payload, Scene& scene) { (*static_cast<Function*>(payload))(scene); };
            command.Destroy = [](void* payload) { static_cast<Function*>(payload)->~Function(); };
            m_Commands.push_back(command);
        }

        void Clear();

    private:
        struct Command
        {
            void* Payload = nullptr;
            void (*Invoke)(void* payload, Scene& scene) = nullptr;
            void (*Destroy)(void* payload) = nullptr;
        };

    private:
        std::vector<Command> m_Commands;
    };
}
