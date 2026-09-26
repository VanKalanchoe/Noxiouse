using Nox;

namespace Facerun;

/// <summary>Removes its entity after <see cref="Lifetime"/> seconds: put it on a prefab that is shot or spawned.</summary>
public sealed class Projectile : EntityBehaviour
{
    [Expose] public float Lifetime = 5.0f; // seconds

    private float _age;

    protected override void OnUpdate(float deltaTime)
    {
        _age += deltaTime;
        if (_age >= Lifetime)
            Scene.Destroy(Self);
    }
}
