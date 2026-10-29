using Nox;

namespace Facerun;

/// <summary>
/// Checks level streaming in Play (Scenes/LevelStreamingTest.nox) and logs every check and a PASS / FAIL summary. Put it on the
/// primary camera: the volume part moves the camera into the scene's streaming volume and out again.
/// <list type="bullet">
/// <item>Script levels: Scene.LoadLevel / UnloadLevel / GetLevelState, the level's entities and scripts coming and going.</item>
/// <item>Volume levels: the level a streaming volume names loads when the camera enters it and unloads UnloadDelay seconds after
/// the camera left.</item>
/// </list>
/// Keys: T runs the checks again, L / U load and unload the script level by hand. Flying into the orange pad (WASD) streams the
/// volume level in by hand.
/// </summary>
public sealed class LevelStreamingTest : EntityBehaviour
{
    const string ScriptLevel = "Scenes/LevelStreamingTest_ScriptLevel.nox";
    const string VolumeLevel = "Scenes/LevelStreamingTest_VolumeLevel.nox";
    const string ScriptMarker = "LST_ScriptMarker";
    const string VolumeMarker = "LST_VolumeMarker";
    const float LoadTimeout = 10.0f;

    [Expose(Tooltip = "Run the checks when Play starts (T runs them again)")] public bool RunOnStart = true;
    [Expose(Tooltip = "A point inside the streaming volume, in world units")] public Vector3 VolumeCenter = new(0.0f, 150.0f, -1500.0f);
    [Expose(Tooltip = "A point outside every streaming volume, in world units")] public Vector3 OutsidePoint = new(0.0f, 150.0f, -3500.0f);
    [Expose(Tooltip = "The volume's Unload Delay, in seconds")] public float VolumeUnloadDelay = 1.0f;

    enum Step
    {
        Idle,
        ScriptStart,
        ScriptWaitLoaded,
        ScriptWaitSpin,
        ScriptCheckUnloaded,
        ScriptCancelLoad,
        ScriptCheckCancelled,
        VolumeStart,
        VolumeWaitLoaded,
        VolumeWaitStillLoaded,
        VolumeWaitUnloaded,
        Done
    }

    Step m_Step = Step.Idle;
    float m_StepTime;
    float m_SpinStart;
    Vector3d m_CameraStart;
    int m_Passed;
    readonly List<string> m_Failures = new();
    bool m_KeyT, m_KeyL, m_KeyU;

    protected override void OnCreate()
    {
        if (RunOnStart)
            Begin();
    }

