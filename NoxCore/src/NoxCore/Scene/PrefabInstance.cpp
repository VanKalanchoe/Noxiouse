#include "PrefabInstance.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "Entity.h"
#include "Prefab.h"
#include "Scene.h"
#include "SceneSerializer.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/PrefabImporter.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Core/Log.h"

namespace Nox
{
    namespace
    {
        // Equal values, numbers to a small tolerance ("1" and "1.0" are the same number).
        bool sameValue(const YAML::Node& a, const YAML::Node& b)
        {
            if (a.Type() != b.Type())
                return false;
            switch (a.Type())
            {
            case YAML::NodeType::Scalar:
            {
                const std::string x = a.Scalar();
                const std::string y = b.Scalar();
                if (x == y)
                    return true;
                try
                {
                    size_t usedX = 0, usedY = 0;
                    const double numberX = std::stod(x, &usedX);
                    const double numberY = std::stod(y, &usedY);
                    if (usedX == x.size() && usedY == y.size())
                        return std::abs(numberX - numberY) <= 1e-5 * std::max(1.0, std::max(std::abs(numberX), std::abs(numberY)));
                }
                catch (...)
                {
                }
                return false;
            }
            case YAML::NodeType::Sequence:
            {
                if (a.size() != b.size())
                    return false;
                for (size_t i = 0; i < a.size(); ++i)
                {
                    if (!sameValue(a[i], b[i]))
                        return false;
                }
                return true;
            }
            case YAML::NodeType::Map:
            {
                if (a.size() != b.size())
                    return false;
                for (auto it = a.begin(); it != a.end(); ++it)
                {
                    const YAML::Node other = b[it->first.as<std::string>()];
                    if (!other || !sameValue(it->second, other))
                        return false;
                }
                return true;
            }
            default:
                return true;
            }
        }

        // A value as flow-style YAML text on one line.
        std::string valueText(const YAML::Node& node)
        {
            YAML::Emitter out;
            out.SetSeqFormat(YAML::Flow);
            out.SetMapFormat(YAML::Flow);
            out << node;
            return out.c_str();
        }

        // The fields of `current` that differ from `baseline` (the prefab's text of one entity).
        void diffEntity(const YAML::Node& baseline, const YAML::Node& current, uint64_t localId, bool isRoot, std::vector<PrefabPropertyOverride>& out)
        {
            // Placement, structure and references are not properties: the root's own name and transform are the instance's, parent
            // and child links belong to the structure, and entity references are remapped per instance.
            static const std::unordered_set<std::string> skippedComponents = { "Entity", "RelationshipComponent", "FolderComponent", "PrefabInstanceComponent" };
            static const std::unordered_set<std::string> skippedFields = { "NodeEntities", "EntityReferences" };

            for (auto component = baseline.begin(); component != baseline.end(); ++component)
            {
                const std::string componentName = component->first.as<std::string>();
                if (skippedComponents.contains(componentName))
                    continue;
                if (isRoot && (componentName == "TagComponent" || componentName == "TransformComponent"))
                    continue;

                const YAML::Node baselineFields = component->second;
                const YAML::Node currentFields = current[componentName];
                if (!currentFields || !baselineFields.IsMap() || !currentFields.IsMap())
                    continue;

                for (auto field = currentFields.begin(); field != currentFields.end(); ++field)
                {
                    const std::string fieldName = field->first.as<std::string>();
                    if (skippedFields.contains(fieldName))
                        continue;
                    const YAML::Node baselineValue = baselineFields[fieldName];
                    if (baselineValue && sameValue(baselineValue, field->second))
                        continue;
                    out.push_back({ localId, componentName, fieldName, valueText(field->second) });
                }
            }
        }

        // What an instance root keeps when it respawns: its own placement. Everything else came from the prefab.
        template <typename Component>
        void removeIfProvided(Entity entity)
        {
            if constexpr (!std::is_same_v<Component, TransformComponent> && !std::is_same_v<Component, WorldTransformComponent> &&
                          !std::is_same_v<Component, RelationshipComponent> && !std::is_same_v<Component, DirtyTransformComponent> &&
                          !std::is_same_v<Component, FolderComponent> && !std::is_same_v<Component, PrefabInstanceComponent> &&
                          !std::is_same_v<Component, PrefabNodeComponent>)
            {
                if (entity.HasComponent<Component>())
                    entity.RemoveComponent<Component>();
            }
        }

        template <typename... Components>
        void removeProvided(Entity entity, ComponentGroup<Components...>)
        {
            (removeIfProvided<Components>(entity), ...);
        }

        // The prefab's text of one entity with an instance's overrides patched in: a copy, the prefab's own text is never touched.
        // (Beware of yaml-cpp: assigning a node to another that shares data with the prefab's overwrites the prefab's data, so
        // the copy is returned by value and never assigned over a shared node.)
        YAML::Node patchedEntityNode(const YAML::Node& node, uint64_t fileID, const std::vector<PrefabPropertyOverride>& overrides,
                                     const std::vector<PrefabStructureChange>& structure)
        {
            const bool hasOverride = std::any_of(overrides.begin(), overrides.end(), [&](const PrefabPropertyOverride& o) { return o.Entity == fileID; }) ||
                                     std::any_of(structure.begin(), structure.end(), [&](const PrefabStructureChange& c) { return c.Entity == fileID; });
            if (!hasOverride)
                return node;

            YAML::Node copy = YAML::Clone(node);
            for (const PrefabStructureChange& change : structure)
            {
                if (change.Entity != fileID)
                    continue;
                if (change.Kind == PrefabStructureChange::ChangeKind::RemovedComponent)
                    copy.remove(change.Component);
                else if (change.Kind == PrefabStructureChange::ChangeKind::AddedComponent && !copy[change.Component])
                    copy[change.Component] = YAML::Load(change.Value);
            }
            for (const PrefabPropertyOverride& propertyOverride : overrides)
            {
                if (propertyOverride.Entity != fileID)
                    continue;
                YAML::Node component = copy[propertyOverride.Component];
                if (component && component.IsMap())
                    component[propertyOverride.Field] = YAML::Load(propertyOverride.Value);
            }
            return copy;
        }

        // An inner prefab instance's node with the changes of an outer layer (a variant, an outer instance) put in place of the ones its text has.
        YAML::Node withNestedChange(const YAML::Node& node, const PrefabNestedChange& change)
        {
            YAML::Node copy = YAML::Clone(node);
            YAML::Node component = copy["PrefabInstanceComponent"];
            if (!component || !component.IsMap())
                return copy;
            for (const char* key : { "Overrides", "Structure", "Nested" })
                component.remove(key);
            const YAML::Node changes = SceneSerializer::PrefabChangesToNode(change.Overrides, change.Structure, change.Nested);
            for (auto it = changes.begin(); it != changes.end(); ++it)
                component[it->first.as<std::string>()] = it->second;
            return copy;
        }

