#include "DotNetBackend.h"

#include "CoralCompat.h"
#include <Coral/String.hpp>
#include <SDL3/SDL_scancode.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Core/Input.h"
#include "NoxCore/Physics/Physics3DScene.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Math/Math.h"
#include "NoxCore/Renderer/DynamicMesh.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/ParticleSystem.h"

#include <thread>

namespace Nox
{
    namespace
    {
        struct ManagedVector3 { float X, Y, Z; };
        struct ManagedTransform
        {
            ManagedVector3 Position;
            ManagedVector3 Rotation;
            ManagedVector3 Scale;
        };
        // Nox.Physics.Raycast's result (Nox.RaycastHit's layout).
        struct ManagedRaycastHit
        {
            uint64_t Entity;
            ManagedVector3 Point;
            ManagedVector3 Normal;
            float Distance;
        };
        static_assert(sizeof(ManagedVector3) == sizeof(glm::vec3) && sizeof(glm::vec2) == 8, "Nox.Vector3 / Vector2 are passed as glm vectors");
        Scene* s_Scene = nullptr;
        std::thread::id s_MainThread;

        // The engine's state is the main thread's: a call from a job (Nox.Jobs, a Task) is refused instead of racing the frame.
        bool OnMainThread(const char* call)
        {
            if (std::this_thread::get_id() == s_MainThread)
                return true;
            NOX_CORE_ERROR("[C#] {} was called from a job thread: engine calls belong in OnCreate / OnUpdate (compute data in the job, "
                           "apply it on the main thread)", call);
            return false;
        }

        void LogInfo(Coral::String message)
        {
            NOX_CORE_INFO("[C#] {}", std::string(message));
        }

        Coral::Bool32 IsKeyDown(int32_t keycode)
        {
            return Input::GameKeysEnabled() && Input::IsKeyPressed(static_cast<SDL_Scancode>(keycode));
        }

        Coral::Bool32 IsMouseButtonDown(int32_t button)
        {
            // 1 = left, 2 = middle, 3 = right
            return Input::GameMouseEnabled() && Input::IsMouseButtonPressed(static_cast<SDL_MouseButtonFlags>(button));
        }

        // Scene.Instantiate("Prefabs/Ball.nprefab", x, y, z): the prefab by its path under the asset directory.
        uint64_t SceneInstantiate(Coral::String path, float x, float y, float z)
        {
            if (!s_Scene) return 0;
            const AssetHandle prefab = AssetManager::FindHandleByPath(std::filesystem::path(std::string(path)), AssetType::Prefab);
            if (prefab == 0)
            {
                NOX_CORE_ERROR("[C#] Scene.Instantiate: no prefab '{}' in the asset registry", std::string(path));
                return 0;
            }
            Entity root = s_Scene->Instantiate(prefab, { x, y, z });
            return root ? static_cast<uint64_t>(root.GetUUID()) : 0;
        }

        void SceneDestroy(uint64_t entityID)
        {
            if (!s_Scene) return;
            if (Entity entity = s_Scene->GetEntityByUUID(UUID(entityID)))
                s_Scene->QueueDestroy(entity);
        }

        // Scene.CreateEntity(name, position): an empty entity (a transform), e.g. to show a Mesh on.
        uint64_t SceneCreateEntity(Coral::String name, float x, float y, float z)
        {
            if (!s_Scene || !OnMainThread("Scene.CreateEntity")) return 0;
            Entity entity = s_Scene->CreateEntity(std::string(name));
            entity.GetComponent<TransformComponent>().Translation = { x, y, z };
            return static_cast<uint64_t>(entity.GetUUID());
        }

        // Nox.Mesh: meshes built at run time (DynamicMeshes). The handle is the mesh's asset handle.
        uint64_t MeshCreate()
        {
            if (!OnMainThread("new Mesh()")) return 0;
            return static_cast<uint64_t>(DynamicMeshes::Create());
        }

        void MeshDestroy(uint64_t mesh)
        {
            if (OnMainThread("Mesh.Dispose"))
                DynamicMeshes::Destroy(AssetHandle(mesh));
        }

        Coral::Bool32 MeshSetData(uint64_t mesh, const glm::vec3* positions, int32_t positionCount, const uint32_t* indices, int32_t indexCount,
                                  const glm::vec3* normals, int32_t normalCount, const glm::vec2* uvs, int32_t uvCount)
        {
            if (!OnMainThread("Mesh.SetData")) return false;
            auto span = [](const auto* data, int32_t count) { return std::span(data, data && count > 0 ? static_cast<size_t>(count) : 0); };
            return DynamicMeshes::SetData(AssetHandle(mesh), span(positions, positionCount), span(indices, indexCount),
                                          span(normals, normalCount), span(uvs, uvCount));
        }

        void MeshClear(uint64_t mesh)
        {
            if (OnMainThread("Mesh.Clear"))
                DynamicMeshes::Clear(AssetHandle(mesh));
        }

        void MeshSetColor(uint64_t mesh, float r, float g, float b, float a, float metallic, float roughness, float emissiveR, float emissiveG,
                          float emissiveB)
        {
            if (OnMainThread("Mesh.SetColor"))
                DynamicMeshes::SetColor(AssetHandle(mesh), { r, g, b, a }, metallic, roughness, { emissiveR, emissiveG, emissiveB });
        }

