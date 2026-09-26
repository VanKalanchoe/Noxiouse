using System.Runtime.InteropServices;

namespace Nox;

internal static unsafe class InternalCalls
{
#pragma warning disable CS0649 // Assigned by Coral when the assembly is loaded.
    private static delegate*<NativeString, void> Log_Info;
    private static delegate*<int, uint> Input_IsKeyDown;
    private static delegate*<int, uint> Input_IsMouseButtonDown;
    private static delegate*<NativeString, float, float, float, ulong> Scene_Instantiate;
    private static delegate*<ulong, void> Scene_Destroy;
    private static delegate*<ulong, float, float, float, void> RigidBody_SetLinearVelocity;
    private static delegate*<ulong, float, float, float, void> RigidBody_AddForce;
    private static delegate*<ulong, float, float, float, void> RigidBody_AddImpulse;
    private static delegate*<ulong, Vector3*, void> RigidBody_GetLinearVelocity;
    private static delegate*<NativeString, ulong> Entity_FindByName;
    private static delegate*<ulong, NativeString, ulong> Entity_FindChild;
    private static delegate*<ulong, int, uint> Entity_HasComponent;
    private static delegate*<ulong, NativeString, float, void> Animator_SetFloat;
    private static delegate*<ulong, NativeString, float> Animator_GetFloat;
    private static delegate*<ulong, NativeString, uint, void> Animator_SetBool;
    private static delegate*<ulong, NativeString, uint> Animator_GetBool;
    private static delegate*<ulong, float, float, float, void> Character_SetMoveVelocity;
    private static delegate*<ulong, float, void> Character_Jump;
    private static delegate*<ulong, uint> Character_IsGrounded;
    private static delegate*<ulong, Vector3*, void> Character_GetVelocity;
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

    internal static bool IsMouseButtonDown(int button) => Input_IsMouseButtonDown(button) != 0;

    internal static ulong InstantiatePrefab(string prefabPath, Vector3 position)
    {
        using NativeString nativePath = new(prefabPath);
        return Scene_Instantiate(nativePath, position.X, position.Y, position.Z);
    }

    internal static void DestroyEntity(ulong entityID) => Scene_Destroy(entityID);

    internal static void SetRigidBodyVelocity(ulong entityID, Vector3 velocity) =>
        RigidBody_SetLinearVelocity(entityID, velocity.X, velocity.Y, velocity.Z);

    internal static void AddRigidBodyForce(ulong entityID, Vector3 force) =>
        RigidBody_AddForce(entityID, force.X, force.Y, force.Z);

    internal static void AddRigidBodyImpulse(ulong entityID, Vector3 impulse) =>
        RigidBody_AddImpulse(entityID, impulse.X, impulse.Y, impulse.Z);

    internal static Vector3 GetRigidBodyVelocity(ulong entityID)
    {
        Vector3 value = default;
        RigidBody_GetLinearVelocity(entityID, &value);
        return value;
    }

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

    internal static void SetAnimatorFloat(ulong entityID, string name, float value)
    {
        using NativeString nativeName = new(name);
        Animator_SetFloat(entityID, nativeName, value);
    }

    internal static float GetAnimatorFloat(ulong entityID, string name)
    {
        using NativeString nativeName = new(name);
        return Animator_GetFloat(entityID, nativeName);
    }

    internal static void SetAnimatorBool(ulong entityID, string name, bool value)
    {
        using NativeString nativeName = new(name);
        Animator_SetBool(entityID, nativeName, value ? 1u : 0u);
    }

    internal static bool GetAnimatorBool(ulong entityID, string name)
    {
        using NativeString nativeName = new(name);
        return Animator_GetBool(entityID, nativeName) != 0;
    }

    internal static void SetCharacterMoveVelocity(ulong entityID, Vector3 velocity) =>
        Character_SetMoveVelocity(entityID, velocity.X, velocity.Y, velocity.Z);

    internal static void CharacterJump(ulong entityID, float speed) => Character_Jump(entityID, speed);

    internal static bool IsCharacterGrounded(ulong entityID) => Character_IsGrounded(entityID) != 0;

    internal static Vector3 GetCharacterVelocity(ulong entityID)
    {
        Vector3 velocity;
        Character_GetVelocity(entityID, &velocity);
        return velocity;
    }

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
