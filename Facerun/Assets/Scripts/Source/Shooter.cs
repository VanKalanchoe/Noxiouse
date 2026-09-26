using System;
using Nox;

namespace Facerun;

/// <summary>
/// Put this on the camera: a left click spawns the prefab in front of it and shoots it along the view direction.
/// </summary>
public sealed class Shooter : EntityBehaviour
{
    [Expose] public string Prefab = "Prefabs/GlowBall.nprefab"; // path under the asset directory
    [Expose] public float Speed = 20.0f;                        // m/s
    [Expose] public float SpawnDistance = 1.5f;                 // m in front of the camera

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