        // Mesh.SetMaterial("Materials/Grass.nmat"): a material asset by its path under the asset directory ("" = the mesh's color).
        Coral::Bool32 MeshSetMaterial(uint64_t mesh, Coral::String pathValue)
        {
            if (!OnMainThread("Mesh.SetMaterial")) return false;
            const std::string path(pathValue);
            AssetHandle material = 0;
            if (!path.empty())
            {
                material = AssetManager::FindHandleByPath(std::filesystem::path(path), AssetType::Material);
                if (material == 0)
                {
                    NOX_CORE_ERROR("[C#] Mesh.SetMaterial: no material '{}' in the asset registry", path);
                    return false;
                }
            }
            DynamicMeshes::SetMaterial(AssetHandle(mesh), material);
            return true;
        }

        // Entity.SetMesh(mesh): the entity draws the mesh (null: nothing).
        void EntitySetMesh(uint64_t entityID, uint64_t mesh)
        {
            if (!s_Scene || !OnMainThread("Entity.SetMesh")) return;
            Entity entity = s_Scene->GetEntityByUUID(UUID(entityID));
            if (!entity) return;
            if (mesh == 0)
            {
                if (entity.HasComponent<MeshComponent>())
                    entity.RemoveComponent<MeshComponent>();
                return;
            }
            entity.AddOrReplaceComponent<MeshComponent>(AssetHandle(mesh));
        }

        // Entity.SetMeshCollider(mesh): a static collider from the mesh's triangles at the entity's transform (null: none).
        void EntitySetMeshCollider(uint64_t entityID, uint64_t mesh, float friction, float restitution)
        {
            if (!s_Scene || !OnMainThread("Entity.SetMeshCollider")) return;
            IPhysics3DScene* physics = s_Scene->GetPhysics3DScene();
            Entity entity = s_Scene->GetEntityByUUID(UUID(entityID));
            if (!physics || !entity) return;
            if (const DynamicMeshes::Triangles* triangles = mesh != 0 ? DynamicMeshes::GetTriangles(AssetHandle(mesh)) : nullptr)
                physics->SetMeshCollider(entity, triangles->Positions, triangles->Indices, friction, restitution);
            else
                physics->RemoveMeshCollider(entity);
        }

        // Physics.Raycast: the first collider along the ray (rigid bodies, mesh colliders; not characters' own capsules).
        Coral::Bool32 PhysicsRaycast(float originX, float originY, float originZ, float directionX, float directionY, float directionZ,
                                     float maxDistance, ManagedRaycastHit* outHit)
        {
            *outHit = {};
            if (!s_Scene || !OnMainThread("Physics.Raycast")) return false;
            IPhysics3DScene* physics = s_Scene->GetPhysics3DScene();
            const glm::vec3 direction(directionX, directionY, directionZ);
            if (!physics || glm::length(direction) < 1e-6f || maxDistance <= 0.0f) return false;

            RayCastHit hit;
            if (!physics->RayCast({ originX, originY, originZ }, glm::normalize(direction), maxDistance, hit))
                return false;
            outHit->Entity = hit.HitEntity ? static_cast<uint64_t>(hit.HitEntity.GetUUID()) : 0;
            outHit->Point = { hit.Position.x, hit.Position.y, hit.Position.z };
            outHit->Normal = { hit.Normal.x, hit.Normal.y, hit.Normal.z };
            outHit->Distance = hit.Distance;
            return true;
        }

        // Nox.ParticleEmitterComponent.
        ParticleEmitterComponent* FindEmitter(uint64_t entityID)
        {
            if (!s_Scene) return nullptr;
            Entity entity = s_Scene->GetEntityByUUID(UUID(entityID));
            return entity && entity.HasComponent<ParticleEmitterComponent>() ? &entity.GetComponent<ParticleEmitterComponent>() : nullptr;
        }

        void ParticlesPlay(uint64_t entityID, Coral::Bool32 restart)
        {
            if (ParticleEmitterComponent* emitter = FindEmitter(entityID))
            {
                if (restart)
                    ParticleSystem::Restart(*emitter);
                emitter->Emitting = true;
            }
        }

        void ParticlesStop(uint64_t entityID)
        {
            if (ParticleEmitterComponent* emitter = FindEmitter(entityID))
                emitter->Emitting = false;
        }

        void ParticlesEmit(uint64_t entityID, int32_t count)
        {
            if (ParticleEmitterComponent* emitter = FindEmitter(entityID); emitter && count > 0)
                emitter->Simulation.PendingBurst += static_cast<uint32_t>(count);
        }

        Coral::Bool32 ParticlesIsEmitting(uint64_t entityID)
        {
            const ParticleEmitterComponent* emitter = FindEmitter(entityID);
            return emitter && emitter->Emitting;
        }

        int32_t ParticlesGetCount(uint64_t entityID)
        {
            const ParticleEmitterComponent* emitter = FindEmitter(entityID);
            return emitter ? static_cast<int32_t>(emitter->Simulation.Particles.size()) : 0;
        }

        // Nox.RigidBody3DComponent: forces and velocities of the entity's physics body (Play / Simulate only).
        template <typename Function>
        void WithRigidBody(uint64_t entityID, Function function)
        {
            if (!s_Scene) return;
            IPhysics3DScene* physics = s_Scene->GetPhysics3DScene();
            Entity entity = s_Scene->GetEntityByUUID(UUID(entityID));
            if (physics && entity && entity.HasComponent<RigidBody3DComponent>())
                function(*physics, entity);
        }

