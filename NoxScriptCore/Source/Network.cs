using System.Collections.Concurrent;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace Nox;

/// <summary>
/// A field the server sends to its clients (Unreal's UPROPERTY(Replicated)): on an entity with a Network Identity, the server's value
/// reaches every client in the next snapshots (only when it changed). Supported types: bool, byte, int, uint, long, ulong, float,
/// double, string, Vector2, Vector3, Vector3d, Entity. A client's own changes to it are overwritten by the server's.
/// <para>OnRep: the name of a method without parameters called on the clients after the field changed (Unreal's ReplicatedUsing).</para>
/// <example><code>[Replicated(OnRep = nameof(OnHealthChanged))] public int Health = 100;</code></example>
/// </summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class ReplicatedAttribute : Attribute
{
    public string OnRep = "";
}

/// <summary>
/// A method a client calls on the server (Unreal's Server RPC): <c>CallServerRpc(nameof(Fire), direction)</c> from the client that
/// owns the entity runs <c>Fire(direction)</c> on the server's copy. Only the owning client may call it (the server ignores anyone
/// else); the server checks the arguments -- never trust a client. On the server or standalone it simply runs.
/// Parameters: the types <see cref="ReplicatedAttribute"/> supports. Reliable (default): always arrives, in order; false: may be lost
/// (frequent, replaceable calls: input).
/// </summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class ServerRpcAttribute : Attribute
{
    public bool Reliable = true;
}

/// <summary>A method the server calls on the client that owns the entity (Unreal's Client RPC): <c>CallClientRpc(nameof(ShowHit))</c>.</summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class ClientRpcAttribute : Attribute
{
    public bool Reliable = true;
}

/// <summary>A method the server runs on itself and on every client (Unreal's NetMulticast): effects, sounds.</summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class MulticastRpcAttribute : Attribute
{
    public bool Reliable = true;
}

/// <summary>
/// How often the server reads a replicated entity (Unreal's ENetDormancy; the Network Identity's Dormancy). A dormant entity costs the
/// server nothing until it changes: flush it after a change (<see cref="Network.FlushDormancy"/>).
/// </summary>
public enum NetDormancy
{
    /// <summary>Read every snapshot (or at its update rate).</summary>
    Awake = 0,
    /// <summary>Not read again until flushed (a door: flush when it opens or closes).</summary>
    DormantAll = 1,
    /// <summary>Its level state until flushed (a chest in the level until it is opened).</summary>
    Initial = 2
}

/// <summary>This game's part in a multiplayer session (Unreal's ENetMode).</summary>
public enum NetMode
{
    /// <summary>No network: the game runs alone.</summary>
    Standalone = 0,
    /// <summary>A dedicated or listen server: its entities are the authority.</summary>
    Server = 1,
    /// <summary>Joined to a server: replicated entities are the server's, shown here.</summary>
    Client = 2
}

/// <summary>
/// Multiplayer (docs/Networking_Plan_2026-11.md): the role of this game, spawning replicated entities, who owns what, and the server's
/// player events. Start NoxRuntime with --server (dedicated), --listen (plays and accepts clients) or --connect=address (joins).
/// </summary>
public static class Network
{
    // What the engine returns for the owner of an entity that is not replicated (it belongs to this machine alone).
    internal const uint NotReplicated = uint.MaxValue;

    public static NetMode Mode => (NetMode)InternalCalls.NetworkGetMode();
    public static bool IsServer => Mode == NetMode.Server;
    public static bool IsClient => Mode == NetMode.Client;
    public static bool IsStandalone => Mode == NetMode.Standalone;
    /// <summary>A dedicated server (NoxRuntime --server): no window, nobody plays on it -- spawn players only for the clients that join.</summary>
    public static bool IsDedicatedServer => InternalCalls.NetworkIsDedicatedServer();
    /// <summary>The game decides here: the server, or a standalone game (Unreal's HasAuthority). Spawn and score on it, not on clients.</summary>
    public static bool HasAuthority => Mode != NetMode.Client;
    /// <summary>This client's id (from 1); 0 on a server or standalone.</summary>
    public static uint LocalClientId => InternalCalls.NetworkGetLocalClientId();

    /// <summary>
    /// The server places a prefab that every client gets too (a Network Identity is added to its root if the prefab has none), owned by
    /// a client (its id; 0: the server). Standalone it is a plain <see cref="Scene.Instantiate"/>; on a client it does nothing.
    /// </summary>
    public static Entity Spawn(string prefabPath, Vector3d position, uint owner = 0) =>
        new(InternalCalls.NetworkSpawn(prefabPath, position, owner));

    /// <summary>The client that owns a replicated entity (0: the server or not replicated).</summary>
    public static uint GetOwner(Entity entity)
    {
        uint owner = InternalCalls.NetworkGetOwner(entity.ID);
        return owner == NotReplicated ? 0 : owner;
    }

