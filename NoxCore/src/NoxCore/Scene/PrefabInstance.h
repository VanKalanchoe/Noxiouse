#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

#include <glm/glm.hpp>

#include "Components.h"

namespace Nox
{
    class Entity;
    class Prefab;
    class Scene;

    // Prefab instances (Stage C, docs/Prefab_Architecture_Plan_2026.md): a prefab (.nprefab) placed in a scene as one root
    // entity (PrefabInstanceComponent) whose entities are spawned from the prefab asset -- like model instances, the scene
    // saves the root only, never the spawned entities. Main thread.
    namespace PrefabInstance
    {
        // A spawned entity's UUID, the same on every load (the instance's UUID and the entity's id in the prefab file decide
        // it): saved references to it (script fields, animator node tables) keep resolving.
        UUID NodeUUID(UUID instance, uint64_t localId);

        // Spawns the entities of the instances whose prefab is loaded; requests the rest.
        void SpawnPending(Scene& scene);

        // Places a prefab in the scene and spawns it at once (Play mode: a script shoots a ball and sets its velocity in the same
        // call). The prefab asset is read synchronously (a small text file); the assets its entities use load in the
        // background as usual. `outSpawned` gets the root first, then the spawned entities. Returns the root, an invalid entity
        // when the prefab cannot be loaded.
        Entity Instantiate(Scene& scene, AssetHandle prefab, const glm::vec3& position, std::vector<Entity>& outSpawned);

        // The entity of an instance with this id in the prefab file (the root for the prefab's root id); invalid when it is not
        // spawned.
        Entity FindEntity(Scene& scene, Entity root, uint64_t localId);

        // What differs from the prefab: worked out from the spawned entities (their serialized components against the prefab's
        // text), or what was captured last while the instance is not spawned. Fields only, on components both have; added or
        // removed components and entities are not overrides yet.
        std::vector<PrefabPropertyOverride> CollectOverrides(Scene& scene, Entity root);

        // The structure differences: entities deleted, components removed and added on the instance's spawned entities (stored ones
        // while it is not spawned).
        std::vector<PrefabStructureChange> CollectStructureChanges(Scene& scene, Entity root);

        // The prefab instances inside this one (nested prefabs) whose differences are not what the prefab's own text says about them;
        // stored ones while the instance is not spawned.
        std::vector<PrefabNestedChange> CollectNested(Scene& scene, Entity root);

        // Everything that differs from the prefab, one row each, for the Inspector (Overrides list).
        struct OverrideEntry
        {
            enum class EntryKind : uint8_t { Property, RemovedEntity, RemovedComponent, AddedComponent, AddedEntity };

            EntryKind Kind = EntryKind::Property;
            uint64_t Entity = 0;        // id in the prefab file (Property, Removed*, AddedComponent); the parent's for AddedEntity
            std::string Component;      // Property, RemovedComponent, AddedComponent
            std::string Field;          // Property
            std::string Value;          // Property, AddedComponent
            UUID SceneEntity = 0;       // AddedEntity: the scene entity
            std::string Name;           // AddedEntity: its name
            std::string TargetName;     // the name of the entity `Entity` stands for (the prefab's name when it was deleted)
        };
        std::vector<OverrideEntry> ListOverrides(Scene& scene, Entity root);

        // Every spawned instance of the prefab takes its current differences into its component. Call before the prefab asset's
        // content changes (afterwards the differences could not be told apart from the prefab's own edit).
        void CaptureOverrides(Scene& scene, AssetHandle prefab);

        // Drops one override (or all of them when null) and spawns the instance again from the prefab; an added entity is deleted.
        void RevertOverride(Scene& scene, Entity root, const OverrideEntry* only);

        // Writes one override into the prefab (the loaded asset and its file) and spawns every instance of it again; the other
        // instances keep the differences they had.
        bool ApplyOverride(Scene& scene, Entity root, const OverrideEntry& entry);

        // Turns the instance into ordinary entities: the link to the prefab is cut and the spawned entities are saved with the
        // scene from now on (Unity's Unpack Completely).
        void Unpack(Scene& scene, Entity root);

        // Prefab Mode for a variant (P6c): the variant's BASE as an instance in `scene` carrying the variant's own changes, and the entities
        // the variant adds attached below it. Editing it is editing an instance; SaveVariant writes the changes back into the variant.
        Entity LoadVariantForEditing(Scene& scene, const Prefab& variant);
        bool SaveVariant(Scene& scene, Entity root, Prefab& variant, const std::filesystem::path& absolutePath);

        // Prefab Mode: the prefab's entities as ordinary entities of `scene` (each keeps its id from the file as its UUID, the
        // root is at the origin). Returns the root; entities whose parent is not in the file hang from it.
        Entity LoadForEditing(Scene& scene, const Prefab& prefab);

        // The prefab changed: every spawned instance of it in `scene` loses its spawned entities and the components it got from
        // the prefab, and spawns again from the (reloaded) asset with the overrides it holds (CaptureOverrides first). Placement
        // (name, transform, parent, folder) is kept.
        void RespawnAll(Scene& scene, AssetHandle prefab);
    }
}
