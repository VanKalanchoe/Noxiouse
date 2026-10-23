namespace Nox;

/// <summary>The running scene: creating and removing entities from scripts.</summary>
public static class Scene
{
    /// <summary>
    /// Places a prefab (its path under the asset directory, e.g. "Prefabs/Ball.nprefab") at a position and spawns it at once,
    /// so its components can be used right away. Returns an invalid entity when the prefab does not exist.
    /// </summary>
    public static Entity Instantiate(string prefabPath, Vector3 position) => new(InternalCalls.InstantiatePrefab(prefabPath, position));

    /// <summary>An empty entity (just a transform) at a position, e.g. to show a <see cref="Mesh"/> on.</summary>
    public static Entity CreateEntity(string name, Vector3 position) => new(InternalCalls.CreateEntity(name, position));

    /// <summary>Destroys the entity (and its children) at the end of this frame's script update.</summary>
    public static void Destroy(Entity entity) => InternalCalls.DestroyEntity(entity.ID);

    /// <summary>
    /// Streams a level -- another scene (its path under the asset directory, e.g. "Levels/Interior.nox") -- into the running scene:
    /// read in the background and its entities created over the next frames; their scripts start once the whole level is in.
    /// False when the scene does not exist. Loading a level already loaded (or loading) does nothing.
    /// </summary>
    public static bool LoadLevel(string scenePath) => InternalCalls.LoadLevel(scenePath);

    /// <summary>Removes a streamed level's entities at the start of the next frame (a level still loading stops).</summary>
    public static void UnloadLevel(string scenePath) => InternalCalls.UnloadLevel(scenePath);

    /// <summary>Where a level stands: not loaded, on its way in, or in the scene.</summary>
    public static LevelState GetLevelState(string scenePath) => (LevelState)InternalCalls.GetLevelState(scenePath);

    /// <summary>True once the level's entities are all in the scene.</summary>
    public static bool IsLevelLoaded(string scenePath) => GetLevelState(scenePath) == LevelState.Loaded;
}

/// <summary>A streamed level's state (<see cref="Scene.GetLevelState"/>).</summary>
public enum LevelState
{
    Unloaded = 0,
    Loading = 1,
    Loaded = 2
}
