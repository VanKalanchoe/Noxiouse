#include "McpEditorTools.h"
#include "McpToolHelpers.h"
#include "McpValidation.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <map>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/SceneSerializer.h"

// MCP2 (docs/MCP_Plan_2026-11.md §5): editing the open scene. Every write is validated first (the component's fields and their kinds
// from its example: its defaults under its current values; ids must name an existing asset or entity), applied through the scene
// serializer (what loading a scene does), read back (what the engine stored, values it clamped reported), rolled back when it fails,
// and recorded as one undo step. Edit mode only: Play and Simulate change a copy that Stop throws away.
namespace Nox::Mcp
{
    namespace
    {
        // ------------------------------------------------------------------------------------------------- components by name

        struct ComponentOps
        {
            const char* Name;
            bool (*Has)(Entity);
            void (*Add)(Entity);
            void (*Remove)(Entity);
            // The scene file leaves some fields out while they are empty or at their default (MaterialComponent's MaterialAssets, a text's
            // outline, a joint axis's drive). Sample sets each of them on a scratch component so the serializer writes it: the tools then
            // know every field and the shape of list items -- learnt from the serializer itself, not a second list of fields. SampleFixes
            // (JSON Patch on what was written) turns sample values into what the check needs: an asset reference back to 0 (takes any id),
            // a value whose kind varies to null (takes anything).
            void (*Sample)(Entity) = nullptr;
            const char* SampleFixes = nullptr;
        };

        template<typename T>
        ComponentOps MakeOps(const char* name, void (*sample)(Entity) = nullptr, const char* fixes = nullptr)
        {
            return { name, [](Entity entity) { return entity.HasComponent<T>(); }, [](Entity entity) { entity.AddComponent<T>(); },
                     [](Entity entity) { entity.RemoveComponent<T>(); }, sample, fixes };
        }
#define NOX_MCP_COMPONENT(T) MakeOps<T>(#T)

        // The components a tool may add, remove and edit (their scene-file names are their type names). Left out: the ones the editor
        // manages itself -- Tag (rename_entity), Relationship (set_parent), Transform (patch only), prefab / model instances, folders,
        // HLODs, the hidden bookkeeping ones.
        const std::vector<ComponentOps>& EditableComponents()
        {
            // (every field the serializer writes only when set: SceneSerializer::SerializeEntity's conditional keys)
            static const std::vector<ComponentOps> components = {
                MakeOps<MeshComponent>("MeshComponent", [](Entity e) { e.GetComponent<MeshComponent>().MorphWeights["*"] = 1.0f; }, nullptr),
                MakeOps<MaterialComponent>("MaterialComponent", [](Entity e)
                {
                    auto& material = e.GetComponent<MaterialComponent>();
                    material.MaterialAssets = { AssetHandle(1) };
                    material.Parameters = { MaterialComponent::Parameter{ MaterialParameter::BaseColor, glm::vec4(1.0f), 0 } };
                }, R"([{ "op": "replace", "path": "/MaterialAssets/0", "value": 0 }])"),
                NOX_MCP_COMPONENT(MetadataComponent),
                NOX_MCP_COMPONENT(DirectionalLightComponent), NOX_MCP_COMPONENT(PointLightComponent), NOX_MCP_COMPONENT(SpotLightComponent),
                NOX_MCP_COMPONENT(RectLightComponent), NOX_MCP_COMPONENT(DiscLightComponent), NOX_MCP_COMPONENT(ReflectionProbeComponent),
                NOX_MCP_COMPONENT(IrradianceVolumeComponent), NOX_MCP_COMPONENT(EnvironmentLightComponent), NOX_MCP_COMPONENT(AnimatorComponent),
                NOX_MCP_COMPONENT(TimelinePlayerComponent), MakeOps<AudioSourceComponent>("AudioSourceComponent", [](Entity e) { e.GetComponent<AudioSourceComponent>().GraphParameters["Parameter"] = 1.0f; },
                                          R"([{ "op": "replace", "path": "/GraphParameters/0/Value", "value": null }])"), NOX_MCP_COMPONENT(ImpactSoundComponent),
                NOX_MCP_COMPONENT(AudioListenerComponent), NOX_MCP_COMPONENT(AudioVolumeComponent), NOX_MCP_COMPONENT(AcousticProbeVolumeComponent), NOX_MCP_COMPONENT(PostProcessVolumeComponent),
                MakeOps<ParticleEmitterComponent>("ParticleEmitterComponent", [](Entity e)
                {
                    auto& emitter = e.GetComponent<ParticleEmitterComponent>();
                    emitter.ColorOverLife = { ParticleColorKey{} };
                    emitter.SizeOverLife = { ParticleSizeKey{} };
                    emitter.BurstSound = AssetHandle(1);
                }, R"([{ "op": "replace", "path": "/BurstSound", "value": 0 }])"), NOX_MCP_COMPONENT(SpriteRendererComponent), NOX_MCP_COMPONENT(CircleRendererComponent),
                NOX_MCP_COMPONENT(CameraComponent), NOX_MCP_COMPONENT(ScriptComponent), NOX_MCP_COMPONENT(RigidBody2DComponent),
                NOX_MCP_COMPONENT(BoxCollider2DComponent), NOX_MCP_COMPONENT(CircleCollider2DComponent), MakeOps<RigidBody3DComponent>("RigidBody3DComponent", [](Entity e) { e.GetComponent<RigidBody3DComponent>().PhysicalSurface = "Surface"; }, nullptr),
                NOX_MCP_COMPONENT(BoxCollider3DComponent), NOX_MCP_COMPONENT(SphereCollider3DComponent), MakeOps<CapsuleCollider3DComponent>("CapsuleCollider3DComponent", [](Entity e) { e.GetComponent<CapsuleCollider3DComponent>().BottomRadius = 1.0f; }, nullptr),
                NOX_MCP_COMPONENT(CylinderCollider3DComponent), NOX_MCP_COMPONENT(PlaneCollider3DComponent), NOX_MCP_COMPONENT(MeshCollider3DComponent),
                MakeOps<PhysicsJoint3DComponent>("PhysicsJoint3DComponent", [](Entity e) { e.GetComponent<PhysicsJoint3DComponent>().Drives[0].Enabled = true; }, nullptr), NOX_MCP_COMPONENT(CollisionFilter3DComponent), NOX_MCP_COMPONENT(CharacterController3DComponent),
                MakeOps<TextComponent>("TextComponent", [](Entity e)
                {
                    auto& text = e.GetComponent<TextComponent>();
                    text.OutlineThickness = 0.1f;
                    text.BackgroundColor.a = 1.0f;
                }, nullptr), NOX_MCP_COMPONENT(BoneSocketComponent), NOX_MCP_COMPONENT(LevelStreamingVolumeComponent),
                NOX_MCP_COMPONENT(WorldPartitionComponent), NOX_MCP_COMPONENT(WorldStreamingSourceComponent), NOX_MCP_COMPONENT(NetworkIdentityComponent),
            };
            return components;
        }
#undef NOX_MCP_COMPONENT

