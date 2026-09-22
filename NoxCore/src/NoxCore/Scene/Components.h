#pragma once
#include <entt/entt.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <optional>
#include "string"
#include <unordered_map>

#include "SceneCamera.h"

#include "NoxCore/Core/UUID.h"

#include "box2d/box2d.h"
#include "NoxCore/Animation/Animator.h"

#include "NoxCore/Renderer/Font.h"

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Scripting/ScriptTypes.h"
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
    // Present on every entity (added at creation). Set instead of added/removed, so marking and clearing are plain
    // writes that tasks can do without structural registry changes.
    struct DirtyTransformComponent
    {
        bool isDirty = true;
    };
    
    struct MeshComponent
    {
        AssetHandle Mesh = 0;
        uint32_t SubmeshIndex = 0;
        uint32_t SubmeshCount = 1;
        
        MeshComponent() = default;
        MeshComponent(const MeshComponent&) = default;
        MeshComponent(AssetHandle mesh) : Mesh(mesh) {};
    };
    
    struct MaterialComponent
    {
        // Per-slot overrides of the mesh's materials (indexed by submesh, UE's OverrideMaterials). A zero handle, or no
        // entry, uses the mesh primitive's own .nmat; without overrides the list is empty and nothing is serialized.
        std::vector<AssetHandle> MaterialAssets;

        MaterialComponent() = default;
        MaterialComponent(const MaterialComponent&) = default;
    };
    
    // What a model instance changed on one of its nodes; everything else comes from the model's cooked node data.
    struct ModelNodeOverride
    {
        uint32_t NodeIndex = 0;
        std::optional<TransformComponent> Transform;
        std::optional<std::string> Name;
        std::vector<AssetHandle> MaterialAssets; // MaterialComponent overrides, empty for none
    };

    // Root of an imported model placed in a scene (UE's Packed Level Actor / Unity's model prefab instance): its node
    // entities are spawned from the model's cooked node data once the model is loaded, and only this component is saved
    // for them -- removed nodes and per-node overrides (§5.11.5).
    struct ModelInstanceComponent
    {
        AssetHandle Model = 0;
        std::vector<uint32_t> RemovedNodes;       // glTF node indices deleted from this instance
        std::vector<ModelNodeOverride> Overrides; // to apply at spawn; the node entities hold them once spawned
        bool Spawned = false;                     // runtime only

        ModelInstanceComponent() = default;
        ModelInstanceComponent(const ModelInstanceComponent&) = default;
    };

    // A node entity spawned from a model instance (never saved: it is spawned again on load).
    struct ModelNodeComponent
    {
        UUID Instance = 0; // the instance root
        uint32_t NodeIndex = 0;

        ModelNodeComponent() = default;
        ModelNodeComponent(const ModelNodeComponent&) = default;
    };

    // Following KHR_Punctual
    struct DirectionalLightComponent
    {
        glm::vec3 Color{ 1.0f, 1.0f, 1.0f };
        float Intensity = 1.0f;
        float AngularDiameter = 0.5357f; // In degrees (UE5 default: 0.5357 deg = astronomical Sun diameter)
        uint32_t ShadowSamples = 1;      // 1 for fast stochastic + DLSS/NRD, 2-8 for high quality

        DirectionalLightComponent() = default;
        DirectionalLightComponent(const DirectionalLightComponent&) = default;
    };

    struct PointLightComponent
    {
        glm::vec3 Color{ 1.0f, 1.0f, 1.0f };
        float Intensity = 5.0f;
        float Range = 10.0f;
        float Radius = 0.05f;         // Light source radius in meters (UE5 default: 0.05m = 5cm bulb)
        uint32_t ShadowSamples = 1;

        PointLightComponent() = default;
        PointLightComponent(const PointLightComponent&) = default;
    };

    struct SpotLightComponent
    {
        glm::vec3 Color{ 1.0f, 1.0f, 1.0f };
        float Intensity = 10.0f;
        float Range = 15.0f;
        float InnerAngle = 20.0f; // degrees
        float OuterAngle = 35.0f; // degrees
        float Radius = 0.05f;         // Light source radius in meters (UE5 default: 0.05m = 5cm bulb)
        uint32_t ShadowSamples = 1;

        SpotLightComponent() = default;
        SpotLightComponent(const SpotLightComponent&) = default;
    };

    // Image-based environment lighting. The renderer will preprocess TexturePath into its
    // diffuse irradiance/specular IBL maps; this is deliberately separate from punctual
    // directional/point/spot lights.
    struct EnvironmentLightComponent
    {
        std::string TexturePath;
        glm::vec3 RadianceScale{ 1.0f, 1.0f, 1.0f };
        float Rotation = 0.0f; // radians around the world Y axis
        bool Enabled = true;

        EnvironmentLightComponent() = default;
        EnvironmentLightComponent(const EnvironmentLightComponent&) = default;
    };
    
    // Holds runtime animation state (tracks current time, playing animation, bone matrices)
    struct AnimatorComponent
    {
        Animator Animator;
        AssetHandle Animation = 0;
        AssetHandle Skeleton = 0;
        // Entity for every source glTF node, indexed by glTF node index. Stored as UUIDs, not entt
        // handles, so the mapping survives scene save/load and the Play-mode scene copy.
        //   Animation set -> the clip drives these node entities' transforms (glTF node animation).
        //   Skeleton set  -> this entity's skin reads its joints' world transforms from these
        //                    entities (glTF skinning: inverse(meshWorld) * jointWorld * inverseBind).
        // Empty means the legacy self-contained skeleton evaluation.
        std::vector<UUID> NodeEntities;
        bool Playing = true;

        // Runtime only: skinning matrices rebuilt from the joint entities each frame.
        std::vector<glm::mat4> SkinMatrices;

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
        bool AutoExposure = false;
        float ExposureCompensation = 0.0f;
        float AutoExposureMinEV = -16.0f;
        float AutoExposureMaxEV = 16.0f;

        CameraComponent() = default;
        CameraComponent(const CameraComponent&) = default;
    };
    
    struct ScriptComponent
    {
        struct EntityReference
        {
            UUID Entity = 0;
            UUID ModelInstance = 0;
            uint32_t ModelNodeIndex = 0;

            bool IsModelNode() const { return ModelInstance != 0; }
        };

        std::vector<std::string> ClassNames;
        std::unordered_map<std::string, std::unordered_map<std::string, EntityReference>> EntityReferences;
        std::unordered_map<std::string, std::unordered_map<std::string, ScriptValue>> FieldOverrides;
        
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

    // 3D Physics

    struct RigidBody3DComponent
    {
        enum class BodyType { Static = 0, Dynamic, Kinematic };
        enum class MotionQuality { Discrete = 0, LinearCast };

        BodyType Type = BodyType::Dynamic;
        MotionQuality Quality = MotionQuality::Discrete;

        float Mass = 1.0f;
        float LinearDamping = 0.05f;
        float AngularDamping = 0.05f;
        float GravityFactor = 1.0f;

        bool AllowSleeping = true;
        bool IsSensor = false;
        uint16_t Layer = 1; // Default to MOVING

        // Storage for runtime (invalid in editor mode)
        uint32_t RuntimeBodyID = 0xFFFFFFFF; // JPH::BodyID::cInvalidBodyID

        RigidBody3DComponent() = default;
        RigidBody3DComponent(const RigidBody3DComponent&) = default;
    };

    struct BoxCollider3DComponent
    {
        glm::vec3 HalfExtents = { 0.5f, 0.5f, 0.5f };
        glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };

        float Friction = 0.5f;
        float Restitution = 0.0f;

        BoxCollider3DComponent() = default;
        BoxCollider3DComponent(const BoxCollider3DComponent&) = default;
    };

    struct SphereCollider3DComponent
    {
        float Radius = 0.5f;
        glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };

        float Friction = 0.5f;
        float Restitution = 0.0f;

        SphereCollider3DComponent() = default;
        SphereCollider3DComponent(const SphereCollider3DComponent&) = default;
    };

    struct CapsuleCollider3DComponent
    {
        float HalfHeight = 0.5f;
        float Radius = 0.5f;
        glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };

        float Friction = 0.5f;
        float Restitution = 0.0f;

        CapsuleCollider3DComponent() = default;
        CapsuleCollider3DComponent(const CapsuleCollider3DComponent&) = default;
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
        MeshComponent, MaterialComponent, ModelInstanceComponent, ModelNodeComponent, DirectionalLightComponent, PointLightComponent, SpotLightComponent, EnvironmentLightComponent, AnimatorComponent,
        SpriteRendererComponent,
            CircleRendererComponent, CameraComponent, ScriptComponent,
            /*NativeScriptComponent,*/ RigidBody2DComponent, BoxCollider2DComponent,
            CircleCollider2DComponent, RigidBody3DComponent, BoxCollider3DComponent,
            SphereCollider3DComponent, CapsuleCollider3DComponent, TextComponent>;
}
