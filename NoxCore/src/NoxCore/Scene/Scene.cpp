#include "Scene.h"

#include <box2d/box2d.h>
#include <algorithm>
#include <functional>

#include "Entity.h"
#include "Components.h"
#include "SceneGraph.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Physics/Physics2D.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Tasks/JobSystem.h"

namespace Nox
{
    template <typename... Component>
    static void CopyComponent(entt::registry& dst, const entt::registry& src,
                              const std::unordered_map<UUID, entt::entity>& enttMap)
    {
        ([&]()
        {
            auto view = src.view<Component>();
            for (auto srcEntity : view)
            {
                entt::entity dstEntity = enttMap.at(src.get<IDComponent>(srcEntity).ID);

                auto& srcComponent = src.get<Component>(srcEntity);
                dst.emplace_or_replace<Component>(dstEntity, srcComponent);
            }
        }(), ...);
    }

    template <typename... Component>
    static void CopyComponent(ComponentGroup<Component...>, entt::registry& dst, entt::registry& src,
                              const std::unordered_map<UUID, entt::entity>& enttMap)
    {
        CopyComponent<Component...>(dst, src, enttMap);
    }

    template <typename... Component>
    static void CopyComponentIfExists(Entity dst, Entity src)
    {
        ([&]()
        {
            if (src.HasComponent<Component>())
                dst.AddOrReplaceComponent<Component>(src.GetComponent<Component>());
        }(), ...);
    }

    template <typename... Component>
    static void CopyComponentIfExists(ComponentGroup<Component...>, Entity dst, Entity src)
    {
        CopyComponentIfExists<Component...>(dst, src);
    }

    namespace
    {
        // Non-component data the submission systems write; named in their declared access.
        struct MeshSubmission {};
        struct LightSubmission {};
        struct Renderer2DSubmission {};

        constexpr uint32_t MeshEntitiesPerChunk = 64;
    }

    template <typename... Component>
    static void CreateComponentStorages(ComponentGroup<Component...>, entt::registry& registry)
    {
        ((void)registry.storage<Component>(), ...);
    }

    Scene::Scene()
    {
        // A non-const registry creates a component's storage the first time any view/get/try_get names the type, and
        // that creation is not thread-safe. Systems access the registry from several workers at once, so every storage
        // exists before the first graph runs (e.g. a scene copied for Play has no sprite storage until something asks).
        CreateComponentStorages(AllComponents{}, m_Registry);
        CreateComponentStorages(ComponentGroup<IDComponent, TagComponent>{}, m_Registry);

        RegisterSystems();
    }

    Ref<Scene> Scene::Copy(Ref<Scene> other)
    {
        Ref<Scene> newScene = CreateRef<Scene>();

        newScene->m_ViewportWidth = other->m_ViewportWidth;
        newScene->m_ViewportHeight = other->m_ViewportHeight;

        auto& srcSceneRegistry = other->m_Registry;
        auto& dstSceneRegistry = newScene->m_Registry;
        std::unordered_map<UUID, entt::entity> enttMap;

        // Create entities in new scene
        auto idView = srcSceneRegistry.view<IDComponent>();
        for (auto e : idView)
        {
            UUID uuid = srcSceneRegistry.get<IDComponent>(e).ID;
            const auto& name = srcSceneRegistry.get<TagComponent>(e).Tag;
            Entity newEntity = newScene->CreateEntityWithUUID(uuid, name);
            enttMap[uuid] = (entt::entity)newEntity;
        }

        // Copy components (except IDComponent and TagComponent)
        CopyComponent(AllComponents{}, dstSceneRegistry, srcSceneRegistry, enttMap);

        return newScene;
    }
    
    Entity Scene::CreateEntity(const std::string& name)
    {
        return CreateEntityWithUUID(UUID(), name); // Generate a new random UUID
    }
    
    Entity Scene::CreateEntityWithUUID(UUID uuid, const std::string& name)
    {
        // 1. Create the raw EnTT ID
        entt::entity entityHandle = m_Registry.create();
        
        // 2. Wrap it in our Handle class
        Entity entity = { entityHandle, this };
        
        // 3. Add the mandatory components
        entity.AddComponent<IDComponent>(uuid);
        entity.AddComponent<TagComponent>(name.empty() ? "Entity" : name);
        entity.AddComponent<TransformComponent>(); // Most engines assume everything has a transform
        
        entity.AddComponent<WorldTransformComponent>(); // Required for scene graph
        entity.AddComponent<DirtyTransformComponent>(); // Mark initial state dirty
        
        m_EntityMap[uuid] = entityHandle;
        
        return entity;
    }
    
