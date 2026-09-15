#include "EntityCommandBuffer.h"

#include "Entity.h"
#include "Scene.h"

namespace Nox
{
    EntityCommandBuffer::~EntityCommandBuffer()
    {
        Clear();
    }

    void EntityCommandBuffer::CreateEntity(UUID uuid, std::string name)
    {
        Record([uuid, name = std::move(name)](Scene& scene) { scene.CreateEntityWithUUID(uuid, name); });
    }

    void EntityCommandBuffer::DestroyEntity(UUID uuid)
    {
        Record([uuid](Scene& scene)
        {
            if (Entity entity = scene.GetEntityByUUID(uuid))
                scene.DestroyEntity(entity);
        });
    }

    void EntityCommandBuffer::Apply(Scene& scene)
    {
        for (const Command& command : m_Commands)
            command.Invoke(command.Payload, scene);
        Clear();
    }

    void EntityCommandBuffer::Clear()
    {
        // Payload memory belongs to the frame arena; only the destructors run here.
        for (const Command& command : m_Commands)
            command.Destroy(command.Payload);
        m_Commands.clear();
    }
}
