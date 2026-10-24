using Nox;

namespace Facerun;

/// <summary>
/// Checks world partition streaming in Play (Scenes/WorldPartitionTest.nox) and logs every check and a PASS / FAIL summary. Put it
/// on the primary camera: it moves the camera around the world and checks which cells' entities exist.
/// <list type="bullet">
/// <item>The world: 6 x 6 cells of 20 m (a floor tile and a turning pillar each, WPT_Tile_x_z / WPT_Cell_x_z), a 70 m slab west of
/// them in a level 2 cell (WPT_Big), a streaming source in the far corner (WPT_Source) and an always-loaded pillar (WPT_Persistent).
/// Loading range 40 m, unloaded 10 % beyond it.</item>
/// <item>Checks: the cells around the camera are in before the first frame; cells follow the camera; a big cell loads by the
/// distance to its content (the slab's edge), not its center; a loaded cell stays inside the hysteresis band and goes beyond it;
/// the streaming source keeps its cells; always-loaded entities stay; streamed cells' scripts run.</item>
/// </list>
/// Keys: T runs the checks again. WASD flies the camera (the World Partition panel shows the cells in Play).
/// </summary>
public sealed class WorldPartitionTest : EntityBehaviour
{
    // The generated world's layout, world units (cm).
    const float CellSize = 2000.0f;
    const float TileMargin = 50.0f;     // a cell's content: its tile, CellSize - 2 * TileMargin wide
    const int Grid = 6;
    const float Range = 4000.0f;
    const float UnloadRange = 4400.0f;  // Range + 10 %
    const float Slack = 60.0f;          // the camera's position is read at the start of the frame after it moved
    static readonly Vector3 SourcePosition = new(11000.0f, 0.0f, 11000.0f);
    static readonly Vector3 BigMin = new(-11500.0f, 0.0f, 2500.0f);
    static readonly Vector3 BigMax = new(-4500.0f, 0.0f, 9500.0f);
    const float SettleTimeout = 10.0f;

    [Expose(Tooltip = "Run the checks when Play starts (T runs them again)")] public bool RunOnStart = true;

    enum Step
    {
        Idle,
        MoveFar,
        WaitFar,
        MoveNearBig,
        WaitNearBig,
        MoveHome,
        WaitHome,
        BandHold,
        BandBeyond,
        Done
    }

    Step m_Step = Step.Idle;
    float m_StepTime;
    Vector3 m_CameraStart;
    float m_SpinStart;
    int m_Passed;
    readonly List<string> m_Failures = new();
    bool m_KeyT;

    protected override void OnCreate()
    {
        if (!RunOnStart)
            return;
        // Play's first cells are loaded before any script starts: the ones around the camera exist right now.
        Begin();
        Vector3 camera = CameraPosition();
        int expected = 0, present = 0;
        ForEachCell((x, z, min, max) =>
        {
            if (Distance(camera, min, max) <= Range - Slack || Distance(SourcePosition, min, max) <= Range - Slack)
            {
                expected++;
                if (Entity.FindByName(CellMarker(x, z)).IsValid)
                    present++;
            }
        });
        Check(expected > 0 && present == expected, $"the cells around the camera and the source are in before the first frame ({present} of {expected})");
        Check(Entity.FindByName("WPT_Persistent").IsValid, "the always-loaded pillar is in the scene");
        Check(!Entity.FindByName("WPT_Big").IsValid, "the slab's cell is not loaded at the start (out of range)");
        Entity marker = Entity.FindByName(CellMarker(0, 0));
        if (marker.IsValid)
            m_SpinStart = marker.GetComponent<TransformComponent>().LocalRotation.Y;
        Next(Step.MoveFar);
    }

