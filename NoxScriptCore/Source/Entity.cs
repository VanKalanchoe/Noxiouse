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

    /// <summary>
    /// The glTF material variants (KHR_materials_variants) of the meshes on this entity and below it: a model's material sets, a
    /// shoe in three colors. On a model's root it switches every part. Names are the ones the Properties panel lists.
    /// </summary>
    /// <returns>False when no mesh there has the variant (or none is loaded).</returns>
    public bool SetMaterialVariant(string variant) => InternalCalls.SetMaterialVariant(ID, variant);

    /// <summary>Selects a variant by its place in the list (0 to <see cref="MaterialVariantCount"/> - 1; -1: the meshes' own materials).</summary>
    public bool SetMaterialVariant(int index) => InternalCalls.SetMaterialVariantIndex(ID, index);

    /// <summary>Gives the meshes their own materials back.</summary>
    public void ClearMaterialVariant() => InternalCalls.SetMaterialVariant(ID, string.Empty);

    /// <summary>How many variants the meshes here have (0 for none).</summary>
    public int MaterialVariantCount => InternalCalls.GetMaterialVariantCount(ID);

    /// <summary>The variant shown now: its index, -1 for the meshes' own materials, -2 when the slots were set some other way.</summary>
    public int MaterialVariantIndex => InternalCalls.GetMaterialVariantIndex(ID);

    public static Entity FindByName(string name) =>
        new(InternalCalls.FindEntityByName(name));
}
