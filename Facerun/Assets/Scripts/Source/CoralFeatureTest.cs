using System;
using Nox;

namespace Facerun;

/// <summary>
/// Enum with explicit non-sequential values to verify Coral 320c90d enum reflection,
/// Inspector dropdowns, and YAML scene serialization.
/// </summary>
public enum CoralPulseMode
{
    Slow = 0,
    Fast = 2,
    Burst = 5
}

public enum CoralReactionStyle
{
    LogOnly = 0,
    ScaleBounce = 1,
    SpinBurst = 2
}

/// <summary>
/// Test script exercising every Coral 320c90d / .NET 10 glue feature:
/// - [Expose] C# enum dropdown (with explicit values)
/// - [Expose] read/write C# property with setter validation/clamping + Unit/Min/Max/Tooltip
/// - [Expose] getter-only computed property (auto ReadOnly)
/// - [Expose] property with private setter (auto ReadOnly)
/// - [Expose] C# event (System.Action) wired in Inspector to CoralFeatureReceiver.OnPulseReceived
/// - [Expose] standard fields (bool, int, float, string, Vector3, Entity)
/// Press Space during Play (or wait for the timer) to fire OnPulse.
/// </summary>
public sealed class CoralFeatureSender : EntityBehaviour
{
    [Expose(Tooltip = "Controls how often OnPulse fires (tests C# enum dropdown + explicit values)")]
    public CoralPulseMode Mode = CoralPulseMode.Slow;

    private float _spinSpeed = 120.0f;

    [Expose(Unit = "m/s", Min = 0.0f, Max = 10.0f, Tooltip = "Clamped by C# property setter (0..1000 world units)")]
    public float SpinSpeed
    {
        get => _spinSpeed;
        set => _spinSpeed = Math.Clamp(value, 0.0f, 1000.0f);
    }

    [Expose(Tooltip = "Auto ReadOnly computed property (getter-only)")]
    public int PulseCount => _pulseCount;

    [Expose(Tooltip = "Auto ReadOnly property (private set)")]
    public bool IsRunning { get; private set; } = false;

    [Expose(Tooltip = "Standard string field")]
    public string TagLabel = "Coral .NET 10 Sender";

    [Expose(Tooltip = "Standard Vector3 field")]
    public Vector3 SpinAxis = new(0.0f, 1.0f, 0.0f);

    [Expose(Tooltip = "Entity reference field")]
    [DisallowSelf]
    public Entity LinkedReceiver;

    [Expose(Tooltip = "Fired automatically on interval or when pressing Space")]
    public event Action? OnPulse;

    private int _pulseCount;
    private float _timer;
    private bool _spaceWasDown;

    protected override void OnCreate()
    {
        IsRunning = true;
        Log.Info($"[CoralTest] Sender '{TagLabel}' started: Mode={Mode} ({(int)Mode}), SpinSpeed={SpinSpeed:F1}, LinkedReceiver={LinkedReceiver.ID}");
    }

    protected override void OnUpdate(float deltaTime)
    {
        // Spin using the property-backed SpinSpeed and Vector3 SpinAxis.
        TransformComponent transform = GetComponent<TransformComponent>();
        float radPerSec = (SpinSpeed / 100.0f) * deltaTime;
        Vector3 rot = transform.LocalRotation;
        rot.X += SpinAxis.X * radPerSec;
        rot.Y += SpinAxis.Y * radPerSec;
        rot.Z += SpinAxis.Z * radPerSec;
        transform.LocalRotation = rot;

        float interval = Mode switch
        {
            CoralPulseMode.Slow => 2.5f,
            CoralPulseMode.Fast => 1.0f,
            CoralPulseMode.Burst => 0.4f,
            _ => 2.0f
        };

        _timer += deltaTime;
        bool spaceDown = Input.IsKeyDown(KeyCode.Space);
        bool manualTrigger = spaceDown && !_spaceWasDown;
        _spaceWasDown = spaceDown;

        if (_timer >= interval || manualTrigger)
        {
            _timer = 0.0f;
            FirePulse();
        }
    }

    public void FirePulse()
    {
        _pulseCount++;
        Log.Info($"[CoralTest] Sender fired OnPulse #{_pulseCount} (Mode={Mode})");
        OnPulse?.Invoke();
    }

    protected override void OnAfterReload()
    {
        Log.Info($"[CoralTest] Sender survived atomic hot-reload! PulseCount={_pulseCount}, Mode={Mode}, SpinSpeed={SpinSpeed:F1}");
    }
}

/// <summary>
/// Receiver script wired to CoralFeatureSender.OnPulse via the Inspector's EventBindings.
/// </summary>
public sealed class CoralFeatureReceiver : EntityBehaviour
{
    [Expose(Tooltip = "How this cube reacts when OnPulse is received")]
    public CoralReactionStyle Reaction = CoralReactionStyle.ScaleBounce;

    [Expose(Tooltip = "Auto ReadOnly counter incremented by wired C# event")]
    public int ReceivedCount => _receivedCount;

    private int _receivedCount;
    private float _bounceTimer;

    protected override void OnCreate()
    {
        Log.Info($"[CoralTest] Receiver ready: Reaction={Reaction}");
    }

    protected override void OnUpdate(float deltaTime)
    {
        if (_bounceTimer <= 0.0f)
            return;

        _bounceTimer = MathF.Max(0.0f, _bounceTimer - deltaTime);
        float t = _bounceTimer / 0.35f;
        TransformComponent transform = GetComponent<TransformComponent>();

        if (Reaction == CoralReactionStyle.ScaleBounce)
        {
            float s = 1.0f + 0.45f * MathF.Sin(t * MathF.PI);
            transform.LocalScale = new Vector3(s, s, s);
        }
        else if (Reaction == CoralReactionStyle.SpinBurst)
        {
            Vector3 rot = transform.LocalRotation;
            rot.Y += 8.0f * deltaTime;
            transform.LocalRotation = rot;
        }
    }

    // Callable from Inspector event binding (CoralFeatureSender.OnPulse -> OnPulseReceived)
    public void OnPulseReceived()
    {
        _receivedCount++;
        _bounceTimer = 0.35f;
        Log.Info($"[CoralTest] Receiver got OnPulse! Total received = {_receivedCount} (Reaction={Reaction})");
    }

    public void ResetCounter()
    {
        _receivedCount = 0;
        Log.Info("[CoralTest] Receiver counter reset to 0. ok");
    }
}