    protected override void OnUpdate(float deltaTime)
    {
        HandleKeys();
        if (m_Step == Step.Idle || m_Step == Step.Done)
            return;
        m_StepTime += deltaTime;

        switch (m_Step)
        {
            case Step.ScriptStart:
                Check(Scene.GetLevelState(ScriptLevel) == LevelState.Unloaded, "script level starts unloaded");
                Check(!Entity.FindByName(ScriptMarker).IsValid, "no script level entity before the load");
                Check(!Scene.LoadLevel("Scenes/LevelStreamingTest_NoSuchLevel.nox"), "LoadLevel of a scene not in the registry returns false (its error in the log is expected)");
                Check(Scene.LoadLevel(ScriptLevel), "LoadLevel returns true");
                // Loads run at the start of the next frame, so it is only queued now.
                Check(Scene.GetLevelState(ScriptLevel) == LevelState.Loading, "state is Loading right after LoadLevel");
                Next(Step.ScriptWaitLoaded);
                break;

            case Step.ScriptWaitLoaded:
                if (Scene.GetLevelState(ScriptLevel) == LevelState.Loaded)
                {
                    Pass($"script level loaded after {m_StepTime:0.00} s");
                    Entity marker = Entity.FindByName(ScriptMarker);
                    Check(marker.IsValid, $"'{ScriptMarker}' exists once the level is loaded");
                    if (marker.IsValid)
                        m_SpinStart = marker.GetComponent<TransformComponent>().LocalRotation.Y;
                    Next(Step.ScriptWaitSpin);
                }
                else if (m_StepTime > LoadTimeout)
                    Abort($"script level not loaded after {LoadTimeout} s (state {Scene.GetLevelState(ScriptLevel)})");
                break;

            case Step.ScriptWaitSpin:
                if (m_StepTime < 0.5f)
                    break;
                Entity spinning = Entity.FindByName(ScriptMarker);
                Check(spinning.IsValid && spinning.GetComponent<TransformComponent>().LocalRotation.Y != m_SpinStart,
                      "the level's script runs (its box turned)");
                Scene.UnloadLevel(ScriptLevel);
                // Unloads run at the start of the next frame too.
                Check(Scene.GetLevelState(ScriptLevel) == LevelState.Loaded, "state stays Loaded until the next frame");
                Next(Step.ScriptCheckUnloaded);
                break;

            case Step.ScriptCheckUnloaded:
                Check(Scene.GetLevelState(ScriptLevel) == LevelState.Unloaded, "state is Unloaded the frame after UnloadLevel");
                Check(!Entity.FindByName(ScriptMarker).IsValid, "the level's entities are gone");
                Next(Step.ScriptCancelLoad);
                break;

            case Step.ScriptCancelLoad:
                // Asked for and dropped again in the same frame: nothing may be left of it.
                Scene.LoadLevel(ScriptLevel);
                Scene.UnloadLevel(ScriptLevel);
                Next(Step.ScriptCheckCancelled);
                break;

            case Step.ScriptCheckCancelled:
                Check(Scene.GetLevelState(ScriptLevel) == LevelState.Unloaded, "load + unload in one frame ends Unloaded");
                Check(!Entity.FindByName(ScriptMarker).IsValid, "load + unload in one frame leaves no entities");
                Next(Step.VolumeStart);
                break;

            case Step.VolumeStart:
                Check(Scene.GetLevelState(VolumeLevel) == LevelState.Unloaded, "volume level starts unloaded (camera outside the volume)");
                Check(!Entity.FindByName(VolumeMarker).IsValid, "no volume level entity before entering the volume");
                SetCameraPosition(VolumeCenter);
                Next(Step.VolumeWaitLoaded);
                break;

            case Step.VolumeWaitLoaded:
                if (Scene.GetLevelState(VolumeLevel) == LevelState.Loaded)
                {
                    Pass($"volume level loaded {m_StepTime:0.00} s after the camera entered the volume");
                    Check(Entity.FindByName(VolumeMarker).IsValid, $"'{VolumeMarker}' exists once the level is loaded");
                    SetCameraPosition(OutsidePoint);
                    Next(Step.VolumeWaitStillLoaded);
                }
                else if (m_StepTime > LoadTimeout)
                    Abort($"volume level not loaded {LoadTimeout} s after entering the volume (state {Scene.GetLevelState(VolumeLevel)})");
                break;

            case Step.VolumeWaitStillLoaded:
                if (m_StepTime < VolumeUnloadDelay * 0.5f)
                    break;
                Check(Scene.GetLevelState(VolumeLevel) == LevelState.Loaded, "volume level stays loaded during its unload delay");
                m_Step = Step.VolumeWaitUnloaded; // (the step time goes on: the time since the camera left)
                break;

            case Step.VolumeWaitUnloaded:
                if (Scene.GetLevelState(VolumeLevel) == LevelState.Unloaded)
                {
                    // One frame of slack: the camera's position is read at the start of the frame after it moved.
                    Check(m_StepTime >= VolumeUnloadDelay - 0.1f, $"volume level unloaded {m_StepTime:0.00} s after leaving (delay {VolumeUnloadDelay} s)");
                    Check(!Entity.FindByName(VolumeMarker).IsValid, "the volume level's entities are gone");
                    Finish();
                }
                else if (m_StepTime > VolumeUnloadDelay + LoadTimeout)
                    Abort($"volume level still {Scene.GetLevelState(VolumeLevel)} {m_StepTime:0.0} s after the camera left the volume");
                break;
        }
    }

    void HandleKeys()
    {
        bool t = Input.IsKeyDown(KeyCode.T), l = Input.IsKeyDown(KeyCode.L), u = Input.IsKeyDown(KeyCode.U);
        if (t && !m_KeyT && (m_Step == Step.Idle || m_Step == Step.Done))
            Begin();
        if (l && !m_KeyL)
            Log.Info($"[LevelStreamingTest] LoadLevel({ScriptLevel}) -> {Scene.LoadLevel(ScriptLevel)}");
        if (u && !m_KeyU)
        {
            Scene.UnloadLevel(ScriptLevel);
            Log.Info($"[LevelStreamingTest] UnloadLevel({ScriptLevel})");
        }
        (m_KeyT, m_KeyL, m_KeyU) = (t, l, u);
    }

    void Begin()
    {
        m_Passed = 0;
        m_Failures.Clear();
        m_CameraStart = GetComponent<TransformComponent>().LocalPosition;
        Log.Info("[LevelStreamingTest] running");
        Next(Step.ScriptStart);
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
            Log.Info($"[LevelStreamingTest] FAIL: {what}");
        }
    }

    void Pass(string what)
    {
        m_Passed++;
        Log.Info($"[LevelStreamingTest] ok: {what}");
    }

    void Abort(string what)
    {
        Check(false, what);
        Finish();
    }

    void Finish()
    {
        SetCameraPosition(m_CameraStart);
        Scene.UnloadLevel(ScriptLevel);
        m_Step = Step.Done;
        if (m_Failures.Count == 0)
            Log.Info($"[LevelStreamingTest] PASS ({m_Passed} checks)");
        else
            Log.Info($"[LevelStreamingTest] FAIL ({m_Failures.Count} of {m_Passed + m_Failures.Count} checks failed): {string.Join("; ", m_Failures)}");
    }

    void SetCameraPosition(Vector3d position) => GetComponent<TransformComponent>().LocalPosition = position;
}
