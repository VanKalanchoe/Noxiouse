using System.Runtime.InteropServices;

namespace Nox;

/// <summary>What <see cref="Physics.Raycast"/> hit.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct RaycastHit
{
    private ulong m_Entity;
    public Vector3 Point;
    public Vector3 Normal;
    public float Distance;

    public Entity Entity => new(m_Entity);
}

/// <summary>Queries against the running scene's 3D physics (Play only).</summary>
public static class Physics
{
    /// <summary>
    /// The first collider along the ray within <paramref name="maxDistance"/>: rigid bodies' colliders and mesh colliders
    /// (<see cref="Entity.SetMeshCollider"/>). Picking a block: cast from the camera, step half a block back along the normal to
    /// place one, half a block in to break one.
    /// </summary>
    public static bool Raycast(Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit) =>
        InternalCalls.Raycast(origin, direction, maxDistance, out hit);
}
