namespace Nox;

/// <summary>
/// The entity's Character Controller 3D: a physics capsule that walks, steps and jumps. Set the desired horizontal
/// velocity every frame; the fixed physics step moves the entity and reports back grounded state and velocity.
/// </summary>
public sealed class CharacterControllerComponent : Component
{
    internal override ComponentType Type => ComponentType.CharacterController;

    /// <summary>
    /// Desired horizontal world velocity in world units per second (<see cref="WorldUnits"/>; the Y part is ignored by
    /// gravity/jump handling).
    /// </summary>
    public void SetMoveVelocity(Vector3 velocity) => InternalCalls.SetCharacterMoveVelocity(EntityID, velocity);

    /// <summary>Jumps with the given upward speed (world units per second) on the next physics step, if standing on the ground.</summary>
    public void Jump(float speed) => InternalCalls.CharacterJump(EntityID, speed);

    public bool IsGrounded => InternalCalls.IsCharacterGrounded(EntityID);
    /// <summary>The velocity the last physics step moved it with, in world units per second.</summary>
    public Vector3 Velocity => InternalCalls.GetCharacterVelocity(EntityID);
}
