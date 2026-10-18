using System.Runtime.InteropServices;

namespace Nox;

[StructLayout(LayoutKind.Sequential)]
public struct Vector2
{
    public float X;
    public float Y;

    public Vector2(float x, float y) => (X, Y) = (x, y);

    public static Vector2 operator +(Vector2 a, Vector2 b) => new(a.X + b.X, a.Y + b.Y);

    public static Vector2 operator -(Vector2 a, Vector2 b) => new(a.X - b.X, a.Y - b.Y);

    public static Vector2 operator *(Vector2 value, float scale) => new(value.X * scale, value.Y * scale);
}