        constexpr const char* TransformName = "TransformComponent";

        // "PointLight", "pointlightcomponent", "PointLightComponent": the scene file's name; empty when it is not one a tool may edit.
        std::string CanonicalComponent(const std::string& name, const ComponentOps** ops)
        {
            std::string wanted = Lower(name);
            if (wanted.size() < 9 || wanted.compare(wanted.size() - 9, 9, "component") != 0)
                wanted += "component";
            *ops = nullptr;
            if (wanted == Lower(TransformName))
                return TransformName;
            for (const ComponentOps& candidate : EditableComponents())
                if (Lower(candidate.Name) == wanted)
                {
                    *ops = &candidate;
                    return candidate.Name;
                }
            return {};
        }

        std::string ComponentList()
        {
            std::string list = TransformName;
            for (const ComponentOps& ops : EditableComponents())
                list += std::string(", ") + ops.Name;
            return list;
        }

        Json StoredComponent(Scene& scene, Entity entity, const std::string& name)
        {
            const YAML::Node node = SceneSerializer::EntityToNode(scene, entity);
            return node[name] ? NodeToJson(node[name]) : Json();
        }

        // A component as the scene file stores it, added to an entity of a scratch scene (with its sample filled in, or not) and
        // serialized. Cached.
        Json StoredOnScratch(const std::string& name, const ComponentOps* ops, bool sampled)
        {
            static std::map<std::string, Json> cache;
            const std::string key = name + (sampled ? "#sample" : "");
            if (auto found = cache.find(key); found != cache.end())
                return found->second;
            Json stored;
            try
            {
                Ref<Scene> scratch = CreateRef<Scene>();
                Entity entity = scratch->CreateEntity("Defaults");
                if (ops)
                    ops->Add(entity);
                if (sampled && ops && ops->Sample)
                    ops->Sample(entity);
                stored = StoredComponent(*scratch, entity, name);
                if (sampled && ops && ops->SampleFixes)
                {
                    Json fixed;
                    if (std::string problem = ApplyPatch(stored, Json::parse(ops->SampleFixes), fixed); problem.empty())
                        stored = fixed;
                    else
                        NOX_CORE_WARN("MCP: the sample of {} does not fit its fixes ({})", name, problem);
                }
            }
            catch (const std::exception& error)
            {
                NOX_CORE_WARN("MCP: {} could not be read from a scratch entity ({}): only its current fields are known", name, error.what());
            }
            cache[key] = stored;
            return stored;
        }

        // A component's defaults as the scene file stores them (its C++ defaults; fields left out while unset are missing here).
        Json DefaultsOf(const std::string& name, const ComponentOps* ops)
        {
            return StoredOnScratch(name, ops, false);
        }

        // Every field a component can have, with the shape of list items: its sample under its defaults.
        Json ShapeOf(const std::string& name, const ComponentOps* ops)
        {
            return MergeExample(StoredOnScratch(name, ops, true), DefaultsOf(name, ops));
        }

        Entity FindEntity(Scene& scene, const Json& value, std::string& error)
        {
            uint64_t id = 0;
            if (!ParseId(value, id))
            {
                error = std::format("{} is not an entity uuid (a string of digits)", value.dump());
                return {};
            }
            Entity entity = scene.GetEntityByUUID(UUID(id));
            if (!entity)
                error = std::format("no entity with uuid {} (get_hierarchy / find_entities list them)", value.dump());
            return entity;
        }

        // New ids written into a component must name something that exists: an asset of the project or an entity of the scene.
        std::string CheckReferences(Scene& scene, const Json& before, const Json& after, const std::string& path = {})
        {
            if (after.is_object())
            {
                for (auto it = after.begin(); it != after.end(); ++it)
                {
                    const Json previous = before.is_object() && before.contains(it.key()) ? before[it.key()] : Json();
                    if (std::string problem = CheckReferences(scene, previous, it.value(), path.empty() ? it.key() : path + "/" + it.key());
                        !problem.empty())
                        return problem;
                }
                return {};
            }
            if (after.is_array())
            {
                for (size_t index = 0; index < after.size(); ++index)
                {
                    const Json previous = before.is_array() && index < before.size() ? before[index] : Json();
                    if (std::string problem = CheckReferences(scene, previous, after[index], std::format("{}/{}", path, index)); !problem.empty())
                        return problem;
                }
                return {};
            }
            if (!IsIdText(after) || after == before)
                return {};
            uint64_t id = 0;
            ParseId(after, id);
            if (AssetManager::IsAssetHandleValid(AssetHandle(id)) || scene.GetEntityByUUID(UUID(id)))
                return {};
            return std::format("'{}': {} is neither an asset of the project nor an entity of the scene (list_assets / find_entities)", path,
                               after.get<std::string>());
        }