        // The full text of a prefab: for a variant, its base's (resolved the same way) with the variant's changes baked in and its own
        // entities added; else its own entities. Instances patch their overrides on top of this.
        std::vector<YAML::Node> effectiveNodes(const Prefab& prefab, int depth = 0)
        {
            std::vector<YAML::Node> result;
            if (prefab.Base != 0)
            {
                Ref<Prefab> base = depth < 16 ? AssetManager::GetAsset<Prefab>(prefab.Base) : Ref<Prefab>();
                if (!base)
                {
                    NOX_CORE_ERROR("Prefab '{}': its base {} could not be loaded (or the variants form a cycle)", prefab.Name, static_cast<uint64_t>(prefab.Base));
                }
                else
                {
                    const std::vector<YAML::Node> baseNodes = effectiveNodes(*base, depth + 1);

                    // Entities the variant deleted, with everything below them.
                    std::unordered_map<uint64_t, uint64_t> parentOf;
                    for (const YAML::Node node : baseNodes)
                    {
                        const auto relationship = node["RelationshipComponent"];
                        if (relationship && relationship["Parent"])
                            parentOf[node["Entity"].as<uint64_t>()] = relationship["Parent"].as<uint64_t>();
                    }
                    std::unordered_set<uint64_t> removed;
                    for (const PrefabStructureChange& change : prefab.Structure)
                    {
                        if (change.Kind == PrefabStructureChange::ChangeKind::RemovedEntity && change.Entity != prefab.Root)
                            removed.insert(change.Entity);
                    }
                    for (bool grew = !removed.empty(); grew;)
                    {
                        grew = false;
                        for (const auto& [id, parent] : parentOf)
                        {
                            if (!removed.contains(id) && removed.contains(parent))
                            {
                                removed.insert(id);
                                grew = true;
                            }
                        }
                    }

                    for (const YAML::Node node : baseNodes)
                    {
                        const uint64_t id = node["Entity"].as<uint64_t>();
                        if (removed.contains(id))
                            continue;
                        YAML::Node patched = patchedEntityNode(node, id, prefab.Overrides, prefab.Structure);
                        for (const PrefabNestedChange& nested : prefab.Nested)
                        {
                            // (a new node each time: assigning over `patched` would overwrite data it may share with the base's text)
                            if (nested.Entity == id)
                                result.push_back(withNestedChange(patched, nested));
                        }
                        if (std::none_of(prefab.Nested.begin(), prefab.Nested.end(), [&](const PrefabNestedChange& nested) { return nested.Entity == id; }))
                            result.push_back(patched);
                    }
                }
            }
            if (prefab.Entities)
            {
                for (const YAML::Node node : *prefab.Entities)
                    result.push_back(node);
            }
            return result;
        }

