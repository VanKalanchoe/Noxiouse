using Nox;

namespace Facerun;

/// <summary>
/// A behaviour attached to an entity in a Nox scene.
/// </summary>
public sealed class DissolveOnKey : EntityBehaviour
{
    private float _amount;
    
    protected override void OnCreate()
    {
    }

    protected override void OnUpdate(float deltaTime)
    {
        _amount += (Input.IsKeyDown(KeyCode.F) ? 0.5f : -0.5f) * deltaTime;
        _amount = System.Math.Clamp(_amount, 0.0f, 1.0f);
        GetComponent<MeshComponent>().SetScalarParameter("Dissolve", _amount);
    }
}
