#pragma once

#include "Components.h"
#include "Scene.h"
#include "NoxCore/Core/core.h"

namespace YAML
{
    class Node;
    class Emitter;
}

namespace Nox
{
    class Entity;
    class Scene;

    class SceneSerializer
    {
    public:
        SceneSerializer(const Ref<Scene>& scene);

        void Serialize(const std::filesystem::path& filepath);
        void SerializeRuntime(const std::filesystem::path& filepath);
        
        bool Deserialize(const std::filesystem::path& filepath);
        bool DeserializeRuntime(const std::filesystem::path& filepath);

        // Reads the components of one serialized entity (a scene file's entity, or a prefab file's) onto `target`. With
        // `keepPlacement` the target keeps its own transform and parent (the root of a prefab instance).
        static void DeserializeEntityComponents(Scene& scene, const YAML::Node& node, Entity target, bool keepPlacement);

        // The changes of a prefab instance (or a variant) as they are saved: into an open map / from a map.
        static void WritePrefabChanges(YAML::Emitter& out, const std::vector<PrefabPropertyOverride>& overrides, const std::vector<PrefabStructureChange>& structure,
                                       const std::vector<PrefabNestedChange>& nested);
        static void ReadPrefabChanges(const YAML::Node& node, std::vector<PrefabPropertyOverride>& overrides, std::vector<PrefabStructureChange>& structure,
                                      std::vector<PrefabNestedChange>& nested);

        // One entity as it would be saved (the components as YAML), including the components of a prefab instance root that a
        // scene file leaves to the prefab. Prefab overrides are the difference between this and the prefab's own text.
        static YAML::Node EntityToNode(Scene& scene, Entity entity);

        // The overrides, structure changes and nested changes of an instance as a YAML map, in the form they are saved: what a nested
        // instance's changes are compared in against the prefab file's text of it.
        static YAML::Node PrefabChangesToNode(const std::vector<PrefabPropertyOverride>& overrides, const std::vector<PrefabStructureChange>& structure,
                                              const std::vector<PrefabNestedChange>& nested);

        // Writes a prefab file (.nprefab) from scene entities: one entity becomes the prefab root with its whole hierarchy, several
        // become the children of a new empty root named `name`, positioned relative to the first one. Modified nowhere: the scene
        // is left as it is. Returns false when the file cannot be written.
        static bool SerializePrefab(Scene& scene, const std::vector<Entity>& entities, const std::string& name, const std::filesystem::path& filepath);
    private:
        Ref<Scene> m_Scene;
    };
 
}