        // The prefab's entities under `root` (the instance root stands for the prefab's root entity: it keeps its own name,
        // transform and parent, and gets the prefab root's other components).
        void spawn(Scene& scene, Entity root, const Prefab& prefab, bool initTransform, std::vector<Entity>* outSpawned = nullptr)
        {
            const UUID rootID = root.GetUUID();
            const std::vector<YAML::Node> nodes = effectiveNodes(prefab);

            // The prefab file's entity id -> the entity's UUID in this scene.
            std::unordered_map<uint64_t, UUID> ids;
            for (const YAML::Node node : nodes)
            {
                const uint64_t fileID = node["Entity"].as<uint64_t>();
                ids[fileID] = fileID == prefab.Root ? rootID : PrefabInstance::NodeUUID(rootID, fileID);
            }
            if (!ids.contains(prefab.Root))
            {
                NOX_CORE_ERROR("Prefab '{}' has no entity with its root id {}", prefab.Name, prefab.Root);
                return;
            }

            // Entities deleted on this instance, with everything below them (by the prefab's own parent links).
            const std::vector<PrefabStructureChange> structure = root.GetComponent<PrefabInstanceComponent>().Structure;
            {
                auto& removedNames = root.GetComponent<PrefabInstanceComponent>().RemovedNames;
                for (const PrefabStructureChange& change : structure)
                {
                    if (change.Kind == PrefabStructureChange::ChangeKind::RemovedEntity && !change.Value.empty())
                        removedNames[change.Entity] = change.Value;
                }
            }
            std::unordered_set<uint64_t> removedIds;
            for (const PrefabStructureChange& change : structure)
            {
                if (change.Kind == PrefabStructureChange::ChangeKind::RemovedEntity && change.Entity != prefab.Root)
                    removedIds.insert(change.Entity);
            }
            for (bool grew = !removedIds.empty(); grew;)
            {
                grew = false;
                for (const YAML::Node node : nodes)
                {
                    const uint64_t fileID = node["Entity"].as<uint64_t>();
                    const auto relationship = node["RelationshipComponent"];
                    if (removedIds.contains(fileID) || !relationship || !relationship["Parent"])
                        continue;
                    if (removedIds.contains(relationship["Parent"].as<uint64_t>()))
                    {
                        removedIds.insert(fileID);
                        grew = true;
                    }
                }
            }
            for (uint64_t removed : removedIds)
                ids.erase(removed);

            // The instance's differences from the prefab (copied: spawning adds components to the root).
            const std::vector<PrefabPropertyOverride> overrides = root.GetComponent<PrefabInstanceComponent>().Overrides;
            const std::vector<PrefabNestedChange> nestedChanges = root.GetComponent<PrefabInstanceComponent>().Nested;

            std::vector<Entity> spawned;
            for (const YAML::Node node : nodes)
            {
                const uint64_t fileID = node["Entity"].as<uint64_t>();
                const bool isRoot = fileID == prefab.Root;
                if (removedIds.contains(fileID))
                    continue;

                // The prefab's text of this entity with the instance's overrides patched in (a copy). Its name comes from it too: a
                // rename on the instance is an override of Tag.
                const YAML::Node patched = patchedEntityNode(node, fileID, overrides, structure);

                Entity entity = root;
                if (!isRoot)
                {
                    std::string name;
                    if (auto tag = patched["TagComponent"])
                        name = tag["Tag"].as<std::string>();
                    entity = scene.CreateEntityWithUUID(ids[fileID], name);
                    auto& prefabNode = entity.AddComponent<PrefabNodeComponent>();
                    prefabNode.Instance = rootID;
                    prefabNode.LocalId = fileID;
                    spawned.push_back(entity);
                }
                SceneSerializer::DeserializeEntityComponents(scene, patched, entity, isRoot);

                // An instance inside this one: what this instance changed about it replaces what the prefab's text says.
                if (!isRoot && entity.HasComponent<PrefabInstanceComponent>())
                {
                    for (const PrefabNestedChange& nested : nestedChanges)
                    {
                        if (nested.Entity != fileID)
                            continue;
                        auto& inner = entity.GetComponent<PrefabInstanceComponent>();
                        inner.Overrides = nested.Overrides;
                        inner.Structure = nested.Structure;
                        inner.Nested = nested.Nested;
                    }
                }

                // A freshly placed instance looks like the prefab: the root's rotation and scale come from the file (the
                // position is where the instance was put).
                if (isRoot && initTransform)
                {
                    if (auto transformNode = node["TransformComponent"])
                    {
                        auto readVec3 = [](const YAML::Node& value) { return glm::vec3(value[0].as<float>(), value[1].as<float>(), value[2].as<float>()); };
                        auto& transform = root.GetComponent<TransformComponent>();
                        transform.Rotation = readVec3(transformNode["Rotation"]);
                        transform.Scale = readVec3(transformNode["Scale"]);
                        root.MarkTransformDirty();
                    }
                }
            }

            // The file stores parent/child links as file ids: they become the spawned entities' UUIDs, and an entity without a
            // parent in the file hangs from the root.
            if (!root.HasComponent<RelationshipComponent>())
                root.AddComponent<RelationshipComponent>();
            for (Entity entity : spawned)
            {
                if (!entity.HasComponent<RelationshipComponent>())
                    entity.AddComponent<RelationshipComponent>();
                auto& relationship = entity.GetComponent<RelationshipComponent>();

                const auto parent = ids.find(static_cast<uint64_t>(relationship.Parent));
                relationship.Parent = relationship.Parent != 0 && parent != ids.end() ? parent->second : rootID;

                std::vector<UUID> children;
                for (UUID child : relationship.Children)
                {
                    const auto found = ids.find(static_cast<uint64_t>(child));
                    if (found != ids.end())
                        children.push_back(found->second);
                }
                relationship.Children = std::move(children);

                if (relationship.Parent == rootID)
                    root.GetComponent<RelationshipComponent>().Children.push_back(entity.GetUUID());
                entity.MarkTransformDirty();
            }

            // A parent's child list is rebuilt from the children's Parent links (a hand-made file may not list every child).
            for (Entity entity : spawned)
            {
                const UUID parentID = entity.GetComponent<RelationshipComponent>().Parent;
                if (parentID == rootID)
                    continue;
                Entity parent = scene.GetEntityByUUID(parentID);
                if (!parent)
                    continue;
                if (!parent.HasComponent<RelationshipComponent>())
                    parent.AddComponent<RelationshipComponent>();
                auto& siblings = parent.GetComponent<RelationshipComponent>().Children;
                if (std::find(siblings.begin(), siblings.end(), entity.GetUUID()) == siblings.end())
                    siblings.push_back(entity.GetUUID());
            }

            // Entities the user attached below a spawned entity (saved with the scene, their parent is a spawned entity's derived UUID)
            // hang from it again.
            {
                std::unordered_set<UUID> spawnedIDs;
                spawnedIDs.insert(rootID);
                for (Entity entity : spawned)
                    spawnedIDs.insert(entity.GetUUID());
                auto relationships = scene.GetAllEntitiesWith<RelationshipComponent>();
                std::vector<std::pair<UUID, UUID>> attached; // parent, child
                for (auto handle : relationships)
                {
                    const UUID parentID = relationships.get<RelationshipComponent>(handle).Parent;
                    if (parentID != 0 && spawnedIDs.contains(parentID))
                        attached.emplace_back(parentID, Entity(handle, &scene).GetUUID());
                }
                for (const auto& [parentID, childID] : attached)
                {
                    Entity parent = scene.GetEntityByUUID(parentID);
                    if (!parent)
                        continue;
                    if (!parent.HasComponent<RelationshipComponent>())
                        parent.AddComponent<RelationshipComponent>();
                    auto& children = parent.GetComponent<RelationshipComponent>().Children;
                    if (std::find(children.begin(), children.end(), childID) == children.end())
                        children.push_back(childID);
                }
            }

            // Script fields that point at another entity of the prefab point at this instance's copy of it.
            auto remapReferences = [&](Entity entity)
            {
                if (!entity.HasComponent<ScriptComponent>())
                    return;
                for (auto& [className, fields] : entity.GetComponent<ScriptComponent>().EntityReferences)
                {
                    for (auto& [fieldName, reference] : fields)
                    {
                        if (reference.IsModelNode())
                            continue;
                        const auto found = ids.find(static_cast<uint64_t>(reference.Entity));
                        if (found != ids.end())
                            reference.Entity = found->second;
                    }
                }
            };
            remapReferences(root);
            for (Entity entity : spawned)
                remapReferences(entity);

            if (outSpawned)
            {
                outSpawned->push_back(root);
                outSpawned->insert(outSpawned->end(), spawned.begin(), spawned.end());
            }
        }
    }

    UUID PrefabInstance::NodeUUID(UUID instance, uint64_t localId)
    {
        // SplitMix64 of the pair, like ModelInstance::NodeUUID (a different salt: the two never produce the same UUID).
        uint64_t value = static_cast<uint64_t>(instance) ^ ((localId + 1) * 0xD6E8FEB86659FD93ull);
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
        return UUID(value ^ (value >> 31));
    }

    void PrefabInstance::SpawnPending(Scene& scene)
    {
        // Collected first: spawning creates entities.
        std::vector<entt::entity> pending;
        auto instances = scene.GetAllEntitiesWith<PrefabInstanceComponent>();
        for (auto handle : instances)
        {
            if (!instances.get<PrefabInstanceComponent>(handle).Spawned)
                pending.push_back(handle);
        }

        for (entt::entity handle : pending)
        {
            Entity root(handle, &scene);
            const AssetHandle prefabHandle = root.GetComponent<PrefabInstanceComponent>().Prefab;

            // A prefab inside itself (directly or through the instances it sits in) would spawn forever.
            bool cycle = false;
            for (Entity outer = root; outer && outer.HasComponent<PrefabNodeComponent>();)
            {
                outer = scene.GetEntityByUUID(outer.GetComponent<PrefabNodeComponent>().Instance);
                if (outer && outer.HasComponent<PrefabInstanceComponent>() && outer.GetComponent<PrefabInstanceComponent>().Prefab == prefabHandle)
                {
                    cycle = true;
                    break;
                }
            }
            if (cycle)
            {
                NOX_CORE_ERROR("Prefab instance '{}': the prefab contains itself, it is not spawned", root.GetName());
                root.GetComponent<PrefabInstanceComponent>().Spawned = true;
                continue;
            }

            const AssetState state = AssetManager::RequestAsset(prefabHandle);
            if (state == AssetState::Loading)
                continue;

            // Spawning adds components to the root (storage may move): nothing keeps a reference into it.
            auto& instance = root.GetComponent<PrefabInstanceComponent>();
            instance.Spawned = true;
            const bool initTransform = instance.InitTransform;
            instance.InitTransform = false;
            const Prefab* prefab = state == AssetState::Ready ? AssetManager::FindLoadedAsset<Prefab>(prefabHandle) : nullptr;
            if (!prefab)
            {
                NOX_CORE_ERROR("Prefab instance '{}': prefab {} could not be loaded", root.GetName(), static_cast<uint64_t>(prefabHandle));
                continue; // nothing to spawn; a reload of the scene tries again
            }
            spawn(scene, root, *prefab, initTransform);
        }
    }