        void RigidBodySetLinearVelocity(uint64_t entityID, float x, float y, float z)
        {
            WithRigidBody(entityID, [&](IPhysics3DScene& physics, Entity entity) { physics.SetLinearVelocity(entity, { x, y, z }); });
        }

        void RigidBodyAddForce(uint64_t entityID, float x, float y, float z)
        {
            WithRigidBody(entityID, [&](IPhysics3DScene& physics, Entity entity) { physics.AddForce(entity, { x, y, z }); });
        }

        void RigidBodyAddImpulse(uint64_t entityID, float x, float y, float z)
        {
            WithRigidBody(entityID, [&](IPhysics3DScene& physics, Entity entity) { physics.AddImpulse(entity, { x, y, z }); });
        }

        void RigidBodyGetLinearVelocity(uint64_t entityID, ManagedVector3* outVelocity)
        {
            glm::vec3 velocity(0.0f);
            WithRigidBody(entityID, [&](IPhysics3DScene& physics, Entity entity) { velocity = physics.GetLinearVelocity(entity); });
            *outVelocity = { velocity.x, velocity.y, velocity.z };
        }

        uint64_t FindEntityByName(Coral::String name)
        {
            if (!s_Scene) return 0;
            Entity entity = s_Scene->FindEntityByName(std::string(name));
            return entity ? static_cast<uint64_t>(entity.GetUUID()) : 0;
        }

        uint64_t FindChild(uint64_t entityID, Coral::String pathValue)
        {
            if (!s_Scene) return 0;
            Entity current = s_Scene->GetEntityByUUID(UUID(entityID));
            std::string path(pathValue);
            size_t begin = 0;
            while (current && begin < path.size())
            {
                const size_t separator = path.find('/', begin);
                const std::string_view segment(path.data() + begin,
                    (separator == std::string::npos ? path.size() : separator) - begin);
                Entity match;
                if (current.HasComponent<RelationshipComponent>())
                {
                    for (UUID childID : current.GetComponent<RelationshipComponent>().Children)
                    {
                        Entity child = s_Scene->GetEntityByUUID(childID);
                        if (child && child.GetName() == segment)
                        {
                            match = child;
                            break;
                        }
                    }
                }
                current = match;
                if (separator == std::string::npos)
                    break;
                begin = separator + 1;
            }
            return current ? static_cast<uint64_t>(current.GetUUID()) : 0;
        }

        Coral::Bool32 HasComponent(uint64_t entityID, int32_t componentType)
        {
            if (!s_Scene) return false;
            Entity entity = s_Scene->GetEntityByUUID(UUID(entityID));
            if (!entity) return false;

            switch (componentType)
            {
                case 1: return entity.HasComponent<TransformComponent>();
                case 2: return entity.HasComponent<AnimatorComponent>();
                case 3: return entity.HasComponent<CharacterController3DComponent>();
                case 4: return entity.HasComponent<RigidBody3DComponent>();
                case 5: return entity.HasComponent<TimelinePlayerComponent>();
                case 6: return entity.HasComponent<ParticleEmitterComponent>();
                default: return false;
            }
        }

        // The Animator's graph parameters (Nox.AnimatorComponent.SetFloat/SetBool/...). Written into the
        // component's GraphInstance.Parameters, which the animation graph's GetParameter nodes read next frame.
        AnimatorComponent* FindAnimator(uint64_t entityID)
        {
            if (!s_Scene) return nullptr;
            Entity entity = s_Scene->GetEntityByUUID(UUID(entityID));
            return entity && entity.HasComponent<AnimatorComponent>() ? &entity.GetComponent<AnimatorComponent>() : nullptr;
        }

        void AnimatorSetFloat(uint64_t entityID, Coral::String name, float value)
        {
            if (AnimatorComponent* animator = FindAnimator(entityID))
                animator->GraphInstance.SetFloat(std::string(name), value);
        }

        float WorldUnitsPerMeter()
        {
            return WorldUnits::PerMeter();
        }

        float AnimatorGetFloat(uint64_t entityID, Coral::String name)
        {
            AnimatorComponent* animator = FindAnimator(entityID);
            return animator ? animator->GraphInstance.GetFloat(std::string(name)) : 0.0f;
        }

        void AnimatorSetBool(uint64_t entityID, Coral::String name, Coral::Bool32 value)
        {
            if (AnimatorComponent* animator = FindAnimator(entityID))
                animator->GraphInstance.SetBool(std::string(name), value != 0);
        }

        Coral::Bool32 AnimatorGetBool(uint64_t entityID, Coral::String name)
        {
            AnimatorComponent* animator = FindAnimator(entityID);
            return animator && animator->GraphInstance.GetBool(std::string(name));
        }

        // Montages (Nox.AnimatorComponent.PlayMontage): the clip by its path relative to the asset folder.
        Coral::Bool32 AnimatorPlayMontage(uint64_t entityID, Coral::String clipPath, Coral::String slot, float blendIn, float blendOut, float rate)
        {
            AnimatorComponent* animator = FindAnimator(entityID);
            if (!animator)
                return false;
            const std::string path = std::string(clipPath);
            const AssetHandle clip = AssetManager::FindHandleByPath(path, AssetType::AnimationSequence);
            if (clip == 0)
            {
                NOX_CORE_WARN("PlayMontage: no animation clip at '{}'", path);
                return false;
            }
            AssetManager::RequestAsset(clip);
            animator->GraphInstance.PlayMontage(clip, std::string(slot), rate, blendIn, blendOut);
            return true;
        }

