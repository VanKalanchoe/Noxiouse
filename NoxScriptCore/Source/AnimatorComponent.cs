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
}