    Entity PrefabInstance::Instantiate(Scene& scene, AssetHandle prefabHandle, const glm::vec3& position, std::vector<Entity>& outSpawned)
    {
        outSpawned.clear();
        Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(prefabHandle);
        if (!prefab)
        {
            NOX_CORE_ERROR("Instantiate: prefab {} could not be loaded", static_cast<uint64_t>(prefabHandle));
            return {};
        }

        Entity root = scene.CreateEntity(prefab->Name);
        root.GetComponent<TransformComponent>().Translation = position;
        auto& instance = root.AddComponent<PrefabInstanceComponent>();
        instance.Prefab = prefabHandle;
        instance.Spawned = true;
        spawn(scene, root, *prefab, true, &outSpawned);
        if (outSpawned.empty())
        {
            scene.DestroyEntity(root);
            return {};
        }

        // Fresh entities have no world matrices yet (the update systems compute them next frame), but physics bodies are
        // created from them right away: work them out now, parents before children.
        std::function<void(Entity, const glm::mat4&)> propagate = [&](Entity entity, const glm::mat4& parentWorld)
        {
            const glm::mat4 world = parentWorld * entity.GetComponent<TransformComponent>().GetTransform();
            if (entity.HasComponent<WorldTransformComponent>())
                entity.GetComponent<WorldTransformComponent>().WorldMatrix = world;
            if (!entity.HasComponent<RelationshipComponent>())
                return;
            for (UUID childID : entity.GetComponent<RelationshipComponent>().Children)
            {
                if (Entity child = scene.GetEntityByUUID(childID))
                    propagate(child, world);
            }
        };
        propagate(root, glm::mat4(1.0f));
        return root;
    }

    void PrefabInstance::Unpack(Scene& scene, Entity root)
    {
        if (!root || !root.HasComponent<PrefabInstanceComponent>())
            return;

        const UUID rootID = root.GetUUID();
        std::vector<entt::entity> spawned;
        auto nodes = scene.GetAllEntitiesWith<PrefabNodeComponent>();
        for (auto handle : nodes)
        {
            if (nodes.get<PrefabNodeComponent>(handle).Instance == rootID)
                spawned.push_back(handle);
        }
        for (entt::entity handle : spawned)
            Entity(handle, &scene).RemoveComponent<PrefabNodeComponent>();
        root.RemoveComponent<PrefabInstanceComponent>();
    }

    Entity PrefabInstance::LoadForEditing(Scene& scene, const Prefab& prefab)
    {
        const YAML::Node& nodes = *prefab.Entities;
        std::unordered_set<uint64_t> fileIDs;
        for (const YAML::Node node : nodes)
            fileIDs.insert(node["Entity"].as<uint64_t>());
        if (!fileIDs.contains(prefab.Root))
            return {};

        Entity root;
        std::vector<Entity> created;
        for (const YAML::Node node : nodes)
        {
            const uint64_t fileID = node["Entity"].as<uint64_t>();
            std::string name;
            if (auto tag = node["TagComponent"])
                name = tag["Tag"].as<std::string>();
            Entity entity = scene.CreateEntityWithUUID(fileID, name);
            SceneSerializer::DeserializeEntityComponents(scene, node, entity, false);
            created.push_back(entity);
            if (fileID == prefab.Root)
                root = entity;
        }

        // The file was written from scene entities: links to entities that are not in it (the root's old parent, the tops' parent)
        // are cut, and the tops hang from the root.
        if (!root.HasComponent<RelationshipComponent>())
            root.AddComponent<RelationshipComponent>();
        root.GetComponent<RelationshipComponent>().Parent = 0;
        root.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f);

        for (Entity entity : created)
        {
            if (!entity.HasComponent<RelationshipComponent>())
                entity.AddComponent<RelationshipComponent>();
            auto& relationship = entity.GetComponent<RelationshipComponent>();
            std::erase_if(relationship.Children, [&](UUID child) { return !fileIDs.contains(static_cast<uint64_t>(child)); });
            if (entity == root)
                continue;

            if (relationship.Parent == 0 || !fileIDs.contains(static_cast<uint64_t>(relationship.Parent)))
                relationship.Parent = root.GetUUID();
        }

