#pragma once
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include "string"

#include "SceneCamera.h"

#include "NoxCore/Core/UUID.h"

#include "box2d/box2d.h"
#include "NoxCore/Animation/Animator.h"

#include "NoxCore/Renderer/Font.h"

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Renderer/DataTypes.h"

namespace Nox
{
    struct IDComponent
    {
        UUID ID;
        IDComponent() = default;
        IDComponent(const IDComponent&) = default;
        IDComponent(const UUID& uuid) : ID(uuid) {}
    };

    struct TagComponent
    {
        std::string Tag;
        TagComponent() = default;
        TagComponent(const TagComponent&) = default;
        TagComponent(const std::string& tag) : Tag(tag) {}
    };
    
    struct TransformComponent
    {
        glm::vec3 Translation = {0.0f, 0.0f, 0.0f};
        glm::vec3 Rotation = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Scale = {1.0f, 1.0f, 1.0f};

        TransformComponent() = default;
        TransformComponent(const TransformComponent&) = default;
        TransformComponent(const glm::vec3& translation) : Translation(translation) {}

        glm::mat4 GetTransform() const
        {
            glm::mat4 rotation = glm::toMat4(glm::quat(Rotation));

            return glm::translate(glm::mat4(1.0f), Translation)
                * rotation
                * glm::scale(glm::mat4(1.0f), Scale);
        }
    };
    
    // Cached World Matrix for Scene Graph
    struct WorldTransformComponent
    {
        glm::mat4 WorldMatrix{ 1.0f };

        WorldTransformComponent() = default;
        WorldTransformComponent(const WorldTransformComponent&) = default;
        WorldTransformComponent(const glm::mat4& world) : WorldMatrix(world) {}

        operator const glm::mat4&() const { return WorldMatrix; }
    };

    // Scene Graph Hierarchy Links (UUID-based for safety)
    struct RelationshipComponent
    {
        UUID Parent = 0;
        std::vector<UUID> Children;

        RelationshipComponent() = default;
        RelationshipComponent(const RelationshipComponent&) = default;
    };

    // Dirty Transform Tracking
    struct DirtyTransformComponent
    {
        bool isDirty = true;
    };
    
    struct MeshComponent
    {
        AssetHandle Mesh = 0;
        uint32_t SubmeshIndex = 0;
        
        MeshComponent() = default;
        MeshComponent(const MeshComponent&) = default;
        MeshComponent(AssetHandle mesh) : Mesh(mesh) {};
    };
    
    struct MaterialComponent
    {
        std::vector<glm::vec4> AlbedoColors = { glm::vec4(1.0f) };
        std::vector<AssetHandle> AlbedoMaps = { 0 };
        std::vector<AlphaMode> Modes = { AlphaMode::Opaque };
        std::vector<float> AlphaCutoffs = { 0.5f };
        std::vector<bool> DoubleSidedFlags = { false };
    
        MaterialComponent() = default;
        MaterialComponent(const MaterialComponent&) = default;
        MaterialComponent(const glm::vec4 color) : AlbedoColors{ color } {}
    };
    
    // Holds runtime animation state (tracks current time, playing animation, bone matrices)
    struct AnimatorComponent
    {
        Animator Animator;
        AssetHandle Animation = 0;
        AssetHandle Skeleton = 0;
        bool Playing = true;

        AnimatorComponent() = default;
        AnimatorComponent(const AnimatorComponent&) = default;
        AnimatorComponent(const Ref<AnimationSequence>& animation)
            : Animator(animation) {}
    };
    
    struct SpriteRendererComponent
    {
        glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
        /*std::string TextureName;*/
        //Ref<Texture2D> Texture;
        AssetHandle Texture = 0;
        float TilingFactor = 1.0f;

        SpriteRendererComponent() = default;
        SpriteRendererComponent(const SpriteRendererComponent&) = default;
        SpriteRendererComponent(const glm::vec4& color) : Color(color) {}
    };
    
    struct CircleRendererComponent
    {
        glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
        float Thickness = 0.5f;
        float Fade = 0.005f;

        CircleRendererComponent() = default;
        CircleRendererComponent(const CircleRendererComponent&) = default;
    };
    
    struct CameraComponent
    {
        SceneCamera Camera;
        bool Primary = true; // todo think about moving to scene
        bool FixedAspectRatio = false;

        CameraComponent() = default;
        CameraComponent(const CameraComponent&) = default;
    };
    
    struct ScriptComponent
    {
        std::string ClassName; // 32 bytes
        
        ScriptComponent() = default;
        ScriptComponent(const ScriptComponent&) = default;
    };
    
    /*// Forward declaration
    class ScriptableEntity;
    struct NativeScriptComponent
    {
        ScriptableEntity* Instance = nullptr;
        
        ScriptableEntity*(*InstantiateScript)();
        void(*DestroyScript)(NativeScriptComponent*);

        template<typename T>
        void Bind()
        {
            InstantiateScript = []() { return static_cast<ScriptableEntity*>(new T()); };
            DestroyScript = [](NativeScriptComponent* nsc) { delete nsc->Instance; nsc->Instance = nullptr; };
        }
    };*/
    
    // Physics

    struct RigidBody2DComponent
    {
        enum class BodyType { Static = 0, Dynamic, Kinematic };
        BodyType Type = BodyType::Static;
        bool FixedRotation = false;

        // Storage for runtime
        b2BodyId RuntimeBody;

        RigidBody2DComponent() = default;
        RigidBody2DComponent(const RigidBody2DComponent&) = default;
    };

    struct BoxCollider2DComponent
    {
        glm::vec2 Offset = { 0.0f, 0.0f };
        glm::vec2 Size = { 0.5f, 0.5f };//1x1m scale object
        
        //documentation what they do // todo move into physics material in the future maybe
        float Density = 1.0f; 
        float Friction = 0.5f;
        float Restitution = 0.0f;
        float RestitutionThreshold = 0.5f;
        
        // Storage for runtime
        void* RuntimeFixture = nullptr;

        BoxCollider2DComponent() = default;
        BoxCollider2DComponent(const BoxCollider2DComponent&) = default;
    };

    struct CircleCollider2DComponent
    {
        glm::vec2 Offset = { 0.0f, 0.0f };
        float Radius = 0.5f;// idk 0.5 doesnt work for me maybe changed with box2d v3 ?1x1m scale object
        
        //documentation what they do // todo move into physics material in the future maybe
        float Density = 1.0f; 
        float Friction = 0.5f;
        float Restitution = 0.0f;
        float RestitutionThreshold = 0.5f;
        
        // Storage for runtime
        void* RuntimeFixture = nullptr;

        CircleCollider2DComponent() = default;
        CircleCollider2DComponent(const CircleCollider2DComponent&) = default;
    };

    struct TextComponent
    {
        std::string TextString;
        Ref<Font> FontAsset = Font::GetDefault();
        glm::vec4 Color{ 1.0f };
        float Kerning = 0.0f;
        float LineSpacing = 0.0f;
    };
    
    template<typename... Component>
    struct ComponentGroup
    {
    };

    using AllComponents = 
        ComponentGroup<TransformComponent, WorldTransformComponent, RelationshipComponent, DirtyTransformComponent,
        MeshComponent, MaterialComponent, AnimatorComponent,
        SpriteRendererComponent,
            CircleRendererComponent, CameraComponent, ScriptComponent,
            /*NativeScriptComponent,*/ RigidBody2DComponent, BoxCollider2DComponent,
            CircleCollider2DComponent, TextComponent>;
}
