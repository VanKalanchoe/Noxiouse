using System;
using Nox;

namespace Facerun;

/// <summary>
/// Walks a Character Controller 3D with WASD (Shift = run, Space = jump), turns it towards its movement direction and
/// feeds the animation graph the parameters "Speed" (float, horizontal m/s) and "IsGrounded" (bool).
/// </summary>
public sealed class CharacterMovement : EntityBehaviour
{
    [Expose] public float WalkSpeed = 2.0f; // m/s
    [Expose] public float RunSpeed = 5.0f; // m/s (Unreal template: 500 cm/s)
    [Expose] public float JumpSpeed = 7.0f; // Unreal's third-person template: 700 cm/s with gravity x1.75
    [Expose] public float TurnSpeed = 12.0f;
    [Expose] public float ModelYawOffset = 0.0f; // radians; the model's forward axis relative to +Z

    private float _yaw;

    protected override void OnUpdate(float deltaTime)
    {
        CharacterControllerComponent character = GetComponent<CharacterControllerComponent>();
        TryGetComponent(out AnimatorComponent? animator);

        Vector3 direction = new();
        if (Input.IsKeyDown(KeyCode.W)) direction.Z -= 1.0f;
        if (Input.IsKeyDown(KeyCode.S)) direction.Z += 1.0f;
        if (Input.IsKeyDown(KeyCode.A)) direction.X -= 1.0f;
        if (Input.IsKeyDown(KeyCode.D)) direction.X += 1.0f;

        float length = MathF.Sqrt(direction.X * direction.X + direction.Z * direction.Z);
        float speed = Input.IsKeyDown(KeyCode.LeftShift) ? RunSpeed : WalkSpeed;

        if (length > 0.0f)
        {
            direction = direction * (1.0f / length);
            character.SetMoveVelocity(direction * speed);

            float targetYaw = MathF.Atan2(direction.X, direction.Z) + ModelYawOffset;
            float delta = MathF.IEEERemainder(targetYaw - _yaw, 2.0f * MathF.PI);
            _yaw += delta * MathF.Min(1.0f, TurnSpeed * deltaTime);

            TransformComponent transform = GetComponent<TransformComponent>();
            Vector3 rotation = transform.LocalRotation;
            rotation.Y = _yaw;
            transform.LocalRotation = rotation;
        }
        else
        {
            character.SetMoveVelocity(new Vector3());
        }

        if (character.IsGrounded && Input.IsKeyDown(KeyCode.Space))
            character.Jump(JumpSpeed);

        Vector3 velocity = character.Velocity;
        // "Speed": horizontal speed in m/s -- the animation graph's state machine picks Idle / Walk / Run from it.
        animator?.SetFloat("Speed", MathF.Sqrt(velocity.X * velocity.X + velocity.Z * velocity.Z));
        animator?.SetBool("IsGrounded", character.IsGrounded);
    }
}
