#include "Entity.h"

namespace Nox
{
    Entity::Entity(entt::entity handle, Scene* scene) : m_EntityHandle(handle), m_Scene(scene)
    {
    }
}
