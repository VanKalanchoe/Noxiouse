using System;
using System.Collections.Generic;
using Nox;

namespace Facerun;

/// <summary>
/// The world partition fly-through (docs/Engine_Architecture_Plan_2026.md §5.13.4): put it on the camera of a world made with
/// Create > Streaming Test World (or any world partition) and press Play. The camera flies one lap of a circle around the world's
/// middle at a steady speed, looking ahead, and the test logs a line a second (cells by state, assets, RAM, VRAM, the second's worst
/// frame) and a PASS / FAIL summary:
/// <list type="bullet">
/// <item>no hitch: no frame longer than <see cref="HitchMilliseconds"/>;</item>
/// <item>no hole: the cells near the camera are in the world on every frame (streaming kept ahead of it);</item>
/// <item>memory follows the camera, not the distance flown: the second half of the lap needs no more RAM, VRAM or assets than the
/// first (what was left behind is freed).</item>
/// </list>
/// Fly it in a Release build: a Debug build's frames are long on their own.
/// </summary>
public sealed class StreamingWorldTest : EntityBehaviour
{
    [Expose(Tooltip = "The camera's speed (40 m/s is 144 km/h)")] public float SpeedMetersPerSecond = 40.0f;
    [Expose(Tooltip = "The camera's height above 0")] public float HeightMeters = 70.0f;
    [Expose(Tooltip = "The circle's radius as a share of the world's smaller side")] public float RadiusFraction = 0.35f;
    [Expose(Tooltip = "A frame longer than this is a hitch")] public float HitchMilliseconds = 50.0f;
    [Expose(Tooltip = "Cells this close to the camera must be in the world on every frame")] public float HoleRangeMeters = 96.0f;
    [Expose(Tooltip = "Allowed growth of the second half's peak memory over the first half's")] public float MemoryGrowthPercent = 15.0f;

    const float MaxWarmUpSeconds = 30.0f;
    const float Pitch = -0.25f; // radians: looking a little down

    enum Phase { WarmUp, Flying, Done }

    Phase m_Phase = Phase.WarmUp;
    float m_Time;
    float m_SampleTime;
    Vector3 m_Center;
    float m_Radius;
    float m_Angle;
    float m_LapAngle;
    Vector3 m_StartPosition;
    Vector3 m_StartRotation;

    readonly List<float> m_Frames = new();
    float m_SecondWorstFrame;
    int m_Hitches;
    int m_HoleFrames;
    float m_HoleTime, m_WorstHole;

    struct Peak { public ulong Process, Gpu; public int Assets, Cells; }
    Peak m_FirstHalf, m_SecondHalf;
    WorldStreamingStats m_FlightStart; // the streaming counters when the flight began (the start's cells excluded)

    protected override void OnCreate()
    {
        WorldStreamingStats world = Scene.GetWorldStreamingStats();
        if (world.Cells == 0)
        {
            Log.Info("[StreamingWorldTest] FAIL: the scene has no world partition cells");
            m_Phase = Phase.Done;
            return;
        }
        TransformComponent transform = GetComponent<TransformComponent>();
        m_StartPosition = transform.World.Position;
        m_StartRotation = transform.World.Rotation;
        m_Center = (world.BoundsMin + world.BoundsMax) * 0.5f;
        float side = MathF.Min(world.BoundsMax.X - world.BoundsMin.X, world.BoundsMax.Z - world.BoundsMin.Z);
        m_Radius = MathF.Max(side * RadiusFraction, WorldUnits.FromMeters(50.0f));
        PlaceCamera();
        Log.Info($"[StreamingWorldTest] {world.Cells} cells, {Meters(world.BoundsMax.X - world.BoundsMin.X):0} x {Meters(world.BoundsMax.Z - world.BoundsMin.Z):0} m; " +
                 $"a lap of {Meters(2.0f * MathF.PI * m_Radius):0} m at {SpeedMetersPerSecond:0} m/s; waiting for the first cells");
    }