    void Scene::DestroyEntity(Entity entity)
    {
        if (!entity)
            return;
        
        // 1. Unlink this entity from its parent (if it has one)
        if (entity.HasComponent<RelationshipComponent>())
        {
            UUID parentUUID = entity.GetComponent<RelationshipComponent>().Parent;
            if (parentUUID != 0)
            {
                Entity parent = GetEntityByUUID(parentUUID);
                if (parent && parent.HasComponent<RelationshipComponent>())
                {
                    auto& parentChildren = parent.GetComponent<RelationshipComponent>().Children;
                    std::erase(parentChildren, entity.GetUUID());
                }
            }
        }
        
        // 2. Recursively destroy all child entities
        if (entity.HasComponent<RelationshipComponent>())
        {
            // Copy the children vector so modifying it during destruction doesn't invalidate iteration
            std::vector<UUID> children = entity.GetComponent<RelationshipComponent>().Children;
            for (UUID childUUID : children)
            {
                Entity childEntity = GetEntityByUUID(childUUID);
                if (childEntity)
                {
                    DestroyEntity(childEntity);
                }
            }
        }
        
        m_EntityMap.erase(entity.GetUUID());
        m_Registry.destroy(entity);
        m_AssetReferencesChanged = true;
    }

    void Scene::CollectAssetReferences(std::unordered_set<AssetHandle>& outHandles)
    {
        for (auto entity : m_Registry.view<MeshComponent>())
            outHandles.insert(m_Registry.get<MeshComponent>(entity).Mesh);

        for (auto entity : m_Registry.view<MaterialComponent>())
        {
            const auto& materials = m_Registry.get<MaterialComponent>(entity).MaterialAssets;
            outHandles.insert(materials.begin(), materials.end());
        }

        for (auto entity : m_Registry.view<AnimatorComponent>())
        {
            const auto& animator = m_Registry.get<AnimatorComponent>(entity);
            outHandles.insert(animator.Animation);
            outHandles.insert(animator.Skeleton);
        }

        for (auto entity : m_Registry.view<SpriteRendererComponent>())
            outHandles.insert(m_Registry.get<SpriteRendererComponent>(entity).Texture);

        outHandles.erase(AssetHandle(0));
    }

    void Scene::OnRuntimeStart()
    {
        m_IsRunning = true;
        OnPhysics2DStart();
        
        /*// Scripting
        {
            ScriptEngine::OnRuntimeStart(this);
            // Instantiate all script entities

            auto view = m_Registry.view<ScriptComponent>();
            for (auto e : view)
            {
                Entity entity = { e, this };
                ScriptEngine::OnCreateEntity(entity);
            }
        }*/
    }

    void Scene::OnRuntimeStop()
    {
        m_IsRunning = false;
        
        OnPhysics2DStop();
        
        /*ScriptEngine::OnRuntimeStop();*/
    }

    void Scene::OnSimulationStart()
    {
        OnPhysics2DStart();
    }

    void Scene::OnSimulationStop()
    {
        OnPhysics2DStop();
    }

    void Scene::OnUpdateRuntime(Timestep ts)
    {
        const bool step = !m_IsPaused || m_StepFrames-- > 0;
        RunUpdateSystems(ts, step, step);

        // The primary camera's world transform is current once the update systems have run.
        Camera* mainCamera = nullptr;
        glm::mat4 cameraTransform;
        {
            auto view = m_Registry.view<WorldTransformComponent, CameraComponent>();
            for (auto entity : view)
            {
                auto [transform, camera] = view.get<WorldTransformComponent, CameraComponent>(entity);
                if (camera.Primary)
                {
                    mainCamera = &camera.Camera;
                    cameraTransform = transform.WorldMatrix;
                    break;
                }
            }
        }

        if (mainCamera)
        {
            m_renderer->BeginScene(*mainCamera, cameraTransform);
            RunSubmitSystems();
            m_renderer->EndScene();
        }
    }

    void Scene::OnUpdateSimulation(Timestep ts, EditorCamera& camera)
    {
        const bool step = !m_IsPaused || m_StepFrames-- > 0;
        RunUpdateSystems(ts, step, step);

        m_renderer->BeginScene(camera);
        RunSubmitSystems();
        m_renderer->EndScene();
    }

    void Scene::OnUpdateEditor(Timestep ts, EditorCamera& camera)
    {
        RunUpdateSystems(ts, false, true);

        m_renderer->BeginScene(camera);
        RunSubmitSystems();
        m_renderer->EndScene();
    }

