namespace Nox;

internal enum ComponentType
{
    Transform = 1,
    Animator = 2,
    CharacterController = 3,
    RigidBody3D = 4
}

public abstract class Component
{
    internal ulong EntityID { get; private set; }
    internal abstract ComponentType Type { get; }

    public Entity Entity => new(EntityID);

    internal void Bind(ulong entityID) => EntityID = entityID;
}