        // Writes a component's values the way loading a scene does; on failure the component is put back as it was.
        std::string WriteComponent(Scene& scene, Entity entity, const ComponentOps* ops, const std::string& name, const Json& values)
        {
            const YAML::Node entityNode = SceneSerializer::EntityToNode(scene, entity);
            const YAML::Node original = entityNode[name] ? YAML::Clone(entityNode[name]) : YAML::Node();
            YAML::Node node(YAML::NodeType::Map);
            node[name] = JsonToYaml(values);
            try
            {
                if (ops && ops->Has(entity))
                    ops->Remove(entity); // (the serializer adds components: the old one goes first)
                SceneSerializer::DeserializeEntityComponents(scene, node, entity, false);
                if (ops && !ops->Has(entity))
                    throw std::runtime_error("the scene format did not create it from these values");
                entity.MarkTransformDirty();
                return {};
            }
            catch (const std::exception& error)
            {
                // Back as it was (the same path, with the values it had).
                try
                {
                    if (ops && ops->Has(entity))
                        ops->Remove(entity);
                    if (original)
                    {
                        YAML::Node back(YAML::NodeType::Map);
                        back[name] = original;
                        SceneSerializer::DeserializeEntityComponents(scene, back, entity, false);
                    }
                }
                catch (const std::exception& restoreError)
                {
                    NOX_CORE_ERROR("MCP: {} could not be put back after a failed write ({}); undo restores it", name, restoreError.what());
                }
                return std::format("{} could not take these values ({}); nothing was changed", name, error.what());
            }
        }

        // ------------------------------------------------------------------------------------------------------------ the edits

        // One edit: its arguments (already checked against the tool's schema), what it answers, the undo step's name.
        struct Outcome
        {
            Json Result = Json::object();
            std::string Step;
        };
        using EditFunction = std::function<std::string(Scene& scene, const Json& arguments, Outcome& outcome)>;

        struct EditTool
        {
            Tool Definition;   // name, description, schema
            EditFunction Edit; // run on the edited scene (main thread, Edit mode)
        };

        // The patched component (values merged or JSON-Patch operations), checked against its example; "" or why not.
        std::string PrepareValues(Scene& scene, Entity entity, const std::string& name, const ComponentOps* ops, const Json& arguments,
                                  const Json& current, Json& values)
        {
            const Json example = MergeExample(ShapeOf(name, ops), current);
            const bool hasValues = arguments.contains("values"), hasOperations = arguments.contains("operations");
            if (hasValues == hasOperations)
                return "give either 'values' (fields to set) or 'operations' (a JSON Patch), not both or neither";
            if (hasValues)
                values = Merge(current, arguments["values"]);
            else if (std::string problem = ApplyPatch(current, arguments["operations"], values); !problem.empty())
                return problem;
            if (std::string problem = CheckAgainstExample(example, values); !problem.empty())
                return std::format("{}: {} (get_component_schema shows its fields)", name, problem);
            return CheckReferences(scene, current, values);
        }

        Json ComponentResult(Scene& scene, Entity entity, const std::string& name, const Json& requested)
        {
            const Json stored = StoredComponent(scene, entity, name);
            Json result{ { "uuid", IdText(static_cast<uint64_t>(entity.GetUUID())) }, { "component", name }, { "values", stored } };
            if (!requested.is_null())
            {
                const Json adjusted = Differences(requested, stored);
                if (!adjusted.empty())
                    result["adjusted"] = adjusted; // (what the engine stored instead of what was asked: clamped, an unknown enum name)
            }
            return result;
        }

