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

    /// <summary>Shows a mesh built at run time on this entity (null: nothing).</summary>
    public void SetMesh(Mesh? mesh) => InternalCalls.SetEntityMesh(ID, mesh?.Handle ?? 0);

    /// <summary>
    /// Makes the entity solid with the mesh's triangles: a static collider at its transform as it is now, for characters,
    /// rigid bodies and <see cref="Physics.Raycast"/> (null removes it). Set it again after the mesh's data changes or the
    /// entity moves. Play only.
    /// </summary>
    public void SetMeshCollider(Mesh? mesh, float friction = 0.5f, float restitution = 0.0f) =>
        InternalCalls.SetEntityMeshCollider(ID, mesh?.Handle ?? 0, friction, restitution);

    public static Entity FindByName(string name) =>
        new(InternalCalls.FindEntityByName(name));
}