        void AnimatorStopMontage(uint64_t entityID, Coral::String slot, float blendOut)
        {
            if (AnimatorComponent* animator = FindAnimator(entityID))
                animator->GraphInstance.StopMontage(std::string(slot), blendOut);
        }

        Coral::Bool32 AnimatorIsMontagePlaying(uint64_t entityID, Coral::String slot)
        {
            AnimatorComponent* animator = FindAnimator(entityID);
            return animator && animator->GraphInstance.IsMontagePlaying(std::string(slot));
        }

        // The Timeline Player (Nox.TimelinePlayerComponent): the next update plays from Time (Scene::UpdateTimelines).
        TimelinePlayerComponent* FindTimelinePlayer(uint64_t entityID)
        {
            if (!s_Scene) return nullptr;
            Entity entity = s_Scene->GetEntityByUUID(UUID(entityID));
            return entity && entity.HasComponent<TimelinePlayerComponent>() ? &entity.GetComponent<TimelinePlayerComponent>() : nullptr;
        }

        void TimelinePlay(uint64_t entityID, Coral::Bool32 fromStart)
        {
            if (TimelinePlayerComponent* player = FindTimelinePlayer(entityID))
            {
                if (fromStart)
                    player->Time = 0.0f;
                player->Playing = true;
            }
        }

        void TimelineStop(uint64_t entityID)
        {
            if (TimelinePlayerComponent* player = FindTimelinePlayer(entityID))
                player->Playing = false;
        }

        Coral::Bool32 TimelineIsPlaying(uint64_t entityID)
        {
            TimelinePlayerComponent* player = FindTimelinePlayer(entityID);
            return player && player->Playing;
        }

        float TimelineGetTime(uint64_t entityID)
        {
            TimelinePlayerComponent* player = FindTimelinePlayer(entityID);
            return player ? player->Time : 0.0f;
        }

        void TimelineSetTime(uint64_t entityID, float time)
        {
            if (TimelinePlayerComponent* player = FindTimelinePlayer(entityID))
                player->Time = std::max(time, 0.0f);
        }

        void TimelineSetSpeed(uint64_t entityID, float speed)
        {
            if (TimelinePlayerComponent* player = FindTimelinePlayer(entityID))
                player->Speed = speed;
        }

        // The Character Controller 3D (Nox.CharacterControllerComponent): inputs are read by the next fixed physics
        // step, state is what the last step produced.
        CharacterController3DComponent* FindCharacter(uint64_t entityID)
        {
            if (!s_Scene) return nullptr;
            Entity entity = s_Scene->GetEntityByUUID(UUID(entityID));
            return entity && entity.HasComponent<CharacterController3DComponent>() ? &entity.GetComponent<CharacterController3DComponent>() : nullptr;
        }

        void CharacterSetMoveVelocity(uint64_t entityID, float x, float y, float z)
        {
            if (CharacterController3DComponent* character = FindCharacter(entityID))
                character->MoveVelocity = { x, y, z };
        }

        void CharacterJump(uint64_t entityID, float speed)
        {
            if (CharacterController3DComponent* character = FindCharacter(entityID))
                character->JumpSpeed = speed;
        }

        Coral::Bool32 CharacterIsGrounded(uint64_t entityID)
        {
            CharacterController3DComponent* character = FindCharacter(entityID);
            return character && character->IsGrounded;
        }

        void CharacterGetVelocity(uint64_t entityID, ManagedVector3* outVelocity)
        {
            CharacterController3DComponent* character = FindCharacter(entityID);
            const glm::vec3 velocity = character ? character->Velocity : glm::vec3(0.0f);
            *outVelocity = { velocity.x, velocity.y, velocity.z };
        }

        ManagedTransform ToManagedTransform(const TransformComponent& transform)
        {
            return {
                { transform.Translation.x, transform.Translation.y, transform.Translation.z },
                { transform.Rotation.x, transform.Rotation.y, transform.Rotation.z },
                { transform.Scale.x, transform.Scale.y, transform.Scale.z }
            };
        }

        TransformComponent FromManagedTransform(const ManagedTransform& transform)
        {
            TransformComponent result;
            result.Translation = { transform.Position.X, transform.Position.Y, transform.Position.Z };
            result.Rotation = { transform.Rotation.X, transform.Rotation.Y, transform.Rotation.Z };
            result.Scale = { transform.Scale.X, transform.Scale.Y, transform.Scale.Z };
            return result;
        }

        void GetLocalTransform(uint64_t entityID, ManagedTransform* value)
        {
            if (!s_Scene || !value) return;
            Entity entity = s_Scene->GetEntityByUUID(UUID(entityID));
            if (!entity || !entity.HasComponent<TransformComponent>()) return;
            *value = ToManagedTransform(entity.GetComponent<TransformComponent>());
        }

        void SetLocalTransform(uint64_t entityID, const ManagedTransform* value)
        {
            if (!s_Scene || !value) return;
            Entity entity = s_Scene->GetEntityByUUID(UUID(entityID));
            if (!entity || !entity.HasComponent<TransformComponent>()) return;
            entity.GetComponent<TransformComponent>() = FromManagedTransform(*value);
            entity.MarkTransformDirty();
        }

        void GetWorldTransform(uint64_t entityID, ManagedTransform* value)
        {
            if (!s_Scene || !value) return;
            Entity entity = s_Scene->GetEntityByUUID(UUID(entityID));
            if (!entity || !entity.HasComponent<TransformComponent>()) return;
            const glm::mat4 world = entity.HasComponent<WorldTransformComponent>()
                ? entity.GetComponent<WorldTransformComponent>().WorldMatrix
                : entity.GetComponent<TransformComponent>().GetTransform();
            TransformComponent transform;
            Math::DecomposeTransform(world, transform.Translation, transform.Rotation, transform.Scale);
            *value = ToManagedTransform(transform);
        }