        std::vector<EditTool> MakeEditTools(const EditorContext& context)
        {
            std::vector<EditTool> tools;
            auto add = [&](const char* name, const char* title, const char* description, Json schema, EditFunction edit, bool destructive = false)
            {
                EditTool tool;
                tool.Definition.Name = name;
                tool.Definition.Title = title;
                tool.Definition.Description = description;
                tool.Definition.InputSchema = std::move(schema);
                tool.Definition.Access = ConsoleAccess::Write;
                tool.Definition.Destructive = destructive;
                tool.Edit = std::move(edit);
                tools.push_back(std::move(tool));
            };

            // ------------------------------------------------------------------------------------------------- create_entity
            {
                Json schema = Schema()
                                  .String("name", "Its name", true)
                                  .String("parent", "The parent's uuid (left out: a root entity)")
                                  .Enum("shape", "A basic shape mesh to give it", context.ShapeNames ? context.ShapeNames() : std::vector<std::string>{})
                                  .Build();
                schema["properties"]["position"] = Json{ { "type", "array" }, { "description", "Position [x, y, z] in centimetres, relative to the parent" },
                                                         { "items", Json{ { "type", "number" } } }, { "minItems", 3 }, { "maxItems", 3 } };
                add("create_entity", "Create an entity",
                    "Creates an entity (with a transform) under the scene's root or 'parent', at 'position', optionally with a basic shape mesh. "
                    "Answers its uuid; add_component / patch_component give it more.",
                    schema,
                    [context](Scene& scene, const Json& arguments, Outcome& outcome) -> std::string
                    {
                        Entity parent;
                        if (arguments.contains("parent") && !arguments["parent"].get<std::string>().empty())
                        {
                            std::string error;
                            if (!(parent = FindEntity(scene, arguments["parent"], error)))
                                return error;
                        }
                        glm::dvec3 position(0.0);
                        if (arguments.contains("position"))
                        {
                            const Json& p = arguments["position"];
                            if (p.size() != 3 || !p[0].is_number() || !p[1].is_number() || !p[2].is_number())
                                return "'position' must be three numbers [x, y, z]";
                            position = glm::dvec3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
                        }
                        AssetHandle mesh = 0;
                        if (arguments.contains("shape"))
                        {
                            mesh = context.ShapeMesh ? context.ShapeMesh(arguments["shape"].get<std::string>()) : AssetHandle(0);
                            if (mesh == 0)
                                return std::format("the shape {} has no mesh asset", arguments["shape"].dump());
                        }
                        const std::string name = arguments["name"].get<std::string>();
                        Entity entity = scene.CreateEntity(name);
                        if (parent)
                            entity.SetParent(parent);
                        entity.GetComponent<TransformComponent>().Translation = position;
                        entity.MarkTransformDirty();
                        if (mesh != 0)
                        {
                            YAML::Node node(YAML::NodeType::Map);
                            node["MeshComponent"]["MeshHandle"] = std::to_string(static_cast<uint64_t>(mesh));
                            SceneSerializer::DeserializeEntityComponents(scene, node, entity, true);
                        }
                        outcome.Result = EntitySummary(scene, entity);
                        outcome.Step = std::format("MCP: Create {}", name);
                        return {};
                    });
            }

            // -------------------------------------------------------------------------------------------- instantiate_prefab
            {
                Json schema = Schema()
                                  .String("prefab", "The prefab: its uuid, or its path under the asset folder (e.g. \"Prefabs/Door.nprefab\")", true)
                                  .String("parent", "The parent's uuid (left out: a root entity)")
                                  .String("name", "The instance's name (default: the prefab's file name)")
                                  .Build();
                schema["properties"]["position"] = Json{ { "type", "array" }, { "description", "Position [x, y, z] in centimetres, relative to the parent" },
                                                         { "items", Json{ { "type", "number" } } }, { "minItems", 3 }, { "maxItems", 3 } };
                add("instantiate_prefab", "Place a prefab",
                    "Places an instance of a prefab at 'position' (as dragging it from the Content Browser does): its root entity now, the "
                    "prefab's entities under it once the prefab has loaded (a frame or two; get_entity / get_hierarchy then). One undo step.",
                    schema,
                    [](Scene& scene, const Json& arguments, Outcome& outcome) -> std::string
                    {
                        std::string error;
                        const AssetHandle prefab = ResolveAsset(arguments["prefab"], AssetType::Prefab, error);
                        if (prefab == 0)
                            return error;
                        Entity parent;
                        if (arguments.contains("parent") && !arguments["parent"].get<std::string>().empty())
                            if (!(parent = FindEntity(scene, arguments["parent"], error)))
                                return error;
                        glm::dvec3 position(0.0);
                        if (arguments.contains("position"))
                        {
                            const Json& p = arguments["position"];
                            if (p.size() != 3 || !p[0].is_number() || !p[1].is_number() || !p[2].is_number())
                                return "'position' must be three numbers [x, y, z]";
                            position = glm::dvec3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
                        }
                        std::string name = arguments.value("name", std::string());
                        if (name.empty())
                            name = std::filesystem::path(AssetPath(prefab)).stem().string();
                        if (name.empty())
                            name = "Prefab";
                        Entity root = scene.CreateEntity(name);
                        if (parent)
                            root.SetParent(parent);
                        auto& instance = root.AddComponent<PrefabInstanceComponent>();
                        instance.Prefab = prefab;
                        instance.InitTransform = true; // (takes the prefab root's rotation and scale when it spawns)
                        root.GetComponent<TransformComponent>().Translation = position;
                        root.MarkTransformDirty();
                        AssetManager::RequestAsset(prefab);
                        outcome.Result = EntitySummary(scene, root);
                        outcome.Result["prefab"] = AssetPath(prefab);
                        outcome.Step = std::format("MCP: Place {}", name);
                        return {};
                    });
            }

            // ------------------------------------------------------------------------------------------------------ place_model
            {
                Json schema = Schema()
                                  .String("model", "The mesh asset (static or skeletal): its uuid, or its path under the asset folder", true)
                                  .String("parent", "The parent's uuid (left out: a root entity)")
                                  .String("name", "The entity's name (default: the asset's file name)")
                                  .Build();
                schema["properties"]["position"] = Json{ { "type", "array" }, { "description", "Position [x, y, z] in centimetres, relative to the parent" },
                                                         { "items", Json{ { "type", "number" } } }, { "minItems", 3 }, { "maxItems", 3 } };
                add("place_model", "Place an imported model",
                    "Places a mesh asset (an imported model: import_asset lists them) at 'position' exactly as dragging it from the Content "
                    "Browser does: a model instance that applies the file's own node transform (a glTF's rotation from Z-up to Y-up, its "
                    "offset) and spawns its parts, a skinned character with its animator. Use it rather than a bare MeshComponent (which "
                    "would show the raw mesh without its node transform). Its entities appear once the mesh has loaded (a frame or two); "
                    "focus_entity frames it after that. One undo step.",
                    schema,
                    [](Scene& scene, const Json& arguments, Outcome& outcome) -> std::string
                    {
                        std::string error;
                        AssetHandle model = ResolveAsset(arguments["model"], AssetType::StaticMesh, error);
                        if (model == 0)
                        {
                            std::string skeletalError;
                            model = ResolveAsset(arguments["model"], AssetType::Mesh, skeletalError);
                            if (model == 0)
                                return error.find("is a Prefab,") != std::string::npos ? error + " (instantiate_prefab places prefabs)"
                                                                                       : std::format("{} (a model is a StaticMesh or a Mesh asset)", error);
                        }
                        Entity parent;
                        if (arguments.contains("parent") && !arguments["parent"].get<std::string>().empty())
                            if (!(parent = FindEntity(scene, arguments["parent"], error)))
                                return error;
                        glm::dvec3 position(0.0);
                        if (arguments.contains("position"))
                        {
                            const Json& p = arguments["position"];
                            if (p.size() != 3 || !p[0].is_number() || !p[1].is_number() || !p[2].is_number())
                                return "'position' must be three numbers [x, y, z]";
                            position = glm::dvec3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
                        }
                        std::string name = arguments.value("name", std::string());
                        if (name.empty())
                            name = std::filesystem::path(AssetPath(model)).stem().string();
                        if (name.empty())
                            name = "Model";
                        // (EditorLayer::PlaceAssets: the same component and settings)
                        Entity root = scene.CreateEntity(name);
                        if (parent)
                            root.SetParent(parent);
                        auto& instance = root.AddComponent<ModelInstanceComponent>();
                        instance.Model = model;
                        instance.AtFileLayout = true; // (the file's node transform, as a drag places it)
                        root.GetComponent<TransformComponent>().Translation = position;
                        root.MarkTransformDirty();
                        AssetManager::RequestAsset(model);
                        outcome.Result = EntitySummary(scene, root);
                        outcome.Result["model"] = AssetPath(model);
                        outcome.Step = std::format("MCP: Place {}", name);
                        return {};
                    });
            }

            // ----------------------------------------------------------------------------------------------- delete_entities
            {
                Json schema = Schema().Build();
                schema["properties"]["uuids"] = Json{ { "type", "array" }, { "description", "The entities' uuids (their children go with them)" },
                                                      { "items", Json{ { "type", "string" } } }, { "minItems", 1 } };
                schema["required"] = Json::array({ "uuids" });
                add("delete_entities", "Delete entities", "Deletes entities and their children (an undo step brings them back).", schema,
                    [context](Scene& scene, const Json& arguments, Outcome& outcome) -> std::string
                    {
                        std::vector<Entity> entities;
                        for (const Json& id : arguments["uuids"])
                        {
                            std::string error;
                            Entity entity = FindEntity(scene, id, error);
                            if (!entity)
                                return error; // (all checked before any is deleted)
                            entities.push_back(entity);
                        }
                        if (context.Select)
                            context.Select({}); // (the selection would point at deleted entities)
                        std::vector<UUID> ids;
                        for (Entity entity : entities)
                            ids.push_back(entity.GetUUID());
                        size_t deleted = 0;
                        for (UUID id : ids)
                            if (Entity entity = scene.GetEntityByUUID(id)) // (a child may have gone with its parent already)
                            {
                                scene.DestroyEntity(entity);
                                ++deleted;
                            }
                        outcome.Result = Json{ { "deleted", deleted } };
                        outcome.Step = deleted == 1 ? "MCP: Delete Entity" : std::format("MCP: Delete {} Entities", deleted);
                        return {};
                    },
                    true);
            }

            // ------------------------------------------------------------------------------------------------- rename_entity
            add("rename_entity", "Rename an entity", "Gives an entity a new name.",
                Schema().String("uuid", "The entity's uuid", true).String("name", "The new name", true),
                [](Scene& scene, const Json& arguments, Outcome& outcome) -> std::string
                {
                    std::string error;
                    Entity entity = FindEntity(scene, arguments["uuid"], error);
                    if (!entity)
                        return error;
                    const std::string name = arguments["name"].get<std::string>();
                    if (name.empty())
                        return "the name must not be empty";
                    entity.GetComponent<TagComponent>().Tag = name;
                    outcome.Result = EntitySummary(scene, entity);
                    outcome.Step = std::format("MCP: Rename {}", name);
                    return {};
                });

            // ---------------------------------------------------------------------------------------------- duplicate_entity
            add("duplicate_entity", "Duplicate an entity", "Copies an entity with its components and children (new uuids); answers the copy.",
                Schema().String("uuid", "The entity's uuid", true),
                [](Scene& scene, const Json& arguments, Outcome& outcome) -> std::string
                {
                    std::string error;
                    Entity entity = FindEntity(scene, arguments["uuid"], error);
                    if (!entity)
                        return error;
                    Entity copy = scene.DuplicateEntity(entity);
                    if (!copy)
                        return "it could not be duplicated";
                    outcome.Result = EntitySummary(scene, copy);
                    outcome.Step = std::format("MCP: Duplicate {}", entity.GetName());
                    return {};
                });

            // ---------------------------------------------------------------------------------------------------- set_parent
            add("set_parent", "Set an entity's parent",
                "Moves an entity under another one ('parent'), or to the scene's root (empty 'parent'). It keeps its place in the world.",
                Schema().String("uuid", "The entity's uuid", true).String("parent", "The new parent's uuid (empty: the root)", true),
                [](Scene& scene, const Json& arguments, Outcome& outcome) -> std::string
                {
                    std::string error;
                    Entity entity = FindEntity(scene, arguments["uuid"], error);
                    if (!entity)
                        return error;
                    Entity parent;
                    if (!arguments["parent"].get<std::string>().empty())
                    {
                        if (!(parent = FindEntity(scene, arguments["parent"], error)))
                            return error;
                        // Not under itself or one of its own children.
                        for (Entity up = parent; up; )
                        {
                            if (up == entity)
                                return "an entity cannot be put under itself or one of its children";
                            const RelationshipComponent* relationship = RelationshipOf(up);
                            up = relationship && relationship->Parent != 0 ? scene.GetEntityByUUID(relationship->Parent) : Entity();
                        }
                    }
                    entity.SetParent(parent);
                    outcome.Result = Json{ { "uuid", IdText(static_cast<uint64_t>(entity.GetUUID())) }, { "path", PathOf(scene, entity) } };
                    outcome.Step = std::format("MCP: Parent {}", entity.GetName());
                    return {};
                });

            // ------------------------------------------------------------------------------------------------- add_component
            {
                Json schema = Schema().String("uuid", "The entity's uuid", true).String("component", "The component, e.g. PointLight or RigidBody3DComponent", true).Build();
                schema["properties"]["values"] = Json{ { "type", "object" }, { "description", "Fields to set over its defaults (get_component_schema)" } };
                add("add_component", "Add a component",
                    "Adds a component with its defaults, and optionally 'values' over them (checked like patch_component). Answers what was stored.",
                    schema,
                    [](Scene& scene, const Json& arguments, Outcome& outcome) -> std::string
                    {
                        std::string error;
                        Entity entity = FindEntity(scene, arguments["uuid"], error);
                        if (!entity)
                            return error;
                        const ComponentOps* ops = nullptr;
                        const std::string name = CanonicalComponent(arguments["component"].get<std::string>(), &ops);
                        if (name.empty())
                            return std::format("{} is not a component a tool may add ({})", arguments["component"].dump(), ComponentList());
                        if (!ops)
                            return "every entity has a TransformComponent already (patch_component changes it)";
                        if (ops->Has(entity))
                            return std::format("it has a {} already (patch_component changes it)", name);
                        ops->Add(entity);
                        Json requested;
                        if (arguments.contains("values"))
                        {
                            Json values;
                            const Json current = StoredComponent(scene, entity, name);
                            std::string problem = PrepareValues(scene, entity, name, ops, Json{ { "values", arguments["values"] } }, current, values);
                            if (problem.empty())
                                problem = WriteComponent(scene, entity, ops, name, values);
                            if (!problem.empty())
                            {
                                if (ops->Has(entity))
                                    ops->Remove(entity); // (not added after all)
                                return problem;
                            }
                            requested = values;
                        }
                        outcome.Result = ComponentResult(scene, entity, name, requested);
                        outcome.Step = std::format("MCP: Add {} to {}", name, entity.GetName());
                        return {};
                    });
            }

            // ---------------------------------------------------------------------------------------------- remove_component
            add("remove_component", "Remove a component", "Removes a component from an entity (not its transform).",
                Schema().String("uuid", "The entity's uuid", true).String("component", "The component's name", true),
                [](Scene& scene, const Json& arguments, Outcome& outcome) -> std::string
                {
                    std::string error;
                    Entity entity = FindEntity(scene, arguments["uuid"], error);
                    if (!entity)
                        return error;
                    const ComponentOps* ops = nullptr;
                    const std::string name = CanonicalComponent(arguments["component"].get<std::string>(), &ops);
                    if (name.empty())
                        return std::format("{} is not a component a tool may remove ({})", arguments["component"].dump(), ComponentList());
                    if (!ops)
                        return "the transform cannot be removed";
                    if (!ops->Has(entity))
                        return std::format("it has no {}", name);
                    ops->Remove(entity);
                    outcome.Result = EntitySummary(scene, entity);
                    outcome.Step = std::format("MCP: Remove {} from {}", name, entity.GetName());
                    return {};
                },
                true);

            // ----------------------------------------------------------------------------------------------- patch_component
            {
                Json schema = Schema().String("uuid", "The entity's uuid", true).String("component", "The component, e.g. Transform or PointLight", true).Build();
                schema["properties"]["values"] = Json{ { "type", "object" }, { "description", "Fields to set (the others keep their values)" } };
                schema["properties"]["operations"] = Json{ { "type", "array" },
                                                           { "description", "Or a JSON Patch: [{ op: add|remove|replace|test, path: \"/Color/0\", value }]" } };
                add("patch_component", "Change a component",
                    "Changes some of a component's fields: 'values' (fields to set) or 'operations' (JSON Patch). Checked against its fields and "
                    "kinds before anything changes (ids as digit strings, naming an existing asset / entity); answers the values stored and any the "
                    "engine 'adjusted' (clamped). World units are centimetres; rotations in radians.",
                    schema,
                    [](Scene& scene, const Json& arguments, Outcome& outcome) -> std::string
                    {
                        std::string error;
                        Entity entity = FindEntity(scene, arguments["uuid"], error);
                        if (!entity)
                            return error;
                        const ComponentOps* ops = nullptr;
                        const std::string name = CanonicalComponent(arguments["component"].get<std::string>(), &ops);
                        if (name.empty())
                            return std::format("{} is not a component a tool may change ({})", arguments["component"].dump(), ComponentList());
                        if (ops && !ops->Has(entity))
                            return std::format("it has no {} (add_component adds it)", name);
                        const Json current = StoredComponent(scene, entity, name);
                        if (current.is_null())
                            return std::format("its {} is not stored in the scene file: nothing to change", name);
                        Json values;
                        if (std::string problem = PrepareValues(scene, entity, name, ops, arguments, current, values); !problem.empty())
                            return problem;
                        if (std::string problem = WriteComponent(scene, entity, ops, name, values); !problem.empty())
                            return problem;
                        outcome.Result = ComponentResult(scene, entity, name, values);
                        outcome.Step = std::format("MCP: Edit {} of {}", name, entity.GetName());
                        return {};
                    });
            }

            return tools;
        }

