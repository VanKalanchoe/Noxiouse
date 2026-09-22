using Nox;
using System;

namespace Facerun;

public sealed class CameraFollow : EntityBehaviour
{
    [Expose] public Entity Target;
    [Expose] public float Distance = 10.0f;
    [Expose] public float Height = 0.0f;

    protected override void OnCreate()
    {
        if (!Target.IsValid)
        {
            Log.Info("CameraFollow has no Target assigned");
            return;
        }

        Log.Info($"CameraFollow attached entity {EntityID} to target {Target.ID}");
        UpdateCamera();
    }

    protected override void OnUpdate(float deltaTime)
    {
        UpdateCamera();
    }

    private void UpdateCamera()
    {
        if (!Target.IsValid)
            return;

        Vector3 targetPosition = Target.WorldTransform.Position;
        Vector3 cameraPosition = targetPosition + new Vector3(0.0f, Height, Distance);
        Transform cameraTransform = WorldTransform;
        cameraTransform.Position = cameraPosition;

        // Gameplay camera math stays in managed script code. Nox cameras look
        // along local -Z and use local +Y as up.
        Vector3 direction = targetPosition - cameraPosition;
        float length = MathF.Sqrt(direction.X * direction.X +
                                  direction.Y * direction.Y +
                                  direction.Z * direction.Z);
        if (length > 0.0001f)
        {
            direction *= (1.0f / length);
            float pitch = MathF.Asin(Math.Clamp(direction.Y, -1.0f, 1.0f));
            float yaw = MathF.Atan2(-direction.X, -direction.Z);
            cameraTransform.Rotation = new Vector3(pitch, yaw, 0.0f);
        }

        WorldTransform = cameraTransform;
    }
}
