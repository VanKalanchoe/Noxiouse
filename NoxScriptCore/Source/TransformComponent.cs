namespace Nox;

public sealed class TransformComponent : Component
{
    internal override ComponentType Type => ComponentType.Transform;

    public Transform Local
    {
        get => InternalCalls.GetLocalTransform(EntityID);
        set => InternalCalls.SetLocalTransform(EntityID, value);
    }

    public Transform World
    {
        get => InternalCalls.GetWorldTransform(EntityID);
        set => InternalCalls.SetWorldTransform(EntityID, value);
    }

    public Vector3 LocalPosition
    {
        get => Local.Position;
        set
        {
            Transform transform = Local;
            transform.Position = value;
            Local = transform;
        }
    }

    public Vector3 LocalRotation
    {
        get => Local.Rotation;
        set
        {
            Transform transform = Local;
            transform.Rotation = value;
            Local = transform;
        }
    }

    public Vector3 LocalScale
    {
        get => Local.Scale;
        set
        {
            Transform transform = Local;
            transform.Scale = value;
            Local = transform;
        }
    }

    public Vector3 WorldPosition
    {
        get => World.Position;
        set
        {
            Transform transform = World;
            transform.Position = value;
            World = transform;
        }
    }

    public Vector3 WorldRotation
    {
        get => World.Rotation;
        set
        {
            Transform transform = World;
            transform.Rotation = value;
            World = transform;
        }
    }

    public Vector3 WorldScale
    {
        get => World.Scale;
        set
        {
            Transform transform = World;
            transform.Scale = value;
            World = transform;
        }
    }
}
