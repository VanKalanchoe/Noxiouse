using Nox;

namespace Facerun;

/// <summary>
/// IK test (Backlog C6): put it on the character (the entity with the Animator). Each frame it sets two IK targets from entities in
/// the scene -- "RightHand" at the entity named HandTargetName (a Two Bone IK node with Target "RightHand" reaches for it) and
/// "LookAt" at the entity named LookTargetName (a Look At node with Target "LookAt" turns the head to it). H toggles the hand,
/// J toggles the look; the weights fade in and out over FadeTime seconds so the switch is smooth.
/// </summary>
public sealed class IKTargetTest : EntityBehaviour
{
    [Expose] public string HandTargetName = "HandTarget";
    [Expose] public string LookTargetName = "LookTarget";
    [Expose] public float FadeTime = 0.3f;

    private bool _hand = true;
    private bool _look = true;
    private bool _handKeyWasDown;
    private bool _lookKeyWasDown;
    private float _handWeight;
    private float _lookWeight;

    protected override void OnUpdate(float deltaTime)
    {
        bool handKey = Input.IsKeyDown(KeyCode.H);
        bool lookKey = Input.IsKeyDown(KeyCode.J);
        if (handKey && !_handKeyWasDown) _hand = !_hand;
        if (lookKey && !_lookKeyWasDown) _look = !_look;
        _handKeyWasDown = handKey;
        _lookKeyWasDown = lookKey;

        float step = FadeTime > 0.0f ? deltaTime / FadeTime : 1.0f;
        _handWeight = Move(_handWeight, _hand ? 1.0f : 0.0f, step);
        _lookWeight = Move(_lookWeight, _look ? 1.0f : 0.0f, step);

        if (!TryGetComponent(out AnimatorComponent? animator) || animator == null)
            return;
        SetTarget(animator, "RightHand", HandTargetName, _handWeight);
        SetTarget(animator, "LookAt", LookTargetName, _lookWeight);
    }

    private static void SetTarget(AnimatorComponent animator, string target, string entityName, float weight)
    {
        Entity entity = Entity.FindByName(entityName);
        if (!entity.IsValid || weight <= 0.0f)
        {
            animator.ClearIKTarget(target);
            return;
        }
        animator.SetIKTarget(target, entity.GetComponent<TransformComponent>().WorldPosition, weight);
    }

    private static float Move(float value, float target, float step) =>
        value < target ? System.MathF.Min(value + step, target) : System.MathF.Max(value - step, target);
}
