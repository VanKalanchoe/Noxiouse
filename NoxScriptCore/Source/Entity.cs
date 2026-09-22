namespace Nox;

public readonly struct Entity
{
    public ulong ID { get; }
    public bool IsValid => ID != 0;

    internal Entity(ulong id) => ID = id;

    public Transform LocalTransform
    {
        get => InternalCalls.GetLocalTransform(ID);
        set => InternalCalls.SetLocalTransform(ID, value);
    }

    public Transform WorldTransform
    {
        get => InternalCalls.GetWorldTransform(ID);
        set => InternalCalls.SetWorldTransform(ID, value);
    }

    public Entity FindChild(string path) => new(InternalCalls.FindChild(ID, path));

    public static Entity FindByName(string name) =>
        new(InternalCalls.FindEntityByName(name));
}
