#include "ModelInstance.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Entity.h"
#include "Scene.h"
#include "NoxCore/Animation/AnimationSequence.h"
#include "NoxCore/Animation/Skeleton.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Renderer/Mesh.h"

namespace Nox
{
    namespace
    {
        // Spawn slots: the glTF nodes, then one per light and one per camera (lights and cameras without a node of
        // their own spawn as extra children of the root; the slots of those on a node stay unused).
        template <typename MeshAsset>
        size_t slotCount(const MeshAsset& mesh)
        {
            return mesh.GetNodes().size() + mesh.GetLights().size() + mesh.GetCameras().size();
        }

        TransformComponent makeTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale)
        {
            TransformComponent transform;
            transform.Translation = translation;
            transform.Rotation = glm::eulerAngles(rotation);
            transform.Scale = scale;
            return transform;
        }

        // A slot's entity as the model spawns it: its name and local transform.
        struct SlotDefaults
        {
            std::string Name;
            TransformComponent Transform;
        };

        template <typename MeshAsset>
        SlotDefaults slotDefaults(const MeshAsset& mesh, size_t slot)
        {
            const auto& nodes = mesh.GetNodes();
            if (slot < nodes.size())
            {
                const MeshNodeData& node = nodes[slot];
                return { node.Name.empty() ? "Node " + std::to_string(slot) : node.Name, makeTransform(node.Translation, node.Rotation, node.Scale) };
            }
            slot -= nodes.size();
            if (slot < mesh.GetLights().size())
            {
                const LightNodeData& light = mesh.GetLights()[slot];
                return { light.Name.empty() ? "Light " + std::to_string(slot) : light.Name, makeTransform(light.Translation, light.Rotation, light.Scale) };
            }
            slot -= mesh.GetLights().size();
            const CameraNodeData& camera = mesh.GetCameras()[slot];
            return { camera.Name.empty() ? "Camera " + std::to_string(slot) : camera.Name, makeTransform(camera.Translation, camera.Rotation, camera.Scale) };
        }

        // Calls function with the model's loaded Mesh or StaticMesh; false when it is not loaded (or not a model).
        template <typename Function>
        bool withModel(AssetHandle model, Function&& function)
        {
            const AssetType type = AssetManager::GetAssetType(model);
            if (type == AssetType::StaticMesh)
            {
                if (const StaticMesh* mesh = AssetManager::FindLoadedAsset<StaticMesh>(model))
                {
                    function(*mesh);
                    return true;
                }
            }
            else if (type == AssetType::Mesh || type == AssetType::MeshSource)
            {
                if (const Mesh* mesh = AssetManager::FindLoadedAsset<Mesh>(model))
                {
                    function(*mesh);
                    return true;
                }
            }
            return false;
        }

        // Parent links only: the child keeps its local transform (Entity::SetParent converts world transforms, which
        // are not computed yet for fresh entities).
        void attach(Entity child, Entity parent)
        {
            if (!child.HasComponent<RelationshipComponent>())
                child.AddComponent<RelationshipComponent>();
            if (!parent.HasComponent<RelationshipComponent>())
                parent.AddComponent<RelationshipComponent>();
            child.GetComponent<RelationshipComponent>().Parent = parent.GetUUID();
            parent.GetComponent<RelationshipComponent>().Children.push_back(child.GetUUID());
        }

        void addLight(Entity entity, const LightNodeData& light)
        {
            if (light.Type == GltfLightType::Directional)
            {
                auto& component = entity.AddComponent<DirectionalLightComponent>();
                component.Color = light.Color;
                component.Intensity = light.Intensity;
            }
            else if (light.Type == GltfLightType::Point)
            {
                auto& component = entity.AddComponent<PointLightComponent>();
                component.Color = light.Color;
                component.Intensity = light.Intensity;
                component.Range = light.Range;
            }
            else if (light.Type == GltfLightType::Spot)
            {
                auto& component = entity.AddComponent<SpotLightComponent>();
                component.Color = light.Color;
                component.Intensity = light.Intensity;
                component.Range = light.Range;
                component.InnerAngle = light.InnerConeAngle;
                component.OuterAngle = light.OuterConeAngle;
            }
        }

        // The model's first camera becomes the scene's primary camera (used by Play) unless the scene has one.
        void addCamera(Scene& scene, Entity entity, const CameraNodeData& camera)
        {
            const bool primary = !scene.GetPrimaryCameraEntity();
            auto& component = entity.AddOrReplaceComponent<CameraComponent>(); // sets the viewport aspect
            if (camera.Type == GltfCameraType::Orthographic)
                component.Camera.SetOrthographic(camera.OrthographicSize, camera.NearClip, camera.FarClip);
            else
                component.Camera.SetPerspective(camera.VerticalFov, camera.NearClip, camera.FarClip);
            component.Primary = primary;
        }

