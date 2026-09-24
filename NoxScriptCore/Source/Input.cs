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

public static class Input
{
    public static bool IsKeyDown(KeyCode key) => InternalCalls.IsKeyDown((int)key);
}
