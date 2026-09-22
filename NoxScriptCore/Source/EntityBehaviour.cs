namespace Nox;

public abstract class EntityBehaviour
{
    // Assigned by the native runtime immediately after construction.
    public ulong EntityID;

    public Transform LocalTransform
    {
        get => InternalCalls.GetLocalTransform(EntityID);
        set => InternalCalls.SetLocalTransform(EntityID, value);
    }

    public Transform WorldTransform
    {
        get => InternalCalls.GetWorldTransform(EntityID);
        set => InternalCalls.SetWorldTransform(EntityID, value);
    }

    public Entity FindChild(string path) => new(InternalCalls.FindChild(EntityID, path));

    protected virtual void OnCreate() { }
    protected virtual void OnUpdate(float deltaTime) { }
    protected virtual void OnDestroy() { }
    protected virtual void OnAfterReload() { }
}
