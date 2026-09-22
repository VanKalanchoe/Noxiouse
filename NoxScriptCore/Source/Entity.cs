namespace Nox;

public readonly struct Entity
{
    public ulong ID { get; }
    public bool IsValid => ID != 0;

    internal Entity(ulong id) => ID = id;

    public bool HasComponent<T>() where T : Component, new()
    {
        T component = new();
        return InternalCalls.HasComponent(ID, component.Type);
    }

    public T GetComponent<T>() where T : Component, new()
    {
        T component = new();
        if (!InternalCalls.HasComponent(ID, component.Type))
            throw new InvalidOperationException($"Entity {ID} does not have component {typeof(T).Name}.");

        component.Bind(ID);
        return component;
    }

    public bool TryGetComponent<T>(out T? component) where T : Component, new()
    {
        T candidate = new();
        if (!InternalCalls.HasComponent(ID, candidate.Type))
        {
            component = null;
            return false;
        }

        candidate.Bind(ID);
        component = candidate;
        return true;
    }

    public Entity FindChild(string path) => new(InternalCalls.FindChild(ID, path));

    public static Entity FindByName(string name) =>
        new(InternalCalls.FindEntityByName(name));
}
