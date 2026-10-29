#include "Scene.h"
#include "ModelInstance.h"
#include "PrefabInstance.h"
#include "ParticleSystem.h"

#include <box2d/box2d.h>
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <functional>
#include <unordered_set>

#include "Entity.h"
#include "Components.h"
#include "SceneGraph.h"
#include "SceneLoader.h"
#include "NoxCore/Animation/AnimPose.h"
#include "NoxCore/Math/Math.h"
#include "NoxCore/Animation/Timeline.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Physics/Physics2D.h"
#include "NoxCore/Physics/Physics3DScene.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/DynamicMesh.h"
#include "NoxCore/Scripting/ScriptEngine.h"
#include "NoxCore/Tasks/JobSystem.h"

namespace Nox
{
    // The scene's sun: its first directional light, or the procedural sky's own sun when there is none (SubmitLights
    // submits that one as the scene's light).
    struct SceneSun
    {
        glm::vec3 Direction = DefaultSkySunDirection(); // towards the sun, not normalized
        glm::vec3 Illuminance = glm::vec3(DefaultSkySunIntensity);
        float AngularRadius = 0.004675f;
    };

    static SceneSun FindSun(entt::registry& registry)
    {
        SceneSun sun;
        auto lights = registry.view<WorldTransformComponent, DirectionalLightComponent>();
        for (auto entity : lights)
        {
            auto [wtc, light] = lights.get<WorldTransformComponent, DirectionalLightComponent>(entity);
            // Renderer::SubmitLight's convention: the light shines along its local -Z, the sun is the other way.
            sun.Direction = -glm::vec3(wtc.WorldMatrix * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
            sun.Illuminance = light.Color * light.Intensity;
            sun.AngularRadius = glm::radians(glm::max(light.AngularDiameter, 0.0f)) * 0.5f;
            break;
        }
        return sun;
    }

    // The atmosphere a procedural sky has (the air the sun and moon shine through).
    static SkyAtmosphereSettings MakeSkyAir(const EnvironmentLightComponent& environment)
    {
        SkyAtmosphereSettings air;
        air.RayleighScale = environment.RayleighScale;
        air.MieScale = environment.MieScale;
        air.OzoneScale = environment.OzoneScale;
        air.AltitudeKm = environment.AltitudeKm;
        return air;
    }

    // The sun and the moon of a procedural sky as they light the scene. Seen through the air (SunThroughAtmosphere) the sun is
    // yellow-white at noon, red at sunset and gone below the horizon. The moon lights the scene and the sky once it counts next
    // to the sun (after sunset in practice: by day it would only cost a shadow ray per pixel). Without a procedural sky the
    // sun is the Directional Light as set and there is no moon.
    struct SkyLights
    {
        SceneSun Sun;
        glm::vec3 SunTransmittance{ 1.0f };
        bool MoonLights = false;
        glm::vec3 MoonDirection{ 0.0f, -1.0f, 0.0f };
        glm::vec3 MoonIlluminance{ 0.0f };     // above the air, its lit fraction included
        glm::vec3 MoonFull{ 0.0f };            // a full moon's (its disc); 0 = no moon
        glm::vec3 MoonTransmittance{ 0.0f };
        float MoonAngularRadius = 0.0046f;
        float StarIntensity = 0.0f;
    };

    static float Luminance(const glm::vec3& color)
    {
        return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    }

    static SkyLights ResolveSkyLights(entt::registry& registry, const EnvironmentLightComponent* environment)
    {
        SkyLights lights;
        lights.Sun = FindSun(registry);
        if (!environment || environment->Source != EnvironmentSource::ProceduralSky)
            return lights;

        const SkyAtmosphereSettings air = MakeSkyAir(*environment);
        const glm::vec3 sunDirection = glm::normalize(lights.Sun.Direction);
        if (environment->SunThroughAtmosphere)
            lights.SunTransmittance = Renderer::ComputeSkyTransmittance(air, sunDirection, lights.Sun.AngularRadius);

        if (environment->Moon && environment->MoonIntensity > 0.0f)
        {
            glm::vec3 moonDirection = -sunDirection;
            if (!environment->MoonOppositeSun)
            {
                const float elevation = glm::radians(glm::clamp(environment->MoonElevation, -90.0f, 90.0f));
                const float azimuth = glm::radians(environment->MoonAzimuth);
                moonDirection = glm::vec3(std::cos(elevation) * std::sin(azimuth), std::sin(elevation), std::cos(elevation) * std::cos(azimuth));
            }
            lights.MoonDirection = moonDirection;
            lights.MoonAngularRadius = glm::radians(glm::max(environment->MoonAngularDiameter, 0.01f)) * 0.5f;
            lights.MoonFull = glm::max(environment->MoonColor, glm::vec3(0.0f)) * environment->MoonIntensity;
            // The lit fraction of its disc: all of it opposite the sun (full), none next to it (new).
            const float litFraction = 0.5f * (1.0f - glm::dot(sunDirection, moonDirection));
            lights.MoonIlluminance = lights.MoonFull * litFraction;
            lights.MoonTransmittance = environment->SunThroughAtmosphere
                ? Renderer::ComputeSkyTransmittance(air, moonDirection, lights.MoonAngularRadius)
                : glm::vec3(moonDirection.y > 0.0f ? 1.0f : 0.0f);
            const float sunAtGround = Luminance(lights.Sun.Illuminance * lights.SunTransmittance);
            const float moonAtGround = Luminance(lights.MoonIlluminance * lights.MoonTransmittance);
            lights.MoonLights = moonAtGround > 0.0f && moonAtGround > 0.01f * sunAtGround;
        }

        // The stars come out as the sky darkens: none while the sun is up, all from 12 degrees below the horizon (nautical dusk).
        if (environment->Stars)
        {
            const float sunElevation = glm::degrees(std::asin(glm::clamp(sunDirection.y, -1.0f, 1.0f)));
            lights.StarIntensity = glm::max(environment->StarIntensity, 0.0f) * glm::smoothstep(0.0f, 12.0f, -sunElevation);
        }
        return lights;
    }

    // A directional light's rotation whose -Z (the way it shines) points away from `towardLight`.
    static glm::mat4 DirectionalLightTransform(const glm::vec3& towardLight)
    {
        const glm::vec3 forward = -glm::normalize(towardLight);
        const glm::vec3 up = std::abs(forward.y) > 0.999f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        return glm::toMat4(glm::quatLookAt(forward, up));
    }

    // The procedural sky's settings: the atmosphere from the component, the sun and moon from the scene.
    static SkyAtmosphereSettings MakeSkyAtmosphereSettings(entt::registry& registry, const EnvironmentLightComponent& environment)
    {
        SkyAtmosphereSettings settings;
        const SkyLights lights = ResolveSkyLights(registry, &environment);
        const SceneSun& sun = lights.Sun;
        settings.SunDirection = sun.Direction;
        settings.SunIlluminance = sun.Illuminance;
        settings.SunAngularRadius = sun.AngularRadius;
        settings.GroundAlbedo = environment.GroundAlbedo;
        settings.RayleighScale = environment.RayleighScale;
        settings.MieScale = environment.MieScale;
        settings.OzoneScale = environment.OzoneScale;
        settings.MieAnisotropy = environment.MieAnisotropy;
        settings.AltitudeKm = environment.AltitudeKm;
        settings.AerialPerspectiveScale = environment.AerialPerspectiveScale;
        settings.Clouds = environment.Clouds;
        settings.CloudCoverage = environment.CloudCoverage;
        settings.CloudDensity = environment.CloudDensity;
        settings.CloudBottomKm = environment.CloudBottomKm;
        settings.CloudThicknessKm = environment.CloudThicknessKm;
        settings.CloudScaleKm = environment.CloudScaleKm;
        settings.CloudOffsetKm = environment.CloudOffsetKm;
        settings.CloudWindKmPerSecond = environment.CloudWind / 1000.0f;
        settings.MoonDirection = lights.MoonDirection;
        settings.MoonIlluminance = lights.MoonLights ? lights.MoonIlluminance : glm::vec3(0.0f);
        settings.MoonDiscIlluminance = lights.MoonFull;
        settings.MoonAngularRadius = lights.MoonAngularRadius;
        settings.StarIntensity = lights.StarIntensity;
        return settings;
    }

    // The scene's environment: the first enabled component with something to show, or null.
    static const EnvironmentLightComponent* FindEnvironment(entt::registry& registry)
    {
        auto view = registry.view<EnvironmentLightComponent>();
        for (auto entity : view)
        {
            const auto& environment = view.get<EnvironmentLightComponent>(entity);
            if (environment.Enabled && (environment.Source == EnvironmentSource::ProceduralSky || !environment.TexturePath.empty()))
                return &environment;
        }
        return nullptr;
    }

    void Scene::SetRenderer(Renderer* renderer)
    {
        m_renderer = renderer;
        if (!m_renderer)
            return;

        // Environment settings are scene data, not global renderer defaults. The first
        // enabled component owns the scene environment; multiple environments are not
        // physically meaningful for the current IBL pipeline.
        // Called on the main thread every frame (editor and runtime player): the one place the environment is applied,
        // since changing it waits for the GPU and rebuilds the IBL set.
        const EnvironmentLightComponent* environment = FindEnvironment(m_Registry);
        if (!environment)
            m_renderer->SetEnvironmentMap({});
        else if (environment->Source == EnvironmentSource::ProceduralSky)
            m_renderer->SetProceduralSky(MakeSkyAtmosphereSettings(m_Registry, *environment));
        else
            m_renderer->SetEnvironmentMap(Project::GetActiveAssetDirectory() / environment->TexturePath, environment->Rotation);

        HeightFogSettings fog;
        if (environment && environment->HeightFog)
        {
            fog.Enabled = true;
            fog.DensityPerKm = environment->FogDensity;
            fog.FalloffPerKm = environment->FogHeightFalloff;
            fog.BaseHeight = environment->FogHeight;
            fog.StartDistance = environment->FogStartDistance;
            fog.MaxOpacity = environment->FogMaxOpacity;
            fog.Color = environment->FogColor;
            fog.FromEnvironment = environment->FogFromEnvironment;
            // The glow towards the sun is of the light that gets through the air (none once it has set).
            const SkyLights lights = ResolveSkyLights(m_Registry, environment);
            fog.SunDirection = lights.Sun.Direction;
            fog.SunColor = lights.Sun.Illuminance * lights.SunTransmittance * environment->FogSunScattering;
            fog.SunExponent = environment->FogSunExponent;
        }
        m_renderer->SetHeightFog(fog);
    }

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
        // Non-component data the systems write; named in their declared access.
        struct MovedEntities {};
        struct LightSubmission {};
        struct Renderer2DSubmission {};
        struct ParticleSubmission {};

        // Identifies the scene that owns the renderer's GPU scene (a scene address can be reused by the next scene).
        uint64_t s_NextSceneID = 1;
    }

    template <typename... Component>
    static void CreateComponentStorages(ComponentGroup<Component...>, entt::registry& registry)
    {
        ((void)registry.storage<Component>(), ...);
    }

    Scene::~Scene() = default;

