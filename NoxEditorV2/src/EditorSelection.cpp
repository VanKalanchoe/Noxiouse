#include "EditorSelection.h"

#include <algorithm>

#include "NoxCore/Scene/Scene.h"

namespace Nox
{
    void EditorSelection::SetScene(const Ref<Scene>& scene)
    {
        m_Scene = scene;
        if (!m_Scene)
            m_Ids.clear();
        else
            std::erase_if(m_Ids, [this](UUID id) { return !m_Scene->GetEntityByUUID(id); });
        Changed(); // (the same ids are other handles now)
    }

    bool EditorSelection::Selectable(Entity entity) const
    {
        return entity && m_Scene && !m_Scene->IsEntityLocked(entity);
    }

    void EditorSelection::Select(Entity entity)
    {
        if (!Selectable(entity))
        {
            Clear();
            return;
        }
        const UUID id = entity.GetUUID();
        if (m_Ids.size() == 1 && m_Ids.front() == id)
            return;
        m_Ids.assign(1, id);
        Changed();
    }

    void EditorSelection::Set(const std::vector<Entity>& entities)
    {
        std::vector<UUID> ids;
        ids.reserve(entities.size());
        for (Entity entity : entities)
        {
            if (!Selectable(entity))
                continue;
            const UUID id = entity.GetUUID();
            if (std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.push_back(id);
        }
        if (ids == m_Ids)
            return;
        m_Ids = std::move(ids);
        Changed();
    }

    void EditorSelection::Add(Entity entity)
    {
        if (!Selectable(entity))
            return;
        const UUID id = entity.GetUUID();
        if (const auto found = std::find(m_Ids.begin(), m_Ids.end(), id); found != m_Ids.end())
        {
            if (found + 1 == m_Ids.end())
                return;
            m_Ids.erase(found); // (added again: it becomes the primary)
        }
        m_Ids.push_back(id);
        Changed();
    }

    void EditorSelection::Toggle(Entity entity)
    {
        if (!Selectable(entity))
            return;
        const UUID id = entity.GetUUID();
        if (const auto found = std::find(m_Ids.begin(), m_Ids.end(), id); found != m_Ids.end())
            m_Ids.erase(found);
        else
            m_Ids.push_back(id);
        Changed();
    }

    void EditorSelection::Remove(UUID id)
    {
        if (const auto found = std::find(m_Ids.begin(), m_Ids.end(), id); found != m_Ids.end())
        {
            m_Ids.erase(found);
            Changed();
        }
    }

    void EditorSelection::Clear()
    {
        if (m_Ids.empty())
            return;
        m_Ids.clear();
        Changed();
    }

    void EditorSelection::Prune()
    {
        if (m_Ids.empty())
            return;
        if (!m_Scene)
        {
            Clear();
            return;
        }
        const size_t before = m_Ids.size();
        std::erase_if(m_Ids, [this](UUID id) { return !m_Scene->GetEntityByUUID(id); });
        if (m_Ids.size() != before)
            Changed();
    }

    bool EditorSelection::Contains(UUID id) const
    {
        return std::find(m_Ids.begin(), m_Ids.end(), id) != m_Ids.end();
    }

    bool EditorSelection::Contains(Entity entity) const
    {
        return entity && Contains(entity.GetUUID());
    }

    const std::vector<Entity>& EditorSelection::GetEntities() const
    {
        if (m_ResolvedVersion != m_Version)
        {
            m_Entities.clear();
            if (m_Scene)
                for (UUID id : m_Ids)
                    if (Entity entity = m_Scene->GetEntityByUUID(id))
                        m_Entities.push_back(entity);
            m_ResolvedVersion = m_Version;
        }
        return m_Entities;
    }

    Entity EditorSelection::GetPrimary() const
    {
        const std::vector<Entity>& entities = GetEntities();
        return entities.empty() ? Entity{} : entities.back();
    }

    void EditorSelection::Changed()
    {
        ++m_Version;
    }
}