    void Scene::RegisterSystems()
    {
        // Game Update. Declared access orders them: Physics 2D and Animation both write transforms, propagation reads them.
        m_UpdateSystems.AddSystem("Physics 2D",
            ComponentAccess().Read<RigidBody2DComponent>().Write<TransformComponent, DirtyTransformComponent>(),
            [this]() { UpdatePhysics2D(); });
        m_UpdateSystems.AddSystem("Animation",
            ComponentAccess().Write<AnimatorComponent, TransformComponent, DirtyTransformComponent>(),
            [this]() { UpdateAnimators(); });
        m_UpdateSystems.AddSystem("Transform Propagation",
            ComponentAccess().Read<TransformComponent, RelationshipComponent>().Write<WorldTransformComponent, DirtyTransformComponent>(),
            [this]() { SceneGraph::UpdateWorldTransforms(m_Registry, m_EntityMap, m_CommandBuffers); });

        // Submission (after BeginScene). No conflicts: the three run in parallel.
        m_SubmitSystems.AddSystem("Submit Meshes",
            ComponentAccess().Read<WorldTransformComponent, MeshComponent, MaterialComponent>().Write<AnimatorComponent, MeshSubmission>(),
            [this]() { SubmitMeshes(); });
        m_SubmitSystems.AddSystem("Submit Lights",
            ComponentAccess().Read<WorldTransformComponent, DirectionalLightComponent, PointLightComponent, SpotLightComponent>().Write<LightSubmission>(),
            [this]() { SubmitLights(); });
        m_SubmitSystems.AddSystem("Submit 2D",
            ComponentAccess().Read<WorldTransformComponent, SpriteRendererComponent, CircleRendererComponent, TextComponent>().Write<Renderer2DSubmission>(),
            [this]() { Submit2D(); });
    }

    void Scene::RunUpdateSystems(Timestep ts, bool stepPhysics, bool stepAnimation)
    {
        NOX_PROFILE_SCOPE("Scene Update Systems");
        m_FrameInput = { static_cast<float>(ts), stepPhysics, stepAnimation };
        m_UpdateSystems.Run();
        ApplySyncPoint();
    }

    void Scene::RunSubmitSystems()
    {
        NOX_PROFILE_SCOPE("Scene Submit Systems");

        // The renderer sizes its chunks before the tasks write them; the same chunk layout splits the entity list.
        m_MeshEntities.clear();
        for (auto entity : m_Registry.view<WorldTransformComponent, MeshComponent>())
            m_MeshEntities.push_back(entity);
        m_renderer->BeginMeshSubmission(JobSystem::Get().GetChunkCount(static_cast<uint32_t>(m_MeshEntities.size()), MeshEntitiesPerChunk));

        m_SubmitSystems.Run();

        m_renderer->EndMeshSubmission(m_MissingAssets.Local());
        ApplySyncPoint();
    }

    void Scene::ApplySyncPoint()
    {
        NOX_PROFILE_SCOPE("Scene Sync Point");

        m_CommandBuffers.ForEach([this](EntityCommandBuffer& commandBuffer) { commandBuffer.Apply(*this); });

        // GetAsset imports on demand, which only the main thread may do. Loaded now, used from the next frame.
        m_MissingAssets.ForEach([](std::vector<AssetHandle>& handles)
        {
            for (AssetHandle handle : handles)
                AssetManager::GetAsset<Asset>(handle);
            handles.clear();
        });
    }

    void Scene::UpdatePhysics2D()
    {
        if (!m_FrameInput.StepPhysics)
            return;

        constexpr int32_t subStepCount = 4;
        b2World_Step(m_PhysicsWorldID, m_FrameInput.Timestep, subStepCount);

        // Retrieve Transform from Box2D
        auto view = m_Registry.view<RigidBody2DComponent, TransformComponent, DirtyTransformComponent>();
        for (auto entity : view)
        {
            auto [rigidBody, transform, dirty] = view.get<RigidBody2DComponent, TransformComponent, DirtyTransformComponent>(entity);

            b2BodyId body = rigidBody.RuntimeBody;
            const b2Vec2 position = b2Body_GetPosition(body);
            transform.Translation.x = position.x;
            transform.Translation.y = position.y;
            transform.Rotation.z = b2Rot_GetAngle(b2Body_GetRotation(body));
            dirty.isDirty = true;
        }
    }

