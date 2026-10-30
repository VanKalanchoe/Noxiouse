namespace Nox;

/// <summary>
/// The entity's mesh. Its morph targets (blend shapes: a face's smile, a door's dent) by name: a weight set here wins over the
/// animation's and the sequencer's for that target until it is cleared; a target never set keeps the animated or imported weight.
/// A blink: <c>GetComponent&lt;MeshComponent&gt;().SetMorphWeight("Blink", 1.0f)</c>.
/// </summary>
public sealed class MeshComponent : Component
{
    internal override ComponentType Type => ComponentType.Mesh;

    /// <summary>Sets a morph target's weight (usually 0..1; it is not clamped).</summary>
    public void SetMorphWeight(string target, float weight) => InternalCalls.SetMorphWeight(EntityID, target, weight);

    /// <summary>The weight the target has now: set here, else the sequencer's, the animation's or the imported default (0 for a
    /// target the mesh does not have).</summary>
    public float GetMorphWeight(string target) => InternalCalls.GetMorphWeight(EntityID, target);

    /// <summary>Gives the target back to the animation (or its imported default).</summary>
    public void ClearMorphWeight(string target) => InternalCalls.ClearMorphWeight(EntityID, target);

    /// <summary>Gives every target back to the animation.</summary>
    public void ClearMorphWeights() => InternalCalls.ClearMorphWeight(EntityID, string.Empty);
}