        // glTF animations are independent clips. Blender exports one per animated object (Bistro: each fan part has its
        // own), and those all play at once; a character's clips (Fox: Survey/Walk/Run) all target the same joints and are
        // alternatives. So: clips whose target nodes overlap form one group that shares a single animator (first clip
        // plays, the rest are swappable), and every disjoint group gets its own animator so they run simultaneously.
        template <typename MeshAsset>
        void addAnimators(const MeshAsset& mesh, const Skeleton* skeleton, const std::vector<Entity>& spawned, const std::vector<UUID>& nodeTable)
        {
            const size_t nodeCount = mesh.GetNodes().size();

            // Skinned meshes follow their joint entities (glTF skinning) instead of evaluating a private skeleton copy.
            Entity firstSkinnedMesh;
            for (size_t index = 0; index < nodeCount; ++index)
            {
                Entity node = spawned[index];
                if (node && node.HasComponent<AnimatorComponent>() && node.GetComponent<AnimatorComponent>().Skeleton != 0)
                {
                    node.GetComponent<AnimatorComponent>().NodeEntities = nodeTable;
                    if (!firstSkinnedMesh)
                        firstSkinnedMesh = node;
                }
            }

            struct ClipTargets
            {
                AssetHandle Handle = 0;
                std::vector<int32_t> Nodes;
            };
            std::vector<ClipTargets> clips;
            for (AssetHandle clipHandle : mesh.GetAnimationAssets())
            {
                const AnimationSequence* clip = AssetManager::FindLoadedAsset<AnimationSequence>(clipHandle);
                if (!clip)
                    continue;
                ClipTargets targets{ clipHandle, {} };
                for (const auto& channel : clip->Channels)
                {
                    if (channel.TargetNodeIndex >= 0 && channel.TargetNodeIndex < static_cast<int32_t>(nodeCount))
                        targets.Nodes.push_back(channel.TargetNodeIndex);
                }
                std::sort(targets.Nodes.begin(), targets.Nodes.end());
                targets.Nodes.erase(std::unique(targets.Nodes.begin(), targets.Nodes.end()), targets.Nodes.end());
                if (!targets.Nodes.empty())
                    clips.push_back(std::move(targets));
            }

            auto overlaps = [](const std::vector<int32_t>& a, const std::vector<int32_t>& b)
            {
                size_t i = 0, j = 0;
                while (i < a.size() && j < b.size())
                {
                    if (a[i] == b[j])
                        return true;
                    if (a[i] < b[j])
                        ++i;
                    else
                        ++j;
                }
                return false;
            };

            // Union-find over clips by shared target nodes.
            std::vector<size_t> group(clips.size());
            for (size_t i = 0; i < clips.size(); ++i)
                group[i] = i;
            std::function<size_t(size_t)> findGroup = [&](size_t i) -> size_t
            {
                return group[i] == i ? i : (group[i] = findGroup(group[i]));
            };
            for (size_t i = 0; i < clips.size(); ++i)
                for (size_t j = i + 1; j < clips.size(); ++j)
                    if (overlaps(clips[i].Nodes, clips[j].Nodes))
                        group[findGroup(j)] = findGroup(i);

            std::vector<int32_t> jointIndices;
            if (skeleton && !skeleton->Skins.empty() && skeleton->Skins[0])
            {
                for (const Node* joint : skeleton->Skins[0]->Joints)
                    if (joint)
                        jointIndices.push_back(joint->Index);
                std::sort(jointIndices.begin(), jointIndices.end());
            }

            std::vector<bool> groupAssigned(clips.size(), false);
            for (size_t i = 0; i < clips.size(); ++i)
            {
                const size_t root = findGroup(i);
                if (groupAssigned[root])
                    continue;
                groupAssigned[root] = true;

                // Clip i is the group's first (sorted) clip -> the default. Joint clips belong to the skinned mesh (it
                // plays them and skins from them); anything else goes on the node it animates.
                Entity owner = firstSkinnedMesh && overlaps(clips[i].Nodes, jointIndices) ? firstSkinnedMesh : spawned[clips[i].Nodes.front()];
                if (!owner)
                    continue;

                if (owner.HasComponent<AnimatorComponent>() && owner.GetComponent<AnimatorComponent>().Animation != 0)
                {
                    NOX_CORE_WARN("Animation clip {} targets entity '{}' which already plays another clip; skipped.",
                                  static_cast<uint64_t>(clips[i].Handle), owner.GetName());
                    continue;
                }

                auto& animator = owner.HasComponent<AnimatorComponent>() ? owner.GetComponent<AnimatorComponent>() : owner.AddComponent<AnimatorComponent>();
                animator.Animation = clips[i].Handle;
                animator.NodeEntities = nodeTable;
            }
        }

