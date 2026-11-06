namespace Nox;

/// <summary>
/// The entity's Animator. In graph mode its animation graph reads named parameters that scripts set here, e.g.
/// <c>GetComponent&lt;AnimatorComponent&gt;().SetFloat("Speed", velocity.Length)</c>. A parameter only has an effect
/// if the graph declares it and something in the graph (a "Parameters / Speed" node) reads it.
/// </summary>
public sealed class AnimatorComponent : Component
{
    internal override ComponentType Type => ComponentType.Animator;

    public void SetFloat(string name, float value) => InternalCalls.SetAnimatorFloat(EntityID, name, value);
    public float GetFloat(string name) => InternalCalls.GetAnimatorFloat(EntityID, name);

    public void SetBool(string name, bool value) => InternalCalls.SetAnimatorBool(EntityID, name, value);
    public bool GetBool(string name) => InternalCalls.GetAnimatorBool(EntityID, name);

    /// <summary>
    /// Plays a clip once over the animation graph in the graph's Slot node of that name (UE's montages: attacks, interactions,
    /// reloads), e.g. <c>PlayMontage("Models/Knight/Meshes/Knight_Attack.nanim")</c>. The path is relative to the asset folder.
    /// It blends in over <paramref name="blendIn"/> seconds and out over the last <paramref name="blendOut"/>; one already
    /// playing in the slot is replaced. False when no animation clip has that path.
    /// </summary>
    public bool PlayMontage(string clipPath, string slot = "DefaultSlot", float blendIn = 0.2f, float blendOut = 0.2f, float rate = 1.0f) =>
        InternalCalls.PlayAnimatorMontage(EntityID, clipPath, slot, blendIn, blendOut, rate);

    /// <summary>Blends the slot's montage out over <paramref name="blendOut"/> seconds.</summary>
    public void StopMontage(string slot = "DefaultSlot", float blendOut = 0.2f) => InternalCalls.StopAnimatorMontage(EntityID, slot, blendOut);

    public bool IsMontagePlaying(string slot = "DefaultSlot") => InternalCalls.IsAnimatorMontagePlaying(EntityID, slot);

    /// <summary>
    /// An IK target (world position) the animation graph's Two Bone IK and Look At nodes reach for by name, e.g.
    /// <c>SetIKTarget("RightHand", doorHandle.Position)</c> or <c>SetIKTarget("LookAt", player.Position)</c>. It stays until cleared or
    /// set again; <paramref name="weight"/> (0..1) fades the IK in and out. A graph without a node reading that name ignores it.
    /// </summary>
    public void SetIKTarget(string name, Vector3d position, float weight = 1.0f) => InternalCalls.SetAnimatorIKTarget(EntityID, name, position, weight);

    /// <summary>Removes an IK target: the nodes reading it go back to the animation.</summary>
    public void ClearIKTarget(string name) => InternalCalls.ClearAnimatorIKTarget(EntityID, name);
}
