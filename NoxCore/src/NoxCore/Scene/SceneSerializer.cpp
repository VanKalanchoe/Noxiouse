#include "SceneSerializer.h"
#include "NoxCore/Core/VirtualFileSystem.h"


#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <unordered_set>

#include <yaml-cpp/yaml.h>

/*#include "VanK/Scripting/ScriptEngine.h"*/
#include "NoxCore/Core/core.h"
#include "NoxCore/Core/Log.h"
#include <iostream>

#include "NoxCore/Core/Hash.h"
#include "NoxCore/Core/UUID.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/NodeGraphSerializer.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/ModelInstance.h"
#include "NoxCore/Scene/PrefabInstance.h"
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

    // World positions (large world coordinates): written at full double precision (yaml-cpp's default for doubles), read from
    // files written either way.
    template <>
    struct convert<glm::dvec3>
    {
        static Node encode(const glm::dvec3& rhs)
        {
            Node node;
            node.push_back(rhs.x);
            node.push_back(rhs.y);
            node.push_back(rhs.z);
            node.SetStyle(EmitterStyle::Flow);
            return node;
        }

        static bool decode(const Node& node, glm::dvec3& rhs)
        {
            if (!node.IsSequence() || node.size() != 3)
                return false;

            rhs.x = node[0].as<double>();
            rhs.y = node[1].as<double>();
            rhs.z = node[2].as<double>();
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

    YAML::Emitter& operator<<(YAML::Emitter& out, const glm::dvec3& v)
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

    // The changes of a prefab instance (fields, structure, nested instances), written into an open map.
    void SceneSerializer::WritePrefabChanges(YAML::Emitter& out, const std::vector<PrefabPropertyOverride>& overrides, const std::vector<PrefabStructureChange>& structure,
                                             const std::vector<PrefabNestedChange>& nested)
    {
        if (!overrides.empty())
        {
            out << YAML::Key << "Overrides" << YAML::Value << YAML::BeginSeq;
            for (const PrefabPropertyOverride& propertyOverride : overrides)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Entity" << YAML::Value << propertyOverride.Entity;
                out << YAML::Key << "Component" << YAML::Value << propertyOverride.Component;
                out << YAML::Key << "Field" << YAML::Value << propertyOverride.Field;
                out << YAML::Key << "Value" << YAML::Value << propertyOverride.Value;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }

        if (!structure.empty())
        {
            out << YAML::Key << "Structure" << YAML::Value << YAML::BeginSeq;
            for (const PrefabStructureChange& change : structure)
            {
                const char* kind = change.Kind == PrefabStructureChange::ChangeKind::RemovedEntity ? "RemovedEntity"
                                 : change.Kind == PrefabStructureChange::ChangeKind::RemovedComponent ? "RemovedComponent" : "AddedComponent";
                out << YAML::BeginMap;
                out << YAML::Key << "Kind" << YAML::Value << kind;
                out << YAML::Key << "Entity" << YAML::Value << change.Entity;
                out << YAML::Key << "Component" << YAML::Value << change.Component;
                out << YAML::Key << "Value" << YAML::Value << change.Value;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }

        if (!nested.empty())
        {
            out << YAML::Key << "Nested" << YAML::Value << YAML::BeginSeq;
            for (const PrefabNestedChange& change : nested)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Entity" << YAML::Value << change.Entity;
                WritePrefabChanges(out, change.Overrides, change.Structure, change.Nested);
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }
    }

    // An entity spawned by a prefab instance that still exists: saved through the instance, never itself.
    static bool IsSpawnedPrefabNode(Scene& scene, UUID id)
    {
        Entity entity = scene.GetEntityByUUID(id);
        if (!entity || !entity.HasComponent<PrefabNodeComponent>())
            return false;
        Entity root = scene.GetEntityByUUID(entity.GetComponent<PrefabNodeComponent>().Instance);
        return root && root.HasComponent<PrefabInstanceComponent>();
    }

    static void SerializeEntity(YAML::Emitter& out, Scene& scene, Entity entity, bool full = false)
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
                if (!IsSpawnedModelNode(scene, childID) && !IsSpawnedPrefabNode(scene, childID))
                    out << childID;
            }
            out << YAML::EndSeq;

            out << YAML::EndMap; // RelationshipComponent
        }

        // A prefab instance root saves its own placement only (name, transform, parent, folder above and here): every other
        // component comes from the prefab when it spawns, so an edit of the prefab reaches the instance.
        if (!full && entity.HasComponent<PrefabInstanceComponent>())
        {
            if (entity.HasComponent<FolderComponent>() && !entity.GetComponent<FolderComponent>().Path.empty())
            {
                out << YAML::Key << "FolderComponent";
                out << YAML::BeginMap;
                out << YAML::Key << "Path" << YAML::Value << entity.GetComponent<FolderComponent>().Path;
                out << YAML::EndMap;
            }

            out << YAML::Key << "PrefabInstanceComponent";
            out << YAML::BeginMap;
            out << YAML::Key << "Prefab" << YAML::Value << entity.GetComponent<PrefabInstanceComponent>().Prefab;

            // What differs from the prefab (worked out from the spawned entities, or what was captured while they are not there), and
            // the same for the prefab instances inside this one.
            SceneSerializer::WritePrefabChanges(out, PrefabInstance::CollectOverrides(scene, entity), PrefabInstance::CollectStructureChanges(scene, entity),
                                                PrefabInstance::CollectNested(scene, entity));
            out << YAML::EndMap;

            out << YAML::EndMap; // entity
            return;
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

        if (entity.HasComponent<FolderComponent>() && !entity.GetComponent<FolderComponent>().Path.empty())
        {
            out << YAML::Key << "FolderComponent";
            out << YAML::BeginMap;
            out << YAML::Key << "Path" << YAML::Value << entity.GetComponent<FolderComponent>().Path;
            out << YAML::EndMap;
        }

        if (entity.HasComponent<ModelInstanceComponent>())
        {
            out << YAML::Key << "ModelInstanceComponent";
            out << YAML::BeginMap;

            const auto& instance = entity.GetComponent<ModelInstanceComponent>();
            out << YAML::Key << "Model" << YAML::Value << static_cast<uint64_t>(instance.Model);
            if (instance.AtFileLayout)
                out << YAML::Key << "AtFileLayout" << YAML::Value << true;
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

        if (entity.HasComponent<RectLightComponent>())
        {
            out << YAML::Key << "RectLightComponent";
            out << YAML::BeginMap;

            auto& rlc = entity.GetComponent<RectLightComponent>();
            out << YAML::Key << "Color" << YAML::Value << rlc.Color;
            out << YAML::Key << "Intensity" << YAML::Value << rlc.Intensity;
            out << YAML::Key << "Width" << YAML::Value << rlc.Width;
            out << YAML::Key << "Height" << YAML::Value << rlc.Height;
            out << YAML::Key << "Range" << YAML::Value << rlc.Range;

            out << YAML::EndMap;
        }

        if (entity.HasComponent<DiscLightComponent>())
        {
            out << YAML::Key << "DiscLightComponent";
            out << YAML::BeginMap;

            auto& dlc = entity.GetComponent<DiscLightComponent>();
            out << YAML::Key << "Color" << YAML::Value << dlc.Color;
            out << YAML::Key << "Intensity" << YAML::Value << dlc.Intensity;
            out << YAML::Key << "Radius" << YAML::Value << dlc.Radius;
            out << YAML::Key << "Range" << YAML::Value << dlc.Range;

            out << YAML::EndMap;
        }

        if (entity.HasComponent<EnvironmentLightComponent>())
        {
            out << YAML::Key << "EnvironmentLightComponent";
            out << YAML::BeginMap;

            auto& elc = entity.GetComponent<EnvironmentLightComponent>();
            out << YAML::Key << "Source" << YAML::Value << (elc.Source == EnvironmentSource::ProceduralSky ? "ProceduralSky" : "Texture");
            out << YAML::Key << "TexturePath" << YAML::Value << elc.TexturePath;
            out << YAML::Key << "RadianceScale" << YAML::Value << elc.RadianceScale;
            out << YAML::Key << "Rotation" << YAML::Value << elc.Rotation;
            out << YAML::Key << "Enabled" << YAML::Value << elc.Enabled;
            out << YAML::Key << "RayleighScale" << YAML::Value << elc.RayleighScale;
            out << YAML::Key << "MieScale" << YAML::Value << elc.MieScale;
            out << YAML::Key << "OzoneScale" << YAML::Value << elc.OzoneScale;
            out << YAML::Key << "MieAnisotropy" << YAML::Value << elc.MieAnisotropy;
            out << YAML::Key << "GroundAlbedo" << YAML::Value << elc.GroundAlbedo;
            out << YAML::Key << "AltitudeKm" << YAML::Value << elc.AltitudeKm;
            out << YAML::Key << "AerialPerspectiveScale" << YAML::Value << elc.AerialPerspectiveScale;
            out << YAML::Key << "Clouds" << YAML::Value << elc.Clouds;
            out << YAML::Key << "CloudCoverage" << YAML::Value << elc.CloudCoverage;
            out << YAML::Key << "CloudDensity" << YAML::Value << elc.CloudDensity;
            out << YAML::Key << "CloudBottomKm" << YAML::Value << elc.CloudBottomKm;
            out << YAML::Key << "CloudThicknessKm" << YAML::Value << elc.CloudThicknessKm;
            out << YAML::Key << "CloudScaleKm" << YAML::Value << elc.CloudScaleKm;
            out << YAML::Key << "CloudOffsetKm" << YAML::Value << elc.CloudOffsetKm;
            out << YAML::Key << "CloudWind" << YAML::Value << elc.CloudWind;
            out << YAML::Key << "SunThroughAtmosphere" << YAML::Value << elc.SunThroughAtmosphere;
            out << YAML::Key << "Moon" << YAML::Value << elc.Moon;
            out << YAML::Key << "MoonIntensity" << YAML::Value << elc.MoonIntensity;
            out << YAML::Key << "MoonColor" << YAML::Value << elc.MoonColor;
            out << YAML::Key << "MoonAngularDiameter" << YAML::Value << elc.MoonAngularDiameter;
            out << YAML::Key << "MoonOppositeSun" << YAML::Value << elc.MoonOppositeSun;
            out << YAML::Key << "MoonElevation" << YAML::Value << elc.MoonElevation;
            out << YAML::Key << "MoonAzimuth" << YAML::Value << elc.MoonAzimuth;
            out << YAML::Key << "Stars" << YAML::Value << elc.Stars;
            out << YAML::Key << "StarIntensity" << YAML::Value << elc.StarIntensity;
            out << YAML::Key << "HeightFog" << YAML::Value << elc.HeightFog;
            out << YAML::Key << "FogDensity" << YAML::Value << elc.FogDensity;
            out << YAML::Key << "FogHeightFalloff" << YAML::Value << elc.FogHeightFalloff;
            out << YAML::Key << "FogHeight" << YAML::Value << elc.FogHeight;
            out << YAML::Key << "FogStartDistance" << YAML::Value << elc.FogStartDistance;
            out << YAML::Key << "FogMaxOpacity" << YAML::Value << elc.FogMaxOpacity;
            out << YAML::Key << "FogColor" << YAML::Value << elc.FogColor;
            out << YAML::Key << "FogFromEnvironment" << YAML::Value << elc.FogFromEnvironment;
            out << YAML::Key << "FogSunScattering" << YAML::Value << elc.FogSunScattering;
            out << YAML::Key << "FogSunExponent" << YAML::Value << elc.FogSunExponent;

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

            // Graph mode (docs/Animation_Graph_Architecture_Plan_2026.md); 0 = single-clip mode above.
            out << YAML::Key << "Graph" << YAML::Value << animatorComponent.Graph;
            out << YAML::Key << "RootMotion" << YAML::Value << animatorComponent.RootMotion;
            out << YAML::Key << "RootBone" << YAML::Value << animatorComponent.RootBone;
            out << YAML::Key << "GraphParameters" << YAML::Value << YAML::BeginSeq;
            for (const auto& [name, value] : animatorComponent.GraphInstance.Parameters)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Name" << YAML::Value << name;
                out << YAML::Key << "Value";
                NodeGraphSerializer::EmitValue(out, value);
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;

            out << YAML::EndMap; // AnimatorComponent
        }

        if (entity.HasComponent<TimelinePlayerComponent>())
        {
            const auto& player = entity.GetComponent<TimelinePlayerComponent>();
            out << YAML::Key << "TimelinePlayerComponent";
            out << YAML::BeginMap;
            out << YAML::Key << "Timeline" << YAML::Value << player.Timeline;
            out << YAML::Key << "PlayOnStart" << YAML::Value << player.PlayOnStart;
            out << YAML::Key << "Speed" << YAML::Value << player.Speed;
            out << YAML::EndMap;
        }

        if (entity.HasComponent<LevelStreamingVolumeComponent>())
        {
            const auto& volume = entity.GetComponent<LevelStreamingVolumeComponent>();
            out << YAML::Key << "LevelStreamingVolumeComponent";
            out << YAML::BeginMap;
            out << YAML::Key << "Levels" << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (AssetHandle level : volume.Levels)
                out << level;
            out << YAML::EndSeq;
            out << YAML::Key << "HalfExtents" << YAML::Value << volume.HalfExtents;
            out << YAML::Key << "UnloadDelay" << YAML::Value << volume.UnloadDelay;
            out << YAML::EndMap;
        }

        if (entity.HasComponent<WorldPartitionComponent>())
        {
            static const char* modeNames[] = { "Auto", "Spatial", "AlwaysLoaded" };
            const auto mode = entity.GetComponent<WorldPartitionComponent>().Mode;
            out << YAML::Key << "WorldPartitionComponent" << YAML::BeginMap;
            out << YAML::Key << "Mode" << YAML::Value << modeNames[static_cast<int>(mode)];
            out << YAML::EndMap;
        }

        if (entity.HasComponent<HLODComponent>())
        {
            const WorldCellCoord& source = entity.GetComponent<HLODComponent>().Source;
            out << YAML::Key << "HLODComponent" << YAML::BeginMap;
            out << YAML::Key << "Source" << YAML::Value << YAML::Flow << YAML::BeginSeq << source.Level << source.X << source.Z << YAML::EndSeq;
            out << YAML::EndMap;
        }

        if (entity.HasComponent<WorldStreamingSourceComponent>())
        {
            const auto& source = entity.GetComponent<WorldStreamingSourceComponent>();
            out << YAML::Key << "WorldStreamingSourceComponent" << YAML::BeginMap;
            out << YAML::Key << "Enabled" << YAML::Value << source.Enabled;
            out << YAML::Key << "TargetState" << YAML::Value
                << (source.Target == WorldStreamingSourceComponent::TargetState::Loaded ? "Loaded" : "Activated");
            out << YAML::Key << "LoadingRangeScale" << YAML::Value << source.LoadingRangeScale;
            out << YAML::EndMap;
        }

        if (entity.HasComponent<ParticleEmitterComponent>())
        {
            const auto& emitter = entity.GetComponent<ParticleEmitterComponent>();
            static const char* shapeNames[] = { "Point", "Sphere", "Box", "Cone" };
            out << YAML::Key << "ParticleEmitterComponent";
            out << YAML::BeginMap;
            out << YAML::Key << "Emitting" << YAML::Value << emitter.Emitting;
            out << YAML::Key << "Looping" << YAML::Value << emitter.Looping;
            out << YAML::Key << "Duration" << YAML::Value << emitter.Duration;
            out << YAML::Key << "SpawnRate" << YAML::Value << emitter.SpawnRate;
            out << YAML::Key << "Burst" << YAML::Value << emitter.Burst;
            out << YAML::Key << "MaxParticles" << YAML::Value << emitter.MaxParticles;
            out << YAML::Key << "Shape" << YAML::Value << shapeNames[static_cast<int>(emitter.Shape)];
            out << YAML::Key << "ShapeSize" << YAML::Value << emitter.ShapeSize;
            out << YAML::Key << "ConeAngle" << YAML::Value << emitter.ConeAngle;
            out << YAML::Key << "LifetimeMin" << YAML::Value << emitter.LifetimeMin;
            out << YAML::Key << "LifetimeMax" << YAML::Value << emitter.LifetimeMax;
            out << YAML::Key << "Velocity" << YAML::Value << emitter.Velocity;
            out << YAML::Key << "VelocityRandom" << YAML::Value << emitter.VelocityRandom;
            out << YAML::Key << "Gravity" << YAML::Value << emitter.Gravity;
            out << YAML::Key << "Drag" << YAML::Value << emitter.Drag;
            out << YAML::Key << "SpinMin" << YAML::Value << emitter.SpinMin;
            out << YAML::Key << "SpinMax" << YAML::Value << emitter.SpinMax;
            out << YAML::Key << "SizeStart" << YAML::Value << emitter.SizeStart;
            out << YAML::Key << "SizeEnd" << YAML::Value << emitter.SizeEnd;
            out << YAML::Key << "SizeRandom" << YAML::Value << emitter.SizeRandom;
            out << YAML::Key << "ColorStart" << YAML::Value << emitter.ColorStart;
            out << YAML::Key << "ColorEnd" << YAML::Value << emitter.ColorEnd;
            if (!emitter.ColorOverLife.empty())
            {
                out << YAML::Key << "ColorOverLife" << YAML::Value << YAML::BeginSeq;
                for (const ParticleColorKey& key : emitter.ColorOverLife)
                    out << YAML::Flow << YAML::BeginSeq << key.Time << key.Color.r << key.Color.g << key.Color.b << key.Color.a << YAML::EndSeq;
                out << YAML::EndSeq;
            }
            if (!emitter.SizeOverLife.empty())
            {
                out << YAML::Key << "SizeOverLife" << YAML::Value << YAML::BeginSeq;
                for (const ParticleSizeKey& key : emitter.SizeOverLife)
                    out << YAML::Flow << YAML::BeginSeq << key.Time << key.Size << YAML::EndSeq;
                out << YAML::EndSeq;
            }
            out << YAML::Key << "Intensity" << YAML::Value << emitter.Intensity;
            out << YAML::Key << "Texture" << YAML::Value << emitter.Texture;
            out << YAML::Key << "FlipbookColumns" << YAML::Value << emitter.FlipbookColumns;
            out << YAML::Key << "FlipbookRows" << YAML::Value << emitter.FlipbookRows;
            out << YAML::Key << "FlipbookFPS" << YAML::Value << emitter.FlipbookFPS;
            out << YAML::Key << "Blend" << YAML::Value << (emitter.Blend == ParticleBlend::Additive ? "Additive" : "Alpha");
            out << YAML::Key << "Lit" << YAML::Value << emitter.Lit;
            out << YAML::Key << "SoftParticles" << YAML::Value << emitter.SoftParticles;
            out << YAML::Key << "SoftDistance" << YAML::Value << emitter.SoftDistance;
            out << YAML::Key << "LocalSpace" << YAML::Value << emitter.LocalSpace;
            out << YAML::EndMap;
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

        if (entity.HasComponent<ScriptComponent>())
        {
            const auto& scriptComponent = entity.GetComponent<ScriptComponent>();
            out << YAML::Key << "ScriptComponent";
            out << YAML::BeginMap;
            out << YAML::Key << "Classes" << YAML::Value << YAML::BeginSeq;
            for (const std::string& className : scriptComponent.ClassNames)
                out << className;
            out << YAML::EndSeq;

            out << YAML::Key << "EntityReferences" << YAML::Value << YAML::BeginSeq;
            for (const auto& [className, fields] : scriptComponent.EntityReferences)
            {
                for (const auto& [fieldName, reference] : fields)
                {
                    out << YAML::BeginMap;
                    out << YAML::Key << "Class" << YAML::Value << className;
                    out << YAML::Key << "Field" << YAML::Value << fieldName;
                    if (reference.IsModelNode())
                    {
                        out << YAML::Key << "ModelInstance" << YAML::Value << reference.ModelInstance;
                        out << YAML::Key << "ModelNode" << YAML::Value << reference.ModelNodeIndex;
                    }
                    else
                    {
                        out << YAML::Key << "Entity" << YAML::Value << reference.Entity;
                    }
                    out << YAML::EndMap;
                }
            }
            out << YAML::EndSeq;

            out << YAML::Key << "FieldOverrides" << YAML::Value << YAML::BeginSeq;
            for (const auto& [className, fields] : scriptComponent.FieldOverrides)
            {
                for (const auto& [fieldName, value] : fields)
                {
                    out << YAML::BeginMap;
                    out << YAML::Key << "Class" << YAML::Value << className;
                    out << YAML::Key << "Field" << YAML::Value << fieldName;
                    std::visit([&](const auto& data)
                    {
                        using T = std::decay_t<decltype(data)>;
                        if constexpr (std::is_same_v<T, bool>) out << YAML::Key << "Type" << YAML::Value << "Bool" << YAML::Key << "Value" << YAML::Value << data;
                        else if constexpr (std::is_same_v<T, int32_t>) out << YAML::Key << "Type" << YAML::Value << "Int" << YAML::Key << "Value" << YAML::Value << data;
                        else if constexpr (std::is_same_v<T, uint32_t>) out << YAML::Key << "Type" << YAML::Value << "UInt" << YAML::Key << "Value" << YAML::Value << data;
                        else if constexpr (std::is_same_v<T, int64_t>) out << YAML::Key << "Type" << YAML::Value << "Long" << YAML::Key << "Value" << YAML::Value << data;
                        else if constexpr (std::is_same_v<T, uint64_t>) out << YAML::Key << "Type" << YAML::Value << "ULong" << YAML::Key << "Value" << YAML::Value << data;
                        else if constexpr (std::is_same_v<T, float>) out << YAML::Key << "Type" << YAML::Value << "Float" << YAML::Key << "Value" << YAML::Value << data;
                        else if constexpr (std::is_same_v<T, double>) out << YAML::Key << "Type" << YAML::Value << "Double" << YAML::Key << "Value" << YAML::Value << data;
                        else if constexpr (std::is_same_v<T, std::string>) out << YAML::Key << "Type" << YAML::Value << "String" << YAML::Key << "Value" << YAML::Value << data;
                        else if constexpr (std::is_same_v<T, glm::vec3>) out << YAML::Key << "Type" << YAML::Value << "Vector3" << YAML::Key << "Value" << YAML::Value << data;
                    }, value);
                    out << YAML::EndMap;
                }
            }
            out << YAML::EndSeq;
            out << YAML::EndMap;
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
            out << YAML::Key << "RollingResistance" << YAML::Value << bc2dComponent.RollingResistance;
            out << YAML::Key << "TangentSpeed" << YAML::Value << bc2dComponent.TangentSpeed;

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
            out << YAML::Key << "RollingResistance" << YAML::Value << cc2dComponent.RollingResistance;
            out << YAML::Key << "TangentSpeed" << YAML::Value << cc2dComponent.TangentSpeed;

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
            out << YAML::Key << "RollingResistance" << YAML::Value << sc3d.RollingResistance;

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
            out << YAML::Key << "RollingResistance" << YAML::Value << cc3d.RollingResistance;

            out << YAML::EndMap; // CapsuleCollider3DComponent
        }

        if (entity.HasComponent<CharacterController3DComponent>())
        {
            out << YAML::Key << "CharacterController3DComponent";
            out << YAML::BeginMap; // CharacterController3DComponent

            auto& cct = entity.GetComponent<CharacterController3DComponent>();
            out << YAML::Key << "Radius" << YAML::Value << cct.Radius;
            out << YAML::Key << "Height" << YAML::Value << cct.Height;
            out << YAML::Key << "StepHeight" << YAML::Value << cct.StepHeight;
            out << YAML::Key << "MaxSlopeDegrees" << YAML::Value << cct.MaxSlopeDegrees;
            out << YAML::Key << "GravityScale" << YAML::Value << cct.GravityScale;
            out << YAML::Key << "AirControl" << YAML::Value << cct.AirControl;
            out << YAML::Key << "MaxAcceleration" << YAML::Value << cct.MaxAcceleration;
            out << YAML::Key << "BrakingDeceleration" << YAML::Value << cct.BrakingDeceleration;

            out << YAML::EndMap; // CharacterController3DComponent
        }

        if (entity.HasComponent<TextComponent>())
        {
            out << YAML::Key << "TextComponent";
            out << YAML::BeginMap; // TextComponent

            auto& textComponent = entity.GetComponent<TextComponent>();
            out << YAML::Key << "TextString" << YAML::Value << textComponent.TextString;
            out << YAML::Key << "Font" << YAML::Value << static_cast<uint64_t>(textComponent.Font);
            out << YAML::Key << "Color" << YAML::Value << textComponent.Color;
            out << YAML::Key << "Kerning" << YAML::Value << textComponent.Kerning;
            out << YAML::Key << "LineSpacing" << YAML::Value << textComponent.LineSpacing;
            if (textComponent.OutlineThickness > 0.0f)
            {
                out << YAML::Key << "OutlineColor" << YAML::Value << textComponent.OutlineColor;
                out << YAML::Key << "OutlineThickness" << YAML::Value << textComponent.OutlineThickness;
            }
            if (textComponent.BackgroundColor.a > 0.0f)
            {
                out << YAML::Key << "BackgroundColor" << YAML::Value << textComponent.BackgroundColor;
                out << YAML::Key << "BackgroundPadding" << YAML::Value << textComponent.BackgroundPadding;
            }

            out << YAML::EndMap; // TextComponent
        }

        if (entity.HasComponent<BoneSocketComponent>())
        {
            const auto& socket = entity.GetComponent<BoneSocketComponent>();
            out << YAML::Key << "BoneSocketComponent" << YAML::BeginMap;
            out << YAML::Key << "Bone" << YAML::Value << socket.Bone;
            out << YAML::Key << "Offset" << YAML::Value << socket.Offset;
            out << YAML::Key << "RotationOffset" << YAML::Value << socket.RotationOffset;
            out << YAML::Key << "Scale" << YAML::Value << socket.Scale;
            out << YAML::EndMap;
        }

        out << YAML::EndMap; // Corrected: No parentheses
    }

    std::string SceneSerializer::EmitScene(size_t& scriptComponentCount, size_t& scriptClassCount, const std::unordered_set<UUID>* exclude)
    {
        YAML::Emitter out;
        scriptComponentCount = 0;
        scriptClassCount = 0;
        out << YAML::BeginMap; // Corrected: No parentheses
        out << YAML::Key << "Scene" << YAML::Value << "Untitled";
        out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq; // Corrected: No parentheses
        m_Scene->m_Registry.view<TagComponent>().each([&](auto entityID, TagComponent&)
        {
            Entity entity(entityID, m_Scene.get());
            if (!entity || IsSpawnedModelNode(*m_Scene, entity.GetUUID()) || IsSpawnedPrefabNode(*m_Scene, entity.GetUUID()))
                return;
            if (exclude && exclude->contains(entity.GetUUID()))
                return; // in a world cell's file
            if (entity.HasComponent<HLODComponent>())
                return; // generated (Build HLODs), kept in the HLOD grid's cells

            if (entity.HasComponent<ScriptComponent>())
            {
                ++scriptComponentCount;
                scriptClassCount += entity.GetComponent<ScriptComponent>().ClassNames.size();
            }

            SerializeEntity(out, *m_Scene, entity);
        });
        out << YAML::EndSeq; // Corrected: No parentheses
        if (!m_Scene->m_Folders.empty())
        {
            out << YAML::Key << "Folders" << YAML::Value << YAML::BeginSeq;
            for (const std::string& folder : m_Scene->m_Folders)
                out << folder;
            out << YAML::EndSeq;
        }
        WriteWorldPartition(out, *m_Scene);
        out << YAML::EndMap; // Corrected: No parentheses
        return std::string(out.c_str(), out.size());
    }

    void SceneSerializer::WriteWorldPartition(YAML::Emitter& out, Scene& scene)
    {
        const WorldPartition& world = scene.GetWorldPartition();
        if (!world.Settings.Enabled)
            return;
        out << YAML::Key << "WorldPartition" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "CellSize" << YAML::Value << world.Settings.CellSize;
        out << YAML::Key << "LoadingRange" << YAML::Value << world.Settings.LoadingRange;
        out << YAML::Key << "HLODCellSize" << YAML::Value << world.Settings.HLODCellSize;
        out << YAML::Key << "HLODLoadingRange" << YAML::Value << world.Settings.HLODLoadingRange;
        out << YAML::Key << "HLODError" << YAML::Value << world.Settings.HLODError;
        out << YAML::Key << "HLODMaxTriangles" << YAML::Value << world.Settings.HLODMaxTriangles;
        out << YAML::Key << "HLODTexelsPerMeter" << YAML::Value << world.Settings.HLODTexelsPerMeter;
        out << YAML::Key << "CellDirectory" << YAML::Value << world.CellDirectory.generic_string();
        // One line a cell: level, x, z, entities, bounds min, bounds max (world units), and the grid when not the world's (HLODs: 1).
        out << YAML::Key << "Cells" << YAML::Value << YAML::BeginSeq;
        for (const auto& [cell, info] : world.Cells)
        {
            out << YAML::Flow << YAML::BeginSeq << cell.Level << cell.X << cell.Z << info.Entities
                << info.BoundsMin.x << info.BoundsMin.y << info.BoundsMin.z << info.BoundsMax.x << info.BoundsMax.y << info.BoundsMax.z;
            if (cell.Grid != WorldGrid::WorldGridIndex)
                out << cell.Grid;
            out << YAML::EndSeq;
        }
        out << YAML::EndSeq;
        out << YAML::EndMap;
    }

    void SceneSerializer::ReadWorldPartition(Scene& scene, const YAML::Node& data)
    {
        const YAML::Node node = data["WorldPartition"];
        if (!node)
            return;
        WorldPartition& world = scene.GetWorldPartition();
        world.Settings.Enabled = true;
        if (node["CellSize"])
            world.Settings.CellSize = node["CellSize"].as<float>();
        if (node["LoadingRange"])
            world.Settings.LoadingRange = node["LoadingRange"].as<float>();
        if (node["HLODCellSize"])
            world.Settings.HLODCellSize = node["HLODCellSize"].as<float>();
        if (node["HLODLoadingRange"])
            world.Settings.HLODLoadingRange = node["HLODLoadingRange"].as<float>();
        if (node["HLODError"])
            world.Settings.HLODError = node["HLODError"].as<float>();
        if (node["HLODMaxTriangles"])
            world.Settings.HLODMaxTriangles = node["HLODMaxTriangles"].as<uint32_t>();
        if (node["HLODTexelsPerMeter"])
            world.Settings.HLODTexelsPerMeter = node["HLODTexelsPerMeter"].as<float>();
        if (node["CellDirectory"])
            world.CellDirectory = node["CellDirectory"].as<std::string>();
        world.Cells.clear();
        if (const YAML::Node cells = node["Cells"])
        {
            for (const YAML::Node& entry : cells)
            {
                if (!entry.IsSequence() || entry.size() < 10)
                    continue;
                const WorldCellCoord cell{ entry[0].as<int32_t>(), entry[1].as<int32_t>(), entry[2].as<int32_t>(),
                                           entry.size() > 10 ? entry[10].as<int32_t>() : WorldGrid::WorldGridIndex };
                WorldCellInfo& info = world.Cells[cell];
                info.Entities = entry[3].as<uint32_t>();
                info.BoundsMin = { entry[4].as<double>(), entry[5].as<double>(), entry[6].as<double>() };
                info.BoundsMax = { entry[7].as<double>(), entry[8].as<double>(), entry[9].as<double>() };
            }
        }
    }

    std::string SceneSerializer::WorldCellText(Scene& scene, const std::vector<Entity>& roots, const std::string& name, uint32_t* outCount)
    {
        // The roots' trees in the order of their IDs: the same entities give the same text, so whether a loaded cell changed is a
        // comparison with its file. The order does not matter to reading (entities find each other by ID).
        std::vector<Entity> entities;
        for (Entity root : roots)
        {
            std::function<void(Entity)> visit = [&](Entity entity)
            {
                if (!IsSpawnedModelNode(scene, entity.GetUUID()) && !IsSpawnedPrefabNode(scene, entity.GetUUID()))
                    entities.push_back(entity);
                if (!entity.HasComponent<RelationshipComponent>())
                    return;
                for (UUID childID : entity.GetComponent<RelationshipComponent>().Children)
                {
                    if (Entity child = scene.GetEntityByUUID(childID))
                        visit(child);
                }
            };
            visit(root);
        }
        std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return static_cast<uint64_t>(a.GetUUID()) < static_cast<uint64_t>(b.GetUUID()); });

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Scene" << YAML::Value << name;
        out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
        for (Entity entity : entities)
            SerializeEntity(out, scene, entity);
        out << YAML::EndSeq;
        out << YAML::EndMap;
        if (outCount)
            *outCount = static_cast<uint32_t>(entities.size());
        return std::string(out.c_str(), out.size());
    }

    std::string SceneSerializer::MergeWorldCellText(Scene& scene, const std::string& fileText, const std::vector<Entity>& roots, const std::string& name,
                                                    uint32_t& outCount)
    {
        // The file's entities with the given trees put in: one already there is replaced, the others kept; in the order of IDs.
        std::map<uint64_t, YAML::Node> entities;
        if (!fileText.empty())
        {
            try
            {
                std::string text = fileText;
                if (IsRuntimeScene(text) && !RuntimeSceneText(fileText, name, text))
                    text.clear();
                const YAML::Node file = YAML::Load(text);
                if (const YAML::Node list = file["Entities"])
                {
                    for (const YAML::Node& entity : list)
                        entities[entity["Entity"].as<uint64_t>()] = entity;
                }
            }
            catch (const YAML::Exception& exception)
            {
                NOX_CORE_ERROR("World cell `{}`: its file could not be read ({}); it is written with the moved entities only", name, exception.what());
            }
        }
        if (!roots.empty())
        {
            const YAML::Node added = YAML::Load(WorldCellText(scene, roots, name, nullptr));
            for (const YAML::Node& entity : added["Entities"])
                entities[entity["Entity"].as<uint64_t>()] = entity;
        }

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Scene" << YAML::Value << name;
        out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
        for (const auto& [id, entity] : entities)
            out << entity;
        out << YAML::EndSeq;
        out << YAML::EndMap;
        outCount = static_cast<uint32_t>(entities.size());
        return std::string(out.c_str(), out.size());
    }

    std::string SceneSerializer::SerializeToText()
    {
        size_t scriptComponentCount = 0;
        size_t scriptClassCount = 0;
        return EmitScene(scriptComponentCount, scriptClassCount, nullptr);
    }

    void SceneSerializer::Serialize(const std::filesystem::path& filepath)
    {
        size_t scriptComponentCount = 0;
        size_t scriptClassCount = 0;
        // A world: the cells' files first (they update the index the scene file holds), and their entities stay out of it.
        std::unordered_set<UUID> inCells;
        if (m_Scene->IsWorldPartitioned() && !m_Scene->SaveWorldCells(filepath, inCells))
            NOX_CORE_ERROR("Saving scene '{}': some world cells could not be written", filepath.string());
        const std::string text = EmitScene(scriptComponentCount, scriptClassCount, inCells.empty() ? nullptr : &inCells);

        const std::filesystem::path filePath(filepath);
        std::ofstream fout(filePath);
        if (!fout)
        {
            NOX_CORE_ERROR("Failed to save scene '{}': could not open the file", filePath.string());
            return;
        }

        fout << text;
        fout.close();
        if (!fout)
        {
            NOX_CORE_ERROR("Failed to save scene '{}': write failed", filePath.string());
            return;
        }

        NOX_CORE_INFO("Saved scene '{}' with {} script component(s) and {} script class assignment(s)",
                      filePath.string(), scriptComponentCount, scriptClassCount);
    }

    YAML::Node SceneSerializer::EntityToNode(Scene& scene, Entity entity)
    {
        YAML::Emitter out;
        SerializeEntity(out, scene, entity, true);
        return YAML::Load(out.c_str());
    }

    std::string SceneSerializer::EntityToText(Scene& scene, Entity entity)
    {
        YAML::Emitter out;
        SerializeEntity(out, scene, entity, true);
        return std::string(out.c_str(), out.size());
    }

    std::string SceneSerializer::PrefabToText(Scene& scene, const std::vector<Entity>& entities, const std::string& name)
    {
        std::unordered_set<UUID> chosen;
        for (Entity entity : entities)
        {
            if (entity && !IsSpawnedModelNode(scene, entity.GetUUID()) && !IsSpawnedPrefabNode(scene, entity.GetUUID()))
                chosen.insert(entity.GetUUID());
        }

        // Selected entities whose parent is not selected: the tops of the prefab. Everything below them comes along.
        std::vector<Entity> tops;
        for (Entity entity : entities)
        {
            if (!entity || !chosen.contains(entity.GetUUID()))
                continue;
            const bool parentChosen = entity.HasComponent<RelationshipComponent>() && chosen.contains(entity.GetComponent<RelationshipComponent>().Parent);
            if (!parentChosen && std::find(tops.begin(), tops.end(), entity) == tops.end())
                tops.push_back(entity);
        }
        if (tops.empty())
            return std::string();

        // The entities a prefab instance spawned are not written (the instance is), but what hangs below them and is not theirs is:
        // it names the spawned parent by its UUID, derived from the instance's (which is its id in the file), and the spawn links it up.
        std::vector<Entity> written; // depth first, parents before children
        std::function<void(Entity, bool)> collect = [&](Entity entity, bool write)
        {
            if (write)
                written.push_back(entity);
            if (!entity.HasComponent<RelationshipComponent>())
                return;
            for (UUID childID : entity.GetComponent<RelationshipComponent>().Children)
            {
                Entity child = scene.GetEntityByUUID(childID);
                if (child && !IsSpawnedModelNode(scene, childID))
                    collect(child, !IsSpawnedPrefabNode(scene, childID));
            }
        };
        for (Entity top : tops)
            collect(top, true);

        auto worldPosition = [](Entity entity) -> glm::dvec3
        {
            if (entity.HasComponent<WorldTransformComponent>())
                return entity.GetComponent<WorldTransformComponent>().GetPosition();
            return entity.GetComponent<TransformComponent>().Translation;
        };

        // Several tops hang from a new empty root. So does a single prefab instance: a prefab whose own root were an instance would be a
        // variant of that prefab (P6c); this makes the instance a nested one.
        const bool synthesizedRoot = tops.size() > 1 || tops.front().HasComponent<PrefabInstanceComponent>();
        const UUID rootID = synthesizedRoot ? UUID() : tops.front().GetUUID();
        const glm::dvec3 pivot = worldPosition(tops.front());

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Prefab" << YAML::Value << name;
        out << YAML::Key << "Root" << YAML::Value << static_cast<uint64_t>(rootID);
        out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;

        if (synthesizedRoot)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Entity" << YAML::Value << static_cast<uint64_t>(rootID);
            out << YAML::Key << "TagComponent" << YAML::Value << YAML::BeginMap << YAML::Key << "Tag" << YAML::Value << name << YAML::EndMap;
            out << YAML::Key << "TransformComponent" << YAML::Value << YAML::BeginMap;
            out << YAML::Key << "Translation" << YAML::Value << glm::vec3(0.0f);
            out << YAML::Key << "Rotation" << YAML::Value << glm::vec3(0.0f);
            out << YAML::Key << "Scale" << YAML::Value << glm::vec3(1.0f);
            out << YAML::EndMap;
            out << YAML::Key << "RelationshipComponent" << YAML::Value << YAML::BeginMap;
            out << YAML::Key << "Parent" << YAML::Value << uint64_t(0);
            out << YAML::Key << "Children" << YAML::Value << YAML::BeginSeq;
            for (Entity top : tops)
                out << static_cast<uint64_t>(top.GetUUID());
            out << YAML::EndSeq << YAML::EndMap;
            out << YAML::EndMap;
        }

        // Script fields that point at an entity which does not end up in the file would point into the scene the prefab was made from: they are
        // cleared in the file (the scene's entity keeps its reference).
        std::unordered_set<UUID> inPrefab;
        for (Entity entity : written)
            inPrefab.insert(entity.GetUUID());
        inPrefab.insert(rootID);
        auto serializeEntry = [&](Entity entity)
        {
            if (!entity.HasComponent<ScriptComponent>())
            {
                SerializeEntity(out, scene, entity);
                return;
            }
            auto& references = entity.GetComponent<ScriptComponent>().EntityReferences;
            const auto saved = references;
            for (auto& [className, fields] : references)
            {
                for (auto& [fieldName, reference] : fields)
                {
                    if (reference.IsModelNode() || reference.Entity == 0 || inPrefab.contains(reference.Entity))
                        continue;
                    NOX_CORE_INFO("Prefab '{}': {}.{} of '{}' points at an entity outside the prefab; it is cleared in the file", name, className, fieldName, entity.GetName());
                    reference.Entity = 0;
                }
            }
            SerializeEntity(out, scene, entity);
            references = saved;
        };

        for (Entity entity : written)
        {
            const bool isTop = std::find(tops.begin(), tops.end(), entity) != tops.end();
            if (synthesizedRoot && isTop)
            {
                // Under the new root at the place it had relative to the first selected entity (its transform is put back).
                auto& transform = entity.GetComponent<TransformComponent>();
                const glm::dvec3 original = transform.Translation;
                transform.Translation = worldPosition(entity) - pivot;
                serializeEntry(entity);
                transform.Translation = original;
            }
            else
            {
                serializeEntry(entity);
            }
        }
        out << YAML::EndSeq << YAML::EndMap;
        return out.c_str();
    }

    bool SceneSerializer::SerializePrefab(Scene& scene, const std::vector<Entity>& entities, const std::string& name, const std::filesystem::path& filepath)
    {
        const std::string text = PrefabToText(scene, entities, name);
        if (text.empty())
            return false;

        std::error_code error;
        std::filesystem::create_directories(filepath.parent_path(), error);
        std::ofstream fout(filepath);
        if (!fout)
        {
            NOX_CORE_ERROR("Failed to save prefab '{}': could not open the file", filepath.string());
            return false;
        }
        fout << text;
        fout.close();
        if (!fout)
        {
            NOX_CORE_ERROR("Failed to save prefab '{}': write failed", filepath.string());
            return false;
        }

        NOX_CORE_INFO("Saved prefab '{}'", filepath.string());
        return true;
    }

    namespace
    {
        // The runtime form of a scene (SerializeRuntime): a header that says what the file is and how long its scene is, then the
        // scene as Serialize writes it. A packaged game can tell a cut-off or foreign file from a scene before it parses a byte; a
        // binary encoding of the components can replace the body later (a new version) without changing the callers.
        constexpr char RuntimeSceneMagic[8] = { 'N', 'O', 'X', 'S', 'C', 'E', 'N', 'E' };
        constexpr uint32_t RuntimeSceneVersion = 1; // body: the scene's YAML text
    }

    bool SceneSerializer::SerializeRuntime(const std::filesystem::path& filepath)
    {
        const std::string text = SerializeToText();
        std::ofstream out(filepath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            NOX_CORE_ERROR("Failed to save runtime scene '{}': could not open the file", filepath.string());
            return false;
        }
        const uint64_t size = text.size();
        out.write(RuntimeSceneMagic, sizeof(RuntimeSceneMagic));
        out.write(reinterpret_cast<const char*>(&RuntimeSceneVersion), sizeof(RuntimeSceneVersion));
        out.write(reinterpret_cast<const char*>(&size), sizeof(size));
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out)
        {
            NOX_CORE_ERROR("Failed to save runtime scene '{}': write failed", filepath.string());
            return false;
        }
        return true;
    }

    bool SceneSerializer::IsRuntimeScene(const std::string& bytes)
    {
        return bytes.size() >= sizeof(RuntimeSceneMagic) && std::equal(std::begin(RuntimeSceneMagic), std::end(RuntimeSceneMagic), bytes.begin());
    }

    void SceneSerializer::ReadPrefabChanges(const YAML::Node& node, std::vector<PrefabPropertyOverride>& overrides, std::vector<PrefabStructureChange>& structure,
                                            std::vector<PrefabNestedChange>& nested)
    {
        if (auto overridesNode = node["Overrides"])
        {
            for (const auto& item : overridesNode)
            {
                PrefabPropertyOverride& propertyOverride = overrides.emplace_back();
                propertyOverride.Entity = item["Entity"].as<uint64_t>();
                propertyOverride.Component = item["Component"].as<std::string>();
                propertyOverride.Field = item["Field"].as<std::string>();
                propertyOverride.Value = item["Value"].as<std::string>();
            }
        }
        if (auto structureNode = node["Structure"])
        {
            for (const auto& item : structureNode)
            {
                PrefabStructureChange& change = structure.emplace_back();
                const std::string kind = item["Kind"].as<std::string>();
                change.Kind = kind == "RemovedEntity" ? PrefabStructureChange::ChangeKind::RemovedEntity
                            : kind == "RemovedComponent" ? PrefabStructureChange::ChangeKind::RemovedComponent : PrefabStructureChange::ChangeKind::AddedComponent;
                change.Entity = item["Entity"].as<uint64_t>();
                change.Component = item["Component"].as<std::string>();
                change.Value = item["Value"].as<std::string>();
            }
        }
        if (auto nestedNode = node["Nested"])
        {
            for (const auto& item : nestedNode)
            {
                PrefabNestedChange& change = nested.emplace_back();
                change.Entity = item["Entity"].as<uint64_t>();
                ReadPrefabChanges(item, change.Overrides, change.Structure, change.Nested);
            }
        }
    }

    YAML::Node SceneSerializer::PrefabChangesToNode(const std::vector<PrefabPropertyOverride>& overrides, const std::vector<PrefabStructureChange>& structure,
                                                    const std::vector<PrefabNestedChange>& nested)
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        WritePrefabChanges(out, overrides, structure, nested);
        out << YAML::EndMap;
        return YAML::Load(out.c_str());
    }

    void SceneSerializer::DeserializeEntityComponents(Scene& scene, const YAML::Node& entity, Entity deserializedEntity, bool keepPlacement)
    {
        auto transformComponent = entity["TransformComponent"];
        if (transformComponent && !keepPlacement)
        {
            // Entities always have transforms
            auto& tc = deserializedEntity.GetComponent<TransformComponent>();
            tc.Translation = transformComponent["Translation"].as<glm::dvec3>();
            tc.Rotation = transformComponent["Rotation"].as<glm::vec3>();
            tc.Scale = transformComponent["Scale"].as<glm::vec3>();
        }

        auto relationshipComponent = entity["RelationshipComponent"];
        if (relationshipComponent && !keepPlacement)
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

        if (auto folderComponent = entity["FolderComponent"])
        {
            if (folderComponent["Path"])
                deserializedEntity.AddComponent<FolderComponent>(folderComponent["Path"].as<std::string>());
        }

        // (An instance root spawned from a prefab keeps its own component: a prefab does not nest itself in P1.)
        if (auto prefabInstance = entity["PrefabInstanceComponent"]; prefabInstance && !deserializedEntity.HasComponent<PrefabInstanceComponent>())
        {
            auto& instance = deserializedEntity.AddComponent<PrefabInstanceComponent>();
            instance.Prefab = prefabInstance["Prefab"].as<uint64_t>();
            ReadPrefabChanges(prefabInstance, instance.Overrides, instance.Structure, instance.Nested);
        }

        if (auto modelInstance = entity["ModelInstanceComponent"])
        {
            auto& instance = deserializedEntity.AddComponent<ModelInstanceComponent>();
            instance.Model = modelInstance["Model"].as<uint64_t>();
            if (modelInstance["AtFileLayout"])
                instance.AtFileLayout = modelInstance["AtFileLayout"].as<bool>();
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
                        transform.Translation = node["Translation"].as<glm::dvec3>();
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

        auto rectLightComponent = entity["RectLightComponent"];
        if (rectLightComponent)
        {
            auto& rlc = deserializedEntity.AddComponent<RectLightComponent>();
            if (rectLightComponent["Color"])
                rlc.Color = rectLightComponent["Color"].as<glm::vec3>();
            if (rectLightComponent["Intensity"])
                rlc.Intensity = rectLightComponent["Intensity"].as<float>();
            if (rectLightComponent["Width"])
                rlc.Width = rectLightComponent["Width"].as<float>();
            if (rectLightComponent["Height"])
                rlc.Height = rectLightComponent["Height"].as<float>();
            if (rectLightComponent["Range"])
                rlc.Range = rectLightComponent["Range"].as<float>();
        }

        auto discLightComponent = entity["DiscLightComponent"];
        if (discLightComponent)
        {
            auto& dlc = deserializedEntity.AddComponent<DiscLightComponent>();
            if (discLightComponent["Color"])
                dlc.Color = discLightComponent["Color"].as<glm::vec3>();
            if (discLightComponent["Intensity"])
                dlc.Intensity = discLightComponent["Intensity"].as<float>();
            if (discLightComponent["Radius"])
                dlc.Radius = discLightComponent["Radius"].as<float>();
            if (discLightComponent["Range"])
                dlc.Range = discLightComponent["Range"].as<float>();
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
            if (environmentLightComponent["Source"])
                elc.Source = environmentLightComponent["Source"].as<std::string>() == "ProceduralSky" ? EnvironmentSource::ProceduralSky : EnvironmentSource::Texture;
            if (environmentLightComponent["RayleighScale"])
                elc.RayleighScale = environmentLightComponent["RayleighScale"].as<float>();
            if (environmentLightComponent["MieScale"])
                elc.MieScale = environmentLightComponent["MieScale"].as<float>();
            if (environmentLightComponent["OzoneScale"])
                elc.OzoneScale = environmentLightComponent["OzoneScale"].as<float>();
            if (environmentLightComponent["MieAnisotropy"])
                elc.MieAnisotropy = environmentLightComponent["MieAnisotropy"].as<float>();
            if (environmentLightComponent["GroundAlbedo"])
                elc.GroundAlbedo = environmentLightComponent["GroundAlbedo"].as<glm::vec3>();
            if (environmentLightComponent["AltitudeKm"])
                elc.AltitudeKm = environmentLightComponent["AltitudeKm"].as<float>();
            if (environmentLightComponent["AerialPerspectiveScale"])
                elc.AerialPerspectiveScale = environmentLightComponent["AerialPerspectiveScale"].as<float>();
            if (environmentLightComponent["Clouds"])
                elc.Clouds = environmentLightComponent["Clouds"].as<bool>();
            if (environmentLightComponent["CloudCoverage"])
                elc.CloudCoverage = environmentLightComponent["CloudCoverage"].as<float>();
            if (environmentLightComponent["CloudDensity"])
                elc.CloudDensity = environmentLightComponent["CloudDensity"].as<float>();
            if (environmentLightComponent["CloudBottomKm"])
                elc.CloudBottomKm = environmentLightComponent["CloudBottomKm"].as<float>();
            if (environmentLightComponent["CloudThicknessKm"])
                elc.CloudThicknessKm = environmentLightComponent["CloudThicknessKm"].as<float>();
            if (environmentLightComponent["CloudScaleKm"])
                elc.CloudScaleKm = environmentLightComponent["CloudScaleKm"].as<float>();
            if (environmentLightComponent["CloudOffsetKm"])
                elc.CloudOffsetKm = environmentLightComponent["CloudOffsetKm"].as<glm::vec2>();
            if (environmentLightComponent["CloudWind"])
                elc.CloudWind = environmentLightComponent["CloudWind"].as<glm::vec2>();
            if (environmentLightComponent["SunThroughAtmosphere"])
                elc.SunThroughAtmosphere = environmentLightComponent["SunThroughAtmosphere"].as<bool>();
            if (environmentLightComponent["Moon"])
                elc.Moon = environmentLightComponent["Moon"].as<bool>();
            if (environmentLightComponent["MoonIntensity"])
            {
                elc.MoonIntensity = environmentLightComponent["MoonIntensity"].as<float>();
                // Scenes saved with the old default (0.03, far too bright a night) take the new one.
                if (elc.MoonIntensity == 0.03f)
                    elc.MoonIntensity = EnvironmentLightComponent{}.MoonIntensity;
            }
            if (environmentLightComponent["MoonColor"])
                elc.MoonColor = environmentLightComponent["MoonColor"].as<glm::vec3>();
            if (environmentLightComponent["MoonAngularDiameter"])
                elc.MoonAngularDiameter = environmentLightComponent["MoonAngularDiameter"].as<float>();
            if (environmentLightComponent["MoonOppositeSun"])
                elc.MoonOppositeSun = environmentLightComponent["MoonOppositeSun"].as<bool>();
            if (environmentLightComponent["MoonElevation"])
                elc.MoonElevation = environmentLightComponent["MoonElevation"].as<float>();
            if (environmentLightComponent["MoonAzimuth"])
                elc.MoonAzimuth = environmentLightComponent["MoonAzimuth"].as<float>();
            if (environmentLightComponent["Stars"])
                elc.Stars = environmentLightComponent["Stars"].as<bool>();
            if (environmentLightComponent["StarIntensity"])
                elc.StarIntensity = environmentLightComponent["StarIntensity"].as<float>();
            if (environmentLightComponent["HeightFog"])
                elc.HeightFog = environmentLightComponent["HeightFog"].as<bool>();
            if (environmentLightComponent["FogDensity"])
                elc.FogDensity = environmentLightComponent["FogDensity"].as<float>();
            if (environmentLightComponent["FogHeightFalloff"])
                elc.FogHeightFalloff = environmentLightComponent["FogHeightFalloff"].as<float>();
            if (environmentLightComponent["FogHeight"])
                elc.FogHeight = environmentLightComponent["FogHeight"].as<float>();
            if (environmentLightComponent["FogStartDistance"])
                elc.FogStartDistance = environmentLightComponent["FogStartDistance"].as<float>();
            if (environmentLightComponent["FogMaxOpacity"])
                elc.FogMaxOpacity = environmentLightComponent["FogMaxOpacity"].as<float>();
            if (environmentLightComponent["FogColor"])
                elc.FogColor = environmentLightComponent["FogColor"].as<glm::vec3>();
            if (environmentLightComponent["FogFromEnvironment"])
                elc.FogFromEnvironment = environmentLightComponent["FogFromEnvironment"].as<bool>();
            if (environmentLightComponent["FogSunScattering"])
                elc.FogSunScattering = environmentLightComponent["FogSunScattering"].as<float>();
            if (environmentLightComponent["FogSunExponent"])
                elc.FogSunExponent = environmentLightComponent["FogSunExponent"].as<float>();
        }

        if (auto timelinePlayer = entity["TimelinePlayerComponent"])
        {
            auto& player = deserializedEntity.AddComponent<TimelinePlayerComponent>();
            if (timelinePlayer["Timeline"])
                player.Timeline = timelinePlayer["Timeline"].as<AssetHandle>();
            if (timelinePlayer["PlayOnStart"])
                player.PlayOnStart = timelinePlayer["PlayOnStart"].as<bool>();
            if (timelinePlayer["Speed"])
                player.Speed = timelinePlayer["Speed"].as<float>();
        }

        if (auto levelVolume = entity["LevelStreamingVolumeComponent"])
        {
            auto& volume = deserializedEntity.AddComponent<LevelStreamingVolumeComponent>();
            if (const YAML::Node levels = levelVolume["Levels"])
            {
                for (const YAML::Node& level : levels)
                    volume.Levels.push_back(level.as<AssetHandle>());
            }
            if (levelVolume["HalfExtents"])
                volume.HalfExtents = levelVolume["HalfExtents"].as<glm::vec3>();
            if (levelVolume["UnloadDelay"])
                volume.UnloadDelay = levelVolume["UnloadDelay"].as<float>();
        }

        if (auto worldPartition = entity["WorldPartitionComponent"])
        {
            auto& component = deserializedEntity.AddComponent<WorldPartitionComponent>();
            const std::string mode = worldPartition["Mode"] ? worldPartition["Mode"].as<std::string>() : std::string("AlwaysLoaded");
            component.Mode = mode == "Auto" ? WorldPartitionComponent::LoadingMode::Auto
                           : mode == "Spatial" ? WorldPartitionComponent::LoadingMode::Spatial
                           : WorldPartitionComponent::LoadingMode::AlwaysLoaded;
        }

        if (auto hlod = entity["HLODComponent"])
        {
            auto& component = deserializedEntity.AddComponent<HLODComponent>();
            if (const YAML::Node source = hlod["Source"]; source && source.IsSequence() && source.size() >= 3)
                component.Source = WorldCellCoord{ source[0].as<int32_t>(), source[1].as<int32_t>(), source[2].as<int32_t>() };
        }

        if (auto streamingSource = entity["WorldStreamingSourceComponent"])
        {
            auto& component = deserializedEntity.AddComponent<WorldStreamingSourceComponent>();
            if (streamingSource["Enabled"])
                component.Enabled = streamingSource["Enabled"].as<bool>();
            if (streamingSource["TargetState"])
                component.Target = streamingSource["TargetState"].as<std::string>() == "Loaded" ? WorldStreamingSourceComponent::TargetState::Loaded
                                                                                              : WorldStreamingSourceComponent::TargetState::Activated;
            if (streamingSource["LoadingRangeScale"])
                component.LoadingRangeScale = std::max(streamingSource["LoadingRangeScale"].as<float>(), 0.0f);
        }

        if (auto particleEmitter = entity["ParticleEmitterComponent"])
        {
            auto& emitter = deserializedEntity.AddComponent<ParticleEmitterComponent>();
            auto read = [&particleEmitter](const char* key, auto& value)
            {
                if (particleEmitter[key])
                    value = particleEmitter[key].as<std::remove_reference_t<decltype(value)>>();
            };
            read("Emitting", emitter.Emitting);
            read("Looping", emitter.Looping);
            read("Duration", emitter.Duration);
            read("SpawnRate", emitter.SpawnRate);
            read("Burst", emitter.Burst);
            read("MaxParticles", emitter.MaxParticles);
            if (particleEmitter["Shape"])
            {
                const std::string shape = particleEmitter["Shape"].as<std::string>();
                emitter.Shape = shape == "Sphere" ? ParticleShape::Sphere : shape == "Box" ? ParticleShape::Box : shape == "Cone" ? ParticleShape::Cone : ParticleShape::Point;
            }
            read("ShapeSize", emitter.ShapeSize);
            read("ConeAngle", emitter.ConeAngle);
            read("LifetimeMin", emitter.LifetimeMin);
            read("LifetimeMax", emitter.LifetimeMax);
            read("Velocity", emitter.Velocity);
            read("VelocityRandom", emitter.VelocityRandom);
            read("Gravity", emitter.Gravity);
            read("Drag", emitter.Drag);
            read("SpinMin", emitter.SpinMin);
            read("SpinMax", emitter.SpinMax);
            read("SizeStart", emitter.SizeStart);
            read("SizeEnd", emitter.SizeEnd);
            read("SizeRandom", emitter.SizeRandom);
            read("ColorStart", emitter.ColorStart);
            read("ColorEnd", emitter.ColorEnd);
            // Keys as [time, r, g, b, a] / [time, meters].
            emitter.ColorOverLife.clear();
            if (const YAML::Node keys = particleEmitter["ColorOverLife"]; keys && keys.IsSequence())
            {
                for (const YAML::Node& key : keys)
                {
                    if (key.IsSequence() && key.size() == 5)
                        emitter.ColorOverLife.push_back({ key[0].as<float>(), glm::vec4(key[1].as<float>(), key[2].as<float>(), key[3].as<float>(), key[4].as<float>()) });
                }
            }
            emitter.SizeOverLife.clear();
            if (const YAML::Node keys = particleEmitter["SizeOverLife"]; keys && keys.IsSequence())
            {
                for (const YAML::Node& key : keys)
                {
                    if (key.IsSequence() && key.size() == 2)
                        emitter.SizeOverLife.push_back({ key[0].as<float>(), key[1].as<float>() });
                }
            }
            read("Intensity", emitter.Intensity);
            read("Texture", emitter.Texture);
            read("FlipbookColumns", emitter.FlipbookColumns);
            read("FlipbookRows", emitter.FlipbookRows);
            read("FlipbookFPS", emitter.FlipbookFPS);
            if (particleEmitter["Blend"])
                emitter.Blend = particleEmitter["Blend"].as<std::string>() == "Alpha" ? ParticleBlend::Alpha : ParticleBlend::Additive;
            read("Lit", emitter.Lit);
            read("SoftParticles", emitter.SoftParticles);
            read("SoftDistance", emitter.SoftDistance);
            read("LocalSpace", emitter.LocalSpace);
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
            if (animatorComponent["Graph"])
                ac.Graph = animatorComponent["Graph"].as<AssetHandle>();
            if (animatorComponent["RootMotion"])
                ac.RootMotion = animatorComponent["RootMotion"].as<bool>();
            if (animatorComponent["RootBone"])
                ac.RootBone = animatorComponent["RootBone"].as<std::string>();
            if (auto parameters = animatorComponent["GraphParameters"])
            {
                for (const auto& paramData : parameters)
                {
                    std::string name = paramData["Name"].as<std::string>();
                    NodeGraphValue value;
                    if (paramData["Value"] && NodeGraphSerializer::ReadValue(paramData["Value"], value))
                        ac.GraphInstance.Parameters[name] = value;
                }
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

        auto scriptComponent = entity["ScriptComponent"];
        if (scriptComponent)
        {
            auto& sc = deserializedEntity.AddComponent<ScriptComponent>();
            for (const auto& classNode : scriptComponent["Classes"])
                sc.ClassNames.push_back(classNode.as<std::string>());

            for (const auto& referenceNode : scriptComponent["EntityReferences"])
            {
                const std::string className = referenceNode["Class"].as<std::string>();
                const std::string fieldName = referenceNode["Field"].as<std::string>();
                auto& reference = sc.EntityReferences[className][fieldName];
                if (referenceNode["ModelInstance"])
                {
                    reference.ModelInstance = referenceNode["ModelInstance"].as<UUID>();
                    reference.ModelNodeIndex = referenceNode["ModelNode"].as<uint32_t>();
                }
                else
                {
                    reference.Entity = referenceNode["Entity"].as<UUID>();
                }
            }

            for (const auto& fieldNode : scriptComponent["FieldOverrides"])
            {
                const std::string className = fieldNode["Class"].as<std::string>();
                const std::string fieldName = fieldNode["Field"].as<std::string>();
                const std::string type = fieldNode["Type"].as<std::string>();
                const YAML::Node value = fieldNode["Value"];
                auto& overrideValue = sc.FieldOverrides[className][fieldName];
                if (type == "Bool") overrideValue = value.as<bool>();
                else if (type == "Int") overrideValue = value.as<int32_t>();
                else if (type == "UInt") overrideValue = value.as<uint32_t>();
                else if (type == "Long") overrideValue = value.as<int64_t>();
                else if (type == "ULong") overrideValue = value.as<uint64_t>();
                else if (type == "Float") overrideValue = value.as<float>();
                else if (type == "Double") overrideValue = value.as<double>();
                else if (type == "String") overrideValue = value.as<std::string>();
                else if (type == "Vector3") overrideValue = value.as<glm::vec3>();
            }
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

                        // A field the script no longer has (renamed or removed since the scene was saved): its value is dropped.
                        if (fields.find(name) == fields.end())
                        {
                            NOX_CORE_WARN("SceneSerializer: script field '{}' is no longer on the entity's script, its saved value is dropped", name);
                            continue;
                        }

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
            if (boxCollider2DComponent["RollingResistance"])
                bc2d.RollingResistance = boxCollider2DComponent["RollingResistance"].as<float>();
            if (boxCollider2DComponent["TangentSpeed"])
                bc2d.TangentSpeed = boxCollider2DComponent["TangentSpeed"].as<float>();
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
            if (circleCollider2DComponent["RollingResistance"])
                cc2d.RollingResistance = circleCollider2DComponent["RollingResistance"].as<float>();
            if (circleCollider2DComponent["TangentSpeed"])
                cc2d.TangentSpeed = circleCollider2DComponent["TangentSpeed"].as<float>();
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
            if (sphereCollider3DComponent["RollingResistance"])
                sc3d.RollingResistance = sphereCollider3DComponent["RollingResistance"].as<float>();
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
            if (capsuleCollider3DComponent["RollingResistance"])
                cc3d.RollingResistance = capsuleCollider3DComponent["RollingResistance"].as<float>();
        }

        auto characterController3DComponent = entity["CharacterController3DComponent"];
        if (characterController3DComponent)
        {
            auto& cct = deserializedEntity.AddComponent<CharacterController3DComponent>();
            if (characterController3DComponent["Radius"])
                cct.Radius = characterController3DComponent["Radius"].as<float>();
            if (characterController3DComponent["Height"])
                cct.Height = characterController3DComponent["Height"].as<float>();
            if (characterController3DComponent["StepHeight"])
                cct.StepHeight = characterController3DComponent["StepHeight"].as<float>();
            if (characterController3DComponent["MaxSlopeDegrees"])
                cct.MaxSlopeDegrees = characterController3DComponent["MaxSlopeDegrees"].as<float>();
            if (characterController3DComponent["GravityScale"])
                cct.GravityScale = characterController3DComponent["GravityScale"].as<float>();
            if (characterController3DComponent["AirControl"])
                cct.AirControl = characterController3DComponent["AirControl"].as<float>();
            if (characterController3DComponent["MaxAcceleration"])
                cct.MaxAcceleration = characterController3DComponent["MaxAcceleration"].as<float>();
            if (characterController3DComponent["BrakingDeceleration"])
                cct.BrakingDeceleration = characterController3DComponent["BrakingDeceleration"].as<float>();
        }

        if (auto socketNode = entity["BoneSocketComponent"])
        {
            auto& socket = deserializedEntity.AddOrReplaceComponent<BoneSocketComponent>();
            if (socketNode["Bone"])
                socket.Bone = socketNode["Bone"].as<std::string>();
            if (socketNode["Offset"])
                socket.Offset = socketNode["Offset"].as<glm::vec3>();
            if (socketNode["RotationOffset"])
                socket.RotationOffset = socketNode["RotationOffset"].as<glm::vec3>();
            if (socketNode["Scale"])
                socket.Scale = socketNode["Scale"].as<glm::vec3>();
        }

        auto textComponent = entity["TextComponent"];
        if (textComponent)
        {
            auto& tc = deserializedEntity.AddComponent<TextComponent>();
            tc.TextString = textComponent["TextString"].as<std::string>();
            if (textComponent["Font"])
                tc.Font = textComponent["Font"].as<uint64_t>();
            tc.Color = textComponent["Color"].as<glm::vec4>();
            tc.Kerning = textComponent["Kerning"].as<float>();
            tc.LineSpacing = textComponent["LineSpacing"].as<float>();
            if (textComponent["OutlineColor"])
                tc.OutlineColor = textComponent["OutlineColor"].as<glm::vec4>();
            if (textComponent["OutlineThickness"])
                tc.OutlineThickness = textComponent["OutlineThickness"].as<float>();
            if (textComponent["BackgroundColor"])
                tc.BackgroundColor = textComponent["BackgroundColor"].as<glm::vec4>();
            if (textComponent["BackgroundPadding"])
                tc.BackgroundPadding = textComponent["BackgroundPadding"].as<float>();
        }
    }

    bool SceneSerializer::Deserialize(const std::filesystem::path& filepath)
    {
        std::string bytes;
        if (!VFS::ReadText(filepath, bytes)) // a loose file, else a mounted pack's entry
        {
            NOX_CORE_ERROR("Failed to load .nox file `{0}`: could not open it", filepath.string());
            return false;
        }
        // A scene saved in its runtime form (a packaged game's) under the same name reads the same way.
        if (IsRuntimeScene(bytes))
            return DeserializeRuntimeBytes(bytes, filepath.string());

        YAML::Node data;
        try
        {
            data = YAML::Load(bytes);
        }
        catch (YAML::ParserException& e)
        {
            NOX_CORE_ERROR("Failed to load .nox file `{0}`\n       {1}", filepath.string(), e.what());
            return false;
        }

        return ReadScene(data);
    }

    bool SceneSerializer::DeserializeText(const std::string& text)
    {
        YAML::Node data;
        try
        {
            data = YAML::Load(text);
        }
        catch (YAML::ParserException& e)
        {
            NOX_CORE_ERROR("Failed to read scene text\n       {0}", e.what());
            return false;
        }

        return ReadScene(data);
    }

    bool SceneSerializer::ReadScene(const YAML::Node& data)
    {
        if (!data["Scene"])
            return false;

        std::string sceneName = data["Scene"].as<std::string>();
        NOX_CORE_TRACE("Deserializing scene '{0}'", sceneName);

        if (auto folders = data["Folders"])
        {
            for (const auto& folder : folders)
                m_Scene->AddFolder(folder.as<std::string>());
        }
        ReadWorldPartition(*m_Scene, data);

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

                DeserializeEntityComponents(*m_Scene, entity, deserializedEntity, false);
            }
        }

        return true;
    }

    bool SceneSerializer::DeserializeRuntime(const std::filesystem::path& filepath)
    {
        std::string bytes;
        if (!VFS::ReadText(filepath, bytes))
        {
            NOX_CORE_ERROR("Failed to load runtime scene '{}': could not open the file", filepath.string());
            return false;
        }
        return DeserializeRuntimeBytes(bytes, filepath.string());
    }

    bool SceneSerializer::RuntimeSceneText(const std::string& bytes, const std::string& name, std::string& outText)
    {
        constexpr size_t headerSize = sizeof(RuntimeSceneMagic) + sizeof(uint32_t) + sizeof(uint64_t);
        if (!IsRuntimeScene(bytes) || bytes.size() < headerSize)
        {
            NOX_CORE_ERROR("'{}' is not a runtime scene", name);
            return false;
        }
        uint32_t version = 0;
        uint64_t size = 0;
        std::memcpy(&version, bytes.data() + sizeof(RuntimeSceneMagic), sizeof(version));
        std::memcpy(&size, bytes.data() + sizeof(RuntimeSceneMagic) + sizeof(version), sizeof(size));
        if (version != RuntimeSceneVersion)
        {
            NOX_CORE_ERROR("Runtime scene '{}' is version {}, this build reads version {}", name, version, RuntimeSceneVersion);
            return false;
        }
        if (size != bytes.size() - headerSize)
        {
            NOX_CORE_ERROR("Runtime scene '{}' is cut off or padded ({} bytes of scene, {} expected)", name, bytes.size() - headerSize, size);
            return false;
        }
        outText = bytes.substr(headerSize);
        return true;
    }

    bool SceneSerializer::DeserializeRuntimeBytes(const std::string& bytes, const std::string& name)
    {
        std::string text;
        return RuntimeSceneText(bytes, name, text) && DeserializeText(text);
    }

    // --- Cooked world cells (§5.13.4) ---

    namespace
    {
        constexpr char CookedCellMagic[8] = { 'N', 'O', 'X', 'C', 'E', 'L', 'L', 'B' };
        constexpr uint32_t CookedCellVersion = 2; // 2: translations in double precision (large world coordinates)
        constexpr size_t CookedCellHeaderSize = sizeof(CookedCellMagic) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t);

        enum CookedEntityKind : uint8_t { CookedBinary = 0, CookedText = 1 };
        enum CookedComponents : uint8_t
        {
            CookedTag = 1 << 0,
            CookedTransform = 1 << 1,
            CookedRelationship = 1 << 2,
            CookedMesh = 1 << 3,
            CookedMaterial = 1 << 4,
            CookedFolder = 1 << 5,
        };
        // Optional fields inside a component, as the text has them or not.
        enum CookedFields : uint8_t
        {
            CookedChildren = 1 << 0,     // RelationshipComponent: Children
            CookedMeshHandle = 1 << 1,   // MeshComponent: MeshHandle, SubmeshIndex, SubmeshCount
            CookedSubmeshIndex = 1 << 2,
            CookedSubmeshCount = 1 << 3,
            CookedMaterialAssets = 1 << 4, // MaterialComponent: MaterialAssets
            CookedFolderPath = 1 << 5,     // FolderComponent: Path
        };

        struct ByteWriter
        {
            std::string Bytes;
            template <typename T> void Put(const T& value) { Bytes.append(reinterpret_cast<const char*>(&value), sizeof(T)); }
            void PutString(const std::string& text)
            {
                Put(static_cast<uint32_t>(text.size()));
                Bytes.append(text);
            }
        };

        struct ByteReader
        {
            const std::string& Bytes;
            size_t Offset = 0;
            bool Failed = false;
            template <typename T> T Get()
            {
                T value{};
                if (Offset + sizeof(T) > Bytes.size())
                {
                    Failed = true;
                    return value;
                }
                std::memcpy(&value, Bytes.data() + Offset, sizeof(T));
                Offset += sizeof(T);
                return value;
            }
            std::string GetString()
            {
                const uint32_t size = Get<uint32_t>();
                if (Failed || Offset + size > Bytes.size())
                {
                    Failed = true;
                    return {};
                }
                std::string text(Bytes.data() + Offset, size);
                Offset += size;
                return text;
            }
        };

        // Whether a map's keys are all among `allowed`.
        bool OnlyKeys(const YAML::Node& map, std::initializer_list<const char*> allowed)
        {
            if (!map.IsMap())
                return false;
            for (const auto& entry : map)
            {
                const std::string key = entry.first.as<std::string>();
                if (std::none_of(allowed.begin(), allowed.end(), [&](const char* name) { return key == name; }))
                    return false;
            }
            return true;
        }

        // One entity's text as values, when it has nothing else (any other key: false, it stays text). The fields are those
        // DeserializeEntityComponents reads, with the same optional ones.
        bool EncodeCookedEntity(const YAML::Node& entity, ByteWriter& out)
        {
            if (!OnlyKeys(entity, { "Entity", "TagComponent", "TransformComponent", "RelationshipComponent", "MeshComponent", "MaterialComponent", "FolderComponent" }))
                return false;
            uint8_t components = 0, fields = 0;
            const YAML::Node tag = entity["TagComponent"], transform = entity["TransformComponent"], relationship = entity["RelationshipComponent"];
            const YAML::Node mesh = entity["MeshComponent"], material = entity["MaterialComponent"], folder = entity["FolderComponent"];
            if (tag && !OnlyKeys(tag, { "Tag" }) || transform && !OnlyKeys(transform, { "Translation", "Rotation", "Scale" }) ||
                relationship && !OnlyKeys(relationship, { "Parent", "Children" }) ||
                mesh && !OnlyKeys(mesh, { "MeshHandle", "SubmeshIndex", "SubmeshCount" }) || material && !OnlyKeys(material, { "MaterialAssets" }) ||
                folder && !OnlyKeys(folder, { "Path" }))
                return false;
            if (tag) components |= CookedTag;
            if (transform) components |= CookedTransform;
            if (relationship) components |= CookedRelationship;
            if (mesh) components |= CookedMesh;
            if (material) components |= CookedMaterial;
            if (folder) components |= CookedFolder;
            if (relationship && relationship["Children"]) fields |= CookedChildren;
            if (mesh && mesh["MeshHandle"]) fields |= CookedMeshHandle;
            if (mesh && mesh["SubmeshIndex"]) fields |= CookedSubmeshIndex;
            if (mesh && mesh["SubmeshCount"]) fields |= CookedSubmeshCount;
            if (material && material["MaterialAssets"]) fields |= CookedMaterialAssets;
            if (folder && folder["Path"]) fields |= CookedFolderPath;

            out.Put(entity["Entity"].as<uint64_t>());
            out.Put(components);
            out.Put(fields);
            if (tag)
                out.PutString(tag["Tag"].as<std::string>());
            if (transform)
            {
                out.Put(transform["Translation"].as<glm::dvec3>()); // double: large world coordinates (version 2)
                out.Put(transform["Rotation"].as<glm::vec3>());
                out.Put(transform["Scale"].as<glm::vec3>());
            }
            if (relationship)
            {
                out.Put(relationship["Parent"].as<uint64_t>());
                if (fields & CookedChildren)
                {
                    const YAML::Node children = relationship["Children"];
                    out.Put(static_cast<uint32_t>(children.size()));
                    for (const YAML::Node& child : children)
                        out.Put(child.as<uint64_t>());
                }
            }
            if (fields & CookedMeshHandle) out.Put(mesh["MeshHandle"].as<uint64_t>());
            if (fields & CookedSubmeshIndex) out.Put(mesh["SubmeshIndex"].as<uint32_t>());
            if (fields & CookedSubmeshCount) out.Put(mesh["SubmeshCount"].as<uint32_t>());
            if (fields & CookedMaterialAssets)
            {
                const YAML::Node assets = material["MaterialAssets"];
                out.Put(static_cast<uint32_t>(assets.size()));
                for (const YAML::Node& asset : assets)
                    out.Put(asset.as<uint64_t>());
            }
            if (fields & CookedFolderPath)
                out.PutString(folder["Path"].as<std::string>());
            return true;
        }

        // A binary entity into a scene, as DeserializeEntityComponents makes it from its text. Null when the bytes run out.
        Entity DecodeCookedEntity(ByteReader& in, Scene& scene, UUID& outID, bool& outSkipped)
        {
            outSkipped = false;
            outID = in.Get<uint64_t>();
            const uint8_t components = in.Get<uint8_t>();
            const uint8_t fields = in.Get<uint8_t>();
            std::string name;
            if (components & CookedTag)
                name = in.GetString();
            glm::dvec3 translation{};
            glm::vec3 rotation{}, scale{};
            if (components & CookedTransform)
            {
                translation = in.Get<glm::dvec3>();
                rotation = in.Get<glm::vec3>();
                scale = in.Get<glm::vec3>();
            }
            uint64_t parent = 0;
            std::vector<UUID> children;
            if (components & CookedRelationship)
            {
                parent = in.Get<uint64_t>();
                if (fields & CookedChildren)
                {
                    const uint32_t count = in.Get<uint32_t>();
                    if (!in.Failed && count <= (in.Bytes.size() - in.Offset) / sizeof(uint64_t))
                    {
                        children.reserve(count);
                        for (uint32_t index = 0; index < count; ++index)
                            children.push_back(in.Get<uint64_t>());
                    }
                    else
                        in.Failed = true;
                }
            }
            const uint64_t meshHandle = (fields & CookedMeshHandle) ? in.Get<uint64_t>() : 0;
            const uint32_t submeshIndex = (fields & CookedSubmeshIndex) ? in.Get<uint32_t>() : 0;
            const uint32_t submeshCount = (fields & CookedSubmeshCount) ? in.Get<uint32_t>() : 0;
            std::vector<AssetHandle> materials;
            if (fields & CookedMaterialAssets)
            {
                const uint32_t count = in.Get<uint32_t>();
                if (!in.Failed && count <= (in.Bytes.size() - in.Offset) / sizeof(uint64_t))
                {
                    materials.reserve(count);
                    for (uint32_t index = 0; index < count; ++index)
                        materials.push_back(in.Get<uint64_t>());
                }
                else
                    in.Failed = true;
            }
            const std::string folderPath = (fields & CookedFolderPath) ? in.GetString() : std::string();
            if (in.Failed)
                return {};
            if (scene.GetEntityByUUID(outID))
            {
                outSkipped = true;
                return {};
            }

            Entity entity = scene.CreateEntityWithUUID(outID, name);
            if (components & CookedTransform)
            {
                TransformComponent& transform = entity.GetComponent<TransformComponent>();
                transform.Translation = translation;
                transform.Rotation = rotation;
                transform.Scale = scale;
            }
            if (components & CookedRelationship)
            {
                RelationshipComponent& relationship = entity.AddComponent<RelationshipComponent>();
                relationship.Parent = parent;
                relationship.Children = std::move(children);
            }
            if (components & CookedMesh)
            {
                MeshComponent& mesh = entity.AddComponent<MeshComponent>();
                if (fields & CookedMeshHandle) mesh.Mesh = meshHandle;
                if (fields & CookedSubmeshIndex) mesh.SubmeshIndex = submeshIndex;
                if (fields & CookedSubmeshCount) mesh.SubmeshCount = submeshCount;
            }
            if (fields & CookedFolderPath)
                entity.AddComponent<FolderComponent>(folderPath);
            if (components & CookedMaterial)
            {
                MaterialComponent& material = entity.AddComponent<MaterialComponent>();
                if (fields & CookedMaterialAssets)
                    material.MaterialAssets = std::move(materials);
            }
            return entity;
        }
    }

    std::string SceneSerializer::CookWorldCell(const std::string& cellText, size_t* outBinaryEntities, size_t* outEntities)
    {
        YAML::Node document;
        try
        {
            document = YAML::Load(cellText);
        }
        catch (const YAML::Exception&)
        {
            return {};
        }
        const YAML::Node entities = document["Entities"];
        const uint32_t count = entities && entities.IsSequence() ? static_cast<uint32_t>(entities.size()) : 0;

        ByteWriter out;
        out.Bytes.reserve(cellText.size() / 3);
        out.Bytes.append(CookedCellMagic, sizeof(CookedCellMagic));
        out.Put(CookedCellVersion);
        out.Put(Hash::compute(cellText));
        out.Put(count);

        // The check: each binary entity built from its text and from its values (scenes of their own), written back as text.
        Ref<Scene> fromText = CreateRef<Scene>(), fromValues = CreateRef<Scene>();
        size_t binary = 0;
        for (uint32_t index = 0; index < count; ++index)
        {
            const YAML::Node entity = entities[index];
            ByteWriter values;
            bool same = false;
            try
            {
                if (EncodeCookedEntity(entity, values))
                {
                    std::string name;
                    if (const YAML::Node tag = entity["TagComponent"])
                        name = tag["Tag"].as<std::string>();
                    Entity expected = fromText->CreateEntityWithUUID(entity["Entity"].as<uint64_t>(), name);
                    DeserializeEntityComponents(*fromText, entity, expected, false);
                    ByteReader reader{ values.Bytes };
                    UUID id;
                    bool skipped = false;
                    Entity decoded = DecodeCookedEntity(reader, *fromValues, id, skipped);
                    same = decoded && !reader.Failed && reader.Offset == values.Bytes.size() &&
                           EntityToText(*fromText, expected) == EntityToText(*fromValues, decoded);
                    fromText->DestroyEntity(expected);
                    if (decoded)
                        fromValues->DestroyEntity(decoded);
                }
            }
            catch (const YAML::Exception&)
            {
                same = false;
            }
            if (same)
            {
                out.Put(static_cast<uint8_t>(CookedBinary));
                out.Bytes.append(values.Bytes);
                ++binary;
                continue;
            }
            YAML::Emitter text;
            text << entity;
            out.Put(static_cast<uint8_t>(CookedText));
            out.PutString(std::string(text.c_str(), text.size()));
        }
        if (outBinaryEntities)
            *outBinaryEntities = binary;
        if (outEntities)
            *outEntities = count;
        return std::move(out.Bytes);
    }

    bool SceneSerializer::IsCookedWorldCellFor(const std::string& cooked, uint64_t textHash)
    {
        if (cooked.size() < CookedCellHeaderSize || !std::equal(std::begin(CookedCellMagic), std::end(CookedCellMagic), cooked.begin()))
            return false;
        uint32_t version = 0;
        uint64_t hash = 0;
        std::memcpy(&version, cooked.data() + sizeof(CookedCellMagic), sizeof(version));
        std::memcpy(&hash, cooked.data() + sizeof(CookedCellMagic) + sizeof(version), sizeof(hash));
        return version == CookedCellVersion && hash == textHash;
    }

    bool SceneSerializer::ReadCookedWorldCell(const std::string& cooked, Scene& scene, AssetHandle level, const std::string& name,
                                              std::vector<UUID>& outCreated, std::string& outError, const std::function<bool()>& cancelled)
    {
        ByteReader in{ cooked, sizeof(CookedCellMagic) + sizeof(uint32_t) + sizeof(uint64_t) };
        const uint32_t count = in.Get<uint32_t>();
        if (in.Failed)
        {
            outError = "cut off";
            return false;
        }
        outCreated.reserve(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            if ((index & 63) == 0 && cancelled && cancelled())
            {
                outError = "cancelled";
                return false;
            }
            const uint8_t kind = in.Get<uint8_t>();
            Entity created;
            UUID id = 0;
            if (kind == CookedBinary)
            {
                bool skipped = false;
                created = DecodeCookedEntity(in, scene, id, skipped);
                if (skipped)
                {
                    NOX_CORE_WARN("World cell `{}`: entity {} is in the scene already, kept as it is", name, static_cast<uint64_t>(id));
                    continue;
                }
            }
            else if (kind == CookedText)
            {
                const std::string text = in.GetString();
                if (in.Failed)
                    break;
                try
                {
                    const YAML::Node entity = YAML::Load(text);
                    id = entity["Entity"].as<uint64_t>();
                    if (scene.GetEntityByUUID(id))
                    {
                        NOX_CORE_WARN("World cell `{}`: entity {} is in the scene already, kept as it is", name, static_cast<uint64_t>(id));
                        continue;
                    }
                    std::string tag;
                    if (const YAML::Node tagNode = entity["TagComponent"])
                        tag = tagNode["Tag"].as<std::string>();
                    created = scene.CreateEntityWithUUID(id, tag);
                    DeserializeEntityComponents(scene, entity, created, false);
                }
                catch (const YAML::Exception& exception)
                {
                    outError = exception.what();
                    return false;
                }
            }
            else
                in.Failed = true;
            if (in.Failed || !created)
            {
                outError = "damaged (entity " + std::to_string(index) + ")";
                return false;
            }
            if (level != 0)
            {
                created.AddComponent<LevelComponent>().Level = level;
                outCreated.push_back(id);
            }
        }
        if (in.Failed)
        {
            outError = "cut off";
            return false;
        }
        return true;
    }
}