    void Scene::UpdateAnimators()
    {
        if (!m_FrameInput.StepAnimation)
            return;

        const float ts = m_FrameInput.Timestep;
        std::vector<AssetHandle>& missingAssets = m_MissingAssets.Local();
        auto view = m_Registry.view<AnimatorComponent>();
        for (auto entity : view)
        {
            auto& animatorComp = view.get<AnimatorComponent>(entity);

            // 1. Sync Animation sequence from AssetHandle if assigned
            if (animatorComp.Animation != 0)
            {
                Ref<AnimationSequence> currentAnim = animatorComp.Animator.GetCurrentAnimation();
                if (!currentAnim || currentAnim->Handle != animatorComp.Animation)
                {
                    // The animator keeps a Ref to the clip it plays.
                    Ref<AnimationSequence> anim(AssetManager::FindLoadedAsset<AnimationSequence>(animatorComp.Animation));
                    if (!anim)
                        missingAssets.push_back(animatorComp.Animation);
                    if (anim)
                    {
                        anim->Handle = animatorComp.Animation;
                        animatorComp.Animator.PlayAnimation(anim);
                        if (!animatorComp.Playing)
                            animatorComp.Animator.Pause();
                    }
                }
            }

            // 2. Legacy self-contained skeleton evaluation. Imports with a node table skin from the
            // joint entities instead (see GetBoneTransforms), so the skeleton isn't evaluated twice.
            if (animatorComp.Skeleton != 0 && animatorComp.NodeEntities.empty())
            {
                const Skeleton* skeleton = AssetManager::FindLoadedAsset<Skeleton>(animatorComp.Skeleton);
                if (!skeleton)
                    missingAssets.push_back(animatorComp.Skeleton);
                if (skeleton && !skeleton->AllNodes.empty())
                {
                    if (animatorComp.Playing)
                        animatorComp.Animator.Update(ts, *skeleton);
                    else
                        animatorComp.Animator.UpdateTransforms(*skeleton);
                }
            }

            // Node/object animations write the evaluated .nanim TRS directly to ECS transforms.
            if (!animatorComp.NodeEntities.empty() && animatorComp.Animation != 0)
            {
                Ref<AnimationSequence> animation(AssetManager::FindLoadedAsset<AnimationSequence>(animatorComp.Animation));
                if (!animation)
                    missingAssets.push_back(animatorComp.Animation);
                if (animation)
                {
                    // Only the nodes this clip's channels target. The node table covers the whole
                    // import (thousands of nodes for Bistro); rewriting and dirtying all of them for
                    // every animator every frame would re-propagate the entire scene graph.
                    std::vector<std::pair<int32_t, entt::entity>> targets;
                    targets.reserve(animation->Channels.size());
                    for (const auto& channel : animation->Channels)
                    {
                        int32_t nodeIndex = channel.TargetNodeIndex;
                        if (nodeIndex < 0 || nodeIndex >= static_cast<int32_t>(animatorComp.NodeEntities.size()))
                            continue;
                        auto it = m_EntityMap.find(animatorComp.NodeEntities[nodeIndex]);
                        if (it == m_EntityMap.end() || !m_Registry.valid(it->second) ||
                            !m_Registry.all_of<TransformComponent>(it->second))
                            continue;
                        targets.emplace_back(nodeIndex, it->second);
                    }

                    int32_t maxTarget = -1;
                    for (const auto& [nodeIndex, nodeEntity] : targets)
                        maxTarget = std::max(maxTarget, nodeIndex);

                    std::vector<Animator::NodeTransform> nodeTransforms(static_cast<size_t>(maxTarget + 1));
                    for (const auto& [nodeIndex, nodeEntity] : targets)
                    {
                        const auto& transform = m_Registry.get<TransformComponent>(nodeEntity);
                        nodeTransforms[nodeIndex].Translation = transform.Translation;
                        nodeTransforms[nodeIndex].Rotation = glm::quat(transform.Rotation);
                        nodeTransforms[nodeIndex].Scale = transform.Scale;
                    }

                    animatorComp.Animator.UpdateNodeAnimation(ts, animation, nodeTransforms);
                    for (const auto& [nodeIndex, nodeEntity] : targets)
                    {
                        auto& transform = m_Registry.get<TransformComponent>(nodeEntity);
                        transform.Translation = nodeTransforms[nodeIndex].Translation;
                        transform.Rotation = glm::eulerAngles(nodeTransforms[nodeIndex].Rotation);
                        transform.Scale = nodeTransforms[nodeIndex].Scale;
                        if (auto* dirty = m_Registry.try_get<DirtyTransformComponent>(nodeEntity))
                            dirty->isDirty = true;
                    }
                }
            }
        }
    }
    
