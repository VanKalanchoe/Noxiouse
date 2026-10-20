namespace Nox;

[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class ExposeAttribute : Attribute
{
    // Optional unit hint for the Inspector, like Unreal's "Units" property metadata: "cm", "m", "cm/s", "m/s" or "km/h". The field
    // is shown and typed in this unit and converted to/from the raw value the script actually runs with (world units, or world
    // units per second -- always the project's own unit, cm by default). Leave unset for a value that is not a length or a speed
    // (an angle, a fraction, a count, ...): those are shown as the plain number, unconverted.
    // Example: [Expose(Unit = "km/h")] public float TopSpeed = 200.0f; -- the field shows "200", the script sees ~5555.6 (cm/s).
    // Always a real string (never null): Coral marshals a null attribute field unsafely, so unset stays "" rather than default(string).
    public string Unit = "";

    // Shown when the mouse is over the field in the Inspector.
    public string Tooltip = "";

    // The range the Inspector lets the value take (in Unit when one is set); Min == Max (the default): no limit. Numbers only.
    // Example: [Expose(Unit = "m/s", Min = 0, Max = 20, Tooltip = "Top running speed")] public float Speed = 600.0f;
    public float Min = 0.0f;
    public float Max = 0.0f;

    // Shown but not editable (a value the script computes, displayed for debugging).
    public bool ReadOnly = false;
}
