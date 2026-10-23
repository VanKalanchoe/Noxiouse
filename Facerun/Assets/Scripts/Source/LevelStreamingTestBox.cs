using Nox;

namespace Facerun;

/// <summary>
/// A colored box built at run time (no mesh asset needed), for the level streaming test scenes. With Spin set it turns, which
/// shows that a streamed level's scripts start when the level comes in and stop when it goes.
/// </summary>
public sealed class LevelStreamingTestBox : EntityBehaviour
{
    [Expose(Tooltip = "Edge lengths in world units")] public Vector3 Size = new(100.0f, 100.0f, 100.0f);
    [Expose(Tooltip = "Base color (0-1)")] public Vector3 Color = new(0.8f, 0.8f, 0.8f);
    [Expose(Tooltip = "Light the box gives off")] public Vector3 Emissive = new(0.0f, 0.0f, 0.0f);
    [Expose(Tooltip = "Turns around Y, in radians per second")] public float SpinSpeed = 0.0f;

    Mesh? m_Mesh;

    protected override void OnCreate()
    {
        m_Mesh = new Mesh();
        m_Mesh.SetData(BuildBox(Size * 0.5f));
        m_Mesh.SetColor(Color.X, Color.Y, Color.Z, Emissive);
        Self.SetMesh(m_Mesh);
    }

    protected override void OnUpdate(float deltaTime)
    {
        if (SpinSpeed == 0.0f)
            return;
        TransformComponent transform = GetComponent<TransformComponent>();
        Vector3 rotation = transform.LocalRotation;
        rotation.Y += SpinSpeed * deltaTime;
        transform.LocalRotation = rotation;
    }

    protected override void OnDestroy()
    {
        m_Mesh?.Dispose();
        m_Mesh = null;
    }

    // Six faces with their own vertices (hard edges), counter-clockwise seen from outside.
    static MeshBuilder BuildBox(Vector3 half)
    {
        MeshBuilder builder = new();
        AddFace(builder, new Vector3(1, 0, 0), new Vector3(0, 0, -1), new Vector3(0, 1, 0), half);
        AddFace(builder, new Vector3(-1, 0, 0), new Vector3(0, 0, 1), new Vector3(0, 1, 0), half);
        AddFace(builder, new Vector3(0, 1, 0), new Vector3(1, 0, 0), new Vector3(0, 0, -1), half);
        AddFace(builder, new Vector3(0, -1, 0), new Vector3(1, 0, 0), new Vector3(0, 0, 1), half);
        AddFace(builder, new Vector3(0, 0, 1), new Vector3(1, 0, 0), new Vector3(0, 1, 0), half);
        AddFace(builder, new Vector3(0, 0, -1), new Vector3(-1, 0, 0), new Vector3(0, 1, 0), half);
        return builder;
    }

    // right x up = normal for every face, so right then up is counter-clockwise seen from outside.
    static void AddFace(MeshBuilder builder, Vector3 normal, Vector3 right, Vector3 up, Vector3 half)
    {
        Vector3 center = Scale(normal, half);
        Vector3 r = Scale(right, half);
        Vector3 u = Scale(up, half);
        uint first = builder.AddVertex(center - r - u, normal, new Vector2(0, 1));
        builder.AddVertex(center + r - u, normal, new Vector2(1, 1));
        builder.AddVertex(center + r + u, normal, new Vector2(1, 0));
        builder.AddVertex(center - r + u, normal, new Vector2(0, 0));
        builder.Indices.AddRange(new[] { first, first + 1, first + 2, first, first + 2, first + 3 });
    }

    static Vector3 Scale(Vector3 a, Vector3 b) => new(a.X * b.X, a.Y * b.Y, a.Z * b.Z);
}