    const std::vector<glm::mat4>* Scene::GetBoneTransforms(entt::entity entity, const glm::mat4& meshWorld)
    {
        AnimatorComponent* animatorComp = m_Registry.try_get<AnimatorComponent>(entity);
        if (!animatorComp)
            return nullptr;

        if (animatorComp->Skeleton == 0 || animatorComp->NodeEntities.empty())
            return &animatorComp->Animator.GetFinalBoneTransforms();

        const Skeleton* skeleton = AssetManager::FindLoadedAsset<Skeleton>(animatorComp->Skeleton);
        if (!skeleton)
            m_MissingAssets.Local().push_back(animatorComp->Skeleton);
        if (!skeleton || skeleton->Skins.empty() || !skeleton->Skins[0])
            return nullptr;

        // glTF 2.0 skinning: jointMatrix = inverse(globalTransform(meshNode)) *
        // globalTransform(jointNode) * inverseBindMatrix. The joints are real entities, so whatever
        // animates them (the root's clip, a script, the gizmo) deforms the mesh.
        const Skin* skin = skeleton->Skins[0];
        const glm::mat4 inverseMeshWorld = glm::inverse(meshWorld);
        animatorComp->SkinMatrices.resize(skin->Joints.size());

        for (size_t i = 0; i < skin->Joints.size(); ++i)
        {
            glm::mat4 jointMatrix(1.0f);
            const Node* joint = skin->Joints[i];
            if (joint && joint->Index >= 0 && joint->Index < static_cast<int32_t>(animatorComp->NodeEntities.size()))
            {
                auto it = m_EntityMap.find(animatorComp->NodeEntities[joint->Index]);
                if (it != m_EntityMap.end() && m_Registry.valid(it->second) &&
                    m_Registry.all_of<WorldTransformComponent>(it->second))
                {
                    const glm::mat4& inverseBind = i < skin->InverseBindMatrices.size()
                        ? skin->InverseBindMatrices[i] : glm::mat4(1.0f);
                    jointMatrix = inverseMeshWorld * m_Registry.get<WorldTransformComponent>(it->second).WorldMatrix * inverseBind;
                }
            }
            animatorComp->SkinMatrices[i] = jointMatrix;
        }

        return &animatorComp->SkinMatrices;
    }

    void Scene::OnViewportResize(uint32_t width, uint32_t height)
    {
        if (m_ViewportWidth == width && m_ViewportHeight == height)
            return;
        
        m_ViewportWidth = width;
        m_ViewportHeight = height;

        // Resize our non-FixedAspectRatio cameras
        auto view = m_Registry.view<CameraComponent>();
        for (auto entity : view)
        {
            auto& cameraComponent = view.get<CameraComponent>(entity);
            if (!cameraComponent.FixedAspectRatio)
            {
                cameraComponent.Camera.SetViewportSize(width, height);
            }
        }
    }
    
    // Strips a trailing " (N)" suffix (if present) and appends the smallest " (N)" that doesn't collide
    // with an existing entity name. Replaces the old "always append ' Copy'" scheme, which grew the tag
    // by 5 characters every time a duplicate was itself duplicated (Ctrl+D repeatedly on the new copy)
    // -- eventually overflowing the 256-byte tag buffer in SceneHierarchyPanel::DrawComponents and
    // crashing strcpy_s. Numbering here re-bases off the stripped stem, so it never grows unbounded.
    std::string Scene::MakeUniqueDuplicateName(const std::string& baseName)
    {
        std::string stem = baseName;
        if (!stem.empty() && stem.back() == ')')
        {
            size_t openParen = stem.find_last_of('(');
            if (openParen != std::string::npos)
            {
                std::string inside = stem.substr(openParen + 1, stem.size() - openParen - 2);
                if (!inside.empty() && inside.find_first_not_of("0123456789") == std::string::npos)
                {
                    stem = stem.substr(0, openParen);
                    while (!stem.empty() && stem.back() == ' ')
                        stem.pop_back();
                }
            }
        }

        int n = 1;
        std::string candidate = stem + " (" + std::to_string(n) + ")";
        while (FindEntityByName(candidate))
        {
            n++;
            candidate = stem + " (" + std::to_string(n) + ")";
        }
        return candidate;
    }