    Scene::Scene()
    {
        // A non-const registry creates a component's storage the first time any view/get/try_get names the type, and
        // that creation is not thread-safe. Systems access the registry from several workers at once, so every storage
        // exists before the first graph runs (e.g. a scene copied for Play has no sprite storage until something asks).
        CreateComponentStorages(AllComponents{}, m_Registry);
        CreateComponentStorages(ComponentGroup<IDComponent, TagComponent>{}, m_Registry);

        // The GPU scene follows mesh entities through their components' signals: added, replaced or patched, removed.
        m_SceneID = s_NextSceneID++;
        m_Registry.on_construct<MeshComponent>().connect<&Scene::OnMeshEntityChanged>(*this);
        m_Registry.on_update<MeshComponent>().connect<&Scene::OnMeshEntityChanged>(*this);
        m_Registry.on_destroy<MeshComponent>().connect<&Scene::OnMeshEntityChanged>(*this);
        m_Registry.on_construct<MaterialComponent>().connect<&Scene::OnMeshEntityChanged>(*this);
        m_Registry.on_update<MaterialComponent>().connect<&Scene::OnMeshEntityChanged>(*this);
        m_Registry.on_destroy<MaterialComponent>().connect<&Scene::OnMeshEntityChanged>(*this);
        m_Registry.on_construct<AnimatorComponent>().connect<&Scene::OnMeshEntityChanged>(*this);
        m_Registry.on_destroy<AnimatorComponent>().connect<&Scene::OnMeshEntityChanged>(*this);
        m_Registry.on_construct<RigidBody3DComponent>().connect<&Scene::OnPhysics3DComponentChanged>(*this);
        m_Registry.on_destroy<RigidBody3DComponent>().connect<&Scene::OnPhysics3DComponentChanged>(*this);
        m_Registry.on_construct<BoxCollider3DComponent>().connect<&Scene::OnPhysics3DComponentChanged>(*this);
        m_Registry.on_destroy<BoxCollider3DComponent>().connect<&Scene::OnPhysics3DComponentChanged>(*this);
        m_Registry.on_construct<SphereCollider3DComponent>().connect<&Scene::OnPhysics3DComponentChanged>(*this);
        m_Registry.on_destroy<SphereCollider3DComponent>().connect<&Scene::OnPhysics3DComponentChanged>(*this);
        m_Registry.on_construct<CapsuleCollider3DComponent>().connect<&Scene::OnPhysics3DComponentChanged>(*this);
        m_Registry.on_destroy<CapsuleCollider3DComponent>().connect<&Scene::OnPhysics3DComponentChanged>(*this);
        m_Registry.on_construct<CharacterController3DComponent>().connect<&Scene::OnPhysics3DComponentChanged>(*this);
        m_Registry.on_destroy<CharacterController3DComponent>().connect<&Scene::OnPhysics3DComponentChanged>(*this);

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
        newScene->m_Folders = other->m_Folders;
        // A world: its settings and index, and the cells the editor has fully loaded (their entities are in the copy; Play / Simulate
        // stream them from memory, BeginWorldStreaming).
        newScene->m_World = other->m_World;
        newScene->m_World.Overrides.clear();
        newScene->m_WorldSnapshots.clear();
        const std::vector<WorldCellCoord> loadedCells = other->GetLoadedWorldCells(false);
        newScene->m_World.EditorLoaded = std::set<WorldCellCoord>(loadedCells.begin(), loadedCells.end());

        return newScene;
    }
    
    void Scene::AddFolder(const std::string& path)
    {
        if (!path.empty() && std::find(m_Folders.begin(), m_Folders.end(), path) == m_Folders.end())
            m_Folders.push_back(path);
    }

    void Scene::RemoveFolder(const std::string& path)
    {
        auto inside = [&](const std::string& other) { return other == path || other.rfind(path + "/", 0) == 0; };
        std::erase_if(m_Folders, inside);
        for (auto entity : m_Registry.view<FolderComponent>())
        {
            if (inside(m_Registry.get<FolderComponent>(entity).Path))
                m_Registry.remove<FolderComponent>(entity);
        }
    }

    void Scene::RenameFolder(const std::string& from, const std::string& to)
    {
        if (from.empty() || to.empty() || from == to)
            return;
        // "a/b" -> "c" also moves "a/b/x" to "c/x".
        auto renamed = [&](const std::string& other) -> std::string
        {
            if (other == from)
                return to;
            if (other.rfind(from + "/", 0) == 0)
                return to + other.substr(from.size());
            return other;
        };
        for (std::string& folder : m_Folders)
            folder = renamed(folder);
        for (auto entity : m_Registry.view<FolderComponent>())
        {
            std::string& path = m_Registry.get<FolderComponent>(entity).Path;
            path = renamed(path);
        }
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

        // A spawned entity deleted on a prefab instance: the instance remembers its name (the deletion is listed under it).
        if (entity.HasComponent<PrefabNodeComponent>())
        {
            const PrefabNodeComponent& node = entity.GetComponent<PrefabNodeComponent>();
            Entity instanceRoot = GetEntityByUUID(node.Instance);
            if (instanceRoot && instanceRoot.HasComponent<PrefabInstanceComponent>())
                instanceRoot.GetComponent<PrefabInstanceComponent>().RemovedNames[node.LocalId] = entity.GetName();
        }

        // A node deleted from a model instance stays deleted when the instance spawns again (it is saved as removed).
        if (entity.HasComponent<ModelNodeComponent>())
        {
            const ModelNodeComponent& node = entity.GetComponent<ModelNodeComponent>();
            Entity root = GetEntityByUUID(node.Instance);
            if (root && root.HasComponent<ModelInstanceComponent>())
                root.GetComponent<ModelInstanceComponent>().RemovedNodes.push_back(node.NodeIndex);
        }
        
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
        
        // Running: its physics body, character and script instances go with it.
        if (m_Physics3DScene)
        {
            if (entity.HasComponent<RigidBody3DComponent>())
                m_Physics3DScene->DestroyBody(entity);
            if (entity.HasComponent<CharacterController3DComponent>())
                m_Physics3DScene->DestroyCharacter(entity);
            m_Physics3DScene->RemoveMeshCollider(entity);
        }
        // Its 2D body (it stayed in the Box2D world, still colliding, after its entity was gone).
        if (entity.HasComponent<RigidBody2DComponent>())
        {
            b2BodyId& body = entity.GetComponent<RigidBody2DComponent>().RuntimeBody;
            if (b2World_IsValid(m_PhysicsWorldID) && b2Body_IsValid(body))
                b2DestroyBody(body);
            body = b2_nullBodyId;
        }
        if (m_IsRunning && entity.HasComponent<ScriptComponent>())
            ScriptEngine::OnDestroyEntity(entity);

        m_EntityMap.erase(entity.GetUUID());
        m_Registry.destroy(entity);
        m_AssetReferencesChanged = true;
    }

    void Scene::CollectAssetReferences(std::unordered_set<AssetHandle>& outHandles)
    {
        // Straight over the components (no entity lookups), and a mesh just seen is not hashed again: a world's props share a few
        // meshes, and tens of thousands of set lookups were most of the unused-asset sweep's time.
        {
            AssetHandle recent[4] = {};
            size_t next = 0;
            m_Registry.view<MeshComponent>().each([&](const MeshComponent& component)
            {
                const AssetHandle mesh = component.Mesh;
                if (mesh == recent[0] || mesh == recent[1] || mesh == recent[2] || mesh == recent[3])
                    return;
                recent[next++ & 3] = mesh;
                outHandles.insert(mesh);
            });
        }

        // Before they spawn, instances reference their model only.
        for (auto entity : m_Registry.view<ModelInstanceComponent>())
            outHandles.insert(m_Registry.get<ModelInstanceComponent>(entity).Model);

        // The prefab asset of an instance; its spawned entities are ordinary entities and are collected like any other.
        for (auto entity : m_Registry.view<PrefabInstanceComponent>())
            outHandles.insert(m_Registry.get<PrefabInstanceComponent>(entity).Prefab);

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
            outHandles.insert(animator.Graph);
        }

        for (auto entity : m_Registry.view<TimelinePlayerComponent>())
            outHandles.insert(m_Registry.get<TimelinePlayerComponent>(entity).Timeline);

        for (auto entity : m_Registry.view<ParticleEmitterComponent>())
            outHandles.insert(m_Registry.get<ParticleEmitterComponent>(entity).Texture);

        for (auto entity : m_Registry.view<SpriteRendererComponent>())
            outHandles.insert(m_Registry.get<SpriteRendererComponent>(entity).Texture);

        // World cells loaded and not (yet) activated: what they were loaded for stays (§5.13.3).
        for (const StreamedLevel& streamed : m_Levels)
        {
            if (streamed.Staged && !streamed.Loader) // (one still loading is its worker's: built, its assets are requested)
                streamed.Staged->CollectAssetReferences(outHandles);
        }