    /// <summary>The server hands a replicated entity to a client (0: back to the server). False on a client or for an entity that is not replicated.</summary>
    public static bool SetOwner(Entity entity, uint owner) => InternalCalls.NetworkSetOwner(entity.ID, owner);

    /// <summary>A replicated entity's dormancy (null: not replicated).</summary>
    public static NetDormancy? GetDormancy(Entity entity)
    {
        int dormancy = InternalCalls.NetworkGetDormancy(entity.ID);
        return dormancy < 0 ? null : (NetDormancy)dormancy;
    }

    /// <summary>On the server: makes an entity dormant or awake again; its state as it is now goes to the clients either way.</summary>
    public static bool SetDormancy(Entity entity, NetDormancy dormancy) => InternalCalls.NetworkSetDormancy(entity.ID, (int)dormancy);

    /// <summary>On the server: a dormant entity changed -- its state is read once more at the next snapshot (Unreal's FlushNetDormancy).</summary>
    public static bool FlushDormancy(Entity entity) => InternalCalls.NetworkFlushDormancy(entity.ID);

    /// <summary>On the server: a client joined (its id). Subscribe in OnCreate, unsubscribe in OnDestroy.</summary>
    public static event Action<uint>? ClientJoined;
    /// <summary>On the server: a client left or was dropped (its id).</summary>
    public static event Action<uint>? ClientLeft;

    // Called by the engine (Net::NetReplication): 0 joined, 1 left.
    internal static void RaiseClientEvent(int kind, uint client)
    {
        try
        {
            (kind == 0 ? ClientJoined : ClientLeft)?.Invoke(client);
        }
        catch (Exception exception)
        {
            Log.Info($"[Network] a {(kind == 0 ? "ClientJoined" : "ClientLeft")} handler failed: {exception}");
        }
    }
}

// The replicated fields and RPCs of a script class, found once by reflection: in name order, the same on the server and every client
// (they run the same assembly -- the join checks it).
internal sealed class NetTypeInfo
{
    internal readonly FieldInfo[] Fields;
    internal readonly MethodInfo?[] OnRep;
    internal readonly MethodInfo[] Rpcs;
    internal readonly int[] RpcKinds;     // 0 server, 1 client, 2 multicast
    internal readonly bool[] RpcReliable;
    internal readonly Dictionary<string, int> RpcIndex = new(StringComparer.Ordinal);

    static readonly ConcurrentDictionary<Type, NetTypeInfo> s_Cache = new();
    internal static NetTypeInfo Get(Type type) => s_Cache.GetOrAdd(type, t => new NetTypeInfo(t));

    NetTypeInfo(Type type)
    {
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;
        var fields = new List<FieldInfo>();
        var methods = new List<(MethodInfo Method, int Kind, bool Reliable)>();
        for (Type? t = type; t != null && t != typeof(EntityBehaviour); t = t.BaseType)
        {
            foreach (FieldInfo field in t.GetFields(flags))
            {
                if (field.GetCustomAttribute<ReplicatedAttribute>() == null)
                    continue;
                if (!NetCodec.Supports(field.FieldType))
                {
                    Log.Info($"[Network] {type.FullName}.{field.Name}: [Replicated] does not support {field.FieldType.Name} (skipped)");
                    continue;
                }
                fields.Add(field);
            }
            foreach (MethodInfo method in t.GetMethods(flags))
            {
                int kind = -1;
                bool reliable = true;
                if (method.GetCustomAttribute<ServerRpcAttribute>() is { } server) { kind = 0; reliable = server.Reliable; }
                else if (method.GetCustomAttribute<ClientRpcAttribute>() is { } client) { kind = 1; reliable = client.Reliable; }
                else if (method.GetCustomAttribute<MulticastRpcAttribute>() is { } multicast) { kind = 2; reliable = multicast.Reliable; }
                if (kind < 0)
                    continue;
                if (method.GetParameters().Any(p => !NetCodec.Supports(p.ParameterType)) || method.ReturnType != typeof(void))
                {
                    Log.Info($"[Network] {type.FullName}.{method.Name}: an RPC returns void and takes the types [Replicated] supports (skipped)");
                    continue;
                }
                methods.Add((method, kind, reliable));
            }
        }
        fields.Sort((a, b) => string.CompareOrdinal(a.DeclaringType!.FullName + "." + a.Name, b.DeclaringType!.FullName + "." + b.Name));
        methods.Sort((a, b) => string.CompareOrdinal(a.Method.Name, b.Method.Name));

        Fields = fields.ToArray();
        OnRep = new MethodInfo?[Fields.Length];
        for (int index = 0; index < Fields.Length; ++index)
        {
            string name = Fields[index].GetCustomAttribute<ReplicatedAttribute>()!.OnRep;
            if (string.IsNullOrEmpty(name))
                continue;
            OnRep[index] = type.GetMethod(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, Type.EmptyTypes);
            if (OnRep[index] == null)
                Log.Info($"[Network] {type.FullName}.{Fields[index].Name}: OnRep method '{name}()' not found");
        }

        var rpcs = new List<MethodInfo>();
        var kinds = new List<int>();
        var reliables = new List<bool>();
        foreach (var (method, kind, reliable) in methods)
        {
            if (RpcIndex.ContainsKey(method.Name))
            {
                Log.Info($"[Network] {type.FullName}.{method.Name}: RPCs cannot be overloaded (the second one is skipped)");
                continue;
            }
            RpcIndex[method.Name] = rpcs.Count;
            rpcs.Add(method);
            kinds.Add(kind);
            reliables.Add(reliable);
        }
        Rpcs = rpcs.ToArray();
        RpcKinds = kinds.ToArray();
        RpcReliable = reliables.ToArray();
    }
}