        // A string argument "$3" stands for the uuid answered by edit 3 of the same batch.
        Json Resolve(const Json& value, const std::vector<std::string>& created, std::string& error)
        {
            if (value.is_string())
            {
                const std::string& text = value.get_ref<const std::string&>();
                if (text.size() > 1 && text[0] == '$' && std::all_of(text.begin() + 1, text.end(), [](char c) { return c >= '0' && c <= '9'; }))
                {
                    const size_t index = std::stoul(text.substr(1));
                    if (index >= created.size() || created[index].empty())
                    {
                        error = std::format("{} names an edit that comes later or answered no uuid", text);
                        return value;
                    }
                    return created[index];
                }
                return value;
            }
            if (value.is_object())
            {
                Json resolved = Json::object();
                for (auto it = value.begin(); it != value.end(); ++it)
                    resolved[it.key()] = Resolve(it.value(), created, error);
                return resolved;
            }
            if (value.is_array())
            {
                Json resolved = Json::array();
                for (const Json& item : value)
                    resolved.push_back(Resolve(item, created, error));
                return resolved;
            }
            return value;
        }

        // The scene the edits change, or why there is none (Edit mode only).
        Ref<Scene> EditableScene(const EditorContext& context, std::string& error)
        {
            const std::string mode = context.Mode ? context.Mode() : std::string("edit");
            if (mode != "edit")
            {
                error = std::format("the editor is in {} mode: edits would change a copy that Stop throws away -- stop it first", mode);
                return nullptr;
            }
            Ref<Scene> scene = context.ActiveScene ? context.ActiveScene() : nullptr;
            if (!scene)
                error = "no scene is open";
            return scene;
        }
    }