        void SetWorldTransform(uint64_t entityID, const ManagedTransform* value)
        {
            if (!s_Scene || !value) return;
            Entity entity = s_Scene->GetEntityByUUID(UUID(entityID));
            if (!entity || !entity.HasComponent<TransformComponent>()) return;
            entity.SetWorldTransform(FromManagedTransform(*value).GetTransform());
        }
    }

    bool DotNetBackend::Initialize(const std::filesystem::path& runtimeDirectory)
    {
        Coral::HostSettings settings;
        settings.CoralDirectory = runtimeDirectory.string();
        settings.MessageCallback = [](std::string_view message, Coral::MessageLevel)
        {
            NOX_CORE_ERROR("[Coral] {}", message);
        };
        settings.ExceptionCallback = [](std::string_view message)
        {
            NOX_CORE_ERROR("[C# exception] {}", message);
        };
        return m_Host.Initialize(std::move(settings)) == Coral::CoralInitStatus::Success;
    }

    void DotNetBackend::Shutdown()
    {
        WaitForJobs(2000);
        ClearModule();
        m_Host.Shutdown();
    }

    bool DotNetBackend::LoadModule(const std::filesystem::path& coreAssembly,
                                   const std::filesystem::path& gameAssembly)
    {
        // Into a load context of its own first: the module running now stays until the new one has loaded (a reload of a
        // broken build keeps the scripts that worked).
        const std::string searchPath = coreAssembly.parent_path().string() + ";" +
                                       gameAssembly.parent_path().string();
        auto loadContext = std::make_unique<Coral::AssemblyLoadContext>(
            m_Host.CreateAssemblyLoadContext("NoxGameScripts-" + std::to_string(++m_LoadContextCount), searchPath));
        Coral::ManagedAssembly* core = &loadContext->LoadAssembly(coreAssembly.string());
        Coral::ManagedAssembly* game = &loadContext->LoadAssembly(gameAssembly.string());
        if (core->GetLoadStatus() != Coral::AssemblyLoadStatus::Success ||
            game->GetLoadStatus() != Coral::AssemblyLoadStatus::Success)
        {
            NOX_CORE_ERROR("Failed to load managed scripting assemblies '{}', '{}'{}", coreAssembly.string(), gameAssembly.string(),
                           m_LoadContext ? ": the scripts loaded before keep running" : "");
            m_Host.UnloadAssemblyLoadContext(*loadContext);
            return false;
        }

        ClearModule();
        m_LoadContext = std::move(loadContext);
        m_CoreAssembly = core;
        m_GameAssembly = game;
        m_CorePath = coreAssembly;
        m_GamePath = gameAssembly;
        // Handles of the module before (kept anywhere) no longer reach the new instances.
        ++m_Generation;

        RegisterInternalCalls(*m_CoreAssembly);
        auto& entityBehaviour = m_CoreAssembly->GetLocalType("Nox.EntityBehaviour");
        for (auto& type : m_GameAssembly->GetLocalTypes())
        {
            if (type.IsSubclassOf(entityBehaviour))
                m_Types.emplace(std::string(type.GetFullName()), &type);
        }

        for (const auto& [className, type] : m_Types)
        {
            Coral::ManagedObject defaults = type->CreateInstance();
            auto& exposed = m_ExposedFields[className];
            for (Coral::FieldInfo field : type->GetFields())
            {
                bool isExposed = false;
                bool disallowSelf = false;
                std::string unit;
                for (Coral::Attribute attribute : field.GetAttributes())
                {
                    const std::string attributeName(attribute.GetType().GetFullName());
                    if (attributeName == "Nox.ExposeAttribute")
                    {
                        isExposed = true;
                        unit = attribute.GetFieldValue<std::string>("Unit");
                    }
                    else if (attributeName == "Nox.DisallowSelfAttribute")
                        disallowSelf = true;
                }
                if (!isExposed) continue;

                const std::string name(field.GetName());
                const std::string fieldType(field.GetType().GetFullName());
                const size_t fieldCountBefore = exposed.size();
                if (fieldType == "System.Boolean") exposed.push_back({name, ScriptFieldType::Bool, defaults.GetFieldValue<bool>(name)});
                else if (fieldType == "System.Int32") exposed.push_back({name, ScriptFieldType::Int, defaults.GetFieldValue<int32_t>(name)});
                else if (fieldType == "System.UInt32") exposed.push_back({name, ScriptFieldType::UInt, defaults.GetFieldValue<uint32_t>(name)});
                else if (fieldType == "System.Int64") exposed.push_back({name, ScriptFieldType::Long, defaults.GetFieldValue<int64_t>(name)});
                else if (fieldType == "System.UInt64") exposed.push_back({name, ScriptFieldType::ULong, defaults.GetFieldValue<uint64_t>(name)});
                else if (fieldType == "System.Single") exposed.push_back({name, ScriptFieldType::Float, defaults.GetFieldValue<float>(name)});
                else if (fieldType == "System.Double") exposed.push_back({name, ScriptFieldType::Double, defaults.GetFieldValue<double>(name)});
                else if (fieldType == "System.String") exposed.push_back({name, ScriptFieldType::String, defaults.GetFieldValue<std::string>(name)});
                else if (fieldType == "Nox.Vector3")
                {
                    const ManagedVector3 value = defaults.GetFieldValue<ManagedVector3>(name);
                    exposed.push_back({name, ScriptFieldType::Vector3, glm::vec3(value.X, value.Y, value.Z)});
                }
                else if (fieldType == "Nox.Entity") exposed.push_back({name, ScriptFieldType::Entity, UUID(0)});
                else NOX_CORE_WARN("Unsupported [Expose] field '{}.{}' of type '{}'", className, name, fieldType);
                if (exposed.size() != fieldCountBefore)
                {
                    exposed.back().DisallowSelf = disallowSelf;
                    exposed.back().Unit = unit;
                }
            }
            defaults.Destroy();
        }
        NOX_CORE_INFO("Loaded {} C# script types", m_Types.size());
        return true;
    }