        // False while the skeleton or a clip is still loading (requested).
        template <typename MeshAsset>
        bool spawn(Scene& scene, Entity root, const MeshAsset& mesh)
        {
            bool loading = false;
            if (mesh.GetSkeletonAsset() != 0)
                loading |= AssetManager::RequestAsset(mesh.GetSkeletonAsset()) == AssetState::Loading;
            for (AssetHandle clip : mesh.GetAnimationAssets())
                loading |= AssetManager::RequestAsset(clip) == AssetState::Loading;
            if (loading)
                return false;

            auto& instance = root.GetComponent<ModelInstanceComponent>();
            const UUID rootID = root.GetUUID();
            const auto& nodes = mesh.GetNodes();
            const std::unordered_set<uint32_t> removed(instance.RemovedNodes.begin(), instance.RemovedNodes.end());
            // A per-mesh asset holds an identity node (a plain drag places that) and the file's instances of the mesh
            // (placed by dragging several assets together): one of the two kinds spawns, never both.
            const bool hasLayoutNodes = std::any_of(nodes.begin(), nodes.end(), [](const MeshNodeData& node) { return node.Parent == MeshNodeData::FileLayoutParent; });
            const bool atFileLayout = instance.AtFileLayout;
            const Skeleton* skeleton = mesh.GetSkeletonAsset() != 0 ? AssetManager::FindLoadedAsset<Skeleton>(mesh.GetSkeletonAsset()) : nullptr;
            const bool skinned = skeleton && !skeleton->Skins.empty();

            // A skinned character is a skeletal mesh (UE5): one entity holding the mesh and its animator, no node or
            // joint entities -- the pose goes straight into skinning matrices (Scene::UpdateAnimators' self-contained
            // path), bones are data. It stops being a model instance, so everything on it saves as ordinary
            // components. Lights and cameras in such a file are not spawned. Models with several skins or with
            // unskinned meshes next to the skin are not handled yet and spawn as nodes below.
            if (skinned && skeleton->Skins.size() == 1)
            {
                const AssetHandle model = instance.Model;
                const auto& clips = mesh.GetAnimationAssets();

                auto& meshComponent = root.AddComponent<MeshComponent>();
                meshComponent.Mesh = model;
                meshComponent.SubmeshIndex = 0;
                meshComponent.SubmeshCount = UINT32_MAX;
                root.AddComponent<MaterialComponent>();

                auto& animator = root.AddComponent<AnimatorComponent>();
                animator.Skeleton = mesh.GetSkeletonAsset();
                if (!clips.empty())
                    animator.Animation = clips.front();

                root.RemoveComponent<ModelInstanceComponent>(); // `instance` is gone
                return true;
            }

            std::vector<Entity> spawned(slotCount(mesh));
            auto spawnSlot = [&](size_t slot) -> Entity
            {
                if (removed.contains(static_cast<uint32_t>(slot)) ||
                    (hasLayoutNodes && slot < nodes.size() && (nodes[slot].Parent == MeshNodeData::FileLayoutParent) != atFileLayout))
                    return {};
                const SlotDefaults defaults = slotDefaults(mesh, slot);
                Entity entity = scene.CreateEntityWithUUID(ModelInstance::NodeUUID(rootID, static_cast<uint32_t>(slot)), defaults.Name);
                auto& node = entity.AddComponent<ModelNodeComponent>();
                node.Instance = rootID;
                node.NodeIndex = static_cast<uint32_t>(slot);
                entity.GetComponent<TransformComponent>() = defaults.Transform;
                spawned[slot] = entity;
                return entity;
            };

            for (size_t index = 0; index < nodes.size(); ++index)
                spawnSlot(index);
            for (size_t index = 0; index < nodes.size(); ++index)
            {
                Entity node = spawned[index];
                if (!node)
                    continue;
                // A node whose parent was removed hangs off the root.
                const int32_t parent = nodes[index].Parent;
                attach(node, parent >= 0 && parent < static_cast<int32_t>(nodes.size()) && spawned[parent] ? spawned[parent] : root);

                if (nodes[index].SubmeshCount > 0)
                {
                    auto& meshComponent = node.AddComponent<MeshComponent>();
                    meshComponent.Mesh = instance.Model;
                    meshComponent.SubmeshIndex = nodes[index].FirstSubmesh;
                    meshComponent.SubmeshCount = nodes[index].SubmeshCount;
                    node.AddComponent<MaterialComponent>(); // no overrides: the mesh's materials
                    if (skinned)
                        node.AddComponent<AnimatorComponent>().Skeleton = mesh.GetSkeletonAsset();
                }
            }

            // A model without nodes draws all of it on the root.
            if (nodes.empty() && !root.HasComponent<MeshComponent>())
            {
                auto& meshComponent = root.AddComponent<MeshComponent>();
                meshComponent.Mesh = instance.Model;
                meshComponent.SubmeshIndex = 0;
                meshComponent.SubmeshCount = UINT32_MAX;
                if (!root.HasComponent<MaterialComponent>())
                    root.AddComponent<MaterialComponent>();
            }

            const auto& lights = mesh.GetLights();
            for (size_t index = 0; index < lights.size(); ++index)
            {
                const int32_t nodeIndex = lights[index].NodeIndex;
                if (nodeIndex >= 0 && nodeIndex < static_cast<int32_t>(nodes.size()))
                {
                    if (spawned[nodeIndex])
                        addLight(spawned[nodeIndex], lights[index]);
                }
                else if (Entity light = spawnSlot(nodes.size() + index))
                {
                    attach(light, root);
                    addLight(light, lights[index]);
                }
            }

            const auto& cameras = mesh.GetCameras();
            for (size_t index = 0; index < cameras.size(); ++index)
            {
                const int32_t nodeIndex = cameras[index].NodeIndex;
                if (nodeIndex >= 0 && nodeIndex < static_cast<int32_t>(nodes.size()))
                {
                    if (spawned[nodeIndex])
                        addCamera(scene, spawned[nodeIndex], cameras[index]);
                }
                else if (Entity camera = spawnSlot(nodes.size() + lights.size() + index))
                {
                    attach(camera, root);
                    addCamera(scene, camera, cameras[index]);
                }
            }

            // glTF node index -> entity, shared by the clips and every skin.
            std::vector<UUID> nodeTable(nodes.size(), UUID(0));
            for (size_t index = 0; index < nodes.size(); ++index)
            {
                if (spawned[index])
                    nodeTable[index] = spawned[index].GetUUID();
            }
            addAnimators(mesh, skeleton, spawned, nodeTable);

            for (const ModelNodeOverride& nodeOverride : instance.Overrides)
            {
                if (nodeOverride.NodeIndex >= spawned.size() || !spawned[nodeOverride.NodeIndex])
                    continue;
                Entity node = spawned[nodeOverride.NodeIndex];
                if (nodeOverride.Transform)
                    node.GetComponent<TransformComponent>() = *nodeOverride.Transform;
                if (nodeOverride.Name)
                    node.GetComponent<TagComponent>().Tag = *nodeOverride.Name;
                if (!nodeOverride.MaterialAssets.empty())
                {
                    auto& material = node.HasComponent<MaterialComponent>() ? node.GetComponent<MaterialComponent>() : node.AddComponent<MaterialComponent>();
                    material.MaterialAssets = nodeOverride.MaterialAssets;
                }
            }
            instance.Overrides.clear();

            // Entities the user attached to nodes kept the node as their parent (a node's UUID never changes): link them.
            std::unordered_map<UUID, Entity> nodeByID;
            for (Entity node : spawned)
            {
                if (node)
                    nodeByID.emplace(node.GetUUID(), node);
            }
            std::vector<std::pair<Entity, Entity>> reattached;
            for (auto handle : scene.GetAllEntitiesWith<RelationshipComponent>())
            {
                Entity entity(handle, &scene);
                if (entity.HasComponent<ModelNodeComponent>() && entity.GetComponent<ModelNodeComponent>().Instance == rootID)
                    continue;
                auto found = nodeByID.find(entity.GetComponent<RelationshipComponent>().Parent);
                if (found != nodeByID.end())
                    reattached.emplace_back(entity, found->second);
            }
            for (auto& [child, node] : reattached)
            {
                node.GetComponent<RelationshipComponent>().Children.push_back(child.GetUUID());
                child.MarkTransformDirty();
            }

            instance.Spawned = true;
            return true;
        }

