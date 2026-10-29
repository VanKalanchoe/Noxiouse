using System.Runtime.InteropServices;

namespace Nox;

/// <summary>The running scene: creating and removing entities from scripts.</summary>
public static class Scene
{
    /// <summary>
    /// Places a prefab (its path under the asset directory, e.g. "Prefabs/Ball.nprefab") at a position and spawns it at once,
    /// so its components can be used right away. Returns an invalid entity when the prefab does not exist.
    /// </summary>
    public static Entity Instantiate(string prefabPath, Vector3d position) => new(InternalCalls.InstantiatePrefab(prefabPath, position));

    /// <summary>An empty entity (just a transform) at a position, e.g. to show a <see cref="Mesh"/> on.</summary>
    public static Entity CreateEntity(string name, Vector3d position) => new(InternalCalls.CreateEntity(name, position));

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

    /// <summary>
    /// A world partition's streaming around a point (Unreal's IsStreamingCompleted): true once every cell whose content is within
    /// <paramref name="range"/> (world units) of <paramref name="position"/> has reached <paramref name="state"/> -- Loaded counts the
    /// cells activating or activated too. E.g. a teleport puts a streaming source (Target State Loaded) at its target and waits for
    /// Loaded, then moves the player: the cells activate without a hitch.
    /// </summary>
    public static bool IsWorldStreamingCompleted(Vector3d position, float range, WorldCellState state = WorldCellState.Activated) =>
        InternalCalls.IsWorldStreamingCompleted(position, range, (int)state);

    /// <summary>A world partition's cells by state right now, and the bounds of the world's content (Unreal's stat streaming).</summary>
    public static WorldStreamingStats GetWorldStreamingStats() => InternalCalls.GetWorldStreamingStats();
}

/// <summary>What <see cref="Scene.GetWorldStreamingStats"/> returns: the world's cells (not its HLODs) and where they are.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct WorldStreamingStats
{
    /// <summary>Cells in the world's index.</summary>
    public int Cells;
    /// <summary>Cells in the world.</summary>
    public int Activated;
    /// <summary>Cells built and waiting outside the world (a streaming source with Target State Loaded).</summary>
    public int Loaded;
    /// <summary>Cells on their way: being read, built, waiting for their assets or going into the world.</summary>
    public int Loading;
    /// <summary>The bounds of every cell's content, world units (zero without cells).</summary>
    public Vector3 BoundsMin;
    public Vector3 BoundsMax;

    // What the cells activated since Play began cost (sums: difference two readings for a stretch of time).
    /// <summary>Cells that went into the world, and their entities.</summary>
    public long CellsActivated;
    public long EntitiesActivated;
    /// <summary>Of those cells, how many were read from their cooked (binary) files instead of their text.</summary>
    public long CellsCooked;
    /// <summary>Reading and parsing their files and building their entities, on workers (ms).</summary>
    public double ReadMs;
    /// <summary>What of their building the main thread did (ms): taking a worker's result, or copying a Play session's snapshot.</summary>
    public double BuildMs;
    /// <summary>Copying them into the world on the main thread (ms).</summary>
    public double ActivateMs;
    /// <summary>Their activations waiting for their assets (s).</summary>
    public double AssetWaitSeconds;
    /// <summary>From a cell being wanted to it being in the world (s); the longest of them.</summary>
    public double LatencySeconds;
    public double MaxLatencySeconds;
}

/// <summary>
/// A world partition cell's state (Unreal's runtime cell states): Loaded -- built and its assets streamed in, not in the world yet;
/// Activated -- in the world (drawn, simulated).
/// </summary>
public enum WorldCellState
{
    Unloaded = 0,
    Loading = 1,
    Loaded = 2,
    Activating = 3,
    Activated = 4
}

/// <summary>A streamed level's state (<see cref="Scene.GetLevelState"/>).</summary>
public enum LevelState
{
    Unloaded = 0,
    Loading = 1,
    Loaded = 2
}
