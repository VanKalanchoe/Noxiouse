using System;
using Nox;

namespace Facerun;

/// <summary>
/// Put this on the camera: a left click spawns the prefab in front of it and shoots it along the view direction.
/// </summary>
public sealed class Shooter : EntityBehaviour
{
    [Expose] public string Prefab = "Prefabs/GlowBall.nprefab"; // path under the asset directory
    // Stored in cm / cm/s (the project's world unit), but Unit shows and edits the Inspector fields in m / m/s.
    [Expose(Unit = "m/s")] public float Speed = 2000.0f;          // 20 m/s
    [Expose(Unit = "m")] public float SpawnDistance = 150.0f;     // 1.5 m in front of the camera

    private bool _wasDown;

    protected override void OnUpdate(float deltaTime)
    {
        bool down = Input.IsMouseButtonDown(MouseButton.Left);
        if (down && !_wasDown)
            Shoot();
        _wasDown = down;
    }

    private void Shoot()
    {
        Transform world = GetComponent<TransformComponent>().World;

        // Nox cameras look along local -Z; the rotation is (pitch, yaw, 0) in radians.
        float pitch = world.Rotation.X;
        float yaw = world.Rotation.Y;
        Vector3 forward = new(-MathF.Sin(yaw) * MathF.Cos(pitch), MathF.Sin(pitch), -MathF.Cos(yaw) * MathF.Cos(pitch));

        Entity shot = Scene.Instantiate(Prefab, world.Position + forward * SpawnDistance);
        if (!shot.IsValid)
            return;
        if (shot.TryGetComponent(out RigidBody3DComponent? body) && body != null)
            body.LinearVelocity = forward * Speed;
    }
}
