using System.Runtime.InteropServices;

namespace Nox;

/// <summary>
/// A position in the world, in double precision (large world coordinates, like Unreal's FVector): a float position loses its
/// precision a few kilometres from the world's origin, this one keeps a fraction of a millimetre at any distance a world reaches.
/// Positions (<see cref="Transform.Position"/>, <see cref="RaycastHit.Point"/>, ...) are Vector3d; directions, velocities and
/// offsets stay <see cref="Vector3"/>. A Vector3 widens to it on its own; narrowing back is an explicit cast, e.g. the difference
/// of two positions as a direction: <c>(Vector3)(target - position)</c>.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct Vector3d
{
    public double X;
    public double Y;
    public double Z;

    public Vector3d(double x, double y, double z) => (X, Y, Z) = (x, y, z);

    public static Vector3d operator +(Vector3d a, Vector3d b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static Vector3d operator -(Vector3d a, Vector3d b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    public static Vector3d operator +(Vector3d position, Vector3 offset) => new(position.X + offset.X, position.Y + offset.Y, position.Z + offset.Z);
    public static Vector3d operator -(Vector3d position, Vector3 offset) => new(position.X - offset.X, position.Y - offset.Y, position.Z - offset.Z);
    public static Vector3d operator *(Vector3d value, double scale) => new(value.X * scale, value.Y * scale, value.Z * scale);

    public static implicit operator Vector3d(Vector3 value) => new(value.X, value.Y, value.Z);
    public static explicit operator Vector3(Vector3d value) => new((float)value.X, (float)value.Y, (float)value.Z);

    public double Length() => System.Math.Sqrt(X * X + Y * Y + Z * Z);
    public static double Distance(Vector3d a, Vector3d b) => (a - b).Length();

    public override string ToString() => $"({X:0.###}, {Y:0.###}, {Z:0.###})";
}