        // Every parent lists its children (rebuilt from the Parent links: a hand-made file may not list them all).
        for (Entity entity : created)
        {
            if (entity == root)
                continue;
            Entity parent = scene.GetEntityByUUID(entity.GetComponent<RelationshipComponent>().Parent);
            if (!parent)
                continue;
            if (!parent.HasComponent<RelationshipComponent>())
                parent.AddComponent<RelationshipComponent>();
            auto& siblings = parent.GetComponent<RelationshipComponent>().Children;
            if (std::find(siblings.begin(), siblings.end(), entity.GetUUID()) == siblings.end())
                siblings.push_back(entity.GetUUID());
        }
        for (Entity entity : created)
            entity.MarkTransformDirty();
        return root;
    }

    namespace
    {
        // The spawned entities go, and the components the prefab gave the root; the instance spawns again on the next update.
        void respawn(Scene& scene, Entity root)
        {
            const UUID rootID = root.GetUUID();

            // Collected by UUID: destroying a parent destroys its children too.
            std::vector<UUID> spawned;
            auto nodes = scene.GetAllEntitiesWith<PrefabNodeComponent>();
            for (auto nodeHandle : nodes)
            {
                if (nodes.get<PrefabNodeComponent>(nodeHandle).Instance == rootID)
                    spawned.push_back(Entity(nodeHandle, &scene).GetUUID());
            }
            // What the user attached below the spawned entities is not the prefab's: it must not go with its parent (it keeps its
            // parent's UUID and hangs from the respawned entity again).
            auto detachAttached = [&](Entity parent)
            {
                if (!parent || !parent.HasComponent<RelationshipComponent>())
                    return;
                std::erase_if(parent.GetComponent<RelationshipComponent>().Children, [&](UUID childID)
                {
                    Entity child = scene.GetEntityByUUID(childID);
                    return child && !child.HasComponent<PrefabNodeComponent>();
                });
            };
            detachAttached(root);
            for (UUID id : spawned)
                detachAttached(scene.GetEntityByUUID(id));

            for (UUID id : spawned)
            {
                if (Entity entity = scene.GetEntityByUUID(id))
                    scene.DestroyEntity(entity);
            }

            removeProvided(root, AllComponents{});
            root.GetComponent<PrefabInstanceComponent>().Spawned = false;
        }

        // Whether an instance of `instancePrefab` is an instance of `target` too: `target` is its prefab or one of the bases it is
        // a variant of.
        bool prefabDependsOn(AssetHandle instancePrefab, AssetHandle target)
        {
            AssetHandle current = instancePrefab;
            for (int depth = 0; depth < 16 && current != 0; ++depth)
            {
                if (current == target)
                    return true;
                const Prefab* prefab = AssetManager::FindLoadedAsset<Prefab>(current);
                current = prefab ? prefab->Base : AssetHandle(0);
            }
            return false;
        }

        std::vector<entt::entity> spawnedInstancesOf(Scene& scene, AssetHandle prefab)
        {
            std::vector<entt::entity> roots;
            auto instances = scene.GetAllEntitiesWith<PrefabInstanceComponent>();
            for (auto handle : instances)
            {
                const auto& instance = instances.get<PrefabInstanceComponent>(handle);
                if (instance.Spawned && prefabDependsOn(instance.Prefab, prefab))
                    roots.push_back(handle);
            }
            return roots;
        }
    }

    void PrefabInstance::RespawnAll(Scene& scene, AssetHandle prefab)
    {
        for (entt::entity handle : spawnedInstancesOf(scene, prefab))
            respawn(scene, Entity(handle, &scene));
    }

    Entity PrefabInstance::FindEntity(Scene& scene, Entity root, uint64_t localId)
    {
        const auto* prefab = AssetManager::FindLoadedAsset<Prefab>(root.GetComponent<PrefabInstanceComponent>().Prefab);
        if (prefab && prefab->Root == localId)
            return root;

        const UUID rootID = root.GetUUID();
        auto nodes = scene.GetAllEntitiesWith<PrefabNodeComponent>();
        for (auto handle : nodes)
        {
            const auto& node = nodes.get<PrefabNodeComponent>(handle);
            if (node.Instance == rootID && node.LocalId == localId)
                return Entity(handle, &scene);
        }
        return {};
    }

    std::vector<PrefabPropertyOverride> PrefabInstance::CollectOverrides(Scene& scene, Entity root)
    {
        const auto& instance = root.GetComponent<PrefabInstanceComponent>();
        if (!instance.Spawned)
            return instance.Overrides;
        const Prefab* prefab = AssetManager::FindLoadedAsset<Prefab>(instance.Prefab);
        if (!prefab)
            return instance.Overrides;

        std::vector<PrefabPropertyOverride> result;
        for (const YAML::Node node : effectiveNodes(*prefab))
        {
            const uint64_t fileID = node["Entity"].as<uint64_t>();
            Entity entity = FindEntity(scene, root, fileID);
            if (entity)
                diffEntity(node, SceneSerializer::EntityToNode(scene, entity), fileID, fileID == prefab->Root, result);
        }
        return result;
    }

    void PrefabInstance::CaptureOverrides(Scene& scene, AssetHandle prefab)
    {
        for (entt::entity handle : spawnedInstancesOf(scene, prefab))
        {
            Entity root(handle, &scene);
            std::vector<PrefabPropertyOverride> overrides = CollectOverrides(scene, root);
            std::vector<PrefabStructureChange> structure = CollectStructureChanges(scene, root);
            auto& instance = root.GetComponent<PrefabInstanceComponent>();
            instance.Overrides = std::move(overrides);
            instance.Structure = std::move(structure);
            instance.Nested = CollectNested(scene, root);
        }
    }

    namespace
    {
        const std::unordered_set<std::string>& skippedStructureKeys()
        {
            static const std::unordered_set<std::string> keys = { "Entity", "RelationshipComponent", "FolderComponent", "PrefabInstanceComponent" };
            return keys;
        }

        uint64_t parentIdOf(const YAML::Node& node, uint64_t rootId)
        {
            const auto relationship = node["RelationshipComponent"];
            if (relationship && relationship["Parent"] && relationship["Parent"].as<uint64_t>() != 0)
                return relationship["Parent"].as<uint64_t>();
            return rootId;
        }

        // Scene entities below the instance's spawned entities that the prefab does not have (top ones: their own children come
        // with them). The parent is the spawned entity they hang from.
        std::vector<std::pair<Entity, Entity>> attachedEntities(Scene& scene, Entity root)
        {
            const UUID rootID = root.GetUUID();
            std::vector<std::pair<Entity, Entity>> result; // child, spawned parent
            auto nodes = scene.GetAllEntitiesWith<PrefabNodeComponent>();
            std::vector<Entity> spawned{ root };
            for (auto handle : nodes)
            {
                if (nodes.get<PrefabNodeComponent>(handle).Instance == rootID)
                    spawned.push_back(Entity(handle, &scene));
            }
            for (Entity parent : spawned)
            {
                if (!parent.HasComponent<RelationshipComponent>())
                    continue;
                for (UUID childID : parent.GetComponent<RelationshipComponent>().Children)
                {
                    Entity child = scene.GetEntityByUUID(childID);
                    if (child && !child.HasComponent<PrefabNodeComponent>())
                        result.emplace_back(child, parent);
                }
            }
            return result;
        }

        // An added entity and everything below it as nodes for a prefab's Entities: their ids are the entities' UUIDs (stable while the
        // entities live on, unique in the file), the added one hangs from `parentLocalId`.
        void appendAddedSubtree(Scene& scene, Entity added, uint64_t parentLocalId, uint64_t rootFallback, YAML::Node& entities)
        {
            std::vector<Entity> subtree;
            std::unordered_set<UUID> inSubtree;
            std::function<void(Entity)> collect = [&](Entity entity)
            {
                subtree.push_back(entity);
                inSubtree.insert(entity.GetUUID());
                if (!entity.HasComponent<RelationshipComponent>())
                    return;
                for (UUID childID : entity.GetComponent<RelationshipComponent>().Children)
                {
                    if (Entity child = scene.GetEntityByUUID(childID))
                        collect(child);
                }
            };
            collect(added);

            for (Entity entity : subtree)
            {
                YAML::Node node = SceneSerializer::EntityToNode(scene, entity);
                node["Entity"] = static_cast<uint64_t>(entity.GetUUID());

                YAML::Node relationship;
                const UUID parentID = entity.HasComponent<RelationshipComponent>() ? entity.GetComponent<RelationshipComponent>().Parent : UUID(0);
                relationship["Parent"] = entity == added ? parentLocalId : (inSubtree.contains(parentID) ? static_cast<uint64_t>(parentID) : rootFallback);
                YAML::Node children(YAML::NodeType::Sequence);
                if (entity.HasComponent<RelationshipComponent>())
                {
                    for (UUID childID : entity.GetComponent<RelationshipComponent>().Children)
                    {
                        if (inSubtree.contains(childID))
                            children.push_back(static_cast<uint64_t>(childID));
                    }
                }
                relationship["Children"] = children;
                node["RelationshipComponent"] = relationship;
                entities.push_back(node);
            }
        }

        uint64_t localIdOf(Entity entity, Entity root, const Prefab& prefab)
        {
            if (entity == root)
                return prefab.Root;
            return entity.HasComponent<PrefabNodeComponent>() ? entity.GetComponent<PrefabNodeComponent>().LocalId : 0;
        }
    }

    std::vector<PrefabStructureChange> PrefabInstance::CollectStructureChanges(Scene& scene, Entity root)
    {
        const auto& instance = root.GetComponent<PrefabInstanceComponent>();
        if (!instance.Spawned)
            return instance.Structure;
        const Prefab* prefab = AssetManager::FindLoadedAsset<Prefab>(instance.Prefab);
        if (!prefab)
            return instance.Structure;

        std::vector<PrefabStructureChange> result;
        std::unordered_set<uint64_t> missing;
        std::unordered_map<uint64_t, uint64_t> parentOf;
        for (const YAML::Node node : effectiveNodes(*prefab))
        {
            const uint64_t fileID = node["Entity"].as<uint64_t>();
            if (fileID != prefab->Root)
                parentOf[fileID] = parentIdOf(node, prefab->Root);
            if (fileID != prefab->Root && !FindEntity(scene, root, fileID))
                missing.insert(fileID);
        }

        for (const YAML::Node node : effectiveNodes(*prefab))
        {
            const uint64_t fileID = node["Entity"].as<uint64_t>();
            if (missing.contains(fileID))
            {
                // Only the top of a deleted subtree: what is below it goes with it.
                if (!missing.contains(parentOf[fileID]))
                {
                    // Named as it was when it was deleted on this instance, else as the prefab has it.
                    std::string name;
                    if (const auto remembered = instance.RemovedNames.find(fileID); remembered != instance.RemovedNames.end())
                        name = remembered->second;
                    else if (const auto tag = node["TagComponent"])
                        name = tag["Tag"].as<std::string>();
                    result.push_back({ PrefabStructureChange::ChangeKind::RemovedEntity, fileID, {}, name });
                }
                continue;
            }

            Entity entity = FindEntity(scene, root, fileID);
            if (!entity)
                continue;
            const YAML::Node current = SceneSerializer::EntityToNode(scene, entity);

            for (auto component = node.begin(); component != node.end(); ++component)
            {
                const std::string key = component->first.as<std::string>();
                if (!skippedStructureKeys().contains(key) && !current[key])
                    result.push_back({ PrefabStructureChange::ChangeKind::RemovedComponent, fileID, key, {} });
            }
            // The root of an instance inside this one has the components ITS prefab gave it: none of them is an addition here.
            if (entity.HasComponent<PrefabInstanceComponent>())
                continue;
            for (auto component = current.begin(); component != current.end(); ++component)
            {
                const std::string key = component->first.as<std::string>();
                if (!skippedStructureKeys().contains(key) && !node[key])
                    result.push_back({ PrefabStructureChange::ChangeKind::AddedComponent, fileID, key, valueText(component->second) });
            }
        }
        return result;
    }

    std::vector<PrefabNestedChange> PrefabInstance::CollectNested(Scene& scene, Entity root)
    {
        const auto& instance = root.GetComponent<PrefabInstanceComponent>();
        if (!instance.Spawned)
            return instance.Nested;
        const Prefab* prefab = AssetManager::FindLoadedAsset<Prefab>(instance.Prefab);
        if (!prefab)
            return instance.Nested;

        std::vector<PrefabNestedChange> result;
        for (const YAML::Node node : effectiveNodes(*prefab))
        {
            const uint64_t fileID = node["Entity"].as<uint64_t>();
            const YAML::Node innerNode = node["PrefabInstanceComponent"];
            if (!innerNode || fileID == prefab->Root)
                continue;
            Entity inner = FindEntity(scene, root, fileID);
            if (!inner || !inner.HasComponent<PrefabInstanceComponent>())
                continue;

            PrefabNestedChange change;
            change.Entity = fileID;
            change.Overrides = CollectOverrides(scene, inner);
            change.Structure = CollectStructureChanges(scene, inner);
            change.Nested = CollectNested(scene, inner);

            // Only what differs from the prefab file's own text of the inner instance is this instance's change.
            YAML::Node baseline(YAML::NodeType::Map);
            for (const char* key : { "Overrides", "Structure", "Nested" })
            {
                if (innerNode[key])
                    baseline[key] = YAML::Clone(innerNode[key]);
            }
            if (!sameValue(baseline, SceneSerializer::PrefabChangesToNode(change.Overrides, change.Structure, change.Nested)))
                result.push_back(std::move(change));
        }
        return result;
    }

    std::vector<PrefabInstance::OverrideEntry> PrefabInstance::ListOverrides(Scene& scene, Entity root)
    {
        std::vector<OverrideEntry> entries;
        for (const PrefabPropertyOverride& o : CollectOverrides(scene, root))
            entries.push_back({ OverrideEntry::EntryKind::Property, o.Entity, o.Component, o.Field, o.Value, UUID(0), {} });
        for (const PrefabStructureChange& change : CollectStructureChanges(scene, root))
        {
            const OverrideEntry::EntryKind kind = change.Kind == PrefabStructureChange::ChangeKind::RemovedEntity ? OverrideEntry::EntryKind::RemovedEntity
                                           : change.Kind == PrefabStructureChange::ChangeKind::RemovedComponent ? OverrideEntry::EntryKind::RemovedComponent
                                                                                                          : OverrideEntry::EntryKind::AddedComponent;
            entries.push_back({ kind, change.Entity, change.Component, {}, change.Value, UUID(0), {} });
        }

        const Prefab* prefab = AssetManager::FindLoadedAsset<Prefab>(root.GetComponent<PrefabInstanceComponent>().Prefab);
        if (prefab && root.GetComponent<PrefabInstanceComponent>().Spawned)
        {
            for (auto [child, parent] : attachedEntities(scene, root))
                entries.push_back({ OverrideEntry::EntryKind::AddedEntity, localIdOf(parent, root, *prefab), {}, {}, {}, child.GetUUID(), child.GetName() });
        }

        // The entity an entry is about, by name: the live one, or the prefab's own text of it (a deleted entity is not there any more).
        if (prefab)
        {
            for (OverrideEntry& entry : entries)
            {
                if (entry.Kind == OverrideEntry::EntryKind::RemovedEntity && !entry.Value.empty())
                {
                    entry.TargetName = entry.Value; // the name it had when it was deleted
                    continue;
                }
                if (Entity target = FindEntity(scene, root, entry.Entity))
                {
                    entry.TargetName = target.GetName();
                    continue;
                }
                for (const YAML::Node node : effectiveNodes(*prefab))
                {
                    if (node["Entity"].as<uint64_t>() != entry.Entity)
                        continue;
                    if (const auto tag = node["TagComponent"])
                        entry.TargetName = tag["Tag"].as<std::string>();
                    break;
                }
            }
        }
        return entries;
    }

    void PrefabInstance::RevertOverride(Scene& scene, Entity root, const OverrideEntry* only)
    {
        if (!root || !root.HasComponent<PrefabInstanceComponent>() || !root.GetComponent<PrefabInstanceComponent>().Spawned)
            return;

        std::vector<PrefabPropertyOverride> properties = CollectOverrides(scene, root);
        std::vector<PrefabStructureChange> structure = CollectStructureChanges(scene, root);
        std::vector<PrefabNestedChange> nested = only ? CollectNested(scene, root) : std::vector<PrefabNestedChange>();

        // The entities added below the spawned ones go (all of them, or the one).
        for (auto [child, parent] : attachedEntities(scene, root))
        {
            if (!only || (only->Kind == OverrideEntry::EntryKind::AddedEntity && only->SceneEntity == child.GetUUID()))
                scene.DestroyEntity(child);
        }

        if (!only)
        {
            properties.clear();
            structure.clear();
        }
        else if (only->Kind == OverrideEntry::EntryKind::Property)
        {
            std::erase_if(properties, [&](const PrefabPropertyOverride& o) { return o.Entity == only->Entity && o.Component == only->Component && o.Field == only->Field; });
        }
        else if (only->Kind != OverrideEntry::EntryKind::AddedEntity)
        {
            // A deleted entity comes back as the prefab has it -- except its name: a rename on this instance was an override that went
            // away with the entity, so the name it had when it was deleted is put back as one.
            if (only->Kind == OverrideEntry::EntryKind::RemovedEntity && !only->TargetName.empty())
            {
                std::string prefabName;
                if (const Prefab* prefab = AssetManager::FindLoadedAsset<Prefab>(root.GetComponent<PrefabInstanceComponent>().Prefab))
                {
                    for (const YAML::Node node : effectiveNodes(*prefab))
                    {
                        if (node["Entity"].as<uint64_t>() != only->Entity)
                            continue;
                        if (const auto tag = node["TagComponent"])
                            prefabName = tag["Tag"].as<std::string>();
                        break;
                    }
                }
                if (prefabName != only->TargetName)
                    properties.push_back({ only->Entity, "TagComponent", "Tag", valueText(YAML::Node(only->TargetName)) });
            }

            const PrefabStructureChange::ChangeKind kind = only->Kind == OverrideEntry::EntryKind::RemovedEntity ? PrefabStructureChange::ChangeKind::RemovedEntity
                                                   : only->Kind == OverrideEntry::EntryKind::RemovedComponent ? PrefabStructureChange::ChangeKind::RemovedComponent
                                                                                                         : PrefabStructureChange::ChangeKind::AddedComponent;
            std::erase_if(structure, [&](const PrefabStructureChange& c) { return c.Kind == kind && c.Entity == only->Entity && c.Component == only->Component; });
        }

        auto& instance = root.GetComponent<PrefabInstanceComponent>();
        instance.Overrides = std::move(properties);
        instance.Structure = std::move(structure);
        instance.Nested = std::move(nested);
        respawn(scene, root);
    }

    bool PrefabInstance::ApplyOverride(Scene& scene, Entity root, const OverrideEntry& entry)
    {
        if (!root || !root.HasComponent<PrefabInstanceComponent>())
            return false;
        const AssetHandle handle = root.GetComponent<PrefabInstanceComponent>().Prefab;
        Prefab* prefab = AssetManager::FindLoadedAsset<Prefab>(handle);
        if (!prefab)
            return false;

        // The instances take their differences first: after the prefab changes, they could not be told apart from its own edit.
        CaptureOverrides(scene, handle);

        // A new content of the entity list: the prefab's nodes as they are, changed where the override says.
        YAML::Node entities = *prefab->Entities; // shares the prefab's data
        bool patched = false;

        auto nodeOf = [&](uint64_t id) -> YAML::Node
        {
            for (size_t i = 0; i < entities.size(); ++i)
            {
                if (entities[i]["Entity"].as<uint64_t>() == id)
                    return entities[i];
            }
            return YAML::Node(YAML::NodeType::Undefined);
        };

        // A variant: what is about the BASE's entities (not its own) is a change the variant holds, not text to patch.
        const bool variantLayer = prefab->Base != 0 && entry.Kind != OverrideEntry::EntryKind::AddedEntity && !nodeOf(entry.Entity).IsDefined();
        if (variantLayer)
        {
            if (entry.Kind == OverrideEntry::EntryKind::Property)
            {
                std::erase_if(prefab->Overrides, [&](const PrefabPropertyOverride& o) { return o.Entity == entry.Entity && o.Component == entry.Component && o.Field == entry.Field; });
                prefab->Overrides.push_back({ entry.Entity, entry.Component, entry.Field, entry.Value });
                patched = true;
            }
            else
            {
                const PrefabStructureChange::ChangeKind kind = entry.Kind == OverrideEntry::EntryKind::RemovedEntity ? PrefabStructureChange::ChangeKind::RemovedEntity
                                                             : entry.Kind == OverrideEntry::EntryKind::RemovedComponent ? PrefabStructureChange::ChangeKind::RemovedComponent
                                                                                                                        : PrefabStructureChange::ChangeKind::AddedComponent;
                std::erase_if(prefab->Structure, [&](const PrefabStructureChange& c) { return c.Kind == kind && c.Entity == entry.Entity && c.Component == entry.Component; });
                prefab->Structure.push_back({ kind, entry.Entity, entry.Component, entry.Kind == OverrideEntry::EntryKind::AddedComponent ? entry.Value : std::string() });
                patched = true;
            }
        }
        else switch (entry.Kind)
        {
        case OverrideEntry::EntryKind::Property:
        {
            YAML::Node node = nodeOf(entry.Entity);
            YAML::Node component = node.IsDefined() ? node[entry.Component] : YAML::Node();
            if (component && component.IsMap())
            {
                component[entry.Field] = YAML::Load(entry.Value);
                patched = true;
            }
            break;
        }
        case OverrideEntry::EntryKind::RemovedComponent:
        {
            YAML::Node node = nodeOf(entry.Entity);
            if (node.IsDefined())
                patched = node.remove(entry.Component);
            break;
        }
        case OverrideEntry::EntryKind::AddedComponent:
        {
            YAML::Node node = nodeOf(entry.Entity);
            if (node.IsDefined())
            {
                node[entry.Component] = YAML::Load(entry.Value);
                patched = true;
            }
            break;
        }
        case OverrideEntry::EntryKind::RemovedEntity:
        {
            // The node and everything below it leave the prefab.
            std::unordered_set<uint64_t> gone{ entry.Entity };
            for (bool grew = true; grew;)
            {
                grew = false;
                for (const YAML::Node node : entities)
                {
                    const uint64_t id = node["Entity"].as<uint64_t>();
                    if (!gone.contains(id) && gone.contains(parentIdOf(node, prefab->Root)) && id != prefab->Root)
                    {
                        gone.insert(id);
                        grew = true;
                    }
                }
            }
            YAML::Node kept;
            for (const YAML::Node node : entities)
            {
                if (!gone.contains(node["Entity"].as<uint64_t>()))
                    kept.push_back(node);
            }
            prefab->Entities = std::make_shared<YAML::Node>(kept);
            patched = true;
            break;
        }
        case OverrideEntry::EntryKind::AddedEntity:
        {
            Entity added = scene.GetEntityByUUID(entry.SceneEntity);
            if (!added)
                break;

            // The added entity and everything below it become nodes of the prefab (its own entities, for a variant); the parent is the
            // spawned entity it hangs from.
            appendAddedSubtree(scene, added, entry.Entity, prefab->Root, entities);
            patched = true;

            // It comes back from the prefab when the instance spawns again: the live copy goes.
            scene.DestroyEntity(added);
            break;
        }
        }
        if (!patched)
            return false;

        const AssetMetadata metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
        if (!PrefabImporter::SavePrefab(*prefab, Project::GetActiveAssetDirectory() / metadata.FilePath))
            NOX_CORE_ERROR("Could not write prefab '{}'", metadata.FilePath.generic_string());

        // What now equals the prefab is no override any more.
        for (entt::entity instanceHandle : spawnedInstancesOf(scene, handle))
        {
            auto& instance = Entity(instanceHandle, &scene).GetComponent<PrefabInstanceComponent>();
            std::erase_if(instance.Overrides, [&](const PrefabPropertyOverride& o) {
                return entry.Kind == OverrideEntry::EntryKind::Property && o.Entity == entry.Entity && o.Component == entry.Component && o.Field == entry.Field && o.Value == entry.Value; });
            std::erase_if(instance.Structure, [&](const PrefabStructureChange& c) {
                return (entry.Kind == OverrideEntry::EntryKind::RemovedEntity && c.Kind == PrefabStructureChange::ChangeKind::RemovedEntity && c.Entity == entry.Entity) ||
                       (entry.Kind == OverrideEntry::EntryKind::RemovedComponent && c.Kind == PrefabStructureChange::ChangeKind::RemovedComponent && c.Entity == entry.Entity && c.Component == entry.Component) ||
                       (entry.Kind == OverrideEntry::EntryKind::AddedComponent && c.Kind == PrefabStructureChange::ChangeKind::AddedComponent && c.Entity == entry.Entity && c.Component == entry.Component); });
        }
        RespawnAll(scene, handle);
        return true;
    }

    Entity PrefabInstance::LoadVariantForEditing(Scene& scene, const Prefab& variant)
    {
        Entity root = scene.CreateEntity(variant.Name);
        auto& instance = root.AddComponent<PrefabInstanceComponent>();
        instance.Prefab = variant.Base;
        instance.Overrides = variant.Overrides;
        instance.Structure = variant.Structure;
        instance.Nested = variant.Nested;

        // The variant's own entities: ordinary entities (their ids are their UUIDs). Below a spawned entity of the base they hang from
        // its derived UUID, which the spawn of the base instance links up.
        const UUID rootID = root.GetUUID();
        std::unordered_set<uint64_t> ownIds;
        if (variant.Entities)
        {
            for (const YAML::Node node : *variant.Entities)
                ownIds.insert(node["Entity"].as<uint64_t>());

            for (const YAML::Node node : *variant.Entities)
            {
                const uint64_t id = node["Entity"].as<uint64_t>();
                std::string name;
                if (auto tag = node["TagComponent"])
                    name = tag["Tag"].as<std::string>();
                Entity entity = scene.CreateEntityWithUUID(id, name);
                SceneSerializer::DeserializeEntityComponents(scene, node, entity, false);

                if (!entity.HasComponent<RelationshipComponent>())
                    entity.AddComponent<RelationshipComponent>();
                auto& relationship = entity.GetComponent<RelationshipComponent>();
                const uint64_t parentId = static_cast<uint64_t>(relationship.Parent);
                if (!ownIds.contains(parentId))
                    relationship.Parent = (parentId == 0 || parentId == variant.Root) ? rootID : NodeUUID(rootID, parentId);
                std::erase_if(relationship.Children, [&](UUID child) { return !ownIds.contains(static_cast<uint64_t>(child)); });
                entity.MarkTransformDirty();
            }
        }
        return root;
    }

    bool PrefabInstance::SaveVariant(Scene& scene, Entity root, Prefab& variant, const std::filesystem::path& absolutePath)
    {
        if (!root || !root.HasComponent<PrefabInstanceComponent>())
            return false;
        const AssetHandle baseHandle = root.GetComponent<PrefabInstanceComponent>().Prefab;
        const Prefab* base = AssetManager::FindLoadedAsset<Prefab>(baseHandle);
        if (!base)
            return false;

        variant.Base = baseHandle;
        variant.Root = base->Root;
        variant.Overrides = CollectOverrides(scene, root);
        variant.Structure = CollectStructureChanges(scene, root);
        variant.Nested = CollectNested(scene, root);

        YAML::Node entities(YAML::NodeType::Sequence);
        for (auto [child, parent] : attachedEntities(scene, root))
            appendAddedSubtree(scene, child, localIdOf(parent, root, *base), base->Root, entities);
        variant.Entities = std::make_shared<YAML::Node>(entities);
        return PrefabImporter::SavePrefab(variant, absolutePath);
    }
}