    std::vector<Tool> CreateEditTools(const EditorContext& context)
    {
        std::vector<Tool> tools;
        auto edits = std::make_shared<std::vector<EditTool>>(MakeEditTools(context));

        // Each edit as a tool of its own: run, recorded as one undo step.
        for (const EditTool& edit : *edits)
        {
            Tool tool = edit.Definition;
            EditFunction function = edit.Edit;
            tool.Handler = [context, function](const Json& arguments, ToolDone done)
            {
                std::string error;
                Ref<Scene> scene = EditableScene(context, error);
                if (!scene)
                {
                    done(ToolResult::Error(error));
                    return;
                }
                Outcome outcome;
                try
                {
                    error = function(*scene, arguments, outcome);
                }
                catch (const std::exception& exception)
                {
                    error = std::format("it failed: {}", exception.what());
                }
                if (!error.empty())
                {
                    done(ToolResult::Error(error));
                    return;
                }
                if (context.RecordEdit && !outcome.Step.empty())
                    context.RecordEdit(outcome.Step);
                done(ToolResult::Data(outcome.Result));
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------------------- apply_edits
        {
            Tool tool;
            tool.Name = "apply_edits";
            tool.Title = "Apply several edits at once";
            tool.Description = "Runs several edits in order as one undo step, all or nothing: if one fails, the scene goes back to how it was "
                               "and the failed edit is named. Each edit is { tool, arguments } with the tool being create_entity, delete_entities, "
                               "rename_entity, duplicate_entity, set_parent, add_component, remove_component or patch_component; a string \"$N\" "
                               "in an edit's arguments stands for the uuid edit N answered (e.g. create, then patch \"$0\").";
            tool.InputSchema = Schema().String("name", "The undo step's name (optional)").Build();
            tool.InputSchema["properties"]["edits"] = Json{ { "type", "array" }, { "description", "[{ tool, arguments }]" }, { "minItems", 1 }, { "maxItems", 200 } };
            tool.InputSchema["required"] = Json::array({ "edits" });
            tool.Access = ConsoleAccess::Write;
            tool.Destructive = true;
            tool.Handler = [context, edits](const Json& arguments, ToolDone done)
            {
                std::string error;
                Ref<Scene> scene = EditableScene(context, error);
                if (!scene)
                {
                    done(ToolResult::Error(error));
                    return;
                }
                const std::string snapshot = context.SnapshotScene ? context.SnapshotScene() : std::string();
                if (snapshot.empty() || !context.RestoreScene)
                {
                    done(ToolResult::Error("the scene cannot be snapshotted, so a batch could not be undone if it failed"));
                    return;
                }
                std::vector<std::string> created;
                Json results = Json::array();
                const Json& list = arguments["edits"];
                for (size_t index = 0; index < list.size(); ++index)
                {
                    const Json& item = list[index];
                    std::string problem;
                    const EditTool* edit = nullptr;
                    if (!item.is_object() || !item.contains("tool") || !item["tool"].is_string())
                        problem = "each edit needs a 'tool'";
                    else
                    {
                        auto found = std::find_if(edits->begin(), edits->end(), [&](const EditTool& candidate) { return candidate.Definition.Name == item["tool"]; });
                        if (found == edits->end())
                            problem = std::format("{} is not an edit tool", item["tool"].dump());
                        else
                            edit = &*found;
                    }
                    Json editArguments;
                    if (problem.empty())
                    {
                        editArguments = Resolve(item.value("arguments", Json::object()), created, problem);
                        if (problem.empty())
                            problem = Protocol::Validate(edit->Definition.InputSchema, editArguments);
                    }
                    Outcome outcome;
                    if (problem.empty())
                    {
                        try
                        {
                            problem = edit->Edit(*scene, editArguments, outcome);
                        }
                        catch (const std::exception& exception)
                        {
                            problem = std::format("it failed: {}", exception.what());
                        }
                    }
                    if (!problem.empty())
                    {
                        // All or nothing: the scene as it was before the batch (the selection keeps what still exists).
                        context.RestoreScene(snapshot);
                        done(ToolResult::Error(std::format("edit {} ({}) failed: {} -- the scene is back as it was before the batch", index,
                                                           item.is_object() ? item.value("tool", std::string("?")) : std::string("?"), problem)));
                        return;
                    }
                    created.push_back(outcome.Result.is_object() ? outcome.Result.value("uuid", std::string()) : std::string());
                    results.push_back(outcome.Result);
                }
                if (context.RecordEdit)
                    context.RecordEdit(arguments.value("name", std::format("MCP: {} edits", list.size())));
                done(ToolResult::Data(Json{ { "results", results } }));
            };
            tools.push_back(std::move(tool));
        }

        // ---------------------------------------------------------------------------------------------------- get_component_schema
        {
            Tool tool;
            tool.Name = "get_component_schema";
            tool.Title = "Component fields";
            tool.Description = "A component's fields as the scene file stores them: each field's kind (number, integer, bool, string, id, "
                               "vectorN, list, object) and default. Without 'component': the components a tool may add and change.";
            tool.InputSchema = Schema().String("component", "The component, e.g. PointLight (left out: the list)");
            tool.Handler = [](const Json& arguments, ToolDone done)
            {
                if (!arguments.contains("component"))
                {
                    Json names = Json::array({ TransformName });
                    for (const ComponentOps& ops : EditableComponents())
                        names.push_back(ops.Name);
                    done(ToolResult::Data(Json{ { "components", names } }));
                    return;
                }
                const ComponentOps* ops = nullptr;
                const std::string name = CanonicalComponent(arguments["component"].get<std::string>(), &ops);
                if (name.empty())
                {
                    done(ToolResult::Error(std::format("{} is not a component a tool may change ({})", arguments["component"].dump(), ComponentList())));
                    return;
                }
                // Every field (with list items' shapes), each with its real default; the ones the file leaves out while unset are
                // optional.
                const Json defaults = DefaultsOf(name, ops);
                Json fields = SchemaFromExample(ShapeOf(name, ops));
                if (fields.is_object() && defaults.is_object())
                    for (auto it = fields.begin(); it != fields.end(); ++it)
                    {
                        if (defaults.contains(it.key()))
                        {
                            if (it.value().contains("default"))
                                it.value()["default"] = defaults[it.key()];
                            continue;
                        }
                        it.value()["optional"] = true; // (left out of the file while unset)
                        const std::string kind = it.value().value("kind", std::string());
                        it.value()["default"] = kind == "list" ? Json::array() : kind == "map" ? Json::object() : Json();
                    }
                done(ToolResult::Data(Json{ { "component", name },
                                            { "fields", fields },
                                            { "note", "Units: centimetres, radians, seconds, m/s where the field says so; ids are digit strings; "
                                                      "a field with an enum takes its name as text (an unknown name is reported as adjusted)." } }));
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------------------------ select
        {
            Tool tool;
            tool.Name = "select";
            tool.Title = "Select entities";
            tool.Description = "Selects entities in the editor (what the Properties panel shows; empty list: nothing). Not an undo step.";
            tool.InputSchema = Schema().Build();
            tool.InputSchema["properties"]["uuids"] = Json{ { "type", "array" }, { "items", Json{ { "type", "string" } } } };
            tool.InputSchema["required"] = Json::array({ "uuids" });
            tool.Access = ConsoleAccess::Write;
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                Ref<Scene> scene = RequireScene(context, done);
                if (!scene)
                    return;
                std::vector<Entity> entities;
                for (const Json& id : arguments["uuids"])
                {
                    std::string error;
                    Entity entity = FindEntity(*scene, id, error);
                    if (!entity)
                    {
                        done(ToolResult::Error(error));
                        return;
                    }
                    entities.push_back(entity);
                }
                if (context.Select)
                    context.Select(entities);
                done(ToolResult::Data(Json{ { "selected", entities.size() } }));
            };
            tools.push_back(std::move(tool));
        }

        // -------------------------------------------------------------------------------------------------------------- save_scene
        {
            Tool tool;
            tool.Name = "save_scene";
            tool.Title = "Save the scene";
            tool.Description = "Saves the open scene (and its sub-levels) to its file, as Ctrl+S does. A scene never saved needs a name from "
                               "the user first.";
            tool.InputSchema = Schema();
            tool.Access = ConsoleAccess::Write;
            tool.Handler = [context](const Json&, ToolDone done)
            {
                std::string error;
                if (!EditableScene(context, error))
                {
                    done(ToolResult::Error(error));
                    return;
                }
                if (!context.SaveScene || !context.SaveScene(error))
                {
                    done(ToolResult::Error(error.empty() ? std::string("the scene could not be saved") : error));
                    return;
                }
                done(ToolResult::Data(Json{ { "saved", context.ScenePath ? context.ScenePath() : std::string() } }));
            };
            tools.push_back(std::move(tool));
        }

        // ----------------------------------------------------------------------------------------------------------- undo / redo
        for (const bool redo : { false, true })
        {
            Tool tool;
            tool.Name = redo ? "redo" : "undo";
            tool.Title = redo ? "Redo" : "Undo";
            tool.Description = redo ? "Redoes the last undone step (Ctrl+Y)." : "Undoes the last step in the editor's history (Ctrl+Z), whoever made it.";
            tool.InputSchema = Schema();
            tool.Access = ConsoleAccess::Write;
            tool.Handler = [context, redo](const Json&, ToolDone done)
            {
                std::string error;
                if (!EditableScene(context, error))
                {
                    done(ToolResult::Error(error));
                    return;
                }
                const std::string step = redo ? (context.Redo ? context.Redo() : std::string()) : (context.Undo ? context.Undo() : std::string());
                if (step.empty())
                    done(ToolResult::Error(redo ? "nothing to redo" : "nothing to undo"));
                else
                    done(ToolResult::Data(Json{ { redo ? "redone" : "undone", step } }));
            };
            tools.push_back(std::move(tool));
        }
        return tools;
    }
}
