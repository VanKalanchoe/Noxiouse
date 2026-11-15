namespace Nox;

/// <summary>
/// The entity's mesh. Its morph targets (blend shapes: a face's smile, a door's dent) by name: a weight set here wins over the
/// animation's and the sequencer's for that target until it is cleared; a target never set keeps the animated or imported weight.
/// A blink: <c>GetComponent&lt;MeshComponent&gt;().SetMorphWeight("Blink", 1.0f)</c>.
/// <para>
/// Its material graph parameters (Unreal's dynamic material instance: SetScalarParameterValue and friends): a value set here is this
/// entity's alone, over its slot material's (the material keeps it for every other object). By the parameter's name in the graph --
/// a Scalar Parameter's or Vector Parameter's Name, a Texture Sample's Parameter Name. A dissolve on one enemy:
/// <c>GetComponent&lt;MeshComponent&gt;().SetScalarParameter("Dissolve", t)</c>. It shows from the next frame; it lasts until Play stops
/// (or <see cref="ClearParameter"/>). A slot whose material has no graph, or a graph without that name, does not change.
/// The slot argument -1 (the default): every material slot of the mesh; else that slot only.
/// </para>
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

    /// <summary>Sets a material graph's Scalar Parameter on this entity.</summary>
    public void SetScalarParameter(string name, float value, int slot = -1) =>
        InternalCalls.SetGraphParameter(EntityID, name, value, 0.0f, 0.0f, 0.0f, slot);

    /// <summary>Sets a material graph's Vector Parameter on this entity (a colour: r, g, b, a in 0..1; HDR above 1).</summary>
    public void SetVectorParameter(string name, float r, float g, float b, float a = 1.0f, int slot = -1) =>
        InternalCalls.SetGraphParameter(EntityID, name, r, g, b, a, slot);

    /// <summary>Sets a material graph's Vector Parameter from a vector (its fourth component 1).</summary>
    public void SetVectorParameter(string name, Vector3 value, int slot = -1) =>
        InternalCalls.SetGraphParameter(EntityID, name, value.X, value.Y, value.Z, 1.0f, slot);

    /// <summary>Sets a material graph's texture parameter (a Texture Sample's Parameter Name) on this entity: the texture's path in the
    /// project's Assets folder, e.g. "Textures/Rust.png" (loaded in the background: the old texture shows until it is ready); "" for none.</summary>
    public void SetTextureParameter(string name, string texturePath, int slot = -1) =>
        InternalCalls.SetGraphTexture(EntityID, name, texturePath, slot);

    /// <summary>The value a Scalar Parameter has on this entity now: its own, else its material's (or the graph's default).
    /// <paramref name="slot"/> -1: the mesh's first slot. 0 when nothing has that name (or the graph is not built yet).</summary>
    public float GetScalarParameter(string name, int slot = -1) =>
        InternalCalls.GetGraphParameter(EntityID, name, slot, out float x, out _, out _, out _) ? x : 0.0f;

    /// <summary>As <see cref="GetScalarParameter"/> for a Vector Parameter (r, g, b; its alpha by <see cref="TryGetVectorParameter"/>).</summary>
    public Vector3 GetVectorParameter(string name, int slot = -1) =>
        InternalCalls.GetGraphParameter(EntityID, name, slot, out float x, out float y, out float z, out _) ? new Vector3(x, y, z) : new Vector3(0.0f, 0.0f, 0.0f);

    /// <summary>A Vector Parameter's four components; false when nothing has that name.</summary>
    public bool TryGetVectorParameter(string name, out float r, out float g, out float b, out float a, int slot = -1) =>
        InternalCalls.GetGraphParameter(EntityID, name, slot, out r, out g, out b, out a);

    /// <summary>Gives the parameter back to the material (scalar, vector or texture of that name; <paramref name="slot"/> -1: every
    /// entry of the name, else that slot's).</summary>
    public void ClearParameter(string name, int slot = -1) => InternalCalls.ClearGraphParameter(EntityID, name, slot);

    /// <summary>Gives every material graph parameter back to the materials.</summary>
    public void ClearParameters() => InternalCalls.ClearGraphParameter(EntityID, string.Empty, -1);
}
