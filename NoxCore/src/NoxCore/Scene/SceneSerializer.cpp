#include "SceneSerializer.h"


#include <fstream>

#include <yaml-cpp/yaml.h>

/*#include "VanK/Scripting/ScriptEngine.h"*/
#include "NoxCore/Core/core.h"
#include "NoxCore/Core/Log.h"
#include <iostream>

#include "NoxCore/Core/UUID.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Components.h"

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

    SceneSerializer::SceneSerializer(const Ref<Scene>& scene) : m_Scene(scene)
    {
    }

    static void SerializeEntity(YAML::Emitter& out, Entity entity)
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
                out << childID;
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

            out << YAML::EndMap; // MeshComponent
        }

        if (entity.HasComponent<MaterialComponent>())
        {
            out << YAML::Key << "MaterialComponent";
            out << YAML::BeginMap; // MaterialComponent

            auto& mc = entity.GetComponent<MaterialComponent>();

            // Base Color
            out << YAML::Key << "BaseColorFactor" << YAML::Value;
            out << YAML::BeginSeq;
            for (const auto& color : mc.BaseColorFactors) out << color;
            out << YAML::EndSeq;

            out << YAML::Key << "BaseColorMaps" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto handle : mc.BaseColorMaps) out << (uint64_t)handle;
            out << YAML::EndSeq;

            out << YAML::Key << "BaseColorTextureSets" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto set : mc.BaseColorTextureSets) out << set;
            out << YAML::EndSeq;

            // PBR Properties
            out << YAML::Key << "MetallicFactors" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto val : mc.MetallicFactors) out << val;
            out << YAML::EndSeq;

            out << YAML::Key << "RoughnessFactors" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto val : mc.RoughnessFactors) out << val;
            out << YAML::EndSeq;

            out << YAML::Key << "MetallicRoughnessMaps" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto handle : mc.MetallicRoughnessMaps) out << (uint64_t)handle;
            out << YAML::EndSeq;

            out << YAML::Key << "PhysicalDescriptorTextureSets" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto set : mc.PhysicalDescriptorTextureSets) out << set;
            out << YAML::EndSeq;

            // Additional Maps
            out << YAML::Key << "NormalMaps" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto handle : mc.NormalMaps) out << (uint64_t)handle;
            out << YAML::EndSeq;

            out << YAML::Key << "NormalTextureSets" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto set : mc.NormalTextureSets) out << set;
            out << YAML::EndSeq;

            out << YAML::Key << "OcclusionMaps" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto handle : mc.OcclusionMaps) out << (uint64_t)handle;
            out << YAML::EndSeq;

            out << YAML::Key << "OcclusionTextureSets" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto set : mc.OcclusionTextureSets) out << set;
            out << YAML::EndSeq;

            // Emission
            out << YAML::Key << "EmissiveFactors" << YAML::Value;
            out << YAML::BeginSeq;
            for (const auto& factor : mc.EmissiveFactors) out << factor;
            out << YAML::EndSeq;

            out << YAML::Key << "EmissiveMaps" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto handle : mc.EmissiveMaps) out << (uint64_t)handle;
            out << YAML::EndSeq;

            out << YAML::Key << "EmissiveTextureSets" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto set : mc.EmissiveTextureSets) out << set;
            out << YAML::EndSeq;

            out << YAML::Key << "EmissiveStrengths" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto strength : mc.EmissiveStrengths) out << strength;
            out << YAML::EndSeq;

            // Settings
            out << YAML::Key << "Modes" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto mode : mc.Modes) out << static_cast<int>(mode);
            out << YAML::EndSeq;

            out << YAML::Key << "AlphaCutoffs" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto cutoff : mc.AlphaCutoffs) out << (float)cutoff;
            out << YAML::EndSeq;

            out << YAML::Key << "DoubleSidedFlags" << YAML::Value;
            out << YAML::BeginSeq;
            for (auto doubleSided : mc.DoubleSidedFlags) out << (bool)doubleSided;
            out << YAML::EndSeq;

            out << YAML::EndMap; // MaterialComponent
        }

        if (entity.HasComponent<AnimatorComponent>())
        {
            out << YAML::Key << "AnimatorComponent";
            out << YAML::BeginMap; // AnimatorComponent

            auto& animatorComponent = entity.GetComponent<AnimatorComponent>();
            out << YAML::Key << "Animation" << YAML::Value << animatorComponent.Animation;
            out << YAML::Key << "Skeleton" << YAML::Value << animatorComponent.Skeleton;
            out << YAML::Key << "Playing" << YAML::Value << animatorComponent.Playing;

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
            if (!entity)
                return;

            SerializeEntity(out, entity);
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

                NOX_CORE_TRACE("Deserialized entity with ID = {0}, name = {1}", uuid, name);

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
                }

                auto materialComponent = entity["MaterialComponent"];
                if (materialComponent)
                {
                    auto& mc = deserializedEntity.AddComponent<MaterialComponent>();

                    // Base Color
                    auto baseColorFactorSeq = materialComponent["BaseColorFactor"];
                    if (baseColorFactorSeq)
                    {
                        mc.BaseColorFactors.clear();
                        for (auto node : baseColorFactorSeq) mc.BaseColorFactors.push_back(node.as<glm::vec4>());
                    }
                    else if (materialComponent["AlbedoColors"]) // Backward compatibility
                    {
                        mc.BaseColorFactors.clear();
                        for (auto node : materialComponent["AlbedoColors"]) mc.BaseColorFactors.push_back(node.as<glm::vec4>());
                    }
                    else if (materialComponent["AlbedoColor"])
                    {
                        mc.BaseColorFactors.clear();
                        mc.BaseColorFactors.push_back(materialComponent["AlbedoColor"].as<glm::vec4>());
                    }

                    auto baseColorMapsSeq = materialComponent["BaseColorMaps"];
                    if (baseColorMapsSeq)
                    {
                        mc.BaseColorMaps.clear();
                        for (auto node : baseColorMapsSeq) mc.BaseColorMaps.push_back(node.as<uint64_t>());
                    }
                    else if (materialComponent["AlbedoMaps"])
                    {
                        mc.BaseColorMaps.clear();
                        for (auto node : materialComponent["AlbedoMaps"]) mc.BaseColorMaps.push_back(node.as<uint64_t>());
                    }

                    auto baseColorSetsSeq = materialComponent["BaseColorTextureSets"];
                    if (baseColorSetsSeq)
                    {
                        mc.BaseColorTextureSets.clear();
                        for (auto node : baseColorSetsSeq) mc.BaseColorTextureSets.push_back(node.as<int32_t>());
                    }

                    // PBR Properties
                    auto metallicSeq = materialComponent["MetallicFactors"];
                    if (metallicSeq)
                    {
                        mc.MetallicFactors.clear();
                        for (auto node : metallicSeq) mc.MetallicFactors.push_back(node.as<float>());
                    }

                    auto roughnessSeq = materialComponent["RoughnessFactors"];
                    if (roughnessSeq)
                    {
                        mc.RoughnessFactors.clear();
                        for (auto node : roughnessSeq) mc.RoughnessFactors.push_back(node.as<float>());
                    }

                    auto metallicRoughnessMapsSeq = materialComponent["MetallicRoughnessMaps"];
                    if (metallicRoughnessMapsSeq)
                    {
                        mc.MetallicRoughnessMaps.clear();
                        for (auto node : metallicRoughnessMapsSeq) mc.MetallicRoughnessMaps.push_back(node.as<uint64_t>());
                    }

                    auto physSetsSeq = materialComponent["PhysicalDescriptorTextureSets"];
                    if (physSetsSeq)
                    {
                        mc.PhysicalDescriptorTextureSets.clear();
                        for (auto node : physSetsSeq) mc.PhysicalDescriptorTextureSets.push_back(node.as<int32_t>());
                    }

                    // Additional Maps
                    auto normalMapsSeq = materialComponent["NormalMaps"];
                    if (normalMapsSeq)
                    {
                        mc.NormalMaps.clear();
                        for (auto node : normalMapsSeq) mc.NormalMaps.push_back(node.as<uint64_t>());
                    }

                    auto normalSetsSeq = materialComponent["NormalTextureSets"];
                    if (normalSetsSeq)
                    {
                        mc.NormalTextureSets.clear();
                        for (auto node : normalSetsSeq) mc.NormalTextureSets.push_back(node.as<int32_t>());
                    }

                    auto occlusionMapsSeq = materialComponent["OcclusionMaps"];
                    if (occlusionMapsSeq)
                    {
                        mc.OcclusionMaps.clear();
                        for (auto node : occlusionMapsSeq) mc.OcclusionMaps.push_back(node.as<uint64_t>());
                    }

                    auto occlusionSetsSeq = materialComponent["OcclusionTextureSets"];
                    if (occlusionSetsSeq)
                    {
                        mc.OcclusionTextureSets.clear();
                        for (auto node : occlusionSetsSeq) mc.OcclusionTextureSets.push_back(node.as<int32_t>());
                    }

                    // Emission
                    auto emissiveFactorsSeq = materialComponent["EmissiveFactors"];
                    if (emissiveFactorsSeq)
                    {
                        mc.EmissiveFactors.clear();
                        for (auto node : emissiveFactorsSeq) mc.EmissiveFactors.push_back(node.as<glm::vec3>());
                    }

                    auto emissiveMapsSeq = materialComponent["EmissiveMaps"];
                    if (emissiveMapsSeq)
                    {
                        mc.EmissiveMaps.clear();
                        for (auto node : emissiveMapsSeq) mc.EmissiveMaps.push_back(node.as<uint64_t>());
                    }

                    auto emissiveSetsSeq = materialComponent["EmissiveTextureSets"];
                    if (emissiveSetsSeq)
                    {
                        mc.EmissiveTextureSets.clear();
                        for (auto node : emissiveSetsSeq) mc.EmissiveTextureSets.push_back(node.as<int32_t>());
                    }

                    auto emissiveStrengthsSeq = materialComponent["EmissiveStrengths"];
                    if (emissiveStrengthsSeq)
                    {
                        mc.EmissiveStrengths.clear();
                        for (auto node : emissiveStrengthsSeq) mc.EmissiveStrengths.push_back(node.as<float>());
                    }

                    // Settings
                    auto modesSeq = materialComponent["Modes"];
                    if (modesSeq)
                    {
                        mc.Modes.clear();
                        for (auto node : modesSeq) mc.Modes.push_back(static_cast<AlphaMode>(node.as<int>()));
                    }

                    auto alphaCutoffsSeq = materialComponent["AlphaCutoffs"];
                    if (alphaCutoffsSeq)
                    {
                        mc.AlphaCutoffs.clear();
                        for (auto node : alphaCutoffsSeq) mc.AlphaCutoffs.push_back(node.as<float>());
                    }

                    auto doubleSidedSeq = materialComponent["DoubleSidedFlags"];
                    if (doubleSidedSeq)
                    {
                        mc.DoubleSidedFlags.clear();
                        for (auto node : doubleSidedSeq) mc.DoubleSidedFlags.push_back(node.as<bool>());
                    }
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