        outHandles.erase(AssetHandle(0));
    }

    Entity Scene::Instantiate(AssetHandle prefab, const glm::vec3& position)
    {
        std::vector<Entity> spawned;
        Entity root = PrefabInstance::Instantiate(*this, prefab, position, spawned);
        if (!root)
            return {};

        // The entities of a prefab placed while the scene runs start like the ones it began with.
        if (m_Physics3DScene)
        {
            for (Entity entity : spawned)
            {
                if (entity.HasComponent<RigidBody3DComponent>())
                    m_Physics3DScene->CreateBody(entity);
                if (entity.HasComponent<CharacterController3DComponent>())
                    m_Physics3DScene->CreateCharacter(entity);
                // Made here (a script may push it right away), not again after the update systems.
                m_PendingPhysics3D.erase(entity.GetUUID());
            }
        }
        // Their scripts start after the script loop, not here: Instantiate is called from a script (managed code), and native code
        // called from managed code must not call back into managed code (the runtime aborts).
        if (m_IsRunning)
        {
            for (Entity entity : spawned)
            {
                if (entity.HasComponent<ScriptComponent>())
                    m_PendingScriptStart.push_back(entity.GetUUID());
            }
        }
        return root;
    }

    bool Scene::LoadLevel(AssetHandle level)
    {
        if (level == 0 || AssetManager::GetAssetType(level) != AssetType::Scene)
        {
            NOX_CORE_ERROR("LoadLevel: {} is not a scene asset", static_cast<uint64_t>(level));
            return false;
        }
        for (StreamedLevel& streamed : m_Levels)
        {
            if (streamed.Level != level || streamed.IsCell)
                continue;
            if (!streamed.Failed)
            {
                streamed.UnloadQueued = false; // loaded again before its unload ran: it stays
                return true;
            }
            // Failed before: asked for again, it is tried again.
            streamed.Failed = false;
            streamed.UnloadQueued = false;
            streamed.OutsideTime = 0.0f;
            streamed.Loader = std::make_unique<SceneLoader>(Project::GetActiveAssetDirectory() / AssetManager::GetMetadata(level).FilePath, *this, level);
            return true;
        }
        StreamedLevel& streamed = m_Levels.emplace_back();
        streamed.Level = level;
        streamed.Name = AssetManager::GetMetadata(level).FilePath.filename().string();
        streamed.Loader = std::make_unique<SceneLoader>(Project::GetActiveAssetDirectory() / AssetManager::GetMetadata(level).FilePath, *this, level);
        return true;
    }

    void Scene::UnloadLevel(AssetHandle level)
    {
        for (StreamedLevel& streamed : m_Levels)
        {
            if (streamed.Level == level && !streamed.IsCell)
                streamed.UnloadQueued = true;
        }
    }

    Scene::LevelState Scene::GetLevelState(AssetHandle level) const
    {
        for (const StreamedLevel& streamed : m_Levels)
        {
            if (streamed.Level == level && !streamed.IsCell)
                return streamed.Failed ? LevelState::Unloaded : streamed.Loaded ? LevelState::Loaded : LevelState::Loading;
        }
        return LevelState::Unloaded;
    }

    void Scene::DestroyLevelEntities(AssetHandle level)
    {
        std::vector<UUID> entities;
        for (auto handle : m_Registry.view<LevelComponent, IDComponent>())
        {
            if (m_Registry.get<LevelComponent>(handle).Level == level)
                entities.push_back(m_Registry.get<IDComponent>(handle).ID);
        }
        // DestroyEntity takes the children with their parent: the ones already gone are skipped.
        for (UUID id : entities)
        {
            if (Entity entity = GetEntityByUUID(id))
                DestroyEntity(entity);
        }
    }

    void Scene::FinishLevelLoad(StreamedLevel& streamed, const std::vector<UUID>& created, const std::vector<UUID>& heldPhysics)
    {
        // What Play / Simulate start does for the entities the scene began with (OnRuntimeStart): 3D bodies after the update systems
        // (SyncPendingPhysics3D), 2D bodies, particles, timelines, and the scripts after the script loop.
        for (UUID id : heldPhysics)
            m_PendingPhysics3D.insert(id);
        for (UUID id : created)
        {
            Entity entity = GetEntityByUUID(id);
            if (!entity)
                continue;
            if (entity.HasComponent<RigidBody2DComponent>() && b2World_IsValid(m_PhysicsWorldID))
                CreatePhysics2DBody(entity);
            if (entity.HasComponent<ParticleEmitterComponent>())
                ParticleSystem::Restart(entity.GetComponent<ParticleEmitterComponent>());
            if (entity.HasComponent<TimelinePlayerComponent>() && m_TimelinesRunning)
            {
                auto& player = entity.GetComponent<TimelinePlayerComponent>();
                player.Time = 0.0f;
                player.Playing = player.PlayOnStart;
                player.FiredEvents.clear();
            }
            if (m_IsRunning && entity.HasComponent<ScriptComponent>())
                m_PendingScriptStart.push_back(id);
            // A cell's roots: where they go when unloaded by hand, saved or their bounds are unknown.
            if (streamed.IsCell && (!entity.HasComponent<RelationshipComponent>() || entity.GetComponent<RelationshipComponent>().Parent == 0))
                m_World.Origins[id] = streamed.Cell;
        }
        if (streamed.IsCell)
            NOX_CORE_TRACE("World cell `{}` loaded ({} entities)", streamed.Name, created.size());
        else
            NOX_CORE_INFO("Level `{}` loaded ({} entities)", streamed.Name, created.size());
    }

    void Scene::CopyEntitiesFrom(Scene& source, const std::vector<UUID>& ids)
    {
        for (UUID id : ids)
        {
            Entity from = source.GetEntityByUUID(id);
            if (!from)
                continue;
            Entity made = CreateEntityWithUUID(id, from.GetName());
            CopyComponentIfExists(AllComponents{}, made, from);
        }
    }

    bool Scene::ActivateWorldCellEntities(StreamedLevel& streamed, double budgetMs)
    {
        // The built cell's entities copied into this scene in file order, every component (what a load into the scene would have
        // made), a slice a frame; one already here (the editor's own copy of an entity saved into this cell) is kept as it is.
        Scene& staged = *streamed.Staged;
        if (streamed.ActivateNext == 0)
        {
            for (const std::string& folder : staged.GetFolders())
                AddFolder(folder);
        }
        const auto start = std::chrono::steady_clock::now();
        while (streamed.ActivateNext < streamed.StagedEntities.size())
        {
            const UUID id = streamed.StagedEntities[streamed.ActivateNext++];
            Entity source = staged.GetEntityByUUID(id);
            if (!source)
                continue;
            if (Entity existing = GetEntityByUUID(id))
            {
                NOX_CORE_WARN("World cell `{}`: entity '{}' ({}) is in the scene already, kept as it is", streamed.Name, existing.GetName(),
                              static_cast<uint64_t>(id));
                continue;
            }
            Entity created = CreateEntityWithUUID(id, source.GetName());
            CopyComponentIfExists(AllComponents{}, created, source);
            created.AddOrReplaceComponent<LevelComponent>().Level = streamed.Level;
            streamed.ActivatedEntities.push_back(id);
            // The clock is read every few entities (an entity takes microseconds).
            if ((streamed.ActivateNext & 15) == 0 &&
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= budgetMs)
                break;
        }
        return streamed.ActivateNext >= streamed.StagedEntities.size();
    }

    // The time a frame may spend creating streamed levels' entities (the rest of a level comes in the next frames).
    static constexpr double LevelStreamingBudgetMs = 3.0;

    void Scene::UpdateLevelStreaming(const glm::vec3* source, float deltaTime)
    {
        // 0. The world partition's cells around the sources (Play / Simulate of a world).
        UpdateWorldStreaming(source);

        // 1. The volumes: a level any volume holding the source names is wanted; one only volumes name, and none holds the source, goes
        //    after the shortest of their unload delays.
        if (source)
        {
            std::unordered_map<AssetHandle, float> unloadDelays; // the levels volumes name, and the shortest delay among them
            std::unordered_set<AssetHandle> wanted;
            for (auto handle : m_Registry.view<WorldTransformComponent, LevelStreamingVolumeComponent>())
            {
                const auto& volume = m_Registry.get<LevelStreamingVolumeComponent>(handle);
                const glm::vec3 local = glm::vec3(glm::inverse(m_Registry.get<WorldTransformComponent>(handle).WorldMatrix) * glm::vec4(*source, 1.0f));
                const bool inside = glm::all(glm::lessThanEqual(glm::abs(local), volume.HalfExtents));
                for (AssetHandle level : volume.Levels)
                {
                    if (level == 0)
                        continue;
                    auto [entry, added] = unloadDelays.try_emplace(level, volume.UnloadDelay);
                    if (!added)
                        entry->second = std::min(entry->second, volume.UnloadDelay);
                    if (inside)
                        wanted.insert(level);
                }
            }
            for (const auto& [level, unloadDelay] : unloadDelays)
            {
                if (wanted.contains(level))
                {
                    bool failed = false;
                    for (StreamedLevel& streamed : m_Levels)
                    {
                        if (streamed.Level == level && !streamed.IsCell)
                        {
                            streamed.OutsideTime = 0.0f;
                            failed = streamed.Failed;
                        }
                    }
                    if (!failed) // (not again every frame: once the source has left, the next visit tries again)
                        LoadLevel(level);
                    continue;
                }
                for (StreamedLevel& streamed : m_Levels)
                {
                    if (streamed.Level != level || streamed.IsCell || streamed.UnloadQueued)
                        continue;
                    streamed.OutsideTime += deltaTime;
                    if (streamed.OutsideTime >= unloadDelay)
                        streamed.UnloadQueued = true;
                }
            }
        }

        // 2. The unloads: a level still loading stops (its loader waits for its reading thread) and loses what it made so far; a cell
        //    also its built scene and what it had put into the world. Then the cells whose target became Loaded leave the world (§5.13.3):
        //    one being activated keeps its built scene; one that was in the world is built again from its file (what ran in it is gone,
        //    as when it is unloaded).
        for (StreamedLevel& streamed : m_Levels)
        {
            if (!streamed.UnloadQueued)
                continue;
            const bool hadEntities = streamed.Loaded || streamed.Loader;
            streamed.Loader.reset();
            if (streamed.IsCell)
            {
                RemoveWorldCellFromWorld(streamed);
                // Its built scene held the cell's assets too (a Loaded cell has nothing in the world to destroy): the sweep frees them.
                if (streamed.Staged)
                    m_AssetReferencesChanged = true;
                streamed.Staged = nullptr;
                NOX_CORE_TRACE("World cell `{}` unloaded", streamed.Name);
                continue;
            }
            DestroyLevelEntities(streamed.Level);
            if (hadEntities)
                NOX_CORE_INFO("Level `{}` unloaded", streamed.Name);
        }
        std::erase_if(m_Levels, [](const StreamedLevel& streamed) { return streamed.UnloadQueued; });
        for (StreamedLevel& streamed : m_Levels)
        {
            if (!streamed.IsCell || streamed.Activate || streamed.Failed || streamed.Empty || (!streamed.Loaded && streamed.ActivateNext == 0))
                continue;
            const bool wasActivated = streamed.Loaded;
            RemoveWorldCellFromWorld(streamed);
            if (wasActivated)
                StageWorldCell(streamed);
            NOX_CORE_TRACE("World cell `{}` deactivated", streamed.Name);
        }

        // 3. The loads, oldest first, within the frame's budget. The 3D physics bodies their components ask for (OnPhysics3DComponentChanged)
        //    are held until the level is complete: a crate made before the floor below it fell through. A world cell is built, then
        //    -- its target activated -- waits for its assets (Play / Simulate) and goes into the world (StepWorldCell).
        const auto start = std::chrono::steady_clock::now();
        for (size_t index = 0; index < m_Levels.size(); ++index)
        {
            StreamedLevel& streamed = m_Levels[index];
            if (streamed.Loaded || (!streamed.Loader && !(streamed.IsCell && streamed.Staged && streamed.Activate)))
                continue;
            const double spent = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (spent >= LevelStreamingBudgetMs)
                break;
            if (streamed.IsCell)
            {
                std::unordered_set<UUID> pendingBefore = std::move(m_PendingPhysics3D);
                m_PendingPhysics3D.clear();
                const CellStep step = StepWorldCell(streamed, LevelStreamingBudgetMs - spent, deltaTime, m_WorldStreaming);
                streamed.HeldPhysics.insert(streamed.HeldPhysics.end(), m_PendingPhysics3D.begin(), m_PendingPhysics3D.end());
                m_PendingPhysics3D = std::move(pendingBefore);
                if (step == CellStep::Failed)
                {
                    RemoveWorldCellFromWorld(streamed);
                    streamed.Failed = true;
                }
                else if (step == CellStep::Activated)
                {
                    FinishLevelLoad(streamed, streamed.ActivatedEntities, streamed.HeldPhysics);
                    streamed.ActivatedEntities.clear();
                    streamed.HeldPhysics.clear();
                    streamed.Loaded = true;
                }
                continue;
            }
            std::unordered_set<UUID> pendingBefore = std::move(m_PendingPhysics3D);
            m_PendingPhysics3D.clear();
            const bool done = streamed.Loader->Step(LevelStreamingBudgetMs - spent);
            streamed.HeldPhysics.insert(streamed.HeldPhysics.end(), m_PendingPhysics3D.begin(), m_PendingPhysics3D.end());
            m_PendingPhysics3D = std::move(pendingBefore);
            if (!done)
                continue;
            if (streamed.Loader->Failed())
            {
                // (Refused or unreadable: the loader logged why. Whatever it made goes again.)
                DestroyLevelEntities(streamed.Level);
                streamed.Loader.reset();
                streamed.HeldPhysics.clear();
                streamed.Failed = true;
                continue;
            }
            FinishLevelLoad(streamed, streamed.Loader->GetCreatedEntities(), streamed.HeldPhysics);
            streamed.Loaded = true;
            streamed.Loader.reset();
            streamed.HeldPhysics.clear();
        }
    }

    void Scene::QueueDestroy(Entity entity)
    {
        if (entity)
            m_PendingDestroy.push_back(entity.GetUUID());
    }

    void Scene::StartPendingScripts()
    {
        std::vector<UUID> pending = std::move(m_PendingScriptStart);
        m_PendingScriptStart.clear();
        for (UUID id : pending)
        {
            Entity entity = GetEntityByUUID(id);
            if (entity && entity.HasComponent<ScriptComponent>())
                ScriptEngine::OnCreateEntity(entity);
        }
    }

    void Scene::FlushPendingDestroy()
    {
        std::vector<UUID> pending = std::move(m_PendingDestroy);
        m_PendingDestroy.clear();
        for (UUID id : pending)
        {
            if (Entity entity = GetEntityByUUID(id))
                DestroyEntity(entity);
        }
    }

    void Scene::OnRuntimeStart()
    {
        // A world: the cells around the primary camera are in before anything starts, so they start with the scene's own entities.
        if (m_World.Settings.Enabled)
        {
            Entity camera = GetPrimaryCameraEntity();
            const glm::vec3 source = camera ? ComposedWorldPosition(camera) : glm::vec3(0.0f);
            BeginWorldStreaming(camera ? &source : nullptr);
        }
        m_IsRunning = true;
        OnPhysics2DStart();
        OnPhysics3DStart();
        StartTimelines();
        StartParticles();
        
        // Scripting
        {
            ScriptEngine::OnRuntimeStart(this);
            // Instantiate all script entities

            auto view = m_Registry.view<ScriptComponent>();
            for (auto e : view)
            {
                Entity entity = { e, this };
                ScriptEngine::OnCreateEntity(entity);
            }
        }
    }

    void Scene::OnRuntimeStop()
    {
        m_IsRunning = false;
        m_WorldStreaming = false;
        m_TimelinesRunning = false;
        
        ScriptEngine::OnRuntimeStop();
        OnPhysics2DStop();
        OnPhysics3DStop();
        // What the scripts built lives as long as the Play session.
        DynamicMeshes::DestroyAll();
    }

    void Scene::OnSimulationStart(const glm::vec3* streamingSource)
    {
        BeginWorldStreaming(streamingSource);
        OnPhysics2DStart();
        OnPhysics3DStart();
        StartTimelines();
        StartParticles();
    }

    void Scene::OnSimulationStop()
    {
        m_TimelinesRunning = false;
        m_WorldStreaming = false;
        OnPhysics2DStop();
        OnPhysics3DStop();
    }

    void Scene::OnUpdateRuntime(Timestep ts)
    {
        const bool step = !m_IsPaused || m_StepFrames-- > 0;
        if (step)
        {
            // Streamed levels first, outside every script call (unloading one calls OnDestroy): where the primary camera stood last
            // frame is the streaming source.
            if (Entity camera = GetPrimaryCameraEntity(); camera && camera.HasComponent<WorldTransformComponent>())
            {
                const glm::vec3 source = glm::vec3(camera.GetComponent<WorldTransformComponent>().WorldMatrix[3]);
                UpdateLevelStreaming(&source, static_cast<float>(ts));
            }
            else
                UpdateLevelStreaming(nullptr, static_cast<float>(ts));

            // Collected first: a script may instantiate prefabs (new entities, new script components) or destroy entities.
            std::vector<entt::entity> scriptEntities;
            {
                auto scripts = m_Registry.view<ScriptComponent>();
                scriptEntities.assign(scripts.begin(), scripts.end());
            }
            // OnFixedUpdate at 60 Hz (as many steps as the frame's time holds, at most 5: a long hitch does not snowball), then
            // OnUpdate once.
            constexpr float fixedStep = 1.0f / 60.0f;
            m_FixedTimeAccumulator = std::min(m_FixedTimeAccumulator + static_cast<float>(ts), 5.0f * fixedStep);
            while (m_FixedTimeAccumulator >= fixedStep)
            {
                m_FixedTimeAccumulator -= fixedStep;
                for (entt::entity handle : scriptEntities)
                {
                    if (m_Registry.valid(handle))
                        ScriptEngine::OnFixedUpdateEntity(Entity(handle, this), fixedStep);
                }
            }
            for (entt::entity handle : scriptEntities)
            {
                if (m_Registry.valid(handle))
                    ScriptEngine::OnUpdateEntity(Entity(handle, this), static_cast<float>(ts));
            }
            // Both run here, outside every script call: they call into managed code (OnCreate / OnDestroy).
            StartPendingScripts();
            FlushPendingDestroy();
        }
        RunUpdateSystems(ts, step, step);
        if (step)
        {
            // Animation notifies the animators crossed this frame, and the timelines' event keys, to the entity's scripts (main
            // thread: managed code).
            std::vector<std::pair<entt::entity, std::string>> notifies;
            for (auto entity : m_Registry.view<AnimatorComponent, ScriptComponent>())
            {
                for (const std::string& name : m_Registry.get<AnimatorComponent>(entity).FiredNotifies)
                    notifies.emplace_back(entity, name);
            }
            for (auto entity : m_Registry.view<TimelinePlayerComponent, ScriptComponent>())
            {
                for (const std::string& name : m_Registry.get<TimelinePlayerComponent>(entity).FiredEvents)
                    notifies.emplace_back(entity, name);
            }
            for (const auto& [entity, name] : notifies)
            {
                if (m_Registry.valid(entity))
                    ScriptEngine::OnAnimationEvent(Entity(entity, this), name);
            }

            // OnLateUpdate: after physics, animation and the transforms of this frame. What it moves is propagated again, so
            // a following camera shows where its target is this frame, not the one before.
            bool lateUpdated = false;
            std::vector<entt::entity> lateEntities;
            {
                auto scripts = m_Registry.view<ScriptComponent>();
                lateEntities.assign(scripts.begin(), scripts.end());
            }
            for (entt::entity handle : lateEntities)
            {
                if (m_Registry.valid(handle))
                    lateUpdated |= ScriptEngine::OnLateUpdateEntity(Entity(handle, this), static_cast<float>(ts));
            }
            if (lateUpdated)
            {
                SceneGraph::UpdateWorldTransforms(m_Registry, m_EntityMap, m_CommandBuffers, m_MovedEntities);
                ApplySyncPoint();
            }
            StartPendingScripts();
            FlushPendingDestroy();
        }
        SyncGpuScene();

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
        if (step)
        {
            // Simulate streams levels around the editor camera (Unreal's Simulate does the same with its viewport).
            const glm::vec3 source = camera.GetPosition();
            UpdateLevelStreaming(&source, static_cast<float>(ts));
        }
        RunUpdateSystems(ts, step, step);
        SyncGpuScene();

        m_renderer->BeginScene(camera);
        RunSubmitSystems();
        m_renderer->EndScene();
    }

    void Scene::OnUpdateEditor(Timestep ts, EditorCamera& camera)
    {
        // A world's cells the editor loads or unloads (by hand: no streaming source while editing), and its HLODs around the camera.
        if (m_World.Settings.Enabled)
        {
            UpdateEditorHLODs(camera.GetPosition());
            if (!m_Levels.empty())
                UpdateLevelStreaming(nullptr, static_cast<float>(ts));
        }
        RunUpdateSystems(ts, false, true);
        SyncGpuScene();

        m_renderer->BeginScene(camera);
        RunSubmitSystems();
        m_renderer->EndScene();
    }

    void Scene::RegisterSystems()
    {
        // Game Update. Declared access orders them: Physics 2D/3D and Animation write transforms, propagation reads them.
        m_UpdateSystems.AddSystem("Physics 2D",
            ComponentAccess().Read<RigidBody2DComponent>().Write<TransformComponent, DirtyTransformComponent>(),
            [this]() { UpdatePhysics2D(); });
        m_UpdateSystems.AddSystem("Physics 3D",
            ComponentAccess().Read<RigidBody3DComponent>().Write<CharacterController3DComponent, TransformComponent, DirtyTransformComponent>(),
            [this]() { UpdatePhysics3D(); });
        m_UpdateSystems.AddSystem("Animation",
            ComponentAccess().Read<WorldTransformComponent, RelationshipComponent>().Write<AnimatorComponent, TransformComponent, DirtyTransformComponent, CharacterController3DComponent, BoneSocketComponent>(),
            [this]() { UpdateAnimators(); UpdateBoneSockets(); });
        m_UpdateSystems.AddSystem("Timelines",
            ComponentAccess().Read<RelationshipComponent, TagComponent>()
                .Write<TimelinePlayerComponent, TransformComponent, DirtyTransformComponent, DirectionalLightComponent, PointLightComponent, SpotLightComponent, CameraComponent>(),
            [this]() { UpdateTimelines(); });
        m_UpdateSystems.AddSystem("Transform Propagation",
            ComponentAccess().Read<TransformComponent, RelationshipComponent, MeshComponent>().Write<WorldTransformComponent, DirtyTransformComponent, MovedEntities>(),
            [this]() { SceneGraph::UpdateWorldTransforms(m_Registry, m_EntityMap, m_CommandBuffers, m_MovedEntities); });
        // Particles spawn where their emitter is now: after propagation.
        m_UpdateSystems.AddSystem("Particles",
            ComponentAccess().Read<WorldTransformComponent>().Write<ParticleEmitterComponent>(),
            [this]() { UpdateParticles(); });

        // Submission (after BeginScene). No conflicts: they run in parallel. Meshes are not submitted: the GPU scene keeps
        // them (SyncGpuScene).
        m_SubmitSystems.AddSystem("Submit Lights",
            ComponentAccess().Read<WorldTransformComponent, CameraComponent, DirectionalLightComponent, PointLightComponent, SpotLightComponent,
                                   RectLightComponent, DiscLightComponent>().Write<LightSubmission>(),
            [this]() { SubmitLights(); });
        m_SubmitSystems.AddSystem("Submit 2D",
            ComponentAccess().Read<WorldTransformComponent, SpriteRendererComponent, CircleRendererComponent, TextComponent>().Write<Renderer2DSubmission>(),
            [this]() { Submit2D(); });
        m_SubmitSystems.AddSystem("Submit Particles",
            ComponentAccess().Read<WorldTransformComponent, ParticleEmitterComponent>().Write<ParticleSubmission>(),
            [this]() { SubmitParticles(); });
    }

    void Scene::RunUpdateSystems(Timestep ts, bool stepPhysics, bool stepAnimation)
    {
        NOX_PROFILE_SCOPE("Scene Update Systems");
        m_FrameInput = { static_cast<float>(ts), stepPhysics, stepAnimation };
        m_UpdateSystems.Run();
        ApplySyncPoint();
        SyncPendingPhysics3D();
    }

    void Scene::RunSubmitSystems()
    {
        NOX_PROFILE_SCOPE("Scene Submit Systems");
        m_SubmitSystems.Run();
        ApplySyncPoint();
    }

    void Scene::OnMeshEntityChanged(entt::registry&, entt::entity entity)
    {
        // Components change on the main thread only (directly or at a sync point).
        m_PendingMeshEntities.push_back(entity);
    }

    void Scene::SyncGpuScene()
    {
        NOX_PROFILE_SCOPE("Sync GPU Scene");
        std::vector<AssetHandle>& missingAssets = m_MissingAssets.Local();

        // 0. Model instances whose model finished loading get their node entities (registered below as new meshes).
        {
            NOX_PROFILE_SCOPE("Spawn Model Instances");
            ModelInstance::SpawnPending(*this);
            PrefabInstance::SpawnPending(*this);
        }

        // 1. Another scene rendered last (e.g. edit <-> play): register every mesh entity again.
        if (m_renderer->BindGpuScene(m_SceneID))
        {
            m_MeshRegistrations.clear();
            m_SkinnedMeshEntities.clear();
            m_PendingMeshEntities.clear();
            for (auto entity : m_Registry.view<MeshComponent>())
                m_PendingMeshEntities.push_back(entity);
        }

        // 2. Entities whose mesh was unloaded under their instances.
        {
            std::vector<int32_t> invalidated;
            m_renderer->ConsumeInvalidatedMeshEntities(invalidated);
            for (int32_t entityID : invalidated)
                m_PendingMeshEntities.push_back(static_cast<entt::entity>(entityID));
        }
        // ... and entities drawing a mesh made at run time that changed (new geometry, material, destroyed).
        {
            std::vector<AssetHandle> changed;
            DynamicMeshes::ConsumeChanged(changed);
            if (!changed.empty())
            {
                const std::unordered_set<AssetHandle> meshes(changed.begin(), changed.end());
                for (auto entity : m_Registry.view<MeshComponent>())
                {
                    if (meshes.contains(m_Registry.get<MeshComponent>(entity).Mesh))
                        m_PendingMeshEntities.push_back(entity);
                }
            }
        }

        // 2b. HLODs shown or hidden by their cells (§5.13.2).
        UpdateHLODVisibility();

        // 3. Added, changed and removed mesh entities. An entity whose assets are not loaded yet stays pending (the
        // sync point requests them).
        if (!m_PendingMeshEntities.empty())
        {
            NOX_PROFILE_SCOPE("Register Mesh Entities");
            std::vector<entt::entity> pending;
            pending.swap(m_PendingMeshEntities);
            std::sort(pending.begin(), pending.end());
            pending.erase(std::unique(pending.begin(), pending.end()), pending.end());

            // New instances are spread over frames: a model's thousands of entities arrive in one frame (Bistro: 2909,
            // 14.5 ms). Removals are never delayed.
            constexpr size_t RegistrationsPerFrame = 512;
            size_t registrationBudget = RegistrationsPerFrame;
            for (entt::entity entity : pending)
            {
                if (auto found = m_MeshRegistrations.find(entity); found != m_MeshRegistrations.end())
                {
                    m_renderer->RemoveMeshInstances(found->second.Instances);
                    if (found->second.Skinned)
                        std::erase(m_SkinnedMeshEntities, entity);
                    m_MeshRegistrations.erase(found);
                }

                if (!m_Registry.valid(entity))
                    continue;
                const MeshComponent* mesh = m_Registry.try_get<MeshComponent>(entity);
                if (!mesh)
                    continue;
                if (const HLODComponent* hlod = m_Registry.try_get<HLODComponent>(entity); hlod && hlod->Hidden)
                    continue; // its cell is in
                const WorldTransformComponent* world = m_Registry.try_get<WorldTransformComponent>(entity);
                if (!world)
                {
                    // Added at the sync point by transform propagation.
                    m_PendingMeshEntities.push_back(entity);
                    continue;
                }
                if (registrationBudget == 0)
                {
                    m_PendingMeshEntities.push_back(entity);
                    continue;
                }
                --registrationBudget;

                MeshRegistration registration;
                if (!m_renderer->AddMeshInstances(world->WorldMatrix, *mesh, m_Registry.try_get<MaterialComponent>(entity),
                                                  static_cast<int32_t>(entity), registration.Instances, missingAssets))
                    m_PendingMeshEntities.push_back(entity);
                if (registration.Instances.empty())
                    continue;

                // Skinning applies to skeletal mesh assets only.
                registration.Skinned = m_Registry.all_of<AnimatorComponent>(entity) && AssetManager::GetAssetType(mesh->Mesh) == AssetType::Mesh;
                if (registration.Skinned)
                    m_SkinnedMeshEntities.push_back(entity);
                m_MeshRegistrations.emplace(entity, std::move(registration));
            }
        }

        // 4. World transforms that changed this frame.
        m_MovedEntities.ForEach([this](std::vector<entt::entity>& entities)
        {
            for (entt::entity entity : entities)
            {
                auto found = m_MeshRegistrations.find(entity);
                if (found != m_MeshRegistrations.end())
                    m_renderer->SetMeshInstancesTransform(found->second.Instances, m_Registry.get<WorldTransformComponent>(entity).WorldMatrix);
            }
            entities.clear();
        });

        // 5. This frame's joint matrices: built in parallel (each skinned mesh writes its own animator's), handed to the renderer in
        //    order.
        const uint32_t skinnedCount = static_cast<uint32_t>(m_SkinnedMeshEntities.size());
        m_SkinnedBones.resize(skinnedCount);
        JobSystem::Get().ParallelFor("Skin Matrices", skinnedCount, SkinnedMeshesPerTask, [this](uint32_t, uint32_t begin, uint32_t end)
        {
            for (uint32_t index = begin; index < end; ++index)
            {
                const entt::entity entity = m_SkinnedMeshEntities[index];
                m_SkinnedBones[index] = GetBoneTransforms(entity, m_Registry.get<WorldTransformComponent>(entity).WorldMatrix);
            }
        });
        for (uint32_t index = 0; index < skinnedCount; ++index)
        {
            const std::vector<glm::mat4>* bones = m_SkinnedBones[index];
            m_renderer->SetMeshInstancesBones(m_MeshRegistrations.at(m_SkinnedMeshEntities[index]).Instances,
                                              bones ? std::span<const glm::mat4>(*bones) : std::span<const glm::mat4>());
        }
    }

    void Scene::ApplySyncPoint()
    {
        NOX_PROFILE_SCOPE("Scene Sync Point");

        m_CommandBuffers.ForEach([this](EntityCommandBuffer& commandBuffer) { commandBuffer.Apply(*this); });

        // Requests happen on the main thread; the entities stay pending until their assets are Ready.
        m_MissingAssets.ForEach([](std::vector<AssetHandle>& handles)
        {
            for (AssetHandle handle : handles)
                AssetManager::RequestAsset(handle);
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

    void Scene::UpdatePhysics3D()
    {
        if (!m_FrameInput.StepPhysics || !m_Physics3DScene)
            return;

        m_Physics3DScene->Step(m_FrameInput.Timestep);
    }

    void Scene::StartParticles()
    {
        // Play / Simulate start every emitter over (the editor's preview ran them while editing).
        for (auto entity : m_Registry.view<ParticleEmitterComponent>())
            ParticleSystem::Restart(m_Registry.get<ParticleEmitterComponent>(entity));
    }

    void Scene::UpdateParticles()
    {
        if (!m_FrameInput.StepAnimation)
            return;
        auto view = m_Registry.view<ParticleEmitterComponent, WorldTransformComponent>();
        for (auto entity : view)
        {
            auto [emitter, world] = view.get<ParticleEmitterComponent, WorldTransformComponent>(entity);
            ParticleSystem::Simulate(emitter, world.WorldMatrix, m_FrameInput.Timestep);
        }
    }

    void Scene::SubmitParticles()
    {
        std::vector<AssetHandle>& missingAssets = m_MissingAssets.Local();
        std::vector<shaderio::GpuParticle> particles;
        auto view = m_Registry.view<ParticleEmitterComponent, WorldTransformComponent>();
        for (auto entity : view)
        {
            auto [emitter, world] = view.get<ParticleEmitterComponent, WorldTransformComponent>(entity);
            if (emitter.Simulation.Particles.empty())
                continue;

            Renderer::ParticleBatchSettings settings;
            if (emitter.Texture != 0)
            {
                // Soft dots until the sheet has loaded (requested at the sync point).
                if (const Texture2D* texture = AssetManager::FindLoadedAsset<Texture2D>(emitter.Texture))
                    settings.TextureSlot = texture->GetDescriptorIndexSlot();
                else
                    missingAssets.push_back(emitter.Texture);
            }
            settings.FlipbookColumns = std::max(emitter.FlipbookColumns, 1u);
            settings.FlipbookRows = std::max(emitter.FlipbookRows, 1u);
            settings.Additive = emitter.Blend == ParticleBlend::Additive;
            settings.Lit = emitter.Lit;
            settings.Soft = emitter.SoftParticles;
            settings.SoftDistance = WorldUnits::FromMeters(std::max(emitter.SoftDistance, 0.0f));

            particles.clear();
            particles.reserve(emitter.Simulation.Particles.size());
            for (const ParticleSimulation::Particle& particle : emitter.Simulation.Particles)
            {
                const ParticleSystem::Appearance appearance = ParticleSystem::Evaluate(emitter, particle, world.WorldMatrix, m_FrameInput.Timestep);
                if (appearance.Color.a <= 0.0f || appearance.Size <= 0.0f)
                    continue;
                shaderio::GpuParticle gpu{};
                gpu.position = appearance.Position;
                gpu.size = appearance.Size;
                gpu.color = appearance.Color;
                gpu.rotation = particle.Rotation;
                gpu.frame = appearance.Frame;
                gpu.previousPosition = appearance.PreviousPosition;
                particles.push_back(gpu);
            }
            m_renderer->SubmitParticles(settings, particles);
        }
    }

    void Scene::StartTimelines()
    {
        m_TimelinesRunning = true;
        for (auto entity : m_Registry.view<TimelinePlayerComponent>())
        {
            auto& player = m_Registry.get<TimelinePlayerComponent>(entity);
            player.Time = 0.0f;
            player.Playing = player.PlayOnStart;
            player.FiredEvents.clear();
        }
    }

    Entity Scene::FindTimelineTarget(Entity player, const std::string& path)
    {
        // Names separated by '/', each one a child of the one before ("" = the player).
        Entity current = player;
        size_t start = 0;
        while (current && start < path.size())
        {
            const size_t end = std::min(path.find('/', start), path.size());
            const std::string name = path.substr(start, end - start);
            start = end + 1;
            if (name.empty())
                continue;
            Entity found;
            if (current.HasComponent<RelationshipComponent>())
            {
                for (UUID childID : current.GetComponent<RelationshipComponent>().Children)
                {
                    Entity child = GetEntityByUUID(childID);
                    if (child && child.HasComponent<TagComponent>() && child.GetComponent<TagComponent>().Tag == name)
                    {
                        found = child;
                        break;
                    }
                }
            }
            current = found;
        }
        return current;
    }

    void Scene::ApplyTimeline(Entity player, const Timeline& timeline, float time, bool cameraCuts)
    {
        for (const TimelineTrack& track : timeline.Tracks)
        {
            if (track.Muted || track.Keys.empty() || track.Property == TimelineProperty::Event)
                continue;
            if (track.Property == TimelineProperty::CameraCut)
            {
                // The camera the cut names becomes the primary one (the others stop being primary), only when it changes.
                const std::string* path = cameraCuts ? TimelineEvaluation::CameraCutAt(track, time) : nullptr;
                Entity camera = path ? FindTimelineTarget(player, *path) : Entity();
                if (camera && camera.HasComponent<CameraComponent>() && GetPrimaryCameraEntity() != camera)
                {
                    for (auto handle : m_Registry.view<CameraComponent>())
                        m_Registry.get<CameraComponent>(handle).Primary = false;
                    camera.GetComponent<CameraComponent>().Primary = true;
                }
                continue;
            }
            Entity target = FindTimelineTarget(player, track.Target);
            if (!target)
                continue;

            const glm::vec4 value = TimelineEvaluation::Evaluate(track, time);
            switch (track.Property)
            {
            case TimelineProperty::Translation:
            case TimelineProperty::Rotation:
            case TimelineProperty::Scale:
            {
                if (!target.HasComponent<TransformComponent>())
                    break;
                auto& transform = target.GetComponent<TransformComponent>();
                if (track.Property == TimelineProperty::Translation)
                    transform.Translation = glm::vec3(value);
                else if (track.Property == TimelineProperty::Rotation)
                    transform.Rotation = glm::radians(glm::vec3(value));
                else
                    transform.Scale = glm::vec3(value);
                // Every entity has one from its creation: no component is added from this (possibly worker) thread.
                if (auto* dirty = m_Registry.try_get<DirtyTransformComponent>(target))
                    dirty->isDirty = true;
                break;
            }
            case TimelineProperty::LightColor:
                if (target.HasComponent<DirectionalLightComponent>())
                    target.GetComponent<DirectionalLightComponent>().Color = glm::vec3(value);
                if (target.HasComponent<PointLightComponent>())
                    target.GetComponent<PointLightComponent>().Color = glm::vec3(value);
                if (target.HasComponent<SpotLightComponent>())
                    target.GetComponent<SpotLightComponent>().Color = glm::vec3(value);
                break;
            case TimelineProperty::LightIntensity:
                if (target.HasComponent<DirectionalLightComponent>())
                    target.GetComponent<DirectionalLightComponent>().Intensity = value.x;
                if (target.HasComponent<PointLightComponent>())
                    target.GetComponent<PointLightComponent>().Intensity = value.x;
                if (target.HasComponent<SpotLightComponent>())
                    target.GetComponent<SpotLightComponent>().Intensity = value.x;
                break;
            case TimelineProperty::CameraFOV:
                if (target.HasComponent<CameraComponent>())
                    target.GetComponent<CameraComponent>().Camera.SetPerspectiveVerticalFOV(glm::radians(glm::clamp(value.x, 1.0f, 179.0f)));
                break;
            default:
                break;
            }
        }
    }

    void Scene::PreviewTimeline(Entity player, float time)
    {
        if (!player || !player.HasComponent<TimelinePlayerComponent>())
            return;
        if (const Timeline* timeline = AssetManager::FindLoadedAsset<Timeline>(player.GetComponent<TimelinePlayerComponent>().Timeline))
            ApplyTimeline(player, *timeline, time);
    }

    void Scene::UpdateTimelines()
    {
        if (!m_TimelinesRunning || !m_FrameInput.StepAnimation)
            return;

        const float ts = m_FrameInput.Timestep;
        std::vector<AssetHandle>& missingAssets = m_MissingAssets.Local();
        for (auto handle : m_Registry.view<TimelinePlayerComponent>())
        {
            auto& player = m_Registry.get<TimelinePlayerComponent>(handle);
            player.FiredEvents.clear();
            if (player.Timeline == 0)
                continue;
            const Timeline* timeline = AssetManager::FindLoadedAsset<Timeline>(player.Timeline);
            if (!timeline)
            {
                missingAssets.push_back(player.Timeline);
                continue;
            }

            // Advance (looping, or stopping at either end), then pose everything the tracks drive.
            const float previous = player.Time;
            bool wrapped = false;
            if (player.Playing)
            {
                const float duration = std::max(timeline->Duration, 0.01f);
                player.Time += ts * player.Speed;
                if (player.Time > duration || player.Time < 0.0f)
                {
                    if (timeline->Loop)
                    {
                        player.Time = std::fmod(std::fmod(player.Time, duration) + duration, duration);
                        wrapped = true;
                    }
                    else
                    {
                        player.Time = std::clamp(player.Time, 0.0f, duration);
                        player.Playing = false;
                    }
                }
                for (const TimelineTrack& track : timeline->Tracks)
                    TimelineEvaluation::CollectEvents(track, previous, player.Time, wrapped, player.FiredEvents);
            }

            ApplyTimeline(Entity(handle, this), *timeline, player.Time, true);
        }
    }

    void Scene::UpdateAnimators()
    {
        if (!m_FrameInput.StepAnimation)
            return;

        // Animators update in parallel (Unreal evaluates each skeletal mesh's animation as a task of its own). The ones that write into
        // the same node table -- one imported model's joints and nodes: its skinned meshes in graph mode, its node clips, root motion on
        // its nodes -- update one after another in one task, in registry order as before; any other two write different entities.
        auto view = m_Registry.view<AnimatorComponent>();
        m_AnimatorOrder.clear();
        for (auto entity : view)
        {
            const AnimatorComponent& animator = view.get<AnimatorComponent>(entity);
            if (animator.NodeEntities.empty())
                m_AnimatorOrder.push_back({ false, static_cast<uint64_t>(entt::to_integral(entity)), entity });
            else
                m_AnimatorOrder.push_back({ true, static_cast<uint64_t>(animator.NodeEntities.front()), entity });
        }
        if (m_AnimatorOrder.empty())
            return;
        std::stable_sort(m_AnimatorOrder.begin(), m_AnimatorOrder.end(), [](const AnimatorTask& a, const AnimatorTask& b)
        {
            return a.SharesNodeTable != b.SharesNodeTable ? a.SharesNodeTable < b.SharesNodeTable : a.Key < b.Key;
        });
        m_AnimatorGroups.clear();
        for (uint32_t index = 0; index < m_AnimatorOrder.size(); ++index)
        {
            if (index == 0 || m_AnimatorOrder[index].SharesNodeTable != m_AnimatorOrder[index - 1].SharesNodeTable ||
                m_AnimatorOrder[index].Key != m_AnimatorOrder[index - 1].Key)
                m_AnimatorGroups.push_back(index);
        }
        m_AnimatorGroups.push_back(static_cast<uint32_t>(m_AnimatorOrder.size()));

        // The pools the animators look components up in, made before the tasks: looking up a component whose pool does not exist yet
        // creates the pool, which is not thread-safe.
        m_Registry.storage<TransformComponent>();
        m_Registry.storage<DirtyTransformComponent>();
        m_Registry.storage<WorldTransformComponent>();
        m_Registry.storage<CharacterController3DComponent>();

        const float ts = m_FrameInput.Timestep;
        const uint32_t groupCount = static_cast<uint32_t>(m_AnimatorGroups.size() - 1);
        JobSystem::Get().ParallelFor("Animators", groupCount, AnimatorGroupsPerTask, [&](uint32_t, uint32_t begin, uint32_t end)
        {
            for (uint32_t group = begin; group < end; ++group)
            {
                for (uint32_t index = m_AnimatorGroups[group]; index < m_AnimatorGroups[group + 1]; ++index)
                {
                    const entt::entity entity = m_AnimatorOrder[index].Entity;
                    UpdateAnimator(entity, view.get<AnimatorComponent>(entity), ts);
                }
            }
        });
    }

    void Scene::UpdateAnimator(entt::entity entity, AnimatorComponent& animatorComp, float ts)
    {
        std::vector<AssetHandle>& missingAssets = m_MissingAssets.Local();

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

        // 1b. Sync the graph asset if assigned (graph mode). Animation and Graph are meant to be mutually
        // exclusive per entity; if both are set, Graph takes precedence below.
        animatorComp.FiredNotifies.clear();
        if (animatorComp.Graph != 0)
        {
            animatorComp.GraphInstance.Sync(animatorComp.Graph, missingAssets);
            animatorComp.GraphInstance.RootMotion = animatorComp.RootMotion;
            animatorComp.GraphInstance.RootBone = animatorComp.RootBone;
        }

        // Single-clip mode's playhead before this step: the notifies it crosses fire like in graph mode.
        const bool clipMode = animatorComp.Graph == 0 && animatorComp.Animation != 0;
        const float clipTimeBefore = animatorComp.Animator.GetCurrentAnimationTime();
        const Ref<AnimationSequence> clipBefore = animatorComp.Animator.GetCurrentAnimation();

        // 2. Legacy self-contained skeleton evaluation. Imports with a node table skin from the
        // joint entities instead (see GetBoneTransforms), so the skeleton isn't evaluated twice.
        if (animatorComp.Skeleton != 0 && animatorComp.NodeEntities.empty())
        {
            const Skeleton* skeleton = AssetManager::FindLoadedAsset<Skeleton>(animatorComp.Skeleton);
            if (!skeleton)
                missingAssets.push_back(animatorComp.Skeleton);
            if (skeleton && !skeleton->AllNodes.empty())
            {
                if (animatorComp.Graph != 0)
                {
                    if (const AnimPose* pose = animatorComp.GraphInstance.Evaluate(ts, animatorComp.Playing, *skeleton))
                        ApplyPoseToSkeleton(*pose, *skeleton, animatorComp.GraphFinalBoneTransforms, &animatorComp.GraphGlobalMatrices);
                }
                else if (animatorComp.Playing)
                    animatorComp.Animator.Update(ts, *skeleton);
                else
                    animatorComp.Animator.UpdateTransforms(*skeleton);
            }
        }

        // Graph mode's node-table skinning path: the whole evaluated pose (every joint, not just one
        // clip's channels) written straight to ECS transforms -- the same joint-entity path
        // Animation/NodeEntities already drives, so GetBoneTransforms doesn't need to know which mode
        // produced them.
        if (!animatorComp.NodeEntities.empty() && animatorComp.Graph != 0 && animatorComp.Skeleton != 0)
        {
            const Skeleton* skeleton = AssetManager::FindLoadedAsset<Skeleton>(animatorComp.Skeleton);
            if (!skeleton)
                missingAssets.push_back(animatorComp.Skeleton);
            if (skeleton && !skeleton->AllNodes.empty())
            {
                if (const AnimPose* pose = animatorComp.GraphInstance.Evaluate(ts, animatorComp.Playing, *skeleton))
                {
                    size_t nodeCount = std::min(animatorComp.NodeEntities.size(), pose->LocalTransforms.size());
                    for (size_t nodeIndex = 0; nodeIndex < nodeCount; nodeIndex++)
                    {
                        auto it = m_EntityMap.find(animatorComp.NodeEntities[nodeIndex]);
                        if (it == m_EntityMap.end() || !m_Registry.valid(it->second) ||
                            !m_Registry.all_of<TransformComponent>(it->second))
                            continue;

                        auto& transform = m_Registry.get<TransformComponent>(it->second);
                        const Animator::NodeTransform& t = pose->LocalTransforms[nodeIndex];
                        transform.Translation = t.Translation;
                        transform.Rotation = glm::eulerAngles(t.Rotation);
                        transform.Scale = t.Scale;
                        if (auto* dirty = m_Registry.try_get<DirtyTransformComponent>(it->second))
                            dirty->isDirty = true;
                    }
                }
            }
        }

        // What the graph's evaluation produced besides the pose: notifies for the scripts, and root motion moving this
        // entity (the skeleton's model space is this entity's: rotate and scale the step into its parent's space).
        // A character controller owns the entity's translation (it would overwrite the step): it gets the root motion as
        // a velocity instead and moves by it in the next physics steps, colliding like any other movement.
        auto* character = m_Registry.try_get<CharacterController3DComponent>(entity);
        if (character)
            character->RootMotionVelocity = glm::vec3(0.0f);
        if (animatorComp.Graph != 0 && animatorComp.Playing)
        {
            animatorComp.FiredNotifies = animatorComp.GraphInstance.GetFiredNotifies();
            const glm::vec3 rootMotion = animatorComp.GraphInstance.ConsumeRootMotion();
            if (rootMotion != glm::vec3(0.0f) && character && ts > 0.0f)
            {
                const auto* world = m_Registry.try_get<WorldTransformComponent>(entity);
                const glm::mat3 toWorld = world ? glm::mat3(world->WorldMatrix)
                                                : glm::mat3(m_Registry.get<TransformComponent>(entity).GetTransform());
                character->RootMotionVelocity = toWorld * rootMotion / ts;
            }
            else if (rootMotion != glm::vec3(0.0f) && m_Registry.all_of<TransformComponent>(entity))
            {
                auto& transform = m_Registry.get<TransformComponent>(entity);
                transform.Translation += glm::mat3(transform.GetTransform()) * rootMotion;
                if (auto* dirty = m_Registry.try_get<DirtyTransformComponent>(entity))
                    dirty->isDirty = true;
            }
        }

        // Node/object animations write the evaluated .nanim TRS directly to ECS transforms.
        if (!animatorComp.NodeEntities.empty() && animatorComp.Graph == 0 && animatorComp.Animation != 0)
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

        // Single-clip mode's notifies: what the playhead crossed this step (across the end when it looped).
        if (clipMode && animatorComp.Playing && clipBefore && clipBefore == animatorComp.Animator.GetCurrentAnimation())
        {
            const float clipTimeAfter = animatorComp.Animator.GetCurrentAnimationTime();
            if (clipTimeAfter != clipTimeBefore)
            {
                const bool forward = animatorComp.Animator.GetPlaybackSpeed() >= 0.0f;
                const bool wrapped = forward ? clipTimeAfter < clipTimeBefore : clipTimeAfter > clipTimeBefore;
                CollectCrossedNotifies(*clipBefore, clipTimeBefore, clipTimeAfter, wrapped, animatorComp.FiredNotifies);
            }
        }
    }

    void Scene::UpdateBoneSockets()
    {
        auto sockets = m_Registry.view<BoneSocketComponent, TransformComponent>();
        if (sockets.begin() == sockets.end())
            return;

        auto parentOf = [&](entt::entity entity) -> entt::entity
        {
            const auto* relationship = m_Registry.try_get<RelationshipComponent>(entity);
            if (!relationship || relationship->Parent == 0)
                return entt::null;
            const auto found = m_EntityMap.find(relationship->Parent);
            return found != m_EntityMap.end() && m_Registry.valid(found->second) ? found->second : entt::null;
        };
        // An entity's transform relative to `ancestor` (the product of the local transforms on the way up), false when it is not below it.
        auto relativeTo = [&](entt::entity entity, entt::entity ancestor, glm::mat4& out) -> bool
        {
            out = glm::mat4(1.0f);
            for (int depth = 0; depth < 256 && entity != entt::null; ++depth)
            {
                if (entity == ancestor)
                    return true;
                if (const auto* transform = m_Registry.try_get<TransformComponent>(entity))
                    out = transform->GetTransform() * out;
                entity = parentOf(entity);
            }
            return false;
        };

        for (auto socketEntity : sockets)
        {
            auto& socket = sockets.get<BoneSocketComponent>(socketEntity);
            if (socket.Bone.empty())
                continue;

            // The nearest animator above it.
            entt::entity animatorEntity = parentOf(socketEntity);
            for (int depth = 0; depth < 256 && animatorEntity != entt::null && !m_Registry.all_of<AnimatorComponent>(animatorEntity); ++depth)
                animatorEntity = parentOf(animatorEntity);
            if (animatorEntity == entt::null || !m_Registry.all_of<AnimatorComponent>(animatorEntity))
                continue;
            const AnimatorComponent& animator = m_Registry.get<AnimatorComponent>(animatorEntity);
            const Skeleton* skeleton = animator.Skeleton != 0 ? AssetManager::FindLoadedAsset<Skeleton>(animator.Skeleton) : nullptr;
            if (!skeleton)
                continue;

            if (socket.CachedSkeleton != animator.Skeleton || socket.CachedIndex < 0 || socket.CachedIndex >= static_cast<int32_t>(skeleton->AllNodes.size()) ||
                !skeleton->AllNodes[socket.CachedIndex] || skeleton->AllNodes[socket.CachedIndex]->Name != socket.Bone)
            {
                socket.CachedIndex = -1;
                socket.CachedSkeleton = animator.Skeleton;
                for (const Node* node : skeleton->AllNodes)
                {
                    if (node && node->Name == socket.Bone)
                    {
                        socket.CachedIndex = node->Index;
                        break;
                    }
                }
            }
            if (socket.CachedIndex < 0)
                continue;

            // The bone in the animator entity's space: from its joint entity (a model's node table), else the animator's own pose.
            glm::mat4 bone(1.0f);
            if (!animator.NodeEntities.empty())
            {
                if (socket.CachedIndex >= static_cast<int32_t>(animator.NodeEntities.size()))
                    continue;
                const auto joint = m_EntityMap.find(animator.NodeEntities[socket.CachedIndex]);
                if (joint == m_EntityMap.end() || !m_Registry.valid(joint->second) || !relativeTo(joint->second, animatorEntity, bone))
                    continue;
            }
            else
            {
                const std::vector<glm::mat4>& globals = animator.Graph != 0 ? animator.GraphGlobalMatrices : animator.Animator.GetGlobalBoneTransforms();
                if (socket.CachedIndex >= static_cast<int32_t>(globals.size()))
                    continue;
                bone = globals[socket.CachedIndex];
            }

            // Its local transform: the bone (with the offset) seen from the socket's parent.
            glm::mat4 parentToAnimator(1.0f);
            const entt::entity parent = parentOf(socketEntity);
            if (parent != animatorEntity && !relativeTo(parent, animatorEntity, parentToAnimator))
                continue;
            const glm::mat4 offset = glm::translate(glm::mat4(1.0f), socket.Offset) * glm::toMat4(glm::quat(socket.RotationOffset)) *
                                     glm::scale(glm::mat4(1.0f), socket.Scale);
            const glm::mat4 local = glm::inverse(parentToAnimator) * bone * offset;

            auto& transform = sockets.get<TransformComponent>(socketEntity);
            glm::vec3 translation, rotation, scale;
            if (Math::DecomposeTransform(local, translation, rotation, scale))
            {
                transform.Translation = translation;
                transform.Rotation = rotation;
                transform.Scale = scale;
                if (auto* dirty = m_Registry.try_get<DirtyTransformComponent>(socketEntity))
                    dirty->isDirty = true;
            }
        }
    }

    const std::vector<glm::mat4>* Scene::GetBoneTransforms(entt::entity entity, const glm::mat4& meshWorld)
    {
        AnimatorComponent* animatorComp = m_Registry.try_get<AnimatorComponent>(entity);
        if (!animatorComp)
            return nullptr;

        if (animatorComp->Skeleton == 0 || animatorComp->NodeEntities.empty())
        {
            // Self-contained: either the plain Animator, or (Graph mode) the graph's pose applied directly.
            return animatorComp->Graph != 0 ? &animatorComp->GraphFinalBoneTransforms
                                            : &animatorComp->Animator.GetFinalBoneTransforms();
        }

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
            MeshComponent, MaterialComponent, ModelInstanceComponent, PrefabInstanceComponent, DirectionalLightComponent,
            PointLightComponent, SpotLightComponent, RectLightComponent, DiscLightComponent, EnvironmentLightComponent, AnimatorComponent, TimelinePlayerComponent,
            FolderComponent, ParticleEmitterComponent, SpriteRendererComponent, CircleRendererComponent, CameraComponent,
            ScriptComponent, RigidBody2DComponent, BoxCollider2DComponent,
            CircleCollider2DComponent, RigidBody3DComponent, BoxCollider3DComponent, SphereCollider3DComponent, CapsuleCollider3DComponent,
            CharacterController3DComponent, TextComponent, LevelStreamingVolumeComponent, WorldPartitionComponent, WorldStreamingSourceComponent>;

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
            // A duplicated model instance spawns its own nodes (their UUIDs derive from its own), with the original's
            // overrides; the original's nodes are not copied.
            if (duplicate.HasComponent<ModelInstanceComponent>())
            {
                auto& instance = duplicate.GetComponent<ModelInstanceComponent>();
                instance.Overrides = ModelInstance::CollectOverrides(*this, source);
                instance.Spawned = false;
            }
            // A duplicated prefab instance spawns its own entities from the prefab.
            if (duplicate.HasComponent<PrefabInstanceComponent>())
            {
                auto& instance = duplicate.GetComponent<PrefabInstanceComponent>();
                instance.Overrides = PrefabInstance::CollectOverrides(*this, source);
                instance.Structure = PrefabInstance::CollectStructureChanges(*this, source);
                instance.Nested = PrefabInstance::CollectNested(*this, source);
                instance.Spawned = false;
            }
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
                if (child && !(child.HasComponent<ModelNodeComponent>() && source.HasComponent<ModelInstanceComponent>()) &&
                    !(child.HasComponent<PrefabNodeComponent>() && source.HasComponent<PrefabInstanceComponent>()))
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
        // Box2D works in the world's units: its tolerances follow the project's units per meter, gravity and speeds are converted.
        b2SetLengthUnitsPerMeter(WorldUnits::PerMeter());
        b2WorldDef worldDef = b2DefaultWorldDef();
        worldDef.gravity = b2Vec2{ 0.0f, -WorldUnits::FromMeters(9.81f) };
        // Restitution threshold is a world setting in Box2D: the smallest of the colliders' (a bouncy ball keeps bouncing).
        float restitutionThreshold = FLT_MAX;
        for (auto e : view)
        {
            Entity entity = {e, this};
            if (entity.HasComponent<BoxCollider2DComponent>())
                restitutionThreshold = std::min(restitutionThreshold, entity.GetComponent<BoxCollider2DComponent>().RestitutionThreshold);
            if (entity.HasComponent<CircleCollider2DComponent>())
                restitutionThreshold = std::min(restitutionThreshold, entity.GetComponent<CircleCollider2DComponent>().RestitutionThreshold);
        }
        if (restitutionThreshold != FLT_MAX)
            worldDef.restitutionThreshold = WorldUnits::FromMeters(std::max(restitutionThreshold, 0.0f));
        m_PhysicsWorldID = b2CreateWorld(&worldDef);

        for (auto e : view)
            CreatePhysics2DBody(Entity(e, this));
    }

    void Scene::CreatePhysics2DBody(Entity entity)
    {
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
                b2Vec2{bc2d.Offset.x * transform.Scale.x, bc2d.Offset.y * transform.Scale.y}, // the offset scales with the entity
                b2MakeRot(0.0f)
            );

            b2ShapeDef shapeDef = b2DefaultShapeDef();
            shapeDef.density = bc2d.Density;
            shapeDef.material.friction = bc2d.Friction;
            shapeDef.material.restitution = bc2d.Restitution;
            shapeDef.material.rollingResistance = bc2d.RollingResistance;
            shapeDef.material.tangentSpeed = WorldUnits::FromMeters(bc2d.TangentSpeed);
            b2ShapeId shapeID = b2CreatePolygonShape(body, &shapeDef, &boxShape);
        }

        if (entity.HasComponent<CircleCollider2DComponent>())
        {
            auto& cc2d = entity.GetComponent<CircleCollider2DComponent>();

            // A circle stays a circle: the larger of the two scales (a non-uniform scale cannot squash it); the offset scales.
            b2Circle circleShape;
            circleShape.center = b2Vec2{cc2d.Offset.x * transform.Scale.x, cc2d.Offset.y * transform.Scale.y};
            circleShape.radius = std::max(std::abs(transform.Scale.x), std::abs(transform.Scale.y)) * cc2d.Radius;

            b2ShapeDef shapeDef = b2DefaultShapeDef();
            shapeDef.density = cc2d.Density;
            shapeDef.material.friction = cc2d.Friction;
            shapeDef.material.restitution = cc2d.Restitution;
            shapeDef.material.rollingResistance = cc2d.RollingResistance;
            shapeDef.material.tangentSpeed = WorldUnits::FromMeters(cc2d.TangentSpeed);
            b2ShapeId shapeID = b2CreateCircleShape(body, &shapeDef, &circleShape);
        }
    }

    void Scene::OnPhysics2DStop()
    {
        b2DestroyWorld(m_PhysicsWorldID);
        m_PhysicsWorldID = b2_nullWorldId;
    }

    void Scene::OnPhysics3DStart()
    {
        m_Physics3DScene = IPhysics3DScene::Create(this);
        m_Physics3DScene->Init();
    }

    void Scene::OnPhysics3DStop()
    {
        m_Physics3DScene.reset();
        m_PendingPhysics3D.clear();
    }

    void Scene::OnPhysics3DComponentChanged(entt::registry& registry, entt::entity entity)
    {
        // Only while the physics scene runs (Play / Simulate start makes every body itself). The ID may already be gone when
        // the whole entity is being destroyed: DestroyEntity removes its body and character itself.
        if (!m_Physics3DScene)
            return;
        if (const IDComponent* id = registry.try_get<IDComponent>(entity))
            m_PendingPhysics3D.insert(id->ID);
    }

    void Scene::SyncPendingPhysics3D()
    {
        // Main thread, after transform propagation: a new entity's world transform is known, nothing else touches the bodies.
        if (!m_Physics3DScene || m_PendingPhysics3D.empty())
            return;
        for (UUID id : m_PendingPhysics3D)
        {
            Entity entity = GetEntityByUUID(id);
            if (!entity)
                continue;
            // Made again from the components as they are now: a collider added or removed changes the body's shape.
            m_Physics3DScene->DestroyBody(entity);
            m_Physics3DScene->DestroyCharacter(entity);
            if (entity.HasComponent<RigidBody3DComponent>())
                m_Physics3DScene->CreateBody(entity);
            if (entity.HasComponent<CharacterController3DComponent>())
                m_Physics3DScene->CreateCharacter(entity);
        }
        m_PendingPhysics3D.clear();
    }
    
    void Scene::SubmitLights()
    {
        // Presentation settings belong to the scene camera even while the editor viewport supplies
        // the view transform. This keeps imported RTXPT scenes visually consistent in both modes.
        bool foundPrimaryCamera = false;
        for (auto entity : m_Registry.view<CameraComponent>())
        {
            const auto& camera = m_Registry.get<CameraComponent>(entity);
            if (!camera.Primary) continue;
            m_renderer->setAutoExposure(camera.AutoExposure, camera.ExposureCompensation,
                                        camera.AutoExposureMinEV, camera.AutoExposureMaxEV);
            foundPrimaryCamera = true;
            break;
        }
        if (!foundPrimaryCamera)
            m_renderer->setAutoExposure(false, 0.0f, -16.0f, 16.0f);

        const EnvironmentLightComponent* environment = FindEnvironment(m_Registry);
        const bool proceduralSky = environment && environment->Source == EnvironmentSource::ProceduralSky;
        const SkyLights sky = ResolveSkyLights(m_Registry, environment);

        // The sun (the first Directional Light) as the air lets it through: not at all once it has set. It goes first, the
        // moon after it: the first directional light submitted gets the denoised shadow mask, which at night is the moon's.
        bool hasDirectionalLight = false;
        {
            auto view = m_Registry.view<WorldTransformComponent, DirectionalLightComponent>();
            for (auto entity : view)
            {
                auto [wtc, light] = view.get<WorldTransformComponent, DirectionalLightComponent>(entity);
                const bool isSun = !hasDirectionalLight;
                hasDirectionalLight = true;
                if (isSun && proceduralSky)
                {
                    DirectionalLightComponent sun = light;
                    sun.Color *= sky.SunTransmittance;
                    if (Luminance(sun.Color) * sun.Intensity > 0.0f)
                        m_renderer->SubmitLight(wtc.WorldMatrix, sun);
                    continue;
                }
                m_renderer->SubmitLight(wtc.WorldMatrix, light);
            }
        }
        // A procedural sky without a Directional Light: the sun it draws also lights the scene (direct light, shadows).
        // A Directional Light added later is the sun from then on.
        if (!hasDirectionalLight && proceduralSky)
        {
            DirectionalLightComponent skySun;
            skySun.Intensity = DefaultSkySunIntensity;
            skySun.Color *= sky.SunTransmittance;
            if (Luminance(skySun.Color) > 0.0f)
                m_renderer->SubmitLight(glm::toMat4(DefaultSkySunRotation()), skySun);
        }
        if (proceduralSky && sky.MoonLights)
        {
            DirectionalLightComponent moon;
            moon.Color = sky.MoonIlluminance * sky.MoonTransmittance;
            moon.Intensity = 1.0f;
            moon.AngularDiameter = glm::degrees(sky.MoonAngularRadius * 2.0f);
            m_renderer->SubmitLight(DirectionalLightTransform(sky.MoonDirection), moon);
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
        {
            auto view = m_Registry.view<WorldTransformComponent, RectLightComponent>();
            for (auto entity : view)
            {
                auto [wtc, light] = view.get<WorldTransformComponent, RectLightComponent>(entity);
                m_renderer->SubmitLight(wtc.WorldMatrix, light);
            }
        }
        {
            auto view = m_Registry.view<WorldTransformComponent, DiscLightComponent>();
            for (auto entity : view)
            {
                auto [wtc, light] = view.get<WorldTransformComponent, DiscLightComponent>(entity);
                m_renderer->SubmitLight(wtc.WorldMatrix, light);
            }
        }
        // The environment itself is applied by SetRenderer (main thread); this system may run on a worker.
        if (environment)
            m_renderer->setScaleIBLAmbient(environment->RadianceScale.x);
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
                // The default font until the text's own has loaded (requested at the sync point).
                Ref<Font> font;
                if (text.Font != 0)
                {
                    if (Font* loaded = AssetManager::FindLoadedAsset<Font>(text.Font))
                        font = Ref<Font>(loaded);
                    else
                        missingAssets.push_back(text.Font);
                }
                if (!font)
                    font = Font::GetDefault();
                m_renderer2D->DrawString(text.TextString, font, transform.WorldMatrix,
                                         { text.Color, text.Kerning, text.LineSpacing, text.OutlineColor, text.OutlineThickness, text.BackgroundColor,
                                           text.BackgroundPadding },
                                         static_cast<int>(entity));
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
    void Scene::OnComponentAdded<ModelInstanceComponent>(Entity entity, ModelInstanceComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<ModelNodeComponent>(Entity entity, ModelNodeComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<FolderComponent>(Entity entity, FolderComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<PrefabInstanceComponent>(Entity entity, PrefabInstanceComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<PrefabNodeComponent>(Entity entity, PrefabNodeComponent& component)
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
    void Scene::OnComponentAdded<RectLightComponent>(Entity entity, RectLightComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<DiscLightComponent>(Entity entity, DiscLightComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<EnvironmentLightComponent>(Entity entity, EnvironmentLightComponent& component)
    {
    }
    
    template <>
    void Scene::OnComponentAdded<AnimatorComponent>(Entity entity, AnimatorComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<TimelinePlayerComponent>(Entity entity, TimelinePlayerComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<ParticleEmitterComponent>(Entity entity, ParticleEmitterComponent& component)
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

    template <>
    void Scene::OnComponentAdded<BoneSocketComponent>(Entity entity, BoneSocketComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<RigidBody3DComponent>(Entity entity, RigidBody3DComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<BoxCollider3DComponent>(Entity entity, BoxCollider3DComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<SphereCollider3DComponent>(Entity entity, SphereCollider3DComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<CapsuleCollider3DComponent>(Entity entity, CapsuleCollider3DComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<CharacterController3DComponent>(Entity entity, CharacterController3DComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<LevelComponent>(Entity entity, LevelComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<LevelStreamingVolumeComponent>(Entity entity, LevelStreamingVolumeComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<WorldPartitionComponent>(Entity entity, WorldPartitionComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<WorldStreamingSourceComponent>(Entity entity, WorldStreamingSourceComponent& component)
    {
    }

    template <>
    void Scene::OnComponentAdded<HLODComponent>(Entity entity, HLODComponent& component)
    {
    }
}