    protected override void OnUpdate(float deltaTime)
    {
        if (m_Phase == Phase.Done)
            return;
        m_Time += deltaTime;
        Vector3 camera = GetComponent<TransformComponent>().World.Position;

        if (m_Phase == Phase.WarmUp)
        {
            // The start's cells in, and a few frames to settle (the first frames of Play are long).
            if (m_Time > 1.0f && Scene.IsWorldStreamingCompleted(camera, WorldUnits.FromMeters(HoleRangeMeters)) || m_Time > MaxWarmUpSeconds)
            {
                Log.Info($"[StreamingWorldTest] flying (warm-up {m_Time:0.0} s)");
                m_FlightStart = Scene.GetWorldStreamingStats();
                m_Phase = Phase.Flying;
                m_Time = 0.0f;
                m_SampleTime = 0.0f;
            }
            return;
        }

        // This frame: how long it took, and whether the camera's surroundings were in the world for it.
        if (m_Frames.Count > 0 || m_Time > 0.25f) // (the frame that started the flight is the warm-up's)
        {
            m_Frames.Add(deltaTime * 1000.0f);
            m_SecondWorstFrame = MathF.Max(m_SecondWorstFrame, deltaTime * 1000.0f);
            if (deltaTime * 1000.0f > HitchMilliseconds)
            {
                m_Hitches++;
                Log.Info($"[StreamingWorldTest] hitch: {deltaTime * 1000.0f:0.0} ms at {Meters(camera.X):0}, {Meters(camera.Z):0} m");
            }
        }
        if (!Scene.IsWorldStreamingCompleted(camera, WorldUnits.FromMeters(HoleRangeMeters)))
        {
            m_HoleFrames++;
            m_HoleTime += deltaTime;
            m_WorstHole = MathF.Max(m_WorstHole, m_HoleTime);
        }
        else
            m_HoleTime = 0.0f;

        m_SampleTime += deltaTime;
        if (m_SampleTime >= 1.0f)
        {
            m_SampleTime -= 1.0f;
            Sample();
        }

        // Along the circle at the set speed.
        float step = WorldUnits.FromMeters(SpeedMetersPerSecond) * deltaTime / m_Radius;
        m_Angle += step;
        m_LapAngle += step;
        if (m_LapAngle >= 2.0f * MathF.PI)
        {
            Sample();
            Finish();
            return;
        }
        PlaceCamera();
    }

    protected override void OnDestroy()
    {
        if (m_Phase == Phase.Flying)
            Log.Info("[StreamingWorldTest] stopped before the lap was done (no result)");
    }

    void PlaceCamera()
    {
        TransformComponent transform = GetComponent<TransformComponent>();
        Transform world = transform.World;
        world.Position = new Vector3(m_Center.X + m_Radius * MathF.Cos(m_Angle), WorldUnits.FromMeters(HeightMeters), m_Center.Z + m_Radius * MathF.Sin(m_Angle));
        // Looking along the circle: forward (-sin, 0, cos) as yaw (forward = (-cos p sin y, sin p, -cos p cos y)).
        float forwardX = -MathF.Sin(m_Angle), forwardZ = MathF.Cos(m_Angle);
        world.Rotation = new Vector3(Pitch, MathF.Atan2(-forwardX, -forwardZ), 0.0f);
        transform.World = world;
    }

    void Sample()
    {
        WorldStreamingStats world = Scene.GetWorldStreamingStats();
        MemoryStats memory = Diagnostics.Memory;
        bool firstHalf = m_LapAngle < MathF.PI;
        ref Peak peak = ref firstHalf ? ref m_FirstHalf : ref m_SecondHalf;
        peak.Process = Math.Max(peak.Process, memory.ProcessBytes);
        peak.Gpu = Math.Max(peak.Gpu, memory.GpuBytes);
        peak.Assets = Math.Max(peak.Assets, memory.LoadedAssets);
        peak.Cells = Math.Max(peak.Cells, world.Activated + world.Loaded + world.Loading);
        Log.Info($"[StreamingWorldTest] {m_LapAngle / (2.0f * MathF.PI) * 100.0f,3:0} %  cells {world.Activated} in, {world.Loaded} loaded, " +
                 $"{world.Loading} loading  assets {memory.LoadedAssets} (+{memory.LoadingAssets})  RAM {Megabytes(memory.ProcessBytes):0} MB  " +
                 $"VRAM {Megabytes(memory.GpuBytes):0} MB  worst frame {m_SecondWorstFrame:0.0} ms");
        m_SecondWorstFrame = 0.0f;
    }

