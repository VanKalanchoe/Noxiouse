using System.Reflection;
using System.Runtime.InteropServices;

namespace Nox;

public abstract class EntityBehaviour
{
    // Assigned by the native runtime immediately after construction.
    public ulong EntityID;

    /// <summary>The entity this script is on.</summary>
    public Entity Self => new(EntityID);

    public bool HasComponent<T>() where T : Component, new() =>
        new Entity(EntityID).HasComponent<T>();

    public T GetComponent<T>() where T : Component, new() =>
        new Entity(EntityID).GetComponent<T>();

    public bool TryGetComponent<T>(out T? component) where T : Component, new() =>
        new Entity(EntityID).TryGetComponent(out component);

    public Entity FindChild(string path) => new(InternalCalls.FindChild(EntityID, path));

    // ---------------------------------------------------------------------------------------------------------------- multiplayer

    /// <summary>The game decides here: the server, or a standalone game (<see cref="Network.HasAuthority"/>).</summary>
    public bool HasAuthority => Network.HasAuthority;
    public bool IsServer => Network.IsServer;
    /// <summary>The client that owns this entity (0: the server, or not replicated).</summary>
    public uint Owner => Network.GetOwner(Self);
    /// <summary>
    /// This machine controls the entity: on a client, the entities it owns (its player); on the server or standalone, the ones the
    /// server owns. An entity without a Network Identity belongs to every machine on its own (always true).
    /// </summary>
    public bool IsOwner
    {
        get
        {
            uint owner = InternalCalls.NetworkGetOwner(EntityID);
            if (owner == Network.NotReplicated)
                return true;
            return Network.IsClient ? owner != 0 && owner == Network.LocalClientId : owner == 0;
        }
    }

    /// <summary>Runs a [ServerRpc] method on the server (from the client that owns the entity; on the server it just runs).</summary>
    protected void CallServerRpc(string method, params object?[] arguments) => CallRpc(0, method, arguments);
    /// <summary>Runs a [ClientRpc] method on the client that owns the entity (the server calls it).</summary>
    protected void CallClientRpc(string method, params object?[] arguments) => CallRpc(1, method, arguments);
    /// <summary>Runs a [MulticastRpc] method here and on every client (the server calls it).</summary>
    protected void CallMulticastRpc(string method, params object?[] arguments) => CallRpc(2, method, arguments);

    void CallRpc(int kind, string name, object?[] arguments)
    {
        NetTypeInfo info = NetTypeInfo.Get(GetType());
        string[] kindNames = { "ServerRpc", "ClientRpc", "MulticastRpc" };
        if (!info.RpcIndex.TryGetValue(name, out int index))
        {
            Log.Info($"[Network] {GetType().Name}.Call{kindNames[kind]}: no [{kindNames[kind]}] method '{name}'");
            return;
        }
        if (info.RpcKinds[index] != kind)
        {
            Log.Info($"[Network] {GetType().Name}.{name} is a [{kindNames[info.RpcKinds[index]]}]: call it with Call{kindNames[info.RpcKinds[index]]}");
            return;
        }
        MethodInfo method = info.Rpcs[index];
        ParameterInfo[] parameters = method.GetParameters();
        if (arguments.Length != parameters.Length)
        {
            Log.Info($"[Network] {GetType().Name}.{name} takes {parameters.Length} argument(s), not {arguments.Length}");
            return;
        }

        NetMode mode = Network.Mode;
        bool runHere;
        bool send;
        switch (kind)
        {
            case 0: // server: runs there; from a client it is sent
                runHere = mode != NetMode.Client;
                send = mode == NetMode.Client;
                break;
            case 1: // the owning client: the server sends it, or runs it when the server owns the entity
                if (mode == NetMode.Client)
                {
                    Log.Info($"[Network] {GetType().Name}.{name}: only the server calls a [ClientRpc]");
                    return;
                }
                runHere = mode == NetMode.Standalone || Owner == 0;
                send = mode == NetMode.Server && Owner != 0;
                break;
            default: // everyone: here and, on a server, every client
                if (mode == NetMode.Client)
                {
                    Log.Info($"[Network] {GetType().Name}.{name}: only the server calls a [MulticastRpc]");
                    return;
                }
                runHere = true;
                send = mode == NetMode.Server;
                break;
        }

        if (send)
        {
            BinaryWriter writer = NetCodec.BeginWrite();
            for (int i = 0; i < parameters.Length; ++i)
                NetCodec.Write(writer, parameters[i].ParameterType, arguments[i]);
            writer.Flush();
            MemoryStream written = NetCodec.Written;
            InternalCalls.NetworkSendRpc(EntityID, GetType().FullName!, kind, index, info.RpcReliable[index],
                                         new ReadOnlySpan<byte>(written.GetBuffer(), 0, (int)written.Length));
        }
        if (runHere)
            Invoke(method, ConvertArguments(parameters, arguments));
    }

    static object?[] ConvertArguments(ParameterInfo[] parameters, object?[] arguments)
    {
        var converted = new object?[arguments.Length];
        for (int i = 0; i < arguments.Length; ++i)
        {
            Type type = parameters[i].ParameterType;
            object? value = arguments[i];
            converted[i] = value == null || type.IsInstanceOfType(value) || !(value is IConvertible) ? value : Convert.ChangeType(value, type);
        }
        return converted;
    }

    void Invoke(MethodInfo method, object?[] arguments)
    {
        try
        {
            method.Invoke(this, arguments);
        }
        catch (TargetInvocationException exception)
        {
            Log.Info($"[Network] {GetType().Name}.{method.Name} threw: {exception.InnerException}");
        }
        catch (Exception exception)
        {
            Log.Info($"[Network] {GetType().Name}.{method.Name} could not be called: {exception.Message}");
        }
    }

