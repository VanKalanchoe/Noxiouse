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
    private static delegate*<NativeString, float, float, float, ulong> Scene_CreateEntity;
    private static delegate*<ulong> Mesh_Create;
    private static delegate*<ulong, void> Mesh_Destroy;
    private static delegate*<ulong, Vector3*, int, uint*, int, Vector3*, int, Vector2*, int, uint> Mesh_SetData;
    private static delegate*<ulong, void> Mesh_Clear;
    private static delegate*<ulong, float, float, float, float, float, float, float, float, float, void> Mesh_SetColor;
    private static delegate*<ulong, NativeString, uint> Mesh_SetMaterial;
    private static delegate*<ulong, ulong, void> Entity_SetMesh;
    private static delegate*<ulong, ulong, float, float, void> Entity_SetMeshCollider;
    private static delegate*<float, float, float, float, float, float, float, RaycastHit*, uint> Physics_Raycast;
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
    private static delegate*<ulong, NativeString, NativeString, float, float, float, uint> Animator_PlayMontage;
    private static delegate*<ulong, NativeString, float, void> Animator_StopMontage;
    private static delegate*<ulong, NativeString, uint> Animator_IsMontagePlaying;
    private static delegate*<ulong, uint, void> Timeline_Play;
    private static delegate*<ulong, void> Timeline_Stop;
    private static delegate*<ulong, uint> Timeline_IsPlaying;
    private static delegate*<ulong, float> Timeline_GetTime;
    private static delegate*<ulong, float, void> Timeline_SetTime;
    private static delegate*<ulong, float, void> Timeline_SetSpeed;
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

    internal static ulong CreateEntity(string name, Vector3 position)
    {
        using NativeString nativeName = new(name);
        return Scene_CreateEntity(nativeName, position.X, position.Y, position.Z);
    }

    internal static ulong CreateMesh() => Mesh_Create();

    internal static void DestroyMesh(ulong mesh) => Mesh_Destroy(mesh);

    internal static bool SetMeshData(ulong mesh, ReadOnlySpan<Vector3> positions, ReadOnlySpan<uint> indices, ReadOnlySpan<Vector3> normals,
                                     ReadOnlySpan<Vector2> uvs)
    {
        fixed (Vector3* positionData = positions)
        fixed (uint* indexData = indices)
        fixed (Vector3* normalData = normals)
        fixed (Vector2* uvData = uvs)
        {
            return Mesh_SetData(mesh, positionData, positions.Length, indexData, indices.Length, normalData, normals.Length, uvData, uvs.Length) != 0;
        }
    }

    internal static void ClearMesh(ulong mesh) => Mesh_Clear(mesh);

    internal static void SetMeshColor(ulong mesh, float r, float g, float b, float a, float metallic, float roughness, Vector3 emissive) =>
        Mesh_SetColor(mesh, r, g, b, a, metallic, roughness, emissive.X, emissive.Y, emissive.Z);

    internal static bool SetMeshMaterial(ulong mesh, string materialPath)
    {
        using NativeString nativePath = new(materialPath);
        return Mesh_SetMaterial(mesh, nativePath) != 0;
    }

    internal static void SetEntityMesh(ulong entityID, ulong mesh) => Entity_SetMesh(entityID, mesh);

    internal static void SetEntityMeshCollider(ulong entityID, ulong mesh, float friction, float restitution) =>
        Entity_SetMeshCollider(entityID, mesh, friction, restitution);

    internal static bool Raycast(Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit)
    {
        RaycastHit result = default;
        bool found = Physics_Raycast(origin.X, origin.Y, origin.Z, direction.X, direction.Y, direction.Z, maxDistance, &result) != 0;
        hit = result;
        return found;
    }

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

    internal static bool PlayAnimatorMontage(ulong entityID, string clipPath, string slot, float blendIn, float blendOut, float rate)
    {
        using NativeString nativePath = new(clipPath);
        using NativeString nativeSlot = new(slot);
        return Animator_PlayMontage(entityID, nativePath, nativeSlot, blendIn, blendOut, rate) != 0;
    }

    internal static void StopAnimatorMontage(ulong entityID, string slot, float blendOut)
    {
        using NativeString nativeSlot = new(slot);
        Animator_StopMontage(entityID, nativeSlot, blendOut);
    }

    internal static bool IsAnimatorMontagePlaying(ulong entityID, string slot)
    {
        using NativeString nativeSlot = new(slot);
        return Animator_IsMontagePlaying(entityID, nativeSlot) != 0;
    }

    internal static void PlayTimeline(ulong entityID, bool fromStart) => Timeline_Play(entityID, fromStart ? 1u : 0u);

    internal static void StopTimeline(ulong entityID) => Timeline_Stop(entityID);

    internal static bool IsTimelinePlaying(ulong entityID) => Timeline_IsPlaying(entityID) != 0;

    internal static float GetTimelineTime(ulong entityID) => Timeline_GetTime(entityID);

    internal static void SetTimelineTime(ulong entityID, float time) => Timeline_SetTime(entityID, time);

    internal static void SetTimelineSpeed(ulong entityID, float speed) => Timeline_SetSpeed(entityID, speed);

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
