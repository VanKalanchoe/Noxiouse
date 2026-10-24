#pragma once

#include "Components.h"
#include "Scene.h"
#include "NoxCore/Core/core.h"
#include <unordered_set>
#include <vector>

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
        // The scene in the form a packaged game loads (a checked header, then the scene): false when it could not be written.
        bool SerializeRuntime(const std::filesystem::path& filepath);
        
        bool Deserialize(const std::filesystem::path& filepath);
        bool DeserializeRuntime(const std::filesystem::path& filepath);
        // A runtime scene already in memory (read out of a pack); name is for the log.
        bool DeserializeRuntimeBytes(const std::string& bytes, const std::string& name);
        // Whether bytes start like a runtime scene (SerializeRuntime) rather than a scene's text.
        static bool IsRuntimeScene(const std::string& bytes);
        // The scene text inside a runtime scene's bytes (checked: header, version, size; logged under `name` when wrong). `outText` may
        // be `bytes` itself.
        static bool RuntimeSceneText(const std::string& bytes, const std::string& name, std::string& outText);

        // The scene file's text without the file: exactly what Serialize writes and Deserialize reads (the editor's undo history
        // keeps scene states this way, so restoring one is the same as saving and reopening the scene).
        std::string SerializeToText();
        bool DeserializeText(const std::string& text);

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
        // The same entity as text, without parsing it back (cheap enough to compare every frame).
        static std::string EntityToText(Scene& scene, Entity entity);

        // The overrides, structure changes and nested changes of an instance as a YAML map, in the form they are saved: what a nested
        // instance's changes are compared in against the prefab file's text of it.
        static YAML::Node PrefabChangesToNode(const std::vector<PrefabPropertyOverride>& overrides, const std::vector<PrefabStructureChange>& structure,
                                              const std::vector<PrefabNestedChange>& nested);

        // The text SerializePrefab would write (empty when there is nothing to write): Prefab Mode compares it with the text at the time it
        // was opened to know whether the prefab was changed.
        static std::string PrefabToText(Scene& scene, const std::vector<Entity>& entities, const std::string& name);

        // Writes a prefab file (.nprefab) from scene entities: one entity becomes the prefab root with its whole hierarchy, several
        // become the children of a new empty root named `name`, positioned relative to the first one. Modified nowhere: the scene
        // is left as it is. Returns false when the file cannot be written.
        static bool SerializePrefab(Scene& scene, const std::vector<Entity>& entities, const std::string& name, const std::filesystem::path& filepath);
        // World partition (WorldPartition.h): the scene file's settings and cell index, and a cell file's text -- the roots' trees in the
        // order of their IDs (the same entities, the same text) -- or a cell file's text with the roots' trees put in (merged by ID).
        static void WriteWorldPartition(YAML::Emitter& out, Scene& scene);
        static void ReadWorldPartition(Scene& scene, const YAML::Node& data);
        static std::string WorldCellText(Scene& scene, const std::vector<Entity>& roots, const std::string& name, uint32_t* outCount);
        static std::string MergeWorldCellText(Scene& scene, const std::string& fileText, const std::vector<Entity>& roots, const std::string& name,
                                              uint32_t& outCount);
    private:
        // `exclude`: entities left out (a world's cells' entities, which are in the cell files).
        std::string EmitScene(size_t& scriptComponentCount, size_t& scriptClassCount, const std::unordered_set<UUID>* exclude);
        bool ReadScene(const YAML::Node& data);
    private:
        Ref<Scene> m_Scene;
    };
 
}