    Entity Scene::DuplicateEntity(Entity entity)
    {
        // Relationship links contain entity UUIDs and must be rebuilt. Copying
        // them directly makes duplicated glTF hierarchies share their children.
        using DuplicatableComponents = ComponentGroup<
            MeshComponent, MaterialComponent, DirectionalLightComponent,
            PointLightComponent, SpotLightComponent, AnimatorComponent,
            SpriteRendererComponent, CircleRendererComponent, CameraComponent,
            ScriptComponent, RigidBody2DComponent, BoxCollider2DComponent,
            CircleCollider2DComponent, TextComponent>;

        // Source UUID -> duplicate UUID, so a duplicated animator drives the duplicated nodes
        // instead of still pointing at the original hierarchy.
        std::unordered_map<UUID, UUID> duplicatedIDs;
        std::vector<Entity> duplicatedAnimators;

        std::function<Entity(Entity, Entity)> duplicateHierarchy =
            [&](Entity source, Entity parent) -> Entity
        {
            Entity duplicate = CreateEntity(MakeUniqueDuplicateName(source.GetName()));
            CopyComponentIfExists(DuplicatableComponents{}, duplicate, source);
            duplicatedIDs[source.GetUUID()] = duplicate.GetUUID();
            if (duplicate.HasComponent<AnimatorComponent>())
                duplicatedAnimators.push_back(duplicate);

            if (parent || source.HasComponent<RelationshipComponent>())
            {
                auto& relationship = duplicate.AddComponent<RelationshipComponent>();
                if (parent)
                {
                    relationship.Parent = parent.GetUUID();
                    parent.GetComponent<RelationshipComponent>().Children.push_back(duplicate.GetUUID());
                }
            }

            std::vector<UUID> children;
            if (source.HasComponent<RelationshipComponent>())
                children = source.GetComponent<RelationshipComponent>().Children;

            for (UUID childUUID : children)
            {
                Entity child = GetEntityByUUID(childUUID);
                if (child)
                    duplicateHierarchy(child, duplicate);
            }

            return duplicate;
        };

        Entity parent;
        if (entity.HasComponent<RelationshipComponent>())
            parent = GetEntityByUUID(entity.GetComponent<RelationshipComponent>().Parent);

        Entity result = duplicateHierarchy(entity, parent);

        for (Entity animatorEntity : duplicatedAnimators)
        {
            for (UUID& nodeID : animatorEntity.GetComponent<AnimatorComponent>().NodeEntities)
            {
                auto it = duplicatedIDs.find(nodeID);
                if (it != duplicatedIDs.end())
                    nodeID = it->second;
            }
        }

        return result;
    }
    
    // bad for performance dont use this often
    Entity Scene::FindEntityByName(std::string_view name)
    {
        auto view = m_Registry.view<TagComponent>();
        for (auto entity : view)
        {
            const TagComponent& tc = view.get<TagComponent>(entity);
            if (tc.Tag == name)
                return { entity, this };
        }
        return {};
    }
    
    Entity Scene::GetEntityByUUID(UUID uuid)
    {
        // Check if it exists in the map
        if (m_EntityMap.find(uuid) != m_EntityMap.end())
        {
            // Return the reconstructed Entity wrapper
            return { m_EntityMap.at(uuid), this }; 
        }

        // Return a null entity if the UUID wasn't found
        return {}; 
    }
    
    Entity Scene::GetPrimaryCameraEntity()
    {
        auto view = m_Registry.view<CameraComponent>();
        for (auto entity : view)
        {
            const auto& camera = view.get<CameraComponent>(entity);
            if (camera.Primary)
            {
                return Entity{entity, this};
            }
        }
        return {};
    }
    
    void Scene::Step(int frames)
    {
        m_StepFrames = frames;
    }
    
