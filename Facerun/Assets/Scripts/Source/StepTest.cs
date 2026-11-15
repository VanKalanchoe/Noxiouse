using Nox;
public class StepTest : EntityBehaviour
{
    bool _wasDown;
    protected override void OnUpdate(float dt)
    {
        bool down = Input.IsKeyDown(KeyCode.F);
        if (down && !_wasDown)
            Audio.PlayOneShot("Audio/Footsteps", GetComponent<TransformComponent>().World.Position);
        _wasDown = down;
    }
}