    void Finish()
    {
        m_Phase = Phase.Done;
        TransformComponent transform = GetComponent<TransformComponent>();
        Transform world = transform.World;
        world.Position = m_StartPosition;
        world.Rotation = m_StartRotation;
        transform.World = world;

        m_Frames.Sort();
        float average = 0.0f;
        foreach (float frame in m_Frames)
            average += frame;
        average /= Math.Max(m_Frames.Count, 1);
        float p99 = m_Frames.Count > 0 ? m_Frames[Math.Min(m_Frames.Count - 1, (int)(m_Frames.Count * 0.99f))] : 0.0f;
        float worst = m_Frames.Count > 0 ? m_Frames[^1] : 0.0f;
        Log.Info($"[StreamingWorldTest] {m_Frames.Count} frames in {m_Time:0} s: average {average:0.0} ms, 99 % under {p99:0.0} ms, worst {worst:0.0} ms");
        // What streaming cost per cell during the flight: where a stall would come from (the file's parse, building the entities,
        // copying them into the world) and whether cells kept up.
        WorldStreamingStats end = Scene.GetWorldStreamingStats();
        long cells = end.CellsActivated - m_FlightStart.CellsActivated;
        if (cells > 0)
        {
            double entities = (double)(end.EntitiesActivated - m_FlightStart.EntitiesActivated) / cells;
            double mainThread = end.BuildMs - m_FlightStart.BuildMs + end.ActivateMs - m_FlightStart.ActivateMs;
            Log.Info($"[StreamingWorldTest] {cells} cells streamed in ({entities:0} entities each): per cell read, parsed and built " +
                     $"{(end.ReadMs - m_FlightStart.ReadMs) / cells:0.00} ms (worker); built {(end.BuildMs - m_FlightStart.BuildMs) / cells:0.00} ms and " +
                     $"activated {(end.ActivateMs - m_FlightStart.ActivateMs) / cells:0.00} ms (main thread); assets waited " +
                     $"{(end.AssetWaitSeconds - m_FlightStart.AssetWaitSeconds) / cells:0.00} s; in the world " +
                     $"{(end.LatencySeconds - m_FlightStart.LatencySeconds) / cells:0.00} s after being wanted (longest {end.MaxLatencySeconds:0.00} s); " +
                     $"main thread {mainThread / m_Time:0.0} ms per second of flight");
        }
        Log.Info($"[StreamingWorldTest] peak (first half / second half): RAM {Megabytes(m_FirstHalf.Process):0} / {Megabytes(m_SecondHalf.Process):0} MB, " +
                 $"VRAM {Megabytes(m_FirstHalf.Gpu):0} / {Megabytes(m_SecondHalf.Gpu):0} MB, assets {m_FirstHalf.Assets} / {m_SecondHalf.Assets}, " +
                 $"cells {m_FirstHalf.Cells} / {m_SecondHalf.Cells}");

        List<string> failures = new();
        if (m_Hitches > 0)
            failures.Add($"{m_Hitches} frame(s) over {HitchMilliseconds:0} ms");
        if (m_HoleFrames > 0)
            failures.Add($"cells within {HoleRangeMeters:0} m not in the world on {m_HoleFrames} frame(s), at most {m_WorstHole:0.00} s in a row");
        float growth = 1.0f + MemoryGrowthPercent / 100.0f;
        const double slackBytes = 64.0 * 1024.0 * 1024.0; // allocator blocks, caches warming up
        if (m_SecondHalf.Process > m_FirstHalf.Process * growth + slackBytes)
            failures.Add($"RAM kept growing ({Megabytes(m_FirstHalf.Process):0} -> {Megabytes(m_SecondHalf.Process):0} MB)");
        if (m_SecondHalf.Gpu > m_FirstHalf.Gpu * growth + slackBytes)
            failures.Add($"VRAM kept growing ({Megabytes(m_FirstHalf.Gpu):0} -> {Megabytes(m_SecondHalf.Gpu):0} MB)");
        if (m_SecondHalf.Assets > m_FirstHalf.Assets * growth + 8)
            failures.Add($"loaded assets kept growing ({m_FirstHalf.Assets} -> {m_SecondHalf.Assets}): what was left behind is not freed");

        if (failures.Count == 0)
            Log.Info("[StreamingWorldTest] PASS: no hitch, no hole, memory flat");
        else
            Log.Info($"[StreamingWorldTest] FAIL: {string.Join("; ", failures)}");
    }

    static float Meters(float worldUnits) => worldUnits / WorldUnits.FromMeters(1.0f);
    static double Megabytes(ulong bytes) => bytes / (1024.0 * 1024.0);
}
