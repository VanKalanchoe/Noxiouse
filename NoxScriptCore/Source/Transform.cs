using System.Runtime.InteropServices;

namespace Nox;

[StructLayout(LayoutKind.Sequential)]
public struct Transform
{
    public Vector3 Position;
    public Vector3 Rotation;
    public Vector3 Scale;

    public Transform(Vector3 position, Vector3 rotation, Vector3 scale) =>
        (Position, Rotation, Scale) = (position, rotation, scale);
}
