#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "Components.h"

namespace Nox
{
    class Entity;
    class Scene;

    // Model instances (§5.11.5): an imported model placed in a scene as one root entity (ModelInstanceComponent) whose
    // node entities are spawned from the model's cooked node data -- the way UE's Packed Level Actors and Unity's model
    // prefab instances keep a scene file small: the scene saves the root, the removed nodes and the per-node overrides,
    // never the nodes themselves. Main thread.
    namespace ModelInstance
    {
        // A node's UUID, the same on every load (the instance's UUID and the node index decide it): references to node
        // entities -- animator node tables, children the user attached, saved overrides -- keep resolving.
        UUID NodeUUID(UUID instance, uint32_t nodeIndex);

        // Spawns the nodes of the instances whose model (with its skeleton and clips) is loaded; requests the rest.
        void SpawnPending(Scene& scene);

        // What the instance's nodes differ in from the model, i.e. what is saved; the stored overrides until it spawned.
        std::vector<ModelNodeOverride> CollectOverrides(Scene& scene, Entity root);

        // Destroys the spawned nodes, keeping their overrides and any children the user attached, and brings back the
        // removed ones: the instance spawns again from the model.
        void Respawn(Scene& scene, Entity root);

        // Unreal's Import Into Level: a glTF's scene as entities that keep its structure -- one root, every node an entity under
        // its parent with its local transform, lights and cameras on their nodes. Mesh nodes reference the per-mesh static mesh
        // assets (MeshAssets: glTF mesh index -> asset); skinned nodes become model instances of SkeletalAsset (one animated
        // entity on load) when there is one. Content Browser imports and drag-in stay flat: only this makes hierarchy.
        struct LevelDescription
        {
            std::string Name;
            float Scale = 1.0f; // Import Scale, on the root
            std::vector<MeshNodeData> Nodes;
            std::vector<LightNodeData> Lights;
            std::vector<CameraNodeData> Cameras;
            std::unordered_map<int32_t, AssetHandle> MeshAssets;
            AssetHandle SkeletalAsset = 0;
            struct Clip
            {
                AssetHandle Handle = 0;
                std::vector<int32_t> Nodes; // the nodes it animates
            };
            std::vector<Clip> Clips; // node animations of the file
        };
        Entity SpawnLevel(Scene& scene, const LevelDescription& level);
    }
}
