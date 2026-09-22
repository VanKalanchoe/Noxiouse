namespace Nox;

public abstract class EntityBehaviour
{
    // Assigned by the native runtime immediately after construction.
    public ulong EntityID;

    public bool HasComponent<T>() where T : Component, new() =>
        new Entity(EntityID).HasComponent<T>();

    public T GetComponent<T>() where T : Component, new() =>
        new Entity(EntityID).GetComponent<T>();

    public bool TryGetComponent<T>(out T? component) where T : Component, new() =>
        new Entity(EntityID).TryGetComponent(out component);

    public Entity FindChild(string path) => new(InternalCalls.FindChild(EntityID, path));

    protected virtual void OnCreate() { }
    protected virtual void OnUpdate(float deltaTime) { }
    protected virtual void OnDestroy() { }
    protected virtual void OnAfterReload() { }
}