        bool nearlyEqual(const glm::vec3& a, const glm::vec3& b)
        {
            return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(1e-4f)));
        }
    }

    UUID ModelInstance::NodeUUID(UUID instance, uint32_t nodeIndex)
    {
        // SplitMix64 of the pair: spread over the whole range like the random UUIDs it lives next to.
        uint64_t value = static_cast<uint64_t>(instance) ^ ((static_cast<uint64_t>(nodeIndex) + 1) * 0x9E3779B97F4A7C15ull);
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
        return UUID(value ^ (value >> 31));
    }

    void ModelInstance::SpawnPending(Scene& scene)
    {
        // Collected first: spawning creates entities.
        std::vector<entt::entity> pending;
        auto instances = scene.GetAllEntitiesWith<ModelInstanceComponent>();
        for (auto handle : instances)
        {
            if (!instances.get<ModelInstanceComponent>(handle).Spawned)
                pending.push_back(handle);
        }

        for (entt::entity handle : pending)
        {
            Entity root(handle, &scene);
            auto& instance = root.GetComponent<ModelInstanceComponent>();
            const AssetState state = AssetManager::RequestAsset(instance.Model);
            if (state == AssetState::Loading)
                continue;

            if (state != AssetState::Ready || !withModel(instance.Model, [&](const auto& mesh) { spawn(scene, root, mesh); }))
            {
                NOX_CORE_ERROR("Model instance '{}': model {} could not be loaded", root.GetName(), static_cast<uint64_t>(instance.Model));
                instance.Spawned = true; // nothing to spawn; a reload of the scene tries again
            }
        }
    }

    std::vector<ModelNodeOverride> ModelInstance::CollectOverrides(Scene& scene, Entity root)
    {
        const auto& instance = root.GetComponent<ModelInstanceComponent>();
        if (!instance.Spawned)
            return instance.Overrides;

        std::vector<ModelNodeOverride> overrides;
        const UUID rootID = root.GetUUID();
        withModel(instance.Model, [&](const auto& mesh)
        {
            const size_t slots = slotCount(mesh);
            for (auto handle : scene.GetAllEntitiesWith<ModelNodeComponent>())
            {
                Entity node(handle, &scene);
                const ModelNodeComponent& nodeComponent = node.GetComponent<ModelNodeComponent>();
                if (nodeComponent.Instance != rootID || nodeComponent.NodeIndex >= slots)
                    continue;

                const SlotDefaults defaults = slotDefaults(mesh, nodeComponent.NodeIndex);
                ModelNodeOverride nodeOverride;
                nodeOverride.NodeIndex = nodeComponent.NodeIndex;
                const TransformComponent& transform = node.GetComponent<TransformComponent>();
                if (!nearlyEqual(transform.Translation, defaults.Transform.Translation) ||
                    !nearlyEqual(transform.Rotation, defaults.Transform.Rotation) ||
                    !nearlyEqual(transform.Scale, defaults.Transform.Scale))
                {
                    nodeOverride.Transform = transform;
                }
                if (node.GetName() != defaults.Name)
                    nodeOverride.Name = node.GetName();
                if (node.HasComponent<MaterialComponent>())
                    nodeOverride.MaterialAssets = node.GetComponent<MaterialComponent>().MaterialAssets;

                if (nodeOverride.Transform || nodeOverride.Name || !nodeOverride.MaterialAssets.empty())
                    overrides.push_back(std::move(nodeOverride));
            }
        });

        std::sort(overrides.begin(), overrides.end(), [](const ModelNodeOverride& a, const ModelNodeOverride& b) { return a.NodeIndex < b.NodeIndex; });
        return overrides;
    }

    void ModelInstance::Respawn(Scene& scene, Entity root)
    {
        std::vector<ModelNodeOverride> overrides = CollectOverrides(scene, root);
        const UUID rootID = root.GetUUID();

        std::vector<Entity> nodes;
        std::unordered_set<UUID> nodeIDs;
        for (auto handle : scene.GetAllEntitiesWith<ModelNodeComponent>())
        {
            Entity node(handle, &scene);
            if (node.GetComponent<ModelNodeComponent>().Instance == rootID)
            {
                nodes.push_back(node);
                nodeIDs.insert(node.GetUUID());
            }
        }

        // Children the user attached stay: unlinked from their node (which keeps being their parent) so destroying the
        // node does not take them along; the spawn links them again.
        for (Entity node : nodes)
        {
            if (!node.HasComponent<RelationshipComponent>())
                continue;
            std::erase_if(node.GetComponent<RelationshipComponent>().Children, [&](UUID child) { return !nodeIDs.contains(child); });
        }
        // Top-level nodes only: destroying one takes its subtree.
        for (Entity node : nodes)
        {
            if (node && node.GetComponent<RelationshipComponent>().Parent == rootID)
                scene.DestroyEntity(node);
        }

        auto& instance = root.GetComponent<ModelInstanceComponent>();
        instance.RemovedNodes.clear();
        instance.Overrides = std::move(overrides);
        instance.Spawned = false;
    }
}
