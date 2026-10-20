namespace Nox;

/// <summary>
/// The entity's Rigidbody 3D (its physics body). Velocities and forces act on the body while the scene plays or simulates;
/// the entity's transform follows the body.
/// </summary>
public sealed class RigidBody3DComponent : Component
{
    internal override ComponentType Type => ComponentType.RigidBody3D;

    /// <summary>The body's linear velocity in world units per second (<see cref="WorldUnits"/>).</summary>
    public Vector3 LinearVelocity
    {
        get => InternalCalls.GetRigidBodyVelocity(EntityID);
        set => InternalCalls.SetRigidBodyVelocity(EntityID, value);
    }

    /// <summary>A force (kg * world units / s^2: newtons in a meter project), applied for the next physics step.</summary>
    public void AddForce(Vector3 force) => InternalCalls.AddRigidBodyForce(EntityID, force);

    /// <summary>An instant change of momentum (kg * world units / s).</summary>
    public void AddImpulse(Vector3 impulse) => InternalCalls.AddRigidBodyImpulse(EntityID, impulse);
}