    bool DotNetBackend::WaitForJobs(uint32_t timeoutMilliseconds)
    {
        if (!m_CoreAssembly)
            return true;
        // Const: the non-const overload of InvokeStaticMethod returns nothing.
        const Coral::Type& jobs = m_CoreAssembly->GetLocalType("Nox.Jobs");
        if (!jobs)
            return true;
        const int32_t stillRunning = jobs.InvokeStaticMethod<int32_t>("WaitForAll", static_cast<int32_t>(timeoutMilliseconds));
        if (stillRunning > 0)
            NOX_CORE_WARN("{} script job(s) still running after {} ms", stillRunning, timeoutMilliseconds);
        return stillRunning <= 0;
    }

    void DotNetBackend::ClearModule()
    {
        for (auto& [_, instance] : m_Instances)
            if (instance->Object.IsValid()) instance->Object.Destroy();
        m_Instances.clear();
        m_Types.clear();
        m_ExposedFields.clear();
        m_CallbackMasks.clear();
        m_CoreAssembly = nullptr;
        m_GameAssembly = nullptr;
        if (m_LoadContext)
        {
            m_Host.UnloadAssemblyLoadContext(*m_LoadContext);
            m_LoadContext.reset();
        }
    }

    void DotNetBackend::RegisterInternalCalls(Coral::ManagedAssembly& assembly)
    {
        assembly.AddInternalCall("Nox.InternalCalls", "Log_Info", reinterpret_cast<void*>(&LogInfo));
        assembly.AddInternalCall("Nox.InternalCalls", "Input_IsKeyDown", reinterpret_cast<void*>(&IsKeyDown));
        assembly.AddInternalCall("Nox.InternalCalls", "Input_IsMouseButtonDown", reinterpret_cast<void*>(&IsMouseButtonDown));
        assembly.AddInternalCall("Nox.InternalCalls", "Scene_Instantiate", reinterpret_cast<void*>(&SceneInstantiate));
        assembly.AddInternalCall("Nox.InternalCalls", "Scene_Destroy", reinterpret_cast<void*>(&SceneDestroy));
        assembly.AddInternalCall("Nox.InternalCalls", "Scene_CreateEntity", reinterpret_cast<void*>(&SceneCreateEntity));
        assembly.AddInternalCall("Nox.InternalCalls", "Mesh_Create", reinterpret_cast<void*>(&MeshCreate));
        assembly.AddInternalCall("Nox.InternalCalls", "Mesh_Destroy", reinterpret_cast<void*>(&MeshDestroy));
        assembly.AddInternalCall("Nox.InternalCalls", "Mesh_SetData", reinterpret_cast<void*>(&MeshSetData));
        assembly.AddInternalCall("Nox.InternalCalls", "Mesh_Clear", reinterpret_cast<void*>(&MeshClear));
        assembly.AddInternalCall("Nox.InternalCalls", "Mesh_SetColor", reinterpret_cast<void*>(&MeshSetColor));
        assembly.AddInternalCall("Nox.InternalCalls", "Mesh_SetMaterial", reinterpret_cast<void*>(&MeshSetMaterial));
        assembly.AddInternalCall("Nox.InternalCalls", "Entity_SetMesh", reinterpret_cast<void*>(&EntitySetMesh));
        assembly.AddInternalCall("Nox.InternalCalls", "Entity_SetMeshCollider", reinterpret_cast<void*>(&EntitySetMeshCollider));
        assembly.AddInternalCall("Nox.InternalCalls", "Physics_Raycast", reinterpret_cast<void*>(&PhysicsRaycast));
        assembly.AddInternalCall("Nox.InternalCalls", "RigidBody_SetLinearVelocity", reinterpret_cast<void*>(&RigidBodySetLinearVelocity));
        assembly.AddInternalCall("Nox.InternalCalls", "RigidBody_AddForce", reinterpret_cast<void*>(&RigidBodyAddForce));
        assembly.AddInternalCall("Nox.InternalCalls", "RigidBody_AddImpulse", reinterpret_cast<void*>(&RigidBodyAddImpulse));
        assembly.AddInternalCall("Nox.InternalCalls", "RigidBody_GetLinearVelocity", reinterpret_cast<void*>(&RigidBodyGetLinearVelocity));
        assembly.AddInternalCall("Nox.InternalCalls", "Entity_FindByName", reinterpret_cast<void*>(&FindEntityByName));
        assembly.AddInternalCall("Nox.InternalCalls", "Entity_FindChild", reinterpret_cast<void*>(&FindChild));
        assembly.AddInternalCall("Nox.InternalCalls", "Entity_HasComponent", reinterpret_cast<void*>(&HasComponent));
        assembly.AddInternalCall("Nox.InternalCalls", "Animator_SetFloat", reinterpret_cast<void*>(&AnimatorSetFloat));
        assembly.AddInternalCall("Nox.InternalCalls", "Animator_GetFloat", reinterpret_cast<void*>(&AnimatorGetFloat));
        assembly.AddInternalCall("Nox.InternalCalls", "Animator_SetBool", reinterpret_cast<void*>(&AnimatorSetBool));
        assembly.AddInternalCall("Nox.InternalCalls", "Animator_GetBool", reinterpret_cast<void*>(&AnimatorGetBool));
        assembly.AddInternalCall("Nox.InternalCalls", "Animator_PlayMontage", reinterpret_cast<void*>(&AnimatorPlayMontage));
        assembly.AddInternalCall("Nox.InternalCalls", "Animator_StopMontage", reinterpret_cast<void*>(&AnimatorStopMontage));
        assembly.AddInternalCall("Nox.InternalCalls", "Animator_IsMontagePlaying", reinterpret_cast<void*>(&AnimatorIsMontagePlaying));
        assembly.AddInternalCall("Nox.InternalCalls", "Timeline_Play", reinterpret_cast<void*>(&TimelinePlay));
        assembly.AddInternalCall("Nox.InternalCalls", "Timeline_Stop", reinterpret_cast<void*>(&TimelineStop));
        assembly.AddInternalCall("Nox.InternalCalls", "Timeline_IsPlaying", reinterpret_cast<void*>(&TimelineIsPlaying));
        assembly.AddInternalCall("Nox.InternalCalls", "Timeline_GetTime", reinterpret_cast<void*>(&TimelineGetTime));
        assembly.AddInternalCall("Nox.InternalCalls", "Timeline_SetTime", reinterpret_cast<void*>(&TimelineSetTime));
        assembly.AddInternalCall("Nox.InternalCalls", "Timeline_SetSpeed", reinterpret_cast<void*>(&TimelineSetSpeed));
        assembly.AddInternalCall("Nox.InternalCalls", "Particles_Play", reinterpret_cast<void*>(&ParticlesPlay));
        assembly.AddInternalCall("Nox.InternalCalls", "Particles_Stop", reinterpret_cast<void*>(&ParticlesStop));
        assembly.AddInternalCall("Nox.InternalCalls", "Particles_Emit", reinterpret_cast<void*>(&ParticlesEmit));
        assembly.AddInternalCall("Nox.InternalCalls", "Particles_IsEmitting", reinterpret_cast<void*>(&ParticlesIsEmitting));
        assembly.AddInternalCall("Nox.InternalCalls", "Particles_GetCount", reinterpret_cast<void*>(&ParticlesGetCount));
        assembly.AddInternalCall("Nox.InternalCalls", "Character_SetMoveVelocity", reinterpret_cast<void*>(&CharacterSetMoveVelocity));
        assembly.AddInternalCall("Nox.InternalCalls", "Character_Jump", reinterpret_cast<void*>(&CharacterJump));
        assembly.AddInternalCall("Nox.InternalCalls", "Character_IsGrounded", reinterpret_cast<void*>(&CharacterIsGrounded));
        assembly.AddInternalCall("Nox.InternalCalls", "Character_GetVelocity", reinterpret_cast<void*>(&CharacterGetVelocity));
        assembly.AddInternalCall("Nox.InternalCalls", "Transform_GetLocal", reinterpret_cast<void*>(&GetLocalTransform));
        assembly.AddInternalCall("Nox.InternalCalls", "Transform_SetLocal", reinterpret_cast<void*>(&SetLocalTransform));
        assembly.AddInternalCall("Nox.InternalCalls", "Transform_GetWorld", reinterpret_cast<void*>(&GetWorldTransform));
        assembly.AddInternalCall("Nox.InternalCalls", "Transform_SetWorld", reinterpret_cast<void*>(&SetWorldTransform));
        assembly.AddInternalCall("Nox.InternalCalls", "WorldUnits_PerMeter", reinterpret_cast<void*>(&WorldUnitsPerMeter));
        assembly.UploadInternalCalls();
    }

