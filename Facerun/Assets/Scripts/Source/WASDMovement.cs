using Nox;

namespace Facerun;

public sealed class WASDMovement : EntityBehaviour
{
    [Expose] public float Speed = 5.0f;

    protected override void OnCreate() =>
        Log.Info($"WASDMovement created for entity {EntityID}");

    protected override void OnUpdate(float deltaTime)
    {
        Vector3 direction = new();
        if (Input.IsKeyDown(KeyCode.W)) direction.Z -= 1.0f;
        if (Input.IsKeyDown(KeyCode.S)) direction.Z += 1.0f;
        if (Input.IsKeyDown(KeyCode.A)) direction.X -= 1.0f;
        if (Input.IsKeyDown(KeyCode.D)) direction.X += 1.0f;

        TransformComponent transform = GetComponent<TransformComponent>();
        transform.LocalPosition += direction * (Speed * deltaTime);
    }
}
