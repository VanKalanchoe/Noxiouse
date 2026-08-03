#include "Scene.h"

#include <box2d/box2d.h>

#include "Entity.h"
#include "Components.h"
#include "SceneGraph.h"
#include "NoxCore/Physics/Physics2D.h"

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
        if (!m_IsPaused || m_StepFrames-- > 0)
        {
            /*// Update Scripts
            {
                // C# Entity OnUpdate
                auto view = m_Registry.view<ScriptComponent>();
                for (auto e : view)
                {
                    Entity entity = { e, this };
                    ScriptEngine::OnUpdateEntity(entity, ts);
                }
                
                m_Registry.view<NativeScriptComponent>().each([=](auto entity, auto& nsc)
                {
                    //todo Move to Scene::OnScenePlay
                    if (!nsc.Instance)
                    {
                        nsc.Instance = nsc.InstantiateScript();
                        nsc.Instance->m_Entity = Entity{entity, this};
                        nsc.Instance->OnCreate();
                    }

                    nsc.Instance->OnUpdate(ts);
                });
            }*/

            // Physics
            {
                int32_t subStepCount = 4;
                b2World_Step(m_PhysicsWorldID, ts, subStepCount);

                // Retrieve Transform from Box2D
                auto view = m_Registry.view<RigidBody2DComponent>();
                for (auto e : view)
                {
                    Entity entity = {e, this};
                    auto& transform = entity.GetComponent<TransformComponent>();
                    auto& r2bd = entity.GetComponent<RigidBody2DComponent>();

                    b2BodyId body = r2bd.RuntimeBody;
                    const auto& position = b2Body_GetPosition(body);
                    transform.Translation.x = position.x;
                    transform.Translation.y = position.y;
                    transform.Rotation.z = b2Rot_GetAngle(b2Body_GetRotation(body));
                    
                    m_Registry.get_or_emplace<DirtyTransformComponent>(e);
                }
            }
        }
        
        SceneGraph::UpdateWorldTransforms(m_Registry, m_EntityMap);
        
       // Render 2D
        //changed from transformcomp to worldcomp
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
            
            // Draw 3D Meshes
            {
               auto view = m_Registry.view<WorldTransformComponent, MeshComponent>();
               for (auto entity : view)
               {
                   auto [transform, mesh] = view.get<WorldTransformComponent, MeshComponent>(entity);
                
                   MaterialComponent* materialComp = m_Registry.try_get<MaterialComponent>(entity);
                
                   MaterialComponent defaultMaterial;
                   MaterialComponent& materialToUse = materialComp ? *materialComp : defaultMaterial;
                
                   m_renderer->SubmitMesh(transform.WorldMatrix, mesh, materialToUse, (int)entity);
               }
            }

            // Draw Sprites
            {
                auto group = m_Registry.group<WorldTransformComponent>(entt::get<SpriteRendererComponent>);
                for (auto entity : group)
                {
                    auto [transform, sprite] = group.get<WorldTransformComponent, SpriteRendererComponent>(entity);
                    //Renderer2D::DrawQuad(transform.Position, transform.Size, transform.Scale, transform.Rotation, sprite.Color);
                    m_renderer2D->DrawSprite(transform.WorldMatrix, sprite, (int)entity);
                }
            }
             
            // Draw Circles
            {
                auto view = m_Registry.view<WorldTransformComponent, CircleRendererComponent>();
                for (auto entity : view)
                {
                    auto [transform, circle] = view.get<WorldTransformComponent, CircleRendererComponent>(entity);

                    m_renderer2D->DrawCircle(transform.WorldMatrix, circle.Color, circle.Thickness, circle.Fade, (int)entity);
                }
            }

            // Draw Text
            {
                auto view = m_Registry.view<WorldTransformComponent, TextComponent>();
                for (auto entity : view)
                {
                    auto [transform, text] = view.get<WorldTransformComponent, TextComponent>(entity);

                    m_renderer2D->DrawString(text.TextString, transform.WorldMatrix, text, (int)entity);
                }
            }
            
            m_renderer->EndScene();
        }
    }

    void Scene::OnUpdateSimulation(Timestep ts, EditorCamera& camera)
    {
        if (!m_IsPaused || m_StepFrames-- > 0)
        {
            // Physics
        
            int32_t subStepCount = 4;
            b2World_Step(m_PhysicsWorldID, ts, subStepCount);

            // Retrieve Transform from Box2D
            auto view = m_Registry.view<RigidBody2DComponent>();
            for (auto e : view)
            {
                Entity entity = {e, this};
                auto& transform = entity.GetComponent<TransformComponent>();
                auto& r2bd = entity.GetComponent<RigidBody2DComponent>();

                b2BodyId body = r2bd.RuntimeBody;
                const auto& position = b2Body_GetPosition(body);
                transform.Translation.x = position.x;
                transform.Translation.y = position.y;
                transform.Rotation.z = b2Rot_GetAngle(b2Body_GetRotation(body));
                
                m_Registry.get_or_emplace<DirtyTransformComponent>(e);
            }
        }
    
        // Render
        RenderScene(camera);
    }

    void Scene::OnUpdateEditor(Timestep ts, EditorCamera& camera)
    {
        RenderScene(camera);
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
    
    Entity Scene::DuplicateEntity(Entity entity)
    {
        // Copy name becuase were going to modify component data structure
        std::string name = entity.GetName();
        Entity newEntity = CreateEntity(name);

        // Copy components (except IDComponent and TagComponent)
        CopyComponentIfExists(AllComponents{}, newEntity, entity);

        return newEntity;
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
    
    void Scene::RenderScene(EditorCamera& camera)
    {
        SceneGraph::UpdateWorldTransforms(m_Registry, m_EntityMap);
        
        m_renderer->BeginScene(camera);
        
        // Draw 3D Meshes
        {
            auto view = m_Registry.view<WorldTransformComponent, MeshComponent>();
            for (auto entity : view)
            {
                auto [transform, mesh] = view.get<WorldTransformComponent, MeshComponent>(entity);
                
                MaterialComponent* materialComp = m_Registry.try_get<MaterialComponent>(entity);
                
                MaterialComponent defaultMaterial;
                MaterialComponent& materialToUse = materialComp ? *materialComp : defaultMaterial;
                
                m_renderer->SubmitMesh(transform.WorldMatrix, mesh, materialToUse, (int)entity);
            }
        }
        
        // Draw Sprites
        {
            auto group = m_Registry.group<WorldTransformComponent>(entt::get<SpriteRendererComponent>);
            for (auto entity : group)
            {
                auto [transform, sprite] = group.get<WorldTransformComponent, SpriteRendererComponent>(entity);
                //Renderer2D::DrawQuad(transform.Position, transform.Size, transform.Scale, transform.Rotation, sprite.Color);
                m_renderer2D->DrawSprite(transform.WorldMatrix, sprite, (int)entity);
                //Renderer2D::DrawRect(transform, sprite, (int)entity);
            }
        }
        
        // Draw Circles
        {
            auto view = m_Registry.view<WorldTransformComponent, CircleRendererComponent>();
            for (auto entity : view)
            {
                auto [transform, circle] = view.get<WorldTransformComponent, CircleRendererComponent>(entity);

                m_renderer2D->DrawCircle(transform.WorldMatrix, circle.Color, circle.Thickness, circle.Fade, (int)entity);
            }
        }
        // Draw Text
        {
            auto view = m_Registry.view<WorldTransformComponent, TextComponent>();
            for (auto entity : view)
            {
                auto [transform, text] = view.get<WorldTransformComponent, TextComponent>(entity);

                m_renderer2D->DrawString(text.TextString, transform.WorldMatrix, text, (int)entity);
            }
        }
        /*
        //Renderer2D::DrawLine(glm::vec3(2.0f), glm::vec3(5.0f), glm::vec4(1.0f, 0.0f, 1.0f, 1.0f));
        //Renderer2D::DrawRect(glm::vec3(0.0f), glm::vec2(1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
        */
        m_renderer->EndScene();
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