    bool DotNetBackend::HasType(std::string_view fullName) const
    {
        return m_Types.contains(std::string(fullName));
    }

    std::vector<ScriptFieldInfo> DotNetBackend::GetExposedFields(std::string_view fullName) const
    {
        auto it = m_ExposedFields.find(std::string(fullName));
        return it == m_ExposedFields.end() ? std::vector<ScriptFieldInfo>{} : it->second;
    }

    ScriptInstanceHandle DotNetBackend::CreateInstance(std::string_view fullName, UUID entity)
    {
        auto it = m_Types.find(std::string(fullName));
        if (it == m_Types.end()) return {};
        auto instance = std::make_unique<Instance>();
        instance->Generation = m_Generation;
        instance->ClassName = it->first;
        instance->Object = it->second->CreateInstance();
        instance->Object.SetFieldValue("EntityID", static_cast<uint64_t>(entity));
        auto mask = m_CallbackMasks.find(instance->ClassName);
        if (mask == m_CallbackMasks.end())
            mask = m_CallbackMasks.emplace(instance->ClassName, instance->Object.InvokeMethod<int32_t>("GetCallbackMask")).first;
        instance->CallbackMask = mask->second;
        const uint64_t id = m_NextInstanceID++;
        m_Instances.emplace(id, std::move(instance));
        return { id, m_Generation };
    }

