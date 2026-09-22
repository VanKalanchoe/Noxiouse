using System.Runtime.InteropServices;

namespace Nox;

internal static unsafe class InternalCalls
{
#pragma warning disable CS0649 // Assigned by Coral when the assembly is loaded.
    private static delegate*<NativeString, void> Log_Info;
    private static delegate*<int, uint> Input_IsKeyDown;
    private static delegate*<NativeString, ulong> Entity_FindByName;
    private static delegate*<ulong, NativeString, ulong> Entity_FindChild;
    private static delegate*<ulong, int, uint> Entity_HasComponent;
    private static delegate*<ulong, Transform*, void> Transform_GetLocal;
    private static delegate*<ulong, Transform*, void> Transform_SetLocal;
    private static delegate*<ulong, Transform*, void> Transform_GetWorld;
    private static delegate*<ulong, Transform*, void> Transform_SetWorld;
#pragma warning restore CS0649

    internal static void LogInfo(string message)
    {
        using NativeString nativeMessage = new(message);
        Log_Info(nativeMessage);
    }

    internal static bool IsKeyDown(int keycode) => Input_IsKeyDown(keycode) != 0;

    internal static ulong FindEntityByName(string name)
    {
        using NativeString nativeName = new(name);
        return Entity_FindByName(nativeName);
    }

    internal static ulong FindChild(ulong entityID, string path)
    {
        using NativeString nativePath = new(path);
        return Entity_FindChild(entityID, nativePath);
    }

    internal static bool HasComponent(ulong entityID, ComponentType type) =>
        Entity_HasComponent(entityID, (int)type) != 0;

    internal static Transform GetLocalTransform(ulong entityID)
    {
        Transform value = default;
        Transform_GetLocal(entityID, &value);
        return value;
    }

    internal static void SetLocalTransform(ulong entityID, Transform value) =>
        Transform_SetLocal(entityID, &value);

    internal static Transform GetWorldTransform(ulong entityID)
    {
        Transform value = default;
        Transform_GetWorld(entityID, &value);
        return value;
    }

    internal static void SetWorldTransform(ulong entityID, Transform value) =>
        Transform_SetWorld(entityID, &value);

    // Matches Coral::String without making the public Nox API depend on
    // Coral.Managed. The allocation lives only for the duration of the call.
    [StructLayout(LayoutKind.Explicit, Size = 16)]
    private struct NativeString : IDisposable
    {
        [FieldOffset(0)] private IntPtr _data;
        [FieldOffset(8)] private uint _isDisposed;

        internal NativeString(string value)
        {
            _data = Marshal.StringToCoTaskMemAuto(value);
            _isDisposed = 0;
        }

        public void Dispose()
        {
            if (_isDisposed != 0)
                return;

            if (_data != IntPtr.Zero)
            {
                Marshal.FreeCoTaskMem(_data);
                _data = IntPtr.Zero;
            }

            _isDisposed = 1;
        }
    }
}
