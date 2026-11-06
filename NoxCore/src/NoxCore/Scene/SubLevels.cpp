// Sub-levels (Unreal's Levels window, Backlog L1): other scenes edited together with this one.

#include "Scene.h"

#include <algorithm>
#include <functional>

#include "Components.h"
#include "Entity.h"
#include "SceneSerializer.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    namespace
    {
        constexpr uint32_t MaxHierarchyDepth = 1024;

        Entity RootOf(Scene& scene, Entity entity)
        {
            for (uint32_t depth = 0; entity && depth < MaxHierarchyDepth; ++depth)
            {
                if (!entity.HasComponent<RelationshipComponent>())
                    return entity;
                const UUID parentID = entity.GetComponent<RelationshipComponent>().Parent;
                if (parentID == 0)
                    return entity;
                Entity parent = scene.GetEntityByUUID(parentID);
                if (!parent)
                    return entity;
                entity = parent;
            }
            return entity;
        }

        bool IsRoot(Entity entity)
        {
            return !entity.HasComponent<RelationshipComponent>() || entity.GetComponent<RelationshipComponent>().Parent == 0;
        }

        std::string LevelName(AssetHandle level)
        {
            return AssetManager::IsAssetHandleValid(level) ? AssetManager::GetMetadata(level).FilePath.stem().string()
                                                           : std::to_string(static_cast<uint64_t>(level));
        }
    }

    const Scene::SubLevel* Scene::FindSubLevel(AssetHandle level) const
    {
        for (const SubLevel& subLevel : m_SubLevels)
        {
            if (subLevel.Level == level)
                return &subLevel;
        }
        return nullptr;
    }

    std::vector<UUID> Scene::TreeOrder(Scene& scene, const std::vector<Entity>& roots)
    {
        std::vector<UUID> order;
        std::function<void(Entity, uint32_t)> visit = [&](Entity entity, uint32_t depth)
        {
            if (!entity || depth > MaxHierarchyDepth)
                return;
            order.push_back(entity.GetUUID());
            if (!entity.HasComponent<RelationshipComponent>())
                return;
            const std::vector<UUID> children = entity.GetComponent<RelationshipComponent>().Children;
            for (UUID child : children)
                visit(scene.GetEntityByUUID(child), depth + 1);
        };
        for (Entity root : roots)
            visit(root, 0);
        return order;
    }

    void Scene::TagLevelTree(Entity root, AssetHandle level)
    {
        std::function<void(Entity, uint32_t)> visit = [&](Entity entity, uint32_t depth)
        {
            if (!entity || depth > MaxHierarchyDepth)
                return;
            if (level != 0)
                entity.AddOrReplaceComponent<LevelComponent>().Level = level;
            else if (entity.HasComponent<LevelComponent>())
                entity.RemoveComponent<LevelComponent>();
            if (!entity.HasComponent<RelationshipComponent>())
                return;
            const std::vector<UUID> children = entity.GetComponent<RelationshipComponent>().Children;
            for (UUID child : children)
                visit(GetEntityByUUID(child), depth + 1);
        };
        visit(root, 0);
    }

    AssetHandle Scene::GetEntityLevel(Entity entity) const
    {
        if (!entity || m_SubLevels.empty())
            return 0;
        Entity root = RootOf(*const_cast<Scene*>(this), entity);
        if (!root.HasComponent<LevelComponent>())
            return 0;
        const AssetHandle level = root.GetComponent<LevelComponent>().Level;
        return FindSubLevel(level) ? level : AssetHandle(0);
    }

    bool Scene::IsEntityLocked(Entity entity) const
    {
        const AssetHandle level = GetEntityLevel(entity);
        const SubLevel* subLevel = level != 0 ? FindSubLevel(level) : nullptr;
        return subLevel && subLevel->Locked;
    }

    std::vector<Entity> Scene::GetSubLevelRoots(AssetHandle level, Scene*& outScene)
    {
        std::vector<Entity> roots;
        if (auto hidden = m_HiddenSubLevels.find(level); hidden != m_HiddenSubLevels.end())
        {
            outScene = hidden->second.Entities.get();
            for (UUID id : hidden->second.Roots)
            {
                if (Entity root = outScene->GetEntityByUUID(id))
                    roots.push_back(root);
            }
            return roots;
        }
        outScene = this;
        for (auto handle : m_Registry.view<LevelComponent, IDComponent>())
        {
            Entity entity(handle, this);
            if (m_Registry.get<LevelComponent>(handle).Level == level && IsRoot(entity))
                roots.push_back(entity);
        }
        // Sorted by ID: the same entities always give the same file.
        std::sort(roots.begin(), roots.end(), [](Entity a, Entity b) { return static_cast<uint64_t>(a.GetUUID()) < static_cast<uint64_t>(b.GetUUID()); });
        return roots;
    }

    bool Scene::AddSubLevel(AssetHandle level, bool alwaysLoaded)
    {
        if (level == 0 || !AssetManager::IsAssetHandleValid(level) || AssetManager::GetAssetType(level) != AssetType::Scene)
        {
            NOX_CORE_ERROR("Add Level: {} is not a scene asset", static_cast<uint64_t>(level));
            return false;
        }
        if (IsWorldPartitioned())
        {
            NOX_CORE_ERROR("Add Level: a world has no sub-levels (its cells are loaded by the World Partition panel)");
            return false;
        }
        if (FindSubLevel(level))
        {
            NOX_CORE_WARN("Add Level: `{}` is a sub-level already", LevelName(level));
            return false;
        }

        m_SubLevels.push_back({ level, alwaysLoaded });
        m_SubLevelEditing = true;
        if (!ReadSubLevelEntities(level))
        {
            m_SubLevels.pop_back();
            return false;
        }
        NOX_CORE_INFO("Level `{}` added", LevelName(level));
        return true;
    }

    bool Scene::ReadSubLevelEntities(AssetHandle level)
    {
        SubLevel* subLevel = const_cast<SubLevel*>(FindSubLevel(level));
        if (!subLevel)
            return false;
        subLevel->Loaded = false;

        const std::filesystem::path path = Project::GetActiveAssetDirectory() / AssetManager::GetMetadata(level).FilePath;
        Ref<Scene> file = CreateRef<Scene>();
        SceneSerializer serializer(file);
        if (!serializer.Deserialize(path))
        {
            NOX_CORE_ERROR("Level `{}` could not be read ({}): it stays listed, and is not written until it is read", LevelName(level), path.string());
            return false;
        }
        if (file->IsWorldPartitioned())
        {
            NOX_CORE_ERROR("Level `{}` is a world: a world cannot be a sub-level", LevelName(level));
            return false;
        }
        // None of its entities may be here already (the scene itself, or a copy of it saved under another name, shares its IDs).
        for (auto handle : file->m_Registry.view<IDComponent>())
        {
            const UUID id = file->m_Registry.get<IDComponent>(handle).ID;
            if (Entity existing = GetEntityByUUID(id))
            {
                NOX_CORE_ERROR("Level `{}` not added: its entity '{}' ({}) is in the scene already (a scene saved as a copy of another keeps its "
                               "entity IDs; make the level a new scene)", LevelName(level), existing.GetName(), static_cast<uint64_t>(id));
                return false;
            }
        }

        std::vector<Entity> roots;
        for (auto handle : file->m_Registry.view<IDComponent>())
        {
            Entity entity(handle, file.get());
            if (IsRoot(entity))
                roots.push_back(entity);
        }
        const std::vector<UUID> order = TreeOrder(*file, roots);
        CopyEntitiesFrom(*file, order);
        for (Entity root : roots)
            TagLevelTree(GetEntityByUUID(root.GetUUID()), level);
        for (const std::string& folder : file->GetFolders())
            AddFolder(folder);
        subLevel->Loaded = true;
        return true;
    }

    void Scene::LoadSubLevelsForEditing()
    {
        if (m_SubLevels.empty())
            return;
        m_SubLevelEditing = true;
        if (IsWorldPartitioned())
        {
            NOX_CORE_ERROR("The scene is a world and lists sub-levels: they are not read (a world has no sub-levels)");
            return;
        }
        for (const SubLevel& subLevel : std::vector<SubLevel>(m_SubLevels))
        {
            if (!ReadSubLevelEntities(subLevel.Level))
                continue;
            if (!subLevel.Visible)
                HideSubLevel(subLevel.Level);
        }
    }

    void Scene::RemoveSubLevel(AssetHandle level)
    {
        if (!FindSubLevel(level))
            return;
        if (!m_HiddenSubLevels.erase(level))
        {
            Scene* scene = nullptr;
            for (Entity root : GetSubLevelRoots(level, scene))
                DestroyEntity(root);
        }
        std::erase_if(m_SubLevels, [level](const SubLevel& subLevel) { return subLevel.Level == level; });
        if (m_CurrentLevel == level)
            m_CurrentLevel = 0;
        NOX_CORE_INFO("Level `{}` removed", LevelName(level));
    }

    void Scene::SetSubLevelAlwaysLoaded(AssetHandle level, bool alwaysLoaded)
    {
        if (SubLevel* subLevel = const_cast<SubLevel*>(FindSubLevel(level)))
            subLevel->AlwaysLoaded = alwaysLoaded;
    }

    void Scene::SetSubLevelVisible(AssetHandle level, bool visible)
    {
        SubLevel* subLevel = const_cast<SubLevel*>(FindSubLevel(level));
        if (!subLevel || subLevel->Visible == visible)
            return;
        subLevel->Visible = visible;
        if (!subLevel->Loaded)
            return;
        if (visible)
            ShowSubLevel(level);
        else
            HideSubLevel(level);
    }

    void Scene::SetSubLevelLocked(AssetHandle level, bool locked)
    {
        if (SubLevel* subLevel = const_cast<SubLevel*>(FindSubLevel(level)))
            subLevel->Locked = locked;
    }

    void Scene::SetCurrentLevel(AssetHandle level)
    {
        const SubLevel* subLevel = level != 0 ? FindSubLevel(level) : nullptr;
        if (level != 0 && (!subLevel || !subLevel->Loaded || !subLevel->Visible || subLevel->Locked))
            return; // new entities would vanish or be uneditable there
        m_CurrentLevel = level;
    }

    void Scene::HideSubLevel(AssetHandle level)
    {
        if (m_HiddenSubLevels.contains(level))
            return;
        Scene* scene = nullptr;
        const std::vector<Entity> roots = GetSubLevelRoots(level, scene);
        SubLevelStash stash;
        stash.Entities = CreateRef<Scene>();
        stash.Order = TreeOrder(*this, roots);
        stash.Entities->CopyEntitiesFrom(*this, stash.Order);
        for (Entity root : roots)
            stash.Roots.push_back(root.GetUUID());
        for (Entity root : roots)
            DestroyEntity(root);
        m_HiddenSubLevels.emplace(level, std::move(stash));
        if (m_CurrentLevel == level)
            m_CurrentLevel = 0;
    }

    void Scene::ShowSubLevel(AssetHandle level)
    {
        auto hidden = m_HiddenSubLevels.find(level);
        if (hidden == m_HiddenSubLevels.end())
            return;
        CopyEntitiesFrom(*hidden->second.Entities, hidden->second.Order);
        for (UUID id : hidden->second.Roots)
            TagLevelTree(GetEntityByUUID(id), level);
        m_HiddenSubLevels.erase(hidden);
    }

    void Scene::MoveToLevel(const std::vector<Entity>& entities, AssetHandle level)
    {
        const SubLevel* target = level != 0 ? FindSubLevel(level) : nullptr;
        if (level != 0 && (!target || !target->Loaded || !target->Visible || target->Locked))
        {
            NOX_CORE_WARN("Move to Level: `{}` is not loaded, visible and unlocked", LevelName(level));
            return;
        }
        std::vector<Entity> roots;
        for (Entity entity : entities)
        {
            Entity root = RootOf(*this, entity);
            if (!root || IsEntityLocked(root) || std::find(roots.begin(), roots.end(), root) != roots.end())
                continue;
            roots.push_back(root);
        }
        for (Entity root : roots)
            TagLevelTree(root, level);
        if (!roots.empty())
            NOX_CORE_INFO("{} entit{} moved to `{}`", roots.size(), roots.size() == 1 ? "y" : "ies", level != 0 ? LevelName(level) : std::string("the persistent level"));
    }
}
