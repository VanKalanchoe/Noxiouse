using System;
using Nox;

namespace Facerun;

/// <summary>
/// The editor camera's fly mode, in Play: put it on the camera entity. Hold the right mouse button to look around (the cursor is
/// locked meanwhile, as in the editor), WASD to move, E / Q up and down, Shift for 3x; the wheel, while looking, changes the speed.
/// The same feel as the viewport: 5 m/s, the editor's look sensitivity, no roll.
/// </summary>
public sealed class FlyCamera : EntityBehaviour
{
    [Expose] public float SpeedMetersPerSecond = 5.0f;
    [Expose] public float FastMultiplier = 3.0f;
    // Radians per pixel of mouse motion: the editor camera's 0.003 x 0.8.
    [Expose] public float LookSensitivity = 0.0024f;
    // Off: the mouse always looks (locked for the whole of Play, like a first person game).
    [Expose] public bool LookWithRightMouseButton = true;

    const float MaxPitch = 1.553f; // 89 degrees: straight up or down has no yaw left
    const float MinSpeed = 0.1f, MaxSpeed = 1000.0f; // m/s, the editor's range

    float m_Pitch;
    float m_Yaw;

    protected override void OnCreate()
    {
        // Where the camera looks now, as pitch and yaw: from its forward direction, whatever Euler angles it was given (the gizmo
        // can store the same rotation as X 180 / Z 180).
        Vector3 forward = Rotate(GetComponent<TransformComponent>().WorldRotation, new Vector3(0.0f, 0.0f, -1.0f));
        m_Pitch = MathF.Asin(Math.Clamp(forward.Y, -1.0f, 1.0f));
        m_Yaw = MathF.Atan2(-forward.X, -forward.Z);
    }

    protected override void OnDestroy()
    {
        if (Input.CursorLocked)
            Input.CursorLocked = false;
    }

    protected override void OnUpdate(float deltaTime)
    {
        bool looking = !LookWithRightMouseButton || Input.IsMouseButtonDown(MouseButton.Right);
        if (looking != Input.CursorLocked)
            Input.CursorLocked = looking;

        TransformComponent transform = GetComponent<TransformComponent>();
        Transform world = transform.World;
        bool changed = false;

        if (looking)
        {
            // Mouse right turns right, mouse down looks down (the editor's signs).
            Vector2 delta = Input.MouseDelta;
            if (delta.X != 0.0f || delta.Y != 0.0f)
            {
                m_Yaw -= delta.X * LookSensitivity;
                m_Pitch = Math.Clamp(m_Pitch - delta.Y * LookSensitivity, -MaxPitch, MaxPitch);
                world.Rotation = new Vector3(m_Pitch, m_Yaw, 0.0f);
                changed = true;
            }
            float scroll = Input.ScrollDelta;
            if (scroll != 0.0f)
                SpeedMetersPerSecond = Math.Clamp(SpeedMetersPerSecond * MathF.Exp(scroll * 0.2f), MinSpeed, MaxSpeed);
        }

        // Forward, right and up of the view (yaw about world up, then pitch about the camera's right).
        float cosPitch = MathF.Cos(m_Pitch), sinPitch = MathF.Sin(m_Pitch);
        float cosYaw = MathF.Cos(m_Yaw), sinYaw = MathF.Sin(m_Yaw);
        Vector3 forwardDirection = new(-cosPitch * sinYaw, sinPitch, -cosPitch * cosYaw);
        Vector3 rightDirection = new(cosYaw, 0.0f, -sinYaw);
        Vector3 upDirection = new(sinPitch * sinYaw, cosPitch, sinPitch * cosYaw);

        Vector3 move = new();
        if (Input.IsKeyDown(KeyCode.W)) move += forwardDirection;
        if (Input.IsKeyDown(KeyCode.S)) move -= forwardDirection;
        if (Input.IsKeyDown(KeyCode.D)) move += rightDirection;
        if (Input.IsKeyDown(KeyCode.A)) move -= rightDirection;
        if (Input.IsKeyDown(KeyCode.E)) move += upDirection;
        if (Input.IsKeyDown(KeyCode.Q)) move -= upDirection;

        float length = MathF.Sqrt(move.X * move.X + move.Y * move.Y + move.Z * move.Z);
        if (length > 1e-3f)
        {
            float speed = WorldUnits.FromMeters(SpeedMetersPerSecond) * (Input.IsKeyDown(KeyCode.LeftShift) ? FastMultiplier : 1.0f);
            world.Position += move * (speed * deltaTime / length);
            changed = true;
        }

        if (changed)
            transform.World = world;
    }

    // A direction turned by Euler angles in radians, the engine's order (glm::quat(euler): X, then Y, then Z).
    static Vector3 Rotate(Vector3 euler, Vector3 v)
    {
        float cx = MathF.Cos(euler.X * 0.5f), sx = MathF.Sin(euler.X * 0.5f);
        float cy = MathF.Cos(euler.Y * 0.5f), sy = MathF.Sin(euler.Y * 0.5f);
        float cz = MathF.Cos(euler.Z * 0.5f), sz = MathF.Sin(euler.Z * 0.5f);
        float w = cx * cy * cz + sx * sy * sz;
        float x = sx * cy * cz - cx * sy * sz;
        float y = cx * sy * cz + sx * cy * sz;
        float z = cx * cy * sz - sx * sy * cz;

        // v + 2w (q x v) + 2 q x (q x v)
        Vector3 q = new(x, y, z);
        Vector3 t = Cross(q, v) * 2.0f;
        return v + t * w + Cross(q, t);
    }

    static Vector3 Cross(Vector3 a, Vector3 b) => new(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);
}