    void Scene::OnPhysics2DStart()
    {
        auto view = m_Registry.view<RigidBody2DComponent>();
        b2WorldDef worldDef = b2DefaultWorldDef();
        worldDef.gravity = b2Vec2{0.0f, -9.8f}; //gravity real world
        for (auto e : view)
        {
            Entity entity = {e, this};
            if (entity.HasComponent<BoxCollider2DComponent>())
            {
                auto& bc2d = entity.GetComponent<BoxCollider2DComponent>();
                worldDef.restitutionThreshold = bc2d.Restitution;
            }
        }
        m_PhysicsWorldID = b2CreateWorld(&worldDef);

        for (auto e : view)
        {
            Entity entity = {e, this};
            auto& transform = entity.GetComponent<TransformComponent>();
            auto& r2bd = entity.GetComponent<RigidBody2DComponent>();

            b2BodyDef bodyDef = b2DefaultBodyDef();;
            bodyDef.type = Utils::Rigidbody2DTypeToBox2DBody(r2bd.Type);
            bodyDef.position = b2Vec2{transform.Translation.x, transform.Translation.y}; //rename to translation
            bodyDef.rotation = b2MakeRot(transform.Rotation.z); //i could set full rotation will see bodyde.rotation

            b2BodyId body = b2CreateBody(m_PhysicsWorldID, &bodyDef);
            b2Body_SetFixedRotation(body, r2bd.FixedRotation);
            r2bd.RuntimeBody = body;

            if (entity.HasComponent<BoxCollider2DComponent>())
            {
                auto& bc2d = entity.GetComponent<BoxCollider2DComponent>();

                /*b2Polygon boxShape = b2MakeBox(bc2d.Size.x * transform.Scale.x, bc2d.Size.y * transform.Scale.y);*/

                b2Polygon boxShape = b2MakeOffsetBox
                (
                    bc2d.Size.x * transform.Scale.x,
                    bc2d.Size.y * transform.Scale.y,
                    b2Vec2{bc2d.Offset.x, bc2d.Offset.y},
                    b2MakeRot(0.0f)
                );

                b2ShapeDef shapeDef = b2DefaultShapeDef();
                shapeDef.density = bc2d.Density;
                shapeDef.material.friction = bc2d.Friction;
                shapeDef.material.restitution = bc2d.Restitution;
                b2ShapeId shapeID = b2CreatePolygonShape(body, &shapeDef, &boxShape);
            }

            if (entity.HasComponent<CircleCollider2DComponent>())
            {
                auto& cc2d = entity.GetComponent<CircleCollider2DComponent>();

                b2Circle circleShape;
                circleShape.center = b2Vec2{cc2d.Offset.x, cc2d.Offset.y};
                circleShape.radius = transform.Scale.x * cc2d.Radius;

                b2ShapeDef shapeDef = b2DefaultShapeDef();
                shapeDef.density = cc2d.Density;
                shapeDef.material.friction = cc2d.Friction;
                shapeDef.material.restitution = cc2d.Restitution;
                b2ShapeId shapeID = b2CreateCircleShape(body, &shapeDef, &circleShape);
            }
        }
    }
    
    void Scene::OnPhysics2DStop()
    {
        b2DestroyWorld(m_PhysicsWorldID);
        m_PhysicsWorldID = b2_nullWorldId;
    }
    
    void Scene::SubmitMeshes()
    {
        // Chunk layout matches Renderer::BeginMeshSubmission (same count and batch size).
        JobSystem::Get().ParallelFor("Submit Mesh Chunk", static_cast<uint32_t>(m_MeshEntities.size()), MeshEntitiesPerChunk,
            [this](uint32_t chunk, uint32_t begin, uint32_t end)
            {
                for (uint32_t index = begin; index < end; ++index)
                {
                    const entt::entity entity = m_MeshEntities[index];
                    const glm::mat4& worldMatrix = m_Registry.get<WorldTransformComponent>(entity).WorldMatrix;
                    const MeshComponent& mesh = m_Registry.get<MeshComponent>(entity);
                    const MaterialComponent* material = m_Registry.try_get<MaterialComponent>(entity);
                    const std::vector<glm::mat4>* boneTransforms = GetBoneTransforms(entity, worldMatrix);

                    m_renderer->SubmitMesh(chunk, worldMatrix, mesh, material, static_cast<int>(entity), boneTransforms);
                }
            });
    }

    void Scene::SubmitLights()
    {
        {
            auto view = m_Registry.view<WorldTransformComponent, DirectionalLightComponent>();
            for (auto entity : view)
            {
                auto [wtc, light] = view.get<WorldTransformComponent, DirectionalLightComponent>(entity);
                m_renderer->SubmitLight(wtc.WorldMatrix, light);
            }
        }
        {
            auto view = m_Registry.view<WorldTransformComponent, PointLightComponent>();
            for (auto entity : view)
            {
                auto [wtc, light] = view.get<WorldTransformComponent, PointLightComponent>(entity);
                m_renderer->SubmitLight(wtc.WorldMatrix, light);
            }
        }
        {
            auto view = m_Registry.view<WorldTransformComponent, SpotLightComponent>();
            for (auto entity : view)
            {
                auto [wtc, light] = view.get<WorldTransformComponent, SpotLightComponent>(entity);
                m_renderer->SubmitLight(wtc.WorldMatrix, light);
            }
        }
    }

