namespace Nox;

/// <summary>
/// The project's world unit (Project Settings: units per meter; 100 = one unit is a centimeter, like Unreal). Positions,
/// velocities and lengths a script reads or sets are in world units: convert real-world values through here instead of
/// assuming meters, so a script works in any project.
/// <code>
/// float jumpHeight = WorldUnits.FromMeters(1.2f);       // 120 in a centimeter project
/// float speedMetersPerSecond = WorldUnits.ToMeters(velocity.Length);
/// </code>
/// </summary>
public static class WorldUnits
{
    /// <summary>World units in one meter.</summary>
    public static float PerMeter => InternalCalls.GetUnitsPerMeter();

    public static float FromMeters(float meters) => meters * PerMeter;
    public static float ToMeters(float worldUnits) => worldUnits / PerMeter;
    public static float FromCentimeters(float centimeters) => centimeters * 0.01f * PerMeter;
    public static float ToCentimeters(float worldUnits) => worldUnits / PerMeter * 100.0f;
}
