using System.Runtime.InteropServices;

namespace Nox;

/// <summary>
/// A mesh a script builds at run time: a voxel chunk, a procedural rock, a generated road. Fill it with <see cref="SetData"/>
/// (or a <see cref="MeshBuilder"/>) and show it with <see cref="Entity.SetMesh"/>; it draws, casts shadows, is ray traced and
/// picked like an imported mesh, and <see cref="Entity.SetMeshCollider"/> makes it solid. Setting new data swaps it in on
/// every entity showing it, in the same frame.
/// <para>
/// Main thread only (OnCreate / OnUpdate): build the arrays in a <see cref="Jobs"/> job if they take long, then call
/// SetData when the job is done. A mesh lives until <see cref="Dispose"/> or until Play stops.
/// </para>
/// </summary>
public sealed class Mesh : IDisposable
{
    public ulong Handle { get; private set; }
    public bool IsValid => Handle != 0;

    public Mesh() => Handle = InternalCalls.CreateMesh();

    /// <summary>
    /// Replaces the geometry. Counter-clockwise triangles seen from the front, three indices each. <paramref name="normals"/>
    /// may be left empty (smooth normals are worked out: give hard edges their own vertices), <paramref name="uvs"/> too
    /// (zero). False, leaving the mesh as it was, when the arrays do not fit together (the log says why).
    /// </summary>
    public bool SetData(ReadOnlySpan<Vector3> positions, ReadOnlySpan<uint> indices, ReadOnlySpan<Vector3> normals = default,
                        ReadOnlySpan<Vector2> uvs = default) =>
        InternalCalls.SetMeshData(Handle, positions, indices, normals, uvs);

    public bool SetData(MeshBuilder builder) =>
        SetData(CollectionsMarshal.AsSpan(builder.Positions), CollectionsMarshal.AsSpan(builder.Indices),
                builder.Normals.Count == builder.Positions.Count ? CollectionsMarshal.AsSpan(builder.Normals) : default,
                builder.UVs.Count == builder.Positions.Count ? CollectionsMarshal.AsSpan(builder.UVs) : default);

    /// <summary>No geometry (entities showing it draw nothing until new data comes).</summary>
    public void Clear() => InternalCalls.ClearMesh(Handle);

    /// <summary>The mesh's own material. Alpha below 1 draws it see-through.</summary>
    public void SetColor(float r, float g, float b, float a = 1.0f, float metallic = 0.0f, float roughness = 0.8f) =>
        InternalCalls.SetMeshColor(Handle, r, g, b, a, metallic, roughness, new Vector3(0.0f, 0.0f, 0.0f));

    /// <summary>Light the surface gives off (in addition to its color), e.g. lava or a glowing ore.</summary>
    public void SetColor(float r, float g, float b, Vector3 emissive, float metallic = 0.0f, float roughness = 0.8f) =>
        InternalCalls.SetMeshColor(Handle, r, g, b, 1.0f, metallic, roughness, emissive);

    /// <summary>
    /// A material asset by its path under the asset directory ("Materials/Grass.nmat"), e.g. a texture atlas the UVs point
    /// into; "" goes back to the mesh's color. False when there is no such material.
    /// </summary>
    public bool SetMaterial(string materialPath) => InternalCalls.SetMeshMaterial(Handle, materialPath);

    /// <summary>Frees the mesh; entities showing it draw nothing.</summary>
    public void Dispose()
    {
        if (Handle == 0)
            return;
        InternalCalls.DestroyMesh(Handle);
        Handle = 0;
    }
}

/// <summary>
/// Geometry being put together for a <see cref="Mesh"/>: plain lists, so it can be filled inside a job (no engine calls) and
/// handed to <see cref="Mesh.SetData(MeshBuilder)"/> on the main thread. Normals and UVs are used when every vertex has one.
/// </summary>
public sealed class MeshBuilder
{
    public readonly List<Vector3> Positions = new();
    public readonly List<Vector3> Normals = new();
    public readonly List<Vector2> UVs = new();
    public readonly List<uint> Indices = new();

    public int VertexCount => Positions.Count;
    public int TriangleCount => Indices.Count / 3;

    /// <summary>Adds a vertex and returns its index.</summary>
    public uint AddVertex(Vector3 position, Vector3 normal, Vector2 uv)
    {
        Positions.Add(position);
        Normals.Add(normal);
        UVs.Add(uv);
        return (uint)(Positions.Count - 1);
    }

    /// <summary>A triangle of three vertices, counter-clockwise seen from the front.</summary>
    public void AddTriangle(uint a, uint b, uint c)
    {
        Indices.Add(a);
        Indices.Add(b);
        Indices.Add(c);
    }

    /// <summary>
    /// A flat quad: four corners counter-clockwise seen from the front (a block face), with its own vertices so its edges stay
    /// hard. <paramref name="uvMin"/> goes to <paramref name="a"/>, <paramref name="uvMax"/> to <paramref name="c"/>.
    /// </summary>
    public void AddQuad(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Vector3 normal, Vector2 uvMin, Vector2 uvMax)
    {
        uint first = AddVertex(a, normal, uvMin);
        AddVertex(b, normal, new Vector2(uvMax.X, uvMin.Y));
        AddVertex(c, normal, uvMax);
        AddVertex(d, normal, new Vector2(uvMin.X, uvMax.Y));
        AddTriangle(first, first + 1, first + 2);
        AddTriangle(first, first + 2, first + 3);
    }

    public void Clear()
    {
        Positions.Clear();
        Normals.Clear();
        UVs.Clear();
        Indices.Clear();
    }
}