    void Scene::Submit2D()
    {
        std::vector<AssetHandle>& missingAssets = m_MissingAssets.Local();

        // Sprites
        {
            auto view = m_Registry.view<WorldTransformComponent, SpriteRendererComponent>();
            for (auto entity : view)
            {
                auto [transform, sprite] = view.get<WorldTransformComponent, SpriteRendererComponent>(entity);
                if (sprite.Texture == 0)
                {
                    m_renderer2D->DrawQuad(transform.WorldMatrix, sprite.Color, static_cast<int>(entity));
                    continue;
                }

                // Renderer2D::DrawSprite without its on-demand texture import (main thread only). Until the texture is
                // loaded the sprite draws untextured.
                Ref<Texture2D> texture(AssetManager::FindLoadedAsset<Texture2D>(sprite.Texture));
                if (!texture)
                {
                    missingAssets.push_back(sprite.Texture);
                    m_renderer2D->DrawQuad(transform.WorldMatrix, sprite.Color, static_cast<int>(entity));
                    continue;
                }
                m_renderer2D->DrawQuad(transform.WorldMatrix, texture, sprite.TilingFactor, sprite.Color, static_cast<int>(entity));
            }
        }

        // Circles
        {
            auto view = m_Registry.view<WorldTransformComponent, CircleRendererComponent>();
            for (auto entity : view)
            {
                auto [transform, circle] = view.get<WorldTransformComponent, CircleRendererComponent>(entity);
                m_renderer2D->DrawCircle(transform.WorldMatrix, circle.Color, circle.Thickness, circle.Fade, static_cast<int>(entity));
            }
        }

        // Text
        {
            auto view = m_Registry.view<WorldTransformComponent, TextComponent>();
            for (auto entity : view)
            {
                auto [transform, text] = view.get<WorldTransformComponent, TextComponent>(entity);
                m_renderer2D->DrawString(text.TextString, transform.WorldMatrix, text, static_cast<int>(entity));
            }
        }
    }

    bool Scene::DumpSystemGraphs(const std::filesystem::path& directory)
    {
        const bool update = m_UpdateSystems.DumpDot(directory / "SceneUpdate.dot");
        const bool submit = m_SubmitSystems.DumpDot(directory / "SceneSubmit.dot");
        return update && submit;
    }

    template <typename T>
    void Scene::OnComponentAdded(Entity entity, T& component)
    {
        //static_assert(false);
        static_assert(sizeof(T) == 0, "Unsupported type"); // compatible with more compilers, not just MSVC
    }

    template <>
    void Scene::OnComponentAdded<IDComponent>(Entity entity, IDComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<TagComponent>(Entity entity, TagComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<TransformComponent>(Entity entity, TransformComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<WorldTransformComponent>(Entity entity, WorldTransformComponent& component)
    {
    }
    
    template <>
    void Scene::OnComponentAdded<RelationshipComponent>(Entity entity, RelationshipComponent& component)
    {
    }
    
    template <>
    void Scene::OnComponentAdded<DirtyTransformComponent>(Entity entity, DirtyTransformComponent& component)
    {
    }
    
    template <>
    void Scene::OnComponentAdded<MeshComponent>(Entity entity, MeshComponent& component)
    {
    }
    
    template <>
    void Scene::OnComponentAdded<MaterialComponent>(Entity entity, MaterialComponent& component)
    {
    }
    
    template <>
    void Scene::OnComponentAdded<DirectionalLightComponent>(Entity entity, DirectionalLightComponent& component)
    {
    }
    
    template <>
    void Scene::OnComponentAdded<PointLightComponent>(Entity entity, PointLightComponent& component)
    {
    }
    
    template <>
    void Scene::OnComponentAdded<SpotLightComponent>(Entity entity, SpotLightComponent& component)
    {
    }
    
    template <>
    void Scene::OnComponentAdded<AnimatorComponent>(Entity entity, AnimatorComponent& component)
    {
    }
    
    template <>
    void Scene::OnComponentAdded<CameraComponent>(Entity entity, CameraComponent& component)
    {
        if (m_ViewportWidth > 0 && m_ViewportHeight > 0)
            component.Camera.SetViewportSize(m_ViewportWidth, m_ViewportHeight);
    }

    template <>
    void Scene::OnComponentAdded<ScriptComponent>(Entity entity, ScriptComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<SpriteRendererComponent>(Entity entity, SpriteRendererComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<CircleRendererComponent>(Entity entity, CircleRendererComponent& component)
    {
    }

    /*template <>
    void Scene::OnComponentAdded<NativeScriptComponent>(Entity entity, NativeScriptComponent& component)
    {
    }*/

    template <>
    void Scene::OnComponentAdded<RigidBody2DComponent>(Entity entity, RigidBody2DComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<BoxCollider2DComponent>(Entity entity, BoxCollider2DComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<CircleCollider2DComponent>(Entity entity, CircleCollider2DComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<TextComponent>(Entity entity, TextComponent& component)
    {
    }
}
