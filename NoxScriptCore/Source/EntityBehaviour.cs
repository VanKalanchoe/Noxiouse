using System.Reflection;

namespace Nox;

public abstract class EntityBehaviour
{
    // Assigned by the native runtime immediately after construction.
    public ulong EntityID;

    /// <summary>The entity this script is on.</summary>
    public Entity Self => new(EntityID);

    public bool HasComponent<T>() where T : Component, new() =>
        new Entity(EntityID).HasComponent<T>();

    public T GetComponent<T>() where T : Component, new() =>
        new Entity(EntityID).GetComponent<T>();

    public bool TryGetComponent<T>(out T? component) where T : Component, new() =>
        new Entity(EntityID).TryGetComponent(out component);

    public Entity FindChild(string path) => new(InternalCalls.FindChild(EntityID, path));

    protected virtual void OnCreate() { }
    protected virtual void OnUpdate(float deltaTime) { }
    /// <summary>
    /// At a fixed rate (<see cref="Time.FixedDeltaTime"/>, 60 per second), zero or more times a frame, before OnUpdate: for
    /// forces, velocities and anything that should not depend on the frame rate.
    /// </summary>
    protected virtual void OnFixedUpdate(float fixedDeltaTime) { }
    /// <summary>
    /// After every script's OnUpdate and after physics and animation moved things this frame: for a camera that follows a
    /// character, or anything that reads where things ended up. Transforms set here show this frame.
    /// </summary>
    protected virtual void OnLateUpdate(float deltaTime) { }
    protected virtual void OnDestroy() { }
    protected virtual void OnAfterReload() { }
    /// <summary>
    /// An animation notify this entity's animator crossed this frame (a named moment of a clip, set in the clip's window:
    /// "FootL", "Hit"), from clips, blend spaces and montages.
    /// </summary>
    protected virtual void OnAnimationEvent(string name) { }

    // Which of the optional callbacks the script's class implements (1: OnFixedUpdate, 2: OnLateUpdate): the engine calls only
    // those, once per class.
    internal int GetCallbackMask()
    {
        int mask = 0;
        if (Overrides(nameof(OnFixedUpdate))) mask |= 1;
        if (Overrides(nameof(OnLateUpdate))) mask |= 2;
        return mask;
    }

    bool Overrides(string name)
    {
        MethodInfo? method = GetType().GetMethod(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        return method != null && method.DeclaringType != typeof(EntityBehaviour);
    }
}
