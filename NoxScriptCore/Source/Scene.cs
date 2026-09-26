namespace Nox;

/// <summary>The running scene: creating and removing entities from scripts.</summary>
public static class Scene
{
    /// <summary>
    /// Places a prefab (its path under the asset directory, e.g. "Prefabs/Ball.nprefab") at a position and spawns it at once,
    /// so its components can be used right away. Returns an invalid entity when the prefab does not exist.
    /// </summary>
    public static Entity Instantiate(string prefabPath, Vector3 position) => new(InternalCalls.InstantiatePrefab(prefabPath, position));

    /// <summary>Destroys the entity (and its children) at the end of this frame's script update.</summary>
    public static void Destroy(Entity entity) => InternalCalls.DestroyEntity(entity.ID);
}
