#pragma once
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
    }
}
