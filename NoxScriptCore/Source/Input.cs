namespace Nox;

public enum KeyCode
{
    A = 4,
    D = 7,
    S = 22,
    W = 26,
    Space = 44,
    LeftShift = 225
}

public enum MouseButton
{
    Left = 1,
    Middle = 2,
    Right = 3
}

public static class Input
{
    public static bool IsKeyDown(KeyCode key) => InternalCalls.IsKeyDown((int)key);

    /// <summary>True while only this mouse button is held.</summary>
    public static bool IsMouseButtonDown(MouseButton button) => InternalCalls.IsMouseButtonDown((int)button);
}
