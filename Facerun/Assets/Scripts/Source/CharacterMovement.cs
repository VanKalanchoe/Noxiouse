using System;
using Nox;

namespace Facerun;

/// <summary>
/// Walks a Character Controller 3D with WASD (Shift = run, Space = jump), turns it towards its movement direction and
/// feeds the animation graph the parameters "Speed" (float, horizontal speed in m/s, whatever the world unit), "VerticalSpeed"
/// (float, m/s, up is positive: a jump rises, a fall is negative) and "IsGrounded" (bool). A graph without one of them ignores it.
/// </summary>
public sealed class CharacterMovement : EntityBehaviour
{
    // Stored in cm/s (the project's world unit), but Unit = "m/s" shows and edits the Inspector field in m/s -- type 2, not 200.
    [Expose(Unit = "m/s")] public float WalkSpeed = 200.0f; // 2 m/s
    [Expose(Unit = "m/s")] public float RunSpeed = 500.0f; // 5 m/s (Unreal template)
    [Expose(Unit = "m/s")] public float JumpSpeed = 700.0f; // Unreal's third-person template, with gravity x1.75
    [Expose] public float TurnSpeed = 12.0f;
    [Expose] public float ModelYawOffset = 0.0f; // radians; the model's forward axis relative to +Z
    // Root motion (the Animator's Root Motion on, a clip set whose root bone travels): on the ground the animation moves the
    // character and this only turns it and tells the graph how fast the player wants to go (Speed); in the air it steers as
    // usual, and keeps the momentum the walk / run gave it when no key is held.
    [Expose] public bool UseRootMotion = false;

    private float _yaw;
    private bool _jumpWasDown;

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

        Vector3 velocity = character.Velocity;
        bool rootMotionOnGround = UseRootMotion && character.IsGrounded;
        if (length > 0.0f)
        {
            direction = direction * (1.0f / length);
            character.SetMoveVelocity(rootMotionOnGround ? new Vector3() : direction * speed);

            float targetYaw = MathF.Atan2(direction.X, direction.Z) + ModelYawOffset;
            float delta = MathF.IEEERemainder(targetYaw - _yaw, 2.0f * MathF.PI);
            _yaw += delta * MathF.Min(1.0f, TurnSpeed * deltaTime);

            TransformComponent transform = GetComponent<TransformComponent>();
            Vector3 rotation = transform.LocalRotation;
            rotation.Y = _yaw;
            transform.LocalRotation = rotation;
        }
        else if (UseRootMotion && !character.IsGrounded)
        {
            character.SetMoveVelocity(new Vector3(velocity.X, 0.0f, velocity.Z)); // no braking in the air: the jump keeps its speed
        }
        else
        {
            character.SetMoveVelocity(new Vector3());
        }

        // One jump per press, like Unreal's character (holding Space does not jump again on every landing).
        bool jumpDown = Input.IsKeyDown(KeyCode.Space);
        if (jumpDown && !_jumpWasDown && character.IsGrounded)
            character.Jump(JumpSpeed);
        _jumpWasDown = jumpDown;

        // "Speed": horizontal speed in m/s -- the animation graph's state machine picks Idle / Walk / Run from it (its
        // thresholds are in m/s). The velocity is in world units per second (cm/s here): converted. With root motion the
        // speed the player asks for (the animation then makes it real); otherwise the speed the character moves at.
        float horizontalSpeed = UseRootMotion ? (length > 0.0f ? speed : 0.0f) : MathF.Sqrt(velocity.X * velocity.X + velocity.Z * velocity.Z);
        animator?.SetFloat("Speed", WorldUnits.ToMeters(horizontalSpeed));
        animator?.SetFloat("VerticalSpeed", WorldUnits.ToMeters(velocity.Y));
        animator?.SetBool("IsGrounded", character.IsGrounded);
    }
}
