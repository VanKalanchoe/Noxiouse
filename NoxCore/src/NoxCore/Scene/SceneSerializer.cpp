#include "SceneSerializer.h"


#include <fstream>

#include <yaml-cpp/yaml.h>

/*#include "VanK/Scripting/ScriptEngine.h"*/
#include "NoxCore/Core/core.h"
#include "NoxCore/Core/Log.h"
#include <iostream>

#include "NoxCore/Core/UUID.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/ModelInstance.h"
#include "NoxCore/Renderer/Mesh.h"

namespace YAML
{
    template <>
    struct convert<glm::vec2>
    {
        static Node encode(const glm::vec2& rhs)
        {
            Node node;
            node.push_back(rhs.x);
            node.push_back(rhs.y);
            node.SetStyle(EmitterStyle::Flow);
            return node;
        }

        static bool decode(const Node& node, glm::vec2& rhs)
        {
            if (!node.IsSequence() || node.size() != 2)
                return false;

            rhs.x = node[0].as<float>();
            rhs.y = node[1].as<float>();
            return true;
        }
    };

    template <>
    struct convert<glm::vec3>
    {
        static Node encode(const glm::vec3& rhs)
        {
            Node node;
            node.push_back(rhs.x);
            node.push_back(rhs.y);
            node.push_back(rhs.z);
            node.SetStyle(EmitterStyle::Flow);
            return node;
        }

        static bool decode(const Node& node, glm::vec3& rhs)
        {
            if (!node.IsSequence() || node.size() != 3)
                return false;

            rhs.x = node[0].as<float>();
            rhs.y = node[1].as<float>();
            rhs.z = node[2].as<float>();
            return true;
        }
    };

    template <>
    struct convert<glm::vec4>
    {
        static Node encode(const glm::vec4& rhs)
        {
            Node node;
            node.push_back(rhs.x);
            node.push_back(rhs.y);
            node.push_back(rhs.z);
            node.push_back(rhs.w);
            node.SetStyle(EmitterStyle::Flow);
            return node;
        }

        static bool decode(const Node& node, glm::vec4& rhs)
        {
            if (!node.IsSequence() || node.size() != 4)
                return false;

            rhs.x = node[0].as<float>();
            rhs.y = node[1].as<float>();
            rhs.z = node[2].as<float>();
            rhs.w = node[3].as<float>();
            return true;
        }
    };

    template <>
    struct convert<Nox::UUID>
    {
        static Node encode(const Nox::UUID& uuid)
        {
            Node node;
            node.push_back((uint64_t)uuid);

            node.SetStyle(EmitterStyle::Flow);
            return node;
        }

        static bool decode(const Node& node, Nox::UUID& uuid)
        {
            uuid = node.as<uint64_t>();
            return true;
        }
    };
}

namespace Nox
{
#define WRITE_SCRIPT_FIELD(FieldType, Type) \
                case ScriptFieldType::FieldType:\
                    out << scriptField.GetValue<Type>();\
                    break

#define READ_SCRIPT_FIELD(FieldType, Type) \
    case ScriptFieldType::FieldType: \
    {\
        Type data = scriptField["Data"].as<Type>();\
        fieldInstance.SetValue(data);\
        break;\
    }

    YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec2& v)
    {
        out << YAML::Flow;
        out << YAML::BeginSeq << v.x << v.y << YAML::EndSeq;
        return out;
    }

    YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec3& v)
    {
        out << YAML::Flow;
        out << YAML::BeginSeq << v.x << v.y << v.z << YAML::EndSeq;
        return out;
    }

    YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec4& v)
    {
        out << YAML::Flow;
        out << YAML::BeginSeq << v.x << v.y << v.z << v.w << YAML::EndSeq;
        return out;
    }

    static std::string Rigidbody2DTypeToString(RigidBody2DComponent::BodyType bodyType)
    {
        switch (bodyType)
        {
        case RigidBody2DComponent::BodyType::Static: return "Static";
        case RigidBody2DComponent::BodyType::Dynamic: return "Dynamic";
        case RigidBody2DComponent::BodyType::Kinematic: return "Kinematic";
        }

        NOX_CORE_ASSERT(false, "Unknown RigidBody2DComponent::BodyType!");
        return {};
    }

    static RigidBody2DComponent::BodyType Rigidbody2DTypeTFromString(const std::string& bodyTypeString)
    {
        if (bodyTypeString == "Static") return RigidBody2DComponent::BodyType::Static;
        if (bodyTypeString == "Dynamic") return RigidBody2DComponent::BodyType::Dynamic;
        if (bodyTypeString == "Kinematic") return RigidBody2DComponent::BodyType::Kinematic;

        NOX_CORE_ASSERT(false, "Unknown RigidBody2DComponent::BodyType!");
        return RigidBody2DComponent::BodyType::Static;
    }

    static std::string_view Rigidbody3DTypeToString(RigidBody3DComponent::BodyType bodyType)
    {
        switch (bodyType)
        {
        case RigidBody3DComponent::BodyType::Static: return "Static";
        case RigidBody3DComponent::BodyType::Dynamic: return "Dynamic";
        case RigidBody3DComponent::BodyType::Kinematic: return "Kinematic";
        }
        return "Static";
    }

    static RigidBody3DComponent::BodyType Rigidbody3DTypeFromString(const std::string& bodyTypeString)
    {
        if (bodyTypeString == "Static") return RigidBody3DComponent::BodyType::Static;
        if (bodyTypeString == "Dynamic") return RigidBody3DComponent::BodyType::Dynamic;
        if (bodyTypeString == "Kinematic") return RigidBody3DComponent::BodyType::Kinematic;
        return RigidBody3DComponent::BodyType::Static;
    }

    static std::string_view MotionQualityToString(RigidBody3DComponent::MotionQuality quality)
    {
        switch (quality)
        {
        case RigidBody3DComponent::MotionQuality::Discrete: return "Discrete";
        case RigidBody3DComponent::MotionQuality::LinearCast: return "LinearCast";
        }
        return "Discrete";
    }

    static RigidBody3DComponent::MotionQuality MotionQualityFromString(const std::string& qualityString)
    {
        if (qualityString == "LinearCast") return RigidBody3DComponent::MotionQuality::LinearCast;
        return RigidBody3DComponent::MotionQuality::Discrete;
    }

    SceneSerializer::SceneSerializer(const Ref<Scene>& scene) : m_Scene(scene)
    {
    }

    // A node spawned by a model instance that still exists: saved through the instance (overrides), never itself.
    static bool IsSpawnedModelNode(Scene& scene, UUID id)
    {
        Entity entity = scene.GetEntityByUUID(id);
        if (!entity || !entity.HasComponent<ModelNodeComponent>())
            return false;
        Entity root = scene.GetEntityByUUID(entity.GetComponent<ModelNodeComponent>().Instance);
        return root && root.HasComponent<ModelInstanceComponent>();
    }

    static void SerializeEntity(YAML::Emitter& out, Scene& scene, Entity entity)
    {
        NOX_CORE_ASSERT(entity.HasComponent<IDComponent>(), "Entity does not have an ID component!");

        out << YAML::BeginMap; // Corrected: No parentheses
        out << YAML::Key << "Entity" << YAML::Value << entity.GetUUID(); // guid

        if (entity.HasComponent<TagComponent>())
        {
            out << YAML::Key << "TagComponent";
            out << YAML::BeginMap;

            auto& tag = entity.GetComponent<TagComponent>().Tag;
            out << YAML::Key << "Tag" << YAML::Value << tag;

            out << YAML::EndMap;
        }

        if (entity.HasComponent<TransformComponent>())
        {
            out << YAML::Key << "TransformComponent";
            out << YAML::BeginMap; // TransformComponent

            auto& tc = entity.GetComponent<TransformComponent>();
            out << YAML::Key << "Translation" << YAML::Value << tc.Translation;
            out << YAML::Key << "Rotation" << YAML::Value << tc.Rotation;
            out << YAML::Key << "Scale" << YAML::Value << tc.Scale;

            out << YAML::EndMap; // TransformComponent
        }

        if (entity.HasComponent<RelationshipComponent>())
        {
            out << YAML::Key << "RelationshipComponent";
            out << YAML::BeginMap; // RelationshipComponent

            auto& relationship = entity.GetComponent<RelationshipComponent>();
            out << YAML::Key << "Parent" << YAML::Value << (uint64_t)relationship.Parent;
            out << YAML::Key << "Children" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto childID : relationship.Children)
            {
                if (!IsSpawnedModelNode(scene, childID))
                    out << childID;
            }
            out << YAML::EndSeq;

            out << YAML::EndMap; // RelationshipComponent
        }

        if (entity.HasComponent<MeshComponent>())
        {
            out << YAML::Key << "MeshComponent";
            out << YAML::BeginMap; // MeshComponent

            auto& meshComponent = entity.GetComponent<MeshComponent>();
            out << YAML::Key << "MeshHandle" << YAML::Value << meshComponent.Mesh;
            out << YAML::Key << "SubmeshIndex" << YAML::Value << meshComponent.SubmeshIndex;
            out << YAML::Key << "SubmeshCount" << YAML::Value << meshComponent.SubmeshCount;

            out << YAML::EndMap; // MeshComponent
        }

        if (entity.HasComponent<ModelInstanceComponent>())
        {
            out << YAML::Key << "ModelInstanceComponent";
            out << YAML::BeginMap;

            const auto& instance = entity.GetComponent<ModelInstanceComponent>();
            out << YAML::Key << "Model" << YAML::Value << static_cast<uint64_t>(instance.Model);
            if (!instance.RemovedNodes.empty())
            {
                out << YAML::Key << "RemovedNodes" << YAML::Value << YAML::Flow << YAML::BeginSeq;
                for (uint32_t node : instance.RemovedNodes)
                    out << node;
                out << YAML::EndSeq;
            }

            const std::vector<ModelNodeOverride> overrides = ModelInstance::CollectOverrides(scene, entity);
            if (!overrides.empty())
            {
                out << YAML::Key << "Overrides" << YAML::Value << YAML::BeginSeq;
                for (const ModelNodeOverride& nodeOverride : overrides)
                {
                    out << YAML::BeginMap;
                    out << YAML::Key << "Node" << YAML::Value << nodeOverride.NodeIndex;
                    if (nodeOverride.Name)
                        out << YAML::Key << "Name" << YAML::Value << *nodeOverride.Name;
                    if (nodeOverride.Transform)
                    {
                        out << YAML::Key << "Translation" << YAML::Value << nodeOverride.Transform->Translation;
                        out << YAML::Key << "Rotation" << YAML::Value << nodeOverride.Transform->Rotation;
                        out << YAML::Key << "Scale" << YAML::Value << nodeOverride.Transform->Scale;
                    }
                    if (!nodeOverride.MaterialAssets.empty())
                    {
                        out << YAML::Key << "MaterialAssets" << YAML::Value << YAML::Flow << YAML::BeginSeq;
                        for (AssetHandle handle : nodeOverride.MaterialAssets)
                            out << static_cast<uint64_t>(handle);
                        out << YAML::EndSeq;
                    }
                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;
            }

            out << YAML::EndMap;
        }

        if (entity.HasComponent<MaterialComponent>())
        {
            out << YAML::Key << "MaterialComponent";
            out << YAML::BeginMap; // MaterialComponent

            auto& mc = entity.GetComponent<MaterialComponent>();

            // Overrides only (a zero entry keeps the mesh's material); none, and the key is left out.
            if (!mc.MaterialAssets.empty())
            {
                out << YAML::Key << "MaterialAssets" << YAML::Value << YAML::BeginSeq;
                for (auto handle : mc.MaterialAssets) out << (uint64_t)handle;
                out << YAML::EndSeq;
            }


            out << YAML::EndMap; // MaterialComponent
        }

        if (entity.HasComponent<DirectionalLightComponent>())
        {
            out << YAML::Key << "DirectionalLightComponent";
            out << YAML::BeginMap;

            auto& dlc = entity.GetComponent<DirectionalLightComponent>();
            out << YAML::Key << "Color" << YAML::Value << dlc.Color;
            out << YAML::Key << "Intensity" << YAML::Value << dlc.Intensity;
            out << YAML::Key << "AngularDiameter" << YAML::Value << dlc.AngularDiameter;
            out << YAML::Key << "ShadowSamples" << YAML::Value << dlc.ShadowSamples;

            out << YAML::EndMap;
        }

        if (entity.HasComponent<PointLightComponent>())
        {
            out << YAML::Key << "PointLightComponent";
            out << YAML::BeginMap;

            auto& plc = entity.GetComponent<PointLightComponent>();
            out << YAML::Key << "Color" << YAML::Value << plc.Color;
            out << YAML::Key << "Intensity" << YAML::Value << plc.Intensity;
            out << YAML::Key << "Range" << YAML::Value << plc.Range;
            out << YAML::Key << "Radius" << YAML::Value << plc.Radius;
            out << YAML::Key << "ShadowSamples" << YAML::Value << plc.ShadowSamples;

            out << YAML::EndMap;
        }

        if (entity.HasComponent<SpotLightComponent>())
        {
            out << YAML::Key << "SpotLightComponent";
            out << YAML::BeginMap;

            auto& slc = entity.GetComponent<SpotLightComponent>();
            out << YAML::Key << "Color" << YAML::Value << slc.Color;
            out << YAML::Key << "Intensity" << YAML::Value << slc.Intensity;
            out << YAML::Key << "Range" << YAML::Value << slc.Range;
            out << YAML::Key << "InnerAngle" << YAML::Value << slc.InnerAngle;
            out << YAML::Key << "OuterAngle" << YAML::Value << slc.OuterAngle;
            out << YAML::Key << "Radius" << YAML::Value << slc.Radius;
            out << YAML::Key << "ShadowSamples" << YAML::Value << slc.ShadowSamples;

            out << YAML::EndMap;
        }

        if (entity.HasComponent<EnvironmentLightComponent>())
        {
            out << YAML::Key << "EnvironmentLightComponent";
            out << YAML::BeginMap;

            auto& elc = entity.GetComponent<EnvironmentLightComponent>();
            out << YAML::Key << "TexturePath" << YAML::Value << elc.TexturePath;
            out << YAML::Key << "RadianceScale" << YAML::Value << elc.RadianceScale;
            out << YAML::Key << "Rotation" << YAML::Value << elc.Rotation;
            out << YAML::Key << "Enabled" << YAML::Value << elc.Enabled;

            out << YAML::EndMap;
        }

        if (entity.HasComponent<AnimatorComponent>())
        {
            out << YAML::Key << "AnimatorComponent";
            out << YAML::BeginMap; // AnimatorComponent

            auto& animatorComponent = entity.GetComponent<AnimatorComponent>();
            out << YAML::Key << "Animation" << YAML::Value << animatorComponent.Animation;
            out << YAML::Key << "Skeleton" << YAML::Value << animatorComponent.Skeleton;
            out << YAML::Key << "Playing" << YAML::Value << animatorComponent.Playing;
            out << YAML::Key << "Looping" << YAML::Value << animatorComponent.Animator.IsLooping();
            out << YAML::Key << "PlaybackSpeed" << YAML::Value << animatorComponent.Animator.GetPlaybackSpeed();
            out << YAML::Key << "NodeEntities" << YAML::Value << YAML::BeginSeq;
            for (UUID nodeID : animatorComponent.NodeEntities)
                out << (uint64_t)nodeID;
            out << YAML::EndSeq;

            out << YAML::EndMap; // AnimatorComponent
        }

        if (entity.HasComponent<CameraComponent>())
        {
            out << YAML::Key << "CameraComponent";
            out << YAML::BeginMap; // CameraComponent

            auto& cameraComponent = entity.GetComponent<CameraComponent>();
            auto& camera = cameraComponent.Camera;

            out << YAML::Key << "Camera" << YAML::Value;
            out << YAML::BeginMap; // Camera
            out << YAML::Key << "ProjectionType" << YAML::Value << (int)camera.GetProjectionType();
            out << YAML::Key << "PerspectiveFOV" << YAML::Value << camera.GetPerspectiveVerticalFOV();
            out << YAML::Key << "PerspectiveNear" << YAML::Value << camera.GetPerspectiveNearClip();
            out << YAML::Key << "PerspectiveFar" << YAML::Value << camera.GetPerspectiveFarClip();
            out << YAML::Key << "OrthographicSize" << YAML::Value << camera.GetOrthographicSize();
            out << YAML::Key << "OrthographicNear" << YAML::Value << camera.GetOrthographicNearClip();
            out << YAML::Key << "OrthographicFar" << YAML::Value << camera.GetOrthographicFarClip();
            out << YAML::EndMap; // Camera

            out << YAML::Key << "Primary" << YAML::Value << cameraComponent.Primary;
            out << YAML::Key << "FixedAspectRatio" << YAML::Value << cameraComponent.FixedAspectRatio;
            out << YAML::Key << "AutoExposure" << YAML::Value << cameraComponent.AutoExposure;
            out << YAML::Key << "ExposureCompensation" << YAML::Value << cameraComponent.ExposureCompensation;
            out << YAML::Key << "AutoExposureMinEV" << YAML::Value << cameraComponent.AutoExposureMinEV;
            out << YAML::Key << "AutoExposureMaxEV" << YAML::Value << cameraComponent.AutoExposureMaxEV;

            out << YAML::EndMap; // CameraComponent
        }

        /*if (entity.HasComponent<ScriptComponent>())
        {
            auto& scriptComponent = entity.GetComponent<ScriptComponent>();

            out << YAML::Key << "ScriptComponent";
            out << YAML::BeginMap; // ScriptComponent
            out << YAML::Key << "ClassName" << YAML::Value << scriptComponent.ClassName;

            // Fields
            Ref<ScriptClass> entityClass = ScriptEngine::GetEntityClass(scriptComponent.ClassName);
            const auto& fields = entityClass->GetFields();
            if (fields.size() > 0)
            {
                out << YAML::Key << "ScriptFields" << YAML::Value;
                auto& entityFields = ScriptEngine::GetScriptFieldMap(entity);
                out << YAML::BeginSeq;
                for (const auto& [name, field] : fields)
                {
                    if (entityFields.find(name) == entityFields.end())
                        continue;

                    out << YAML::BeginMap; // ScriptField
                    out << YAML::Key << "Name" << YAML::Value << name;
                    out << YAML::Key << "Type" << YAML::Value << Utils::ScriptFieldTypeToString(field.Type);

                    out << YAML::Key << "Data" << YAML::Value;
                    ScriptFieldInstance& scriptField = entityFields.at(name);

                    switch (field.Type)
                    {
                        WRITE_SCRIPT_FIELD(Float,   float     );
                        WRITE_SCRIPT_FIELD(Double,  double    );
                        WRITE_SCRIPT_FIELD(Bool,    bool      );
                        WRITE_SCRIPT_FIELD(Char,    char      );
                        WRITE_SCRIPT_FIELD(Byte,    int8_t    );
                        WRITE_SCRIPT_FIELD(Short,   int16_t   );
                        WRITE_SCRIPT_FIELD(Int,     int32_t   );
                        WRITE_SCRIPT_FIELD(Long,    int64_t   );
                        WRITE_SCRIPT_FIELD(UByte,   uint8_t   );
                        WRITE_SCRIPT_FIELD(UShort,  uint16_t  );
                        WRITE_SCRIPT_FIELD(UInt,    uint32_t  );
                        WRITE_SCRIPT_FIELD(ULong,   uint64_t  );
                        WRITE_SCRIPT_FIELD(Vector2, glm::vec2 );
                        WRITE_SCRIPT_FIELD(Vector3, glm::vec3 );
                        WRITE_SCRIPT_FIELD(Vector4, glm::vec4 );
                        WRITE_SCRIPT_FIELD(Entity,  UUID      );
                    }
                    out << YAML::EndMap; // ScriptFields
                }
                out << YAML::EndSeq;
            }

            out << YAML::EndMap; // ScriptComponent
        }*/

        if (entity.HasComponent<SpriteRendererComponent>())
        {
            out << YAML::Key << "SpriteRendererComponent";
            out << YAML::BeginMap; // SpriteRendererComponent

            auto& spriteRendererComponent = entity.GetComponent<SpriteRendererComponent>();
            out << YAML::Key << "Color" << YAML::Value << spriteRendererComponent.Color;
            out << YAML::Key << "TextureHandle" << YAML::Value << spriteRendererComponent.Texture;

            out << YAML::Key << "TilingFactor" << YAML::Value << spriteRendererComponent.TilingFactor;

            out << YAML::EndMap; // SpriteRendererComponent
        }

        if (entity.HasComponent<CircleRendererComponent>())
        {
            out << YAML::Key << "CircleRendererComponent";
            out << YAML::BeginMap; // CircleRendererComponent

            auto& circleRendererComponent = entity.GetComponent<CircleRendererComponent>();
            out << YAML::Key << "Color" << YAML::Value << circleRendererComponent.Color;
            out << YAML::Key << "Thickness" << YAML::Value << circleRendererComponent.Thickness;
            out << YAML::Key << "Fade" << YAML::Value << circleRendererComponent.Fade;

            out << YAML::EndMap; // CircleRendererComponent
        }

        if (entity.HasComponent<RigidBody2DComponent>())
        {
            out << YAML::Key << "RigidBody2DComponent";
            out << YAML::BeginMap; // RigidBody2DComponent

            auto& rb2dComponent = entity.GetComponent<RigidBody2DComponent>();
            out << YAML::Key << "BodyType" << YAML::Value << Rigidbody2DTypeToString(rb2dComponent.Type);
            out << YAML::Key << "FixedRotation" << YAML::Value << rb2dComponent.FixedRotation;

            out << YAML::EndMap; // RigidBody2DComponent
        }

        if (entity.HasComponent<BoxCollider2DComponent>())
        {
            out << YAML::Key << "BoxCollider2DComponent";
            out << YAML::BeginMap; // BoxCollider2DComponent

            auto& bc2dComponent = entity.GetComponent<BoxCollider2DComponent>();
            out << YAML::Key << "Offset" << YAML::Value << bc2dComponent.Offset;
            out << YAML::Key << "Size" << YAML::Value << bc2dComponent.Size;
            out << YAML::Key << "Density" << YAML::Value << bc2dComponent.Density;
            out << YAML::Key << "Friction" << YAML::Value << bc2dComponent.Friction;
            out << YAML::Key << "Restitution" << YAML::Value << bc2dComponent.Restitution;
            out << YAML::Key << "RestitutionThreshold" << YAML::Value << bc2dComponent.RestitutionThreshold;

            out << YAML::EndMap; // BoxCollider2DComponent
        }

        if (entity.HasComponent<CircleCollider2DComponent>())
        {
            out << YAML::Key << "CircleCollider2DComponent";
            out << YAML::BeginMap; // CircleCollider2DComponent

            auto& cc2dComponent = entity.GetComponent<CircleCollider2DComponent>();
            out << YAML::Key << "Offset" << YAML::Value << cc2dComponent.Offset;
            out << YAML::Key << "Radius" << YAML::Value << cc2dComponent.Radius;
            out << YAML::Key << "Density" << YAML::Value << cc2dComponent.Density;
            out << YAML::Key << "Friction" << YAML::Value << cc2dComponent.Friction;
            out << YAML::Key << "Restitution" << YAML::Value << cc2dComponent.Restitution;
            out << YAML::Key << "RestitutionThreshold" << YAML::Value << cc2dComponent.RestitutionThreshold;

            out << YAML::EndMap; // CircleCollider2DComponent
        }

        if (entity.HasComponent<RigidBody3DComponent>())
        {
            out << YAML::Key << "RigidBody3DComponent";
            out << YAML::BeginMap; // RigidBody3DComponent

            auto& rb3dComponent = entity.GetComponent<RigidBody3DComponent>();
            out << YAML::Key << "BodyType" << YAML::Value << std::string(Rigidbody3DTypeToString(rb3dComponent.Type));
            out << YAML::Key << "MotionQuality" << YAML::Value << std::string(MotionQualityToString(rb3dComponent.Quality));
            out << YAML::Key << "Mass" << YAML::Value << rb3dComponent.Mass;
            out << YAML::Key << "LinearDamping" << YAML::Value << rb3dComponent.LinearDamping;
            out << YAML::Key << "AngularDamping" << YAML::Value << rb3dComponent.AngularDamping;
            out << YAML::Key << "GravityFactor" << YAML::Value << rb3dComponent.GravityFactor;
            out << YAML::Key << "AllowSleeping" << YAML::Value << rb3dComponent.AllowSleeping;
            out << YAML::Key << "IsSensor" << YAML::Value << rb3dComponent.IsSensor;
            out << YAML::Key << "Layer" << YAML::Value << rb3dComponent.Layer;

            out << YAML::EndMap; // RigidBody3DComponent
        }

        if (entity.HasComponent<BoxCollider3DComponent>())
        {
            out << YAML::Key << "BoxCollider3DComponent";
            out << YAML::BeginMap; // BoxCollider3DComponent

            auto& bc3d = entity.GetComponent<BoxCollider3DComponent>();
            out << YAML::Key << "HalfExtents" << YAML::Value << bc3d.HalfExtents;
            out << YAML::Key << "Offset" << YAML::Value << bc3d.Offset;
            out << YAML::Key << "Friction" << YAML::Value << bc3d.Friction;
            out << YAML::Key << "Restitution" << YAML::Value << bc3d.Restitution;

            out << YAML::EndMap; // BoxCollider3DComponent
        }

        if (entity.HasComponent<SphereCollider3DComponent>())
        {
            out << YAML::Key << "SphereCollider3DComponent";
            out << YAML::BeginMap; // SphereCollider3DComponent

            auto& sc3d = entity.GetComponent<SphereCollider3DComponent>();
            out << YAML::Key << "Radius" << YAML::Value << sc3d.Radius;
            out << YAML::Key << "Offset" << YAML::Value << sc3d.Offset;
            out << YAML::Key << "Friction" << YAML::Value << sc3d.Friction;
            out << YAML::Key << "Restitution" << YAML::Value << sc3d.Restitution;

            out << YAML::EndMap; // SphereCollider3DComponent
        }

        if (entity.HasComponent<CapsuleCollider3DComponent>())
        {
            out << YAML::Key << "CapsuleCollider3DComponent";
            out << YAML::BeginMap; // CapsuleCollider3DComponent

            auto& cc3d = entity.GetComponent<CapsuleCollider3DComponent>();
            out << YAML::Key << "HalfHeight" << YAML::Value << cc3d.HalfHeight;
            out << YAML::Key << "Radius" << YAML::Value << cc3d.Radius;
            out << YAML::Key << "Offset" << YAML::Value << cc3d.Offset;
            out << YAML::Key << "Friction" << YAML::Value << cc3d.Friction;
            out << YAML::Key << "Restitution" << YAML::Value << cc3d.Restitution;

            out << YAML::EndMap; // CapsuleCollider3DComponent
        }

        if (entity.HasComponent<TextComponent>())
        {
            out << YAML::Key << "TextComponent";
            out << YAML::BeginMap; // TextComponent

            auto& textComponent = entity.GetComponent<TextComponent>();
            out << YAML::Key << "TextString" << YAML::Value << textComponent.TextString;
            // todo: textComponent.FontAsset;
            out << YAML::Key << "Color" << YAML::Value << textComponent.Color;
            out << YAML::Key << "Kerning" << YAML::Value << textComponent.Kerning;
            out << YAML::Key << "LineSpacing" << YAML::Value << textComponent.LineSpacing;

            out << YAML::EndMap; // TextComponent
        }

        out << YAML::EndMap; // Corrected: No parentheses
    }

    void SceneSerializer::Serialize(const std::filesystem::path& filepath)
    {
        YAML::Emitter out;
        out << YAML::BeginMap; // Corrected: No parentheses
        out << YAML::Key << "Scene" << YAML::Value << "Untitled";
        out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq; // Corrected: No parentheses
        m_Scene->m_Registry.view<TagComponent>().each([&](auto entityID, TagComponent&)
        {
            Entity entity(entityID, m_Scene.get());
            if (!entity || IsSpawnedModelNode(*m_Scene, entity.GetUUID()))
                return;

            SerializeEntity(out, *m_Scene, entity);
        });
        out << YAML::EndSeq; // Corrected: No parentheses
        out << YAML::EndMap; // Corrected: No parentheses

        std::filesystem::path file_path(filepath);
        std::cout << file_path << std::endl;
        std::ofstream fout(file_path);
        if (!fout)
        {
            std::cerr << "Failed to open file: " << file_path << std::endl;
            return; // Return or handle the error
        }

        fout << out.c_str();
    }

    void SceneSerializer::SerializeRuntime(const std::filesystem::path& filepath)
    {
        // Not Implemented
        NOX_CORE_ASSERT(false, "SerializeRuntime");
    }

    bool SceneSerializer::Deserialize(const std::filesystem::path& filepath)
    {
        /*NOX_CORE_ERROR("{0}", filepath);
        std::ifstream stream(filepath);
        std::stringstream strStream;
        strStream << stream.rdbuf();*/

        YAML::Node data;
        try
        {
            data = YAML::LoadFile(filepath.string());
        }
        catch (YAML::ParserException& e)
        {
            NOX_CORE_ERROR("Failed to load .nox file `{0}`\n       {1}", filepath.string(), e.what());
            return false;
        }

        if (!data["Scene"])
            return false;

        std::string sceneName = data["Scene"].as<std::string>();
        NOX_CORE_TRACE("Deserializing scene '{0}'", sceneName);

        auto entities = data["Entities"];
        if (entities)
        {
            for (auto entity : entities)
            {
                uint64_t uuid = entity["Entity"].as<uint64_t>();

                std::string name;
                auto tagComponent = entity["TagComponent"];
                if (tagComponent)
                    name = tagComponent["Tag"].as<std::string>();

                Entity deserializedEntity = m_Scene->CreateEntityWithUUID(uuid, name);

                auto transformComponent = entity["TransformComponent"];
                if (transformComponent)
                {
                    // Entities always have transforms
                    auto& tc = deserializedEntity.GetComponent<TransformComponent>();
                    tc.Translation = transformComponent["Translation"].as<glm::vec3>();
                    tc.Rotation = transformComponent["Rotation"].as<glm::vec3>();
                    tc.Scale = transformComponent["Scale"].as<glm::vec3>();
                }

                auto relationshipComponent = entity["RelationshipComponent"];
                if (relationshipComponent)
                {
                    auto& rc = deserializedEntity.AddComponent<RelationshipComponent>();
                    rc.Parent = relationshipComponent["Parent"].as<uint64_t>(); // Or uses UUID converter if registered

                    auto childrenSeq = relationshipComponent["Children"];
                    if (childrenSeq)
                    {
                        for (auto child : childrenSeq)
                        {
                            rc.Children.push_back(child.as<uint64_t>());
                        }
                    }
                }

                auto meshComponent = entity["MeshComponent"];
                if (meshComponent)
                {
                    auto& mc = deserializedEntity.AddComponent<MeshComponent>();
                    if (meshComponent["MeshHandle"])
                        mc.Mesh = meshComponent["MeshHandle"].as<AssetHandle>();
                    if (meshComponent["SubmeshIndex"])
                        mc.SubmeshIndex = meshComponent["SubmeshIndex"].as<uint32_t>();
                    if (meshComponent["SubmeshCount"])
                        mc.SubmeshCount = meshComponent["SubmeshCount"].as<uint32_t>();
                }

                if (auto modelInstance = entity["ModelInstanceComponent"])
                {
                    auto& instance = deserializedEntity.AddComponent<ModelInstanceComponent>();
                    instance.Model = modelInstance["Model"].as<uint64_t>();
                    if (auto removedNodes = modelInstance["RemovedNodes"])
                    {
                        for (auto node : removedNodes)
                            instance.RemovedNodes.push_back(node.as<uint32_t>());
                    }
                    if (auto overrides = modelInstance["Overrides"])
                    {
                        for (auto node : overrides)
                        {
                            ModelNodeOverride& nodeOverride = instance.Overrides.emplace_back();
                            nodeOverride.NodeIndex = node["Node"].as<uint32_t>();
                            if (node["Name"])
                                nodeOverride.Name = node["Name"].as<std::string>();
                            if (node["Translation"])
                            {
                                TransformComponent transform;
                                transform.Translation = node["Translation"].as<glm::vec3>();
                                transform.Rotation = node["Rotation"].as<glm::vec3>();
                                transform.Scale = node["Scale"].as<glm::vec3>();
                                nodeOverride.Transform = transform;
                            }
                            if (auto materials = node["MaterialAssets"])
                            {
                                for (auto material : materials)
                                    nodeOverride.MaterialAssets.push_back(material.as<uint64_t>());
                            }
                        }
                    }
                }

                auto materialComponent = entity["MaterialComponent"];
                if (materialComponent)
                {
                    auto& mc = deserializedEntity.AddComponent<MaterialComponent>();

                    auto materialAssetsSeq = materialComponent["MaterialAssets"];
                    if (materialAssetsSeq)
                    {
                        mc.MaterialAssets.clear();
                        for (auto node : materialAssetsSeq)
                            mc.MaterialAssets.push_back(node.as<uint64_t>());
                    }
                }

                auto directionalLightComponent = entity["DirectionalLightComponent"];
                if (directionalLightComponent)
                {
                    auto& dlc = deserializedEntity.AddComponent<DirectionalLightComponent>();
                    dlc.Color = directionalLightComponent["Color"].as<glm::vec3>();
                    dlc.Intensity = directionalLightComponent["Intensity"].as<float>();
                    if (directionalLightComponent["AngularDiameter"])
                        dlc.AngularDiameter = directionalLightComponent["AngularDiameter"].as<float>();
                    if (directionalLightComponent["ShadowSamples"])
                        dlc.ShadowSamples = directionalLightComponent["ShadowSamples"].as<uint32_t>();
                }

                auto pointLightComponent = entity["PointLightComponent"];
                if (pointLightComponent)
                {
                    auto& plc = deserializedEntity.AddComponent<PointLightComponent>();
                    plc.Color = pointLightComponent["Color"].as<glm::vec3>();
                    plc.Intensity = pointLightComponent["Intensity"].as<float>();
                    plc.Range = pointLightComponent["Range"].as<float>();
                    if (pointLightComponent["Radius"])
                        plc.Radius = pointLightComponent["Radius"].as<float>();
                    if (pointLightComponent["ShadowSamples"])
                        plc.ShadowSamples = pointLightComponent["ShadowSamples"].as<uint32_t>();
                }

                auto spotLightComponent = entity["SpotLightComponent"];
                if (spotLightComponent)
                {
                    auto& slc = deserializedEntity.AddComponent<SpotLightComponent>();
                    slc.Color = spotLightComponent["Color"].as<glm::vec3>();
                    slc.Intensity = spotLightComponent["Intensity"].as<float>();
                    slc.Range = spotLightComponent["Range"].as<float>();
                    slc.InnerAngle = spotLightComponent["InnerAngle"].as<float>();
                    slc.OuterAngle = spotLightComponent["OuterAngle"].as<float>();
                    if (spotLightComponent["Radius"])
                        slc.Radius = spotLightComponent["Radius"].as<float>();
                    if (spotLightComponent["ShadowSamples"])
                        slc.ShadowSamples = spotLightComponent["ShadowSamples"].as<uint32_t>();
                }

                auto environmentLightComponent = entity["EnvironmentLightComponent"];
                if (environmentLightComponent)
                {
                    auto& elc = deserializedEntity.AddComponent<EnvironmentLightComponent>();
                    if (environmentLightComponent["TexturePath"])
                        elc.TexturePath = environmentLightComponent["TexturePath"].as<std::string>();
                    if (environmentLightComponent["RadianceScale"])
                        elc.RadianceScale = environmentLightComponent["RadianceScale"].as<glm::vec3>();
                    if (environmentLightComponent["Rotation"])
                        elc.Rotation = environmentLightComponent["Rotation"].as<float>();
                    if (environmentLightComponent["Enabled"])
                        elc.Enabled = environmentLightComponent["Enabled"].as<bool>();
                }

                auto animatorComponent = entity["AnimatorComponent"];
                if (animatorComponent)
                {
                    auto& ac = deserializedEntity.AddComponent<AnimatorComponent>();
                    if (animatorComponent["Animation"])
                        ac.Animation = animatorComponent["Animation"].as<AssetHandle>();
                    if (animatorComponent["Skeleton"])
                        ac.Skeleton = animatorComponent["Skeleton"].as<AssetHandle>();
                    if (animatorComponent["Playing"])
                        ac.Playing = animatorComponent["Playing"].as<bool>();
                    if (animatorComponent["Looping"])
                        ac.Animator.SetLooping(animatorComponent["Looping"].as<bool>());
                    if (animatorComponent["PlaybackSpeed"])
                        ac.Animator.SetPlaybackSpeed(animatorComponent["PlaybackSpeed"].as<float>());
                    if (auto nodeEntities = animatorComponent["NodeEntities"])
                    {
                        for (auto nodeID : nodeEntities)
                            ac.NodeEntities.push_back(nodeID.as<uint64_t>());
                    }
                }

                auto cameraComponent = entity["CameraComponent"];
                if (cameraComponent)
                {
                    auto& cc = deserializedEntity.AddComponent<CameraComponent>();

                    YAML::Node cameraProps = cameraComponent["Camera"];

                    cc.Camera.SetProjectionType((SceneCamera::ProjectionType)cameraProps["ProjectionType"].as<int>());

                    cc.Camera.SetPerspectiveVerticalFOV(cameraProps["PerspectiveFOV"].as<float>());
                    cc.Camera.SetPerspectiveNearClip(cameraProps["PerspectiveNear"].as<float>());
                    cc.Camera.SetPerspectiveFarClip(cameraProps["PerspectiveFar"].as<float>());

                    cc.Camera.SetOrthographicSize(cameraProps["OrthographicSize"].as<float>());
                    cc.Camera.SetOrthographicNearClip(cameraProps["OrthographicNear"].as<float>());
                    cc.Camera.SetOrthographicFarClip(cameraProps["OrthographicFar"].as<float>());

                    cc.Primary = cameraComponent["Primary"].as<bool>();
                    cc.FixedAspectRatio = cameraComponent["FixedAspectRatio"].as<bool>();
                    if (cameraComponent["AutoExposure"])
                        cc.AutoExposure = cameraComponent["AutoExposure"].as<bool>();
                    if (cameraComponent["ExposureCompensation"])
                        cc.ExposureCompensation = cameraComponent["ExposureCompensation"].as<float>();
                    if (cameraComponent["AutoExposureMinEV"])
                        cc.AutoExposureMinEV = cameraComponent["AutoExposureMinEV"].as<float>();
                    if (cameraComponent["AutoExposureMaxEV"])
                        cc.AutoExposureMaxEV = cameraComponent["AutoExposureMaxEV"].as<float>();
                }

                /*auto scriptComponent = entity["ScriptComponent"];
                if (scriptComponent)
                {
                    auto& sc = deserializedEntity.AddComponent<ScriptComponent>();
                    sc.ClassName = scriptComponent["ClassName"].as<std::string>();

                    auto scriptFields = scriptComponent["ScriptFields"];
                    if (scriptFields)
                    {
                        Ref<ScriptClass> entityClass = ScriptEngine::GetEntityClass(sc.ClassName);
                        if (entityClass)
                        {
                            const auto& fields = entityClass->GetFields();
                            auto& entityFields = ScriptEngine::GetScriptFieldMap(deserializedEntity);

                            for (auto scriptField : scriptFields)
                            {
                                std::string name = scriptField["Name"].as<std::string>();
                                std::string typeString = scriptField["Type"].as<std::string>();
                                ScriptFieldType type = Utils::ScriptFieldTypeFromString(typeString);

                                ScriptFieldInstance& fieldInstance = entityFields[name];

                                // TODO(Yan): turn this assert into Hazelnut log warning
                                NOX_CORE_ASSERT(fields.find(name) != fields.end(), "field not found");

                                if (fields.find(name) == fields.end())
                                    continue;

                                fieldInstance.Field = fields.at(name);

                                switch (type)
                                {
                                    READ_SCRIPT_FIELD(Float, float);
                                    READ_SCRIPT_FIELD(Double, double);
                                    READ_SCRIPT_FIELD(Bool, bool);
                                    READ_SCRIPT_FIELD(Char, char);
                                    READ_SCRIPT_FIELD(Byte, int8_t);
                                    READ_SCRIPT_FIELD(Short, int16_t);
                                    READ_SCRIPT_FIELD(Int, int32_t);
                                    READ_SCRIPT_FIELD(Long, int64_t);
                                    READ_SCRIPT_FIELD(UByte, uint8_t);
                                    READ_SCRIPT_FIELD(UShort, uint16_t);
                                    READ_SCRIPT_FIELD(UInt, uint32_t);
                                    READ_SCRIPT_FIELD(ULong, uint64_t);
                                    READ_SCRIPT_FIELD(Vector2, glm::vec2);
                                    READ_SCRIPT_FIELD(Vector3, glm::vec3);
                                    READ_SCRIPT_FIELD(Vector4, glm::vec4);
                                    READ_SCRIPT_FIELD(Entity, UUID);
                                }
                            }
                        }
                    }

                }
                */

                auto spriteRendererComponent = entity["SpriteRendererComponent"];
                if (spriteRendererComponent)
                {
                    auto& src = deserializedEntity.AddComponent<SpriteRendererComponent>();
                    src.Color = spriteRendererComponent["Color"].as<glm::vec4>();
                    if (spriteRendererComponent["TexturePath"])
                    {
                        /*// legacy, could try and find somehting in the asset registry that matches ?
                        std::string texturePath = spriteRendererComponent["TexturePath"].as<std::string>();
                        auto path = Project::GetAssetFileSystemPath(texturePath);
                        src.Texture = Texture2D::Create(path.string(), Renderer2D::m_sampler);*/
                    }

                    if (spriteRendererComponent["TextureHandle"])
                        src.Texture = spriteRendererComponent["TextureHandle"].as<AssetHandle>();

                    if (spriteRendererComponent["TilingFactor"])
                        src.TilingFactor = spriteRendererComponent["TilingFactor"].as<float>();
                }

                auto circleRendererComponent = entity["CircleRendererComponent"];
                if (circleRendererComponent)
                {
                    auto& crc = deserializedEntity.AddComponent<CircleRendererComponent>();
                    crc.Color = circleRendererComponent["Color"].as<glm::vec4>();
                    crc.Thickness = circleRendererComponent["Thickness"].as<float>();
                    crc.Fade = circleRendererComponent["Fade"].as<float>();
                }

                auto rigidbody2DComponent = entity["RigidBody2DComponent"];
                if (rigidbody2DComponent)
                {
                    auto& r2bd = deserializedEntity.AddComponent<RigidBody2DComponent>();
                    r2bd.Type = Rigidbody2DTypeTFromString(rigidbody2DComponent["BodyType"].as<std::string>());
                    r2bd.FixedRotation = rigidbody2DComponent["FixedRotation"].as<bool>();
                }

                auto boxCollider2DComponent = entity["BoxCollider2DComponent"];
                if (boxCollider2DComponent)
                {
                    auto& bc2d = deserializedEntity.AddComponent<BoxCollider2DComponent>();
                    bc2d.Offset = boxCollider2DComponent["Offset"].as<glm::vec2>();
                    bc2d.Size = boxCollider2DComponent["Size"].as<glm::vec2>();
                    bc2d.Density = boxCollider2DComponent["Density"].as<float>();
                    bc2d.Friction = boxCollider2DComponent["Friction"].as<float>();
                    bc2d.Restitution = boxCollider2DComponent["Restitution"].as<float>();
                    bc2d.RestitutionThreshold = boxCollider2DComponent["RestitutionThreshold"].as<float>();
                }

                auto circleCollider2DComponent = entity["CircleCollider2DComponent"];
                if (circleCollider2DComponent)
                {
                    auto& cc2d = deserializedEntity.AddComponent<CircleCollider2DComponent>();
                    cc2d.Offset = circleCollider2DComponent["Offset"].as<glm::vec2>();
                    cc2d.Radius = circleCollider2DComponent["Radius"].as<float>();
                    cc2d.Density = circleCollider2DComponent["Density"].as<float>();
                    cc2d.Friction = circleCollider2DComponent["Friction"].as<float>();
                    cc2d.Restitution = circleCollider2DComponent["Restitution"].as<float>();
                    cc2d.RestitutionThreshold = circleCollider2DComponent["RestitutionThreshold"].as<float>();
                }

                auto rigidbody3DComponent = entity["RigidBody3DComponent"];
                if (rigidbody3DComponent)
                {
                    auto& rb3d = deserializedEntity.AddComponent<RigidBody3DComponent>();
                    if (rigidbody3DComponent["BodyType"])
                        rb3d.Type = Rigidbody3DTypeFromString(rigidbody3DComponent["BodyType"].as<std::string>());
                    if (rigidbody3DComponent["MotionQuality"])
                        rb3d.Quality = MotionQualityFromString(rigidbody3DComponent["MotionQuality"].as<std::string>());
                    if (rigidbody3DComponent["Mass"])
                        rb3d.Mass = rigidbody3DComponent["Mass"].as<float>();
                    if (rigidbody3DComponent["LinearDamping"])
                        rb3d.LinearDamping = rigidbody3DComponent["LinearDamping"].as<float>();
                    if (rigidbody3DComponent["AngularDamping"])
                        rb3d.AngularDamping = rigidbody3DComponent["AngularDamping"].as<float>();
                    if (rigidbody3DComponent["GravityFactor"])
                        rb3d.GravityFactor = rigidbody3DComponent["GravityFactor"].as<float>();
                    if (rigidbody3DComponent["AllowSleeping"])
                        rb3d.AllowSleeping = rigidbody3DComponent["AllowSleeping"].as<bool>();
                    if (rigidbody3DComponent["IsSensor"])
                        rb3d.IsSensor = rigidbody3DComponent["IsSensor"].as<bool>();
                    if (rigidbody3DComponent["Layer"])
                        rb3d.Layer = rigidbody3DComponent["Layer"].as<uint16_t>();
                }

                auto boxCollider3DComponent = entity["BoxCollider3DComponent"];
                if (boxCollider3DComponent)
                {
                    auto& bc3d = deserializedEntity.AddComponent<BoxCollider3DComponent>();
                    if (boxCollider3DComponent["HalfExtents"])
                        bc3d.HalfExtents = boxCollider3DComponent["HalfExtents"].as<glm::vec3>();
                    if (boxCollider3DComponent["Offset"])
                        bc3d.Offset = boxCollider3DComponent["Offset"].as<glm::vec3>();
                    if (boxCollider3DComponent["Friction"])
                        bc3d.Friction = boxCollider3DComponent["Friction"].as<float>();
                    if (boxCollider3DComponent["Restitution"])
                        bc3d.Restitution = boxCollider3DComponent["Restitution"].as<float>();
                }

                auto sphereCollider3DComponent = entity["SphereCollider3DComponent"];
                if (sphereCollider3DComponent)
                {
                    auto& sc3d = deserializedEntity.AddComponent<SphereCollider3DComponent>();
                    if (sphereCollider3DComponent["Radius"])
                        sc3d.Radius = sphereCollider3DComponent["Radius"].as<float>();
                    if (sphereCollider3DComponent["Offset"])
                        sc3d.Offset = sphereCollider3DComponent["Offset"].as<glm::vec3>();
                    if (sphereCollider3DComponent["Friction"])
                        sc3d.Friction = sphereCollider3DComponent["Friction"].as<float>();
                    if (sphereCollider3DComponent["Restitution"])
                        sc3d.Restitution = sphereCollider3DComponent["Restitution"].as<float>();
                }

                auto capsuleCollider3DComponent = entity["CapsuleCollider3DComponent"];
                if (capsuleCollider3DComponent)
                {
                    auto& cc3d = deserializedEntity.AddComponent<CapsuleCollider3DComponent>();
                    if (capsuleCollider3DComponent["HalfHeight"])
                        cc3d.HalfHeight = capsuleCollider3DComponent["HalfHeight"].as<float>();
                    if (capsuleCollider3DComponent["Radius"])
                        cc3d.Radius = capsuleCollider3DComponent["Radius"].as<float>();
                    if (capsuleCollider3DComponent["Offset"])
                        cc3d.Offset = capsuleCollider3DComponent["Offset"].as<glm::vec3>();
                    if (capsuleCollider3DComponent["Friction"])
                        cc3d.Friction = capsuleCollider3DComponent["Friction"].as<float>();
                    if (capsuleCollider3DComponent["Restitution"])
                        cc3d.Restitution = capsuleCollider3DComponent["Restitution"].as<float>();
                }

                auto textComponent = entity["TextComponent"];
                if (textComponent)
                {
                    auto& tc = deserializedEntity.AddComponent<TextComponent>();
                    tc.TextString = textComponent["TextString"].as<std::string>();
                    // tc.FontAsset // todo
                    tc.Color = textComponent["Color"].as<glm::vec4>();
                    tc.Kerning = textComponent["Kerning"].as<float>();
                    tc.LineSpacing = textComponent["LineSpacing"].as<float>();
                }
            }
        }

        return true;
    }

    bool SceneSerializer::DeserializeRuntime(const std::filesystem::path& filepath)
    {
        // Not Implemented
        NOX_CORE_ASSERT(false, "DeserializeRuntime");
        return false;
    }
}