    // The engine's side (Net::NetReplication through the script backend).
    internal int NetReplicatedFieldCount() => NetTypeInfo.Get(GetType()).Fields.Length;

    // Each [Replicated] field as u16 length + value; returns the bytes written, or the size needed (negated) when the buffer is small.
    internal int NetWriteReplicated(IntPtr buffer, int capacity)
    {
        NetTypeInfo info = NetTypeInfo.Get(GetType());
        if (info.Fields.Length == 0)
            return 0;
        BinaryWriter writer = NetCodec.BeginWrite();
        MemoryStream stream = NetCodec.Written;
        foreach (FieldInfo field in info.Fields)
        {
            long start = stream.Position;
            writer.Write((ushort)0);
            NetCodec.Write(writer, field.FieldType, field.GetValue(this));
            writer.Flush();
            long length = stream.Position - start - 2;
            if (length > ushort.MaxValue)
            {
                Log.Info($"[Network] {GetType().Name}.{field.Name} is larger than 64 KB: not replicated");
                stream.Position = start + 2;
                stream.SetLength(start + 2);
                length = 0;
            }
            long end = stream.Position;
            stream.Position = start;
            writer.Write((ushort)length);
            writer.Flush();
            stream.Position = end;
        }
        int total = (int)stream.Length;
        if (total > capacity)
            return -total;
        Marshal.Copy(stream.GetBuffer(), 0, buffer, total);
        return total;
    }

    // Records of changed fields (u16 index, u16 length, value): set them, then call their OnRep methods.
    internal void NetReadReplicated(IntPtr data, int length)
    {
        NetTypeInfo info = NetTypeInfo.Get(GetType());
        byte[] bytes = new byte[length];
        Marshal.Copy(data, bytes, 0, length);
        using var stream = new MemoryStream(bytes, writable: false);
        using var reader = new BinaryReader(stream);
        List<MethodInfo>? notify = null;
        while (stream.Position + 4 <= length)
        {
            int field = reader.ReadUInt16();
            int size = reader.ReadUInt16();
            long end = stream.Position + size;
            if (field < info.Fields.Length && end <= length)
            {
                try
                {
                    info.Fields[field].SetValue(this, NetCodec.Read(reader, info.Fields[field].FieldType));
                    if (info.OnRep[field] is { } onRep && (notify ??= new()).Contains(onRep) == false)
                        notify.Add(onRep);
                }
                catch (Exception exception)
                {
                    Log.Info($"[Network] {GetType().Name}.{info.Fields[field].Name}: a replicated value could not be read ({exception.Message})");
                }
            }
            stream.Position = Math.Min(end, length);
        }
        if (notify != null)
            foreach (MethodInfo onRep in notify)
                Invoke(onRep, Array.Empty<object?>());
    }

    // An RPC from the network: its method (index among the class's RPCs) must be of the kind it was sent as.
    internal void NetInvokeRpc(int kind, int method, IntPtr data, int length)
    {
        NetTypeInfo info = NetTypeInfo.Get(GetType());
        if (method < 0 || method >= info.Rpcs.Length || info.RpcKinds[method] != kind)
        {
            Log.Info($"[Network] {GetType().Name}: an RPC ({method}, kind {kind}) that this class does not have (ignored)");
            return;
        }
        ParameterInfo[] parameters = info.Rpcs[method].GetParameters();
        var arguments = new object?[parameters.Length];
        try
        {
            byte[] bytes = new byte[length];
            if (length > 0)
                Marshal.Copy(data, bytes, 0, length);
            using var reader = new BinaryReader(new MemoryStream(bytes, writable: false));
            for (int i = 0; i < parameters.Length; ++i)
                arguments[i] = NetCodec.Read(reader, parameters[i].ParameterType);
        }
        catch (Exception exception)
        {
            Log.Info($"[Network] {GetType().Name}.{info.Rpcs[method].Name}: malformed arguments ({exception.Message})");
            return;
        }
        Invoke(info.Rpcs[method], arguments);
    }

    protected virtual void OnCreate() { }
    protected virtual void OnUpdate(float deltaTime) { }
    /// <summary>
    /// At a fixed rate (<see cref="Time.FixedDeltaTime"/>, 60 per second), zero or more times a frame, before OnUpdate: for
    /// forces, velocities and anything that should not depend on the frame rate.
    /// </summary>
    protected virtual void OnFixedUpdate(float fixedDeltaTime) { }
    /// <summary>
    /// After every script's OnUpdate and after physics and animation moved things this frame: for a camera that follows a
    /// character, or anything that reads where things ended up. Transforms set here show this frame.
    /// </summary>
    protected virtual void OnLateUpdate(float deltaTime) { }
    protected virtual void OnDestroy() { }
    protected virtual void OnAfterReload() { }
    /// <summary>
    /// An animation notify this entity's animator crossed this frame (a named moment of a clip, set in the clip's window:
    /// "FootL", "Hit"), from clips, blend spaces and montages.
    /// </summary>
    protected virtual void OnAnimationEvent(string name) { }

    // Which of the optional callbacks the script's class implements (1: OnFixedUpdate, 2: OnLateUpdate): the engine calls only
    // those, once per class.
    internal int GetCallbackMask()
    {
        int mask = 0;
        if (Overrides(nameof(OnFixedUpdate))) mask |= 1;
        if (Overrides(nameof(OnLateUpdate))) mask |= 2;
        return mask;
    }

    bool Overrides(string name)
    {
        MethodInfo? method = GetType().GetMethod(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        return method != null && method.DeclaringType != typeof(EntityBehaviour);
    }
}