// How replicated values and RPC arguments are written (little-endian; strings as UTF-8 with a 7-bit-encoded length). Only scripts
// read them back -- the engine moves the bytes.
internal static class NetCodec
{
    internal static bool Supports(Type type) =>
        type == typeof(bool) || type == typeof(byte) || type == typeof(int) || type == typeof(uint) || type == typeof(long) ||
        type == typeof(ulong) || type == typeof(float) || type == typeof(double) || type == typeof(string) || type == typeof(Vector2) ||
        type == typeof(Vector3) || type == typeof(Vector3d) || type == typeof(Entity);

    internal static void Write(BinaryWriter writer, Type type, object? value)
    {
        if (type == typeof(bool)) writer.Write(value is true);
        else if (type == typeof(byte)) writer.Write(Convert.ToByte(value ?? 0));
        else if (type == typeof(int)) writer.Write(Convert.ToInt32(value ?? 0));
        else if (type == typeof(uint)) writer.Write(Convert.ToUInt32(value ?? 0u));
        else if (type == typeof(long)) writer.Write(Convert.ToInt64(value ?? 0L));
        else if (type == typeof(ulong)) writer.Write(Convert.ToUInt64(value ?? 0UL));
        else if (type == typeof(float)) writer.Write(Convert.ToSingle(value ?? 0.0f));
        else if (type == typeof(double)) writer.Write(Convert.ToDouble(value ?? 0.0));
        else if (type == typeof(string)) writer.Write((string?)value ?? string.Empty);
        else if (type == typeof(Vector2)) { var v = value is Vector2 v2 ? v2 : default; writer.Write(v.X); writer.Write(v.Y); }
        else if (type == typeof(Vector3)) { var v = value is Vector3 v3 ? v3 : default; writer.Write(v.X); writer.Write(v.Y); writer.Write(v.Z); }
        else if (type == typeof(Vector3d)) { var v = value is Vector3d v3d ? v3d : default; writer.Write(v.X); writer.Write(v.Y); writer.Write(v.Z); }
        else if (type == typeof(Entity)) writer.Write(value is Entity entity ? entity.ID : 0UL);
        else throw new NotSupportedException(type.Name);
    }

    internal static object Read(BinaryReader reader, Type type)
    {
        if (type == typeof(bool)) return reader.ReadBoolean();
        if (type == typeof(byte)) return reader.ReadByte();
        if (type == typeof(int)) return reader.ReadInt32();
        if (type == typeof(uint)) return reader.ReadUInt32();
        if (type == typeof(long)) return reader.ReadInt64();
        if (type == typeof(ulong)) return reader.ReadUInt64();
        if (type == typeof(float)) return reader.ReadSingle();
        if (type == typeof(double)) return reader.ReadDouble();
        if (type == typeof(string)) return reader.ReadString();
        if (type == typeof(Vector2)) return new Vector2(reader.ReadSingle(), reader.ReadSingle());
        if (type == typeof(Vector3)) return new Vector3(reader.ReadSingle(), reader.ReadSingle(), reader.ReadSingle());
        if (type == typeof(Vector3d)) return new Vector3d(reader.ReadDouble(), reader.ReadDouble(), reader.ReadDouble());
        if (type == typeof(Entity)) return new Entity(reader.ReadUInt64());
        throw new NotSupportedException(type.Name);
    }

    // One buffer for the main thread's writes (replication and RPCs run there).
    [ThreadStatic] static MemoryStream? s_Stream;
    [ThreadStatic] static BinaryWriter? s_Writer;

    internal static BinaryWriter BeginWrite()
    {
        s_Stream ??= new MemoryStream(1024);
        s_Writer ??= new BinaryWriter(s_Stream, Encoding.UTF8, leaveOpen: true);
        s_Stream.SetLength(0);
        return s_Writer;
    }

    internal static MemoryStream Written => s_Stream!;
}
