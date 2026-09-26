#include "DotNetBackend.h"

#include <Coral/Attribute.hpp>
#include <Coral/String.hpp>
#include <SDL3/SDL_scancode.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Core/Input.h"
#include "NoxCore/Physics/Physics3DScene.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Math/Math.h"
#include "NoxCore/Scene/Entity.h"

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
        Scene* s_Scene = nullptr;

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
            const AssetHandle prefab = Project::GetActive()->GetEditorAssetManager()->FindHandleByPath(std::filesystem::path(std::string(path)), AssetType::Prefab);
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
        ClearModule();
        m_Host.Shutdown();
    }

    bool DotNetBackend::LoadModule(const std::filesystem::path& coreAssembly,
                                   const std::filesystem::path& gameAssembly)
    {
        ClearModule();
        m_CorePath = coreAssembly;
        m_GamePath = gameAssembly;

        const std::string searchPath = coreAssembly.parent_path().string() + ";" +
                                       gameAssembly.parent_path().string();
        m_LoadContext = std::make_unique<Coral::AssemblyLoadContext>(
            m_Host.CreateAssemblyLoadContext("NoxGameScripts", searchPath));
        m_CoreAssembly = &m_LoadContext->LoadAssembly(coreAssembly.string());
        m_GameAssembly = &m_LoadContext->LoadAssembly(gameAssembly.string());
        if (m_CoreAssembly->GetLoadStatus() != Coral::AssemblyLoadStatus::Success ||
            m_GameAssembly->GetLoadStatus() != Coral::AssemblyLoadStatus::Success)
        {
            NOX_CORE_ERROR("Failed to load managed scripting assemblies");
            ClearModule();
            return false;
        }

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
                for (Coral::Attribute attribute : field.GetAttributes())
                {
                    const std::string attributeName(attribute.GetType().GetFullName());
                    if (attributeName == "Nox.ExposeAttribute")
                        isExposed = true;
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
                    exposed.back().DisallowSelf = disallowSelf;
            }
            defaults.Destroy();
        }
        NOX_CORE_INFO("Loaded {} C# script types", m_Types.size());
        return true;
    }

    bool DotNetBackend::ReloadModule()
    {
        ++m_Generation;
        return LoadModule(m_CorePath, m_GamePath);
    }

    void DotNetBackend::ClearModule()
    {
        for (auto& [_, instance] : m_Instances)
            if (instance->Object.IsValid()) instance->Object.Destroy();
        m_Instances.clear();
        m_Types.clear();
        m_ExposedFields.clear();
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
        assembly.AddInternalCall("Nox.InternalCalls", "Character_SetMoveVelocity", reinterpret_cast<void*>(&CharacterSetMoveVelocity));
        assembly.AddInternalCall("Nox.InternalCalls", "Character_Jump", reinterpret_cast<void*>(&CharacterJump));
        assembly.AddInternalCall("Nox.InternalCalls", "Character_IsGrounded", reinterpret_cast<void*>(&CharacterIsGrounded));
        assembly.AddInternalCall("Nox.InternalCalls", "Character_GetVelocity", reinterpret_cast<void*>(&CharacterGetVelocity));
        assembly.AddInternalCall("Nox.InternalCalls", "Transform_GetLocal", reinterpret_cast<void*>(&GetLocalTransform));
        assembly.AddInternalCall("Nox.InternalCalls", "Transform_SetLocal", reinterpret_cast<void*>(&SetLocalTransform));
        assembly.AddInternalCall("Nox.InternalCalls", "Transform_GetWorld", reinterpret_cast<void*>(&GetWorldTransform));
        assembly.AddInternalCall("Nox.InternalCalls", "Transform_SetWorld", reinterpret_cast<void*>(&SetWorldTransform));
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
        instance->Object = it->second->CreateInstance();
        instance->Object.SetFieldValue("EntityID", static_cast<uint64_t>(entity));
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
        }
    }

    void DotNetBackend::SetSceneContext(Scene* scene)
    {
        m_Scene = scene;
        s_Scene = scene;
    }
}