    void DotNetBackend::SetEntityField(ScriptInstanceHandle handle, std::string_view fieldName, UUID entity)
    {
        auto it = m_Instances.find(handle.ID);
        if (it == m_Instances.end() || handle.Generation != m_Generation)
            return;

        struct ManagedEntity { uint64_t ID; };
        it->second->Object.SetFieldValue(fieldName, ManagedEntity{ static_cast<uint64_t>(entity) });
    }

    void DotNetBackend::SetFieldValue(ScriptInstanceHandle handle, std::string_view fieldName, const ScriptValue& value)
    {
        auto it = m_Instances.find(handle.ID);
        if (it == m_Instances.end() || handle.Generation != m_Generation) return;
        auto& object = it->second->Object;
        std::visit([&](const auto& data)
        {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, std::string>) object.SetFieldValue(fieldName, data);
            else if constexpr (std::is_same_v<T, glm::vec3>) object.SetFieldValue(fieldName, ManagedVector3{data.x, data.y, data.z});
            else if constexpr (!std::is_same_v<T, std::monostate> && !std::is_same_v<T, UUID>) object.SetFieldValue(fieldName, data);
        }, value);
    }

    ScriptValue DotNetBackend::GetFieldValue(ScriptInstanceHandle handle, std::string_view fieldName, ScriptFieldType type) const
    {
        auto it = m_Instances.find(handle.ID);
        if (it == m_Instances.end() || handle.Generation != m_Generation)
            return {};
        const Coral::ManagedObject& object = it->second->Object;
        switch (type)
        {
            case ScriptFieldType::Bool: return object.GetFieldValue<bool>(fieldName);
            case ScriptFieldType::Int: return object.GetFieldValue<int32_t>(fieldName);
            case ScriptFieldType::UInt: return object.GetFieldValue<uint32_t>(fieldName);
            case ScriptFieldType::Long: return object.GetFieldValue<int64_t>(fieldName);
            case ScriptFieldType::ULong: return object.GetFieldValue<uint64_t>(fieldName);
            case ScriptFieldType::Float: return object.GetFieldValue<float>(fieldName);
            case ScriptFieldType::Double: return object.GetFieldValue<double>(fieldName);
            case ScriptFieldType::String: return object.GetFieldValue<std::string>(fieldName);
            case ScriptFieldType::Vector3:
            {
                const ManagedVector3 value = object.GetFieldValue<ManagedVector3>(fieldName);
                return glm::vec3(value.X, value.Y, value.Z);
            }
            case ScriptFieldType::Entity:
            {
                struct ManagedEntity { uint64_t ID; };
                return UUID(object.GetFieldValue<ManagedEntity>(fieldName).ID);
            }
        }
        return {};
    }

    std::string DotNetBackend::GetInstanceClass(ScriptInstanceHandle handle) const
    {
        auto it = m_Instances.find(handle.ID);
        return it == m_Instances.end() || handle.Generation != m_Generation ? std::string() : it->second->ClassName;
    }

    void DotNetBackend::DestroyInstance(ScriptInstanceHandle handle)
    {
        auto it = m_Instances.find(handle.ID);
        if (it == m_Instances.end()) return;
        it->second->Object.InvokeMethod("OnDestroy");
        it->second->Object.Destroy();
        m_Instances.erase(it);
    }

    void DotNetBackend::Invoke(ScriptInstanceHandle handle, ScriptCallback callback, float argument)
    {
        auto it = m_Instances.find(handle.ID);
        if (it == m_Instances.end() || handle.Generation != m_Generation) return;
        switch (callback)
        {
            case ScriptCallback::Create: it->second->Object.InvokeMethod("OnCreate"); break;
            case ScriptCallback::Update: it->second->Object.InvokeMethod("OnUpdate", argument); break;
            case ScriptCallback::Destroy: it->second->Object.InvokeMethod("OnDestroy"); break;
            case ScriptCallback::AfterReload: it->second->Object.InvokeMethod("OnAfterReload"); break;
            case ScriptCallback::FixedUpdate:
                if (it->second->CallbackMask & 1)
                    it->second->Object.InvokeMethod("OnFixedUpdate", argument);
                break;
            case ScriptCallback::LateUpdate:
                if (it->second->CallbackMask & 2)
                    it->second->Object.InvokeMethod("OnLateUpdate", argument);
                break;
        }
    }

    bool DotNetBackend::Implements(ScriptInstanceHandle handle, ScriptCallback callback) const
    {
        auto it = m_Instances.find(handle.ID);
        if (it == m_Instances.end() || handle.Generation != m_Generation)
            return false;
        if (callback == ScriptCallback::FixedUpdate)
            return (it->second->CallbackMask & 1) != 0;
        if (callback == ScriptCallback::LateUpdate)
            return (it->second->CallbackMask & 2) != 0;
        return true;
    }

    void DotNetBackend::InvokeWithString(ScriptInstanceHandle handle, std::string_view method, std::string_view argument)
    {
        auto it = m_Instances.find(handle.ID);
        if (it == m_Instances.end() || handle.Generation != m_Generation) return;
        Coral::String value = Coral::String::New(argument);
        it->second->Object.InvokeMethod(method, value);
        Coral::String::Free(value);
    }

    void DotNetBackend::SetSceneContext(Scene* scene)
    {
        m_Scene = scene;
        s_Scene = scene;
        s_MainThread = std::this_thread::get_id();
    }
}