    protected override void OnUpdate(float deltaTime)
    {
        if (Input.IsKeyDown(KeyCode.T) && !m_KeyT && (m_Step == Step.Idle || m_Step == Step.Done))
        {
            Begin();
            Next(Step.MoveFar);
        }
        m_KeyT = Input.IsKeyDown(KeyCode.T);
        if (m_Step == Step.Idle || m_Step == Step.Done)
            return;
        m_StepTime += deltaTime;

        switch (m_Step)
        {
            case Step.MoveFar:
            {
                if (m_StepTime < 0.5f)
                    break;
                Entity marker = Entity.FindByName(CellMarker(0, 0));
                Check(marker.IsValid && marker.GetComponent<TransformComponent>().LocalRotation.Y != m_SpinStart,
                      "a streamed cell's script runs (its pillar turned)");
                SetCameraPosition(new Vector3(9000.0f, 170.0f, 9000.0f));
                Next(Step.WaitFar);
                break;
            }

            case Step.WaitFar:
                if (WaitSettled("after the camera moved to the far corner"))
                {
                    Check(!Entity.FindByName(CellMarker(0, 0)).IsValid, "the start cell went when the camera left");
                    Check(Entity.FindByName(CellMarker(4, 4)).IsValid, "the cell under the camera came in");
                    Next(Step.MoveNearBig);
                }
                break;

            case Step.MoveNearBig:
                SetCameraPosition(new Vector3(-1500.0f, 170.0f, 6000.0f));
                Next(Step.WaitNearBig);
                break;

            case Step.WaitNearBig:
                // 30 m from the slab's edge, 65 m from its center: its level 2 cell streams by its content.
                if (!Entity.FindByName("WPT_Big").IsValid)
                {
                    if (m_StepTime > SettleTimeout)
                        Abort("the big slab's cell did not load 30 m from its edge (65 m from its center)");
                    break;
                }
                if (WaitSettled("near the slab"))
                {
                    Pass("the big slab's cell loaded by the distance to its edge, not its center");
                    Next(Step.MoveHome);
                }
                break;

            case Step.MoveHome:
                SetCameraPosition(new Vector3(1000.0f, 170.0f, 1000.0f));
                Next(Step.WaitHome);
                break;

            case Step.WaitHome:
                if (Entity.FindByName("WPT_Big").IsValid)
                {
                    if (m_StepTime > SettleTimeout)
                        Abort("the slab's cell still loaded 55 m from its edge");
                    break;
                }
                if (WaitSettled("back at the start"))
                {
                    Pass("the slab's cell went when the camera left it");
                    Check(Entity.FindByName(CellMarker(2, 0)).IsValid, "cell 2,0 is loaded (30.5 m away)");
                    // Into the hysteresis band of cell 2,0: 42 m from its content (more than the range, less than 10 % beyond it).
                    SetCameraPosition(new Vector3(-150.0f, 170.0f, 1000.0f));
                    Next(Step.BandHold);
                }
                break;

            case Step.BandHold:
                if (m_StepTime < 1.0f)
                    break;
                Check(Entity.FindByName(CellMarker(2, 0)).IsValid, "a loaded cell stays while the camera is within 10 % beyond the range");
                // 46 m: beyond the band.
                SetCameraPosition(new Vector3(-550.0f, 170.0f, 1000.0f));
                Next(Step.BandBeyond);
                break;

            case Step.BandBeyond:
                if (!Entity.FindByName(CellMarker(2, 0)).IsValid)
                {
                    Pass($"the cell went {m_StepTime:0.00} s after the camera passed the band");
                    Finish();
                }
                else if (m_StepTime > SettleTimeout)
                    Abort("cell 2,0 still loaded with the camera beyond the hysteresis band");
                break;
        }
    }

    // True once every cell within range of the camera or the source exists and every one beyond the band from both is gone; checks
    // the persistent pillar and the source's cell on the way. Fails the run after the timeout.
    bool WaitSettled(string where)
    {
        Vector3 camera = CameraPosition();
        bool settled = true;
        string missing = "";
        ForEachCell((x, z, min, max) =>
        {
            float distance = MathF.Min(Distance(camera, min, max), Distance(SourcePosition, min, max));
            bool exists = Entity.FindByName(CellMarker(x, z)).IsValid;
            if (distance <= Range - Slack && !exists)
            {
                settled = false;
                missing = $"cell {x},{z} missing";
            }
            else if (distance > UnloadRange + Slack && exists)
            {
                settled = false;
                missing = $"cell {x},{z} still loaded";
            }
        });
        if (settled)
        {
            Pass($"cells settled {m_StepTime:0.00} s {where}");
            Check(Entity.FindByName("WPT_Persistent").IsValid, $"the always-loaded pillar stays ({where})");
            Check(Entity.FindByName(CellMarker(5, 5)).IsValid, $"the streaming source keeps its cell ({where})");
            return true;
        }
        if (m_StepTime > SettleTimeout)
            Abort($"cells not settled {SettleTimeout} s {where}: {missing}");
        return false;
    }

    static void ForEachCell(Action<int, int, Vector3, Vector3> visit)
    {
        for (int x = 0; x < Grid; x++)
        {
            for (int z = 0; z < Grid; z++)
            {
                Vector3 min = new(x * CellSize + TileMargin, 0.0f, z * CellSize + TileMargin);
                Vector3 max = new((x + 1) * CellSize - TileMargin, 0.0f, (z + 1) * CellSize - TileMargin);
                visit(x, z, min, max);
            }
        }
    }

    static float Distance(Vector3 point, Vector3 min, Vector3 max)
    {
        float dx = MathF.Max(MathF.Max(min.X - point.X, 0.0f), point.X - max.X);
        float dz = MathF.Max(MathF.Max(min.Z - point.Z, 0.0f), point.Z - max.Z);
        return MathF.Sqrt(dx * dx + dz * dz);
    }

    static string CellMarker(int x, int z) => $"WPT_Cell_{x}_{z}";

    void Begin()
    {
        m_Passed = 0;
        m_Failures.Clear();
        m_CameraStart = CameraPosition();
        Log.Info("[WorldPartitionTest] running");
    }

    void Next(Step step)
    {
        m_Step = step;
        m_StepTime = 0.0f;
    }

    void Check(bool condition, string what)
    {
        if (condition)
            Pass(what);
        else
        {
            m_Failures.Add(what);
            Log.Info($"[WorldPartitionTest] FAIL: {what}");
        }
    }

    void Pass(string what)
    {
        m_Passed++;
        Log.Info($"[WorldPartitionTest] ok: {what}");
    }

    void Abort(string what)
    {
        Check(false, what);
        Finish();
    }

    void Finish()
    {
        SetCameraPosition(m_CameraStart);
        m_Step = Step.Done;
        if (m_Failures.Count == 0)
            Log.Info($"[WorldPartitionTest] PASS ({m_Passed} checks)");
        else
            Log.Info($"[WorldPartitionTest] FAIL ({m_Failures.Count} of {m_Passed + m_Failures.Count} checks failed): {string.Join("; ", m_Failures)}");
    }

    Vector3 CameraPosition() => GetComponent<TransformComponent>().LocalPosition;
    void SetCameraPosition(Vector3 position) => GetComponent<TransformComponent>().LocalPosition = position;
}
