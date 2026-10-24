namespace Nox;

/// <summary>Keys by their place on the keyboard (USB HID / SDL scancodes): W is the key left of E on any layout.</summary>
public enum KeyCode
{
    A = 4, B = 5, C = 6, D = 7, E = 8, F = 9, G = 10, H = 11, I = 12, J = 13, K = 14, L = 15, M = 16,
    N = 17, O = 18, P = 19, Q = 20, R = 21, S = 22, T = 23, U = 24, V = 25, W = 26, X = 27, Y = 28, Z = 29,

    Alpha1 = 30, Alpha2 = 31, Alpha3 = 32, Alpha4 = 33, Alpha5 = 34, Alpha6 = 35, Alpha7 = 36, Alpha8 = 37, Alpha9 = 38, Alpha0 = 39,

    Return = 40,
    Escape = 41,
    Backspace = 42,
    Tab = 43,
    Space = 44,

    F1 = 58, F2 = 59, F3 = 60, F4 = 61, F5 = 62, F6 = 63, F7 = 64, F8 = 65, F9 = 66, F10 = 67, F11 = 68, F12 = 69,

    Right = 79,
    Left = 80,
    Down = 81,
    Up = 82,

    LeftControl = 224,
    LeftShift = 225,
    LeftAlt = 226,
    RightControl = 228,
    RightShift = 229,
    RightAlt = 230
}

public enum MouseButton
{
    Left = 1,
    Middle = 2,
    Right = 3
}

/// <summary>
/// The keyboard and mouse, for the game. In the editor the game has the keys while the viewport is the target and the mouse
/// while the cursor is over it: the calls read nothing otherwise (a click in the hierarchy or a key typed in a text field is not
/// the game's).
/// </summary>
public static class Input
{
    public static bool IsKeyDown(KeyCode key) => InternalCalls.IsKeyDown((int)key);

    /// <summary>True while this mouse button is held.</summary>
    public static bool IsMouseButtonDown(MouseButton button) => InternalCalls.IsMouseButtonDown((int)button);

    /// <summary>How far the mouse moved during the last frame, in pixels (X right, Y down). Also while the cursor is locked.</summary>
    public static Vector2 MouseDelta => InternalCalls.GetMouseDelta();

    /// <summary>Mouse wheel notches turned during the last frame (positive: away from you).</summary>
    public static float ScrollDelta => InternalCalls.GetScrollDelta();

    /// <summary>
    /// Locked, the cursor is hidden and held in the window and <see cref="MouseDelta"/> keeps reporting motion (mouse look). Locking
    /// needs the game's mouse (in the editor: the cursor over the viewport); Stop (or Esc) in the editor lets it go.
    /// </summary>
    public static bool CursorLocked
    {
        get => InternalCalls.IsCursorLocked();
        set => InternalCalls.SetCursorLocked(value);
    }
}
