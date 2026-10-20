namespace Nox;

/// <summary>The engine's clock as scripts see it.</summary>
public static class Time
{
    /// <summary>Seconds between two OnFixedUpdate calls (60 per second).</summary>
    public const float FixedDeltaTime = 1.0f / 60.0f;
}
