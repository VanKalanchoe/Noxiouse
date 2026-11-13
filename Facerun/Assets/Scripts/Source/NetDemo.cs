using Nox;

namespace Facerun;

// The multiplayer demo (docs/Networking_Plan_2026-11.md, NET2 / NET3; scene Scenes/NetDemo.nox): a server that spawns boxes and a
// player for every client, players that walk with WASD and jump with Space (predicted on their own machine). Run NoxRuntime --server --scene=Scenes/NetDemo.nox and one or more
// NoxRuntime --windowed --connect=localhost:7777 --scene=Scenes/NetDemo.nox (or --listen to play on the server too).

/// <summary>
/// The game's rules (Unreal's GameMode): only the authority acts -- it spawns a player for each client that joins (and for the one
/// playing on a listen server or standalone), removes it when the client leaves, and drops a box every few seconds.
/// </summary>
public sealed class NetGameMode : EntityBehaviour
{
    [Expose(Tooltip = "The player prefab, under the asset directory")] public string PlayerPrefab = "Prefabs/NetPlayer.nprefab";
    [Expose(Tooltip = "The box prefab, under the asset directory")] public string BoxPrefab = "Prefabs/NetBox.nprefab";
    [Expose(Tooltip = "Seconds between two boxes", Min = 0.1f, Max = 30.0f)] public float BoxInterval = 1.5f;
    [Expose(Tooltip = "Boxes at once; the oldest goes when another falls", Min = 1, Max = 500)] public int MaxBoxes = 30;

    readonly Dictionary<uint, Entity> m_Players = new();
    readonly Queue<Entity> m_Boxes = new();
    readonly Random m_Random = new();
    float m_BoxTimer;

    protected override void OnCreate()
    {
        if (!HasAuthority)
            return; // a client's copy of the scene: the server runs the game
        Network.ClientJoined += OnClientJoined;
        Network.ClientLeft += OnClientLeft;
        if (!Network.IsDedicatedServer)
            SpawnPlayer(0); // the one playing here (a listen server or standalone)
        Log.Info($"[NetDemo] game mode on the {(Network.IsServer ? (Network.IsDedicatedServer ? "dedicated server" : "listen server") : "standalone game")}");
    }

    protected override void OnDestroy()
    {
        Network.ClientJoined -= OnClientJoined;
        Network.ClientLeft -= OnClientLeft;
    }

    void OnClientJoined(uint client) => SpawnPlayer(client);

    void OnClientLeft(uint client)
    {
        if (m_Players.Remove(client, out Entity player))
            Scene.Destroy(player);
        Log.Info($"[NetDemo] client {client} left: its player is removed");
    }

    void SpawnPlayer(uint owner)
    {
        Vector3d position = new(-600.0 + 200.0 * (owner % 7), 200.0, 400.0);
        Entity player = Network.Spawn(PlayerPrefab, position, owner);
        if (!player.IsValid)
        {
            Log.Info($"[NetDemo] could not spawn '{PlayerPrefab}' (is the prefab under Facerun/Assets?)");
            return;
        }
        m_Players[owner] = player;
        Log.Info($"[NetDemo] spawned the player of {(owner == 0 ? "the server" : $"client {owner}")}");
    }

    protected override void OnUpdate(float deltaTime)
    {
        if (!HasAuthority)
            return;
        m_BoxTimer += deltaTime;
        if (m_BoxTimer < BoxInterval)
            return;
        m_BoxTimer = 0.0f;
        Vector3d position = new(m_Random.NextDouble() * 1600.0 - 800.0, 900.0, m_Random.NextDouble() * 1600.0 - 1400.0);
        Entity box = Network.Spawn(BoxPrefab, position);
        if (!box.IsValid)
            return;
        m_Boxes.Enqueue(box);
        while (m_Boxes.Count > MaxBoxes)
            Scene.Destroy(m_Boxes.Dequeue());
    }
}

/// <summary>A falling box: the server picks its color; physics runs on the server, the clients show where it is.</summary>
public sealed class NetBox : EntityBehaviour
{
    [Replicated(OnRep = nameof(OnColorChanged))] public Vector3 Color = new(0.8f, 0.8f, 0.8f);
    [Expose(Tooltip = "Edge length in world units")] public float Size = 100.0f;

    Mesh? m_Mesh;

    protected override void OnCreate()
    {
        if (HasAuthority)
        {
            var random = new Random();
            Color = new Vector3(0.3f + 0.7f * random.NextSingle(), 0.3f + 0.7f * random.NextSingle(), 0.3f + 0.7f * random.NextSingle());
        }
        m_Mesh = NetDemoMesh.Box(new Vector3(Size, Size, Size), Color);
        Self.SetMesh(m_Mesh);
    }

    // On a client, when the server's color arrives (right after the box appears).
    void OnColorChanged() => m_Mesh?.SetColor(Color.X, Color.Y, Color.Z, new Vector3(0.0f, 0.0f, 0.0f));

    protected override void OnDestroy()
    {
        m_Mesh?.Dispose();
        m_Mesh = null;
    }
}

/// <summary>
/// A player: the machine that owns it moves its character with WASD / Space at once, exactly as in a single-player game -- the engine
/// predicts it, sends the inputs to the server, which moves the server's character with them, and corrects the prediction when the two
/// disagree (NET3). Everyone else sees it drawn smoothly between snapshots. The camera follows the player this machine owns. Jumps are
/// counted on the server (a replicated field) and announced to everyone (a MulticastRpc).
/// </summary>
public sealed class NetPlayer : EntityBehaviour
{
    [Replicated(OnRep = nameof(OnColorChanged))] public Vector3 Color = new(1.0f, 1.0f, 1.0f);
    [Replicated] public int Jumps;
    [Expose(Unit = "m/s", Tooltip = "Walking speed")] public float Speed = 500.0f;
    [Expose(Unit = "m/s", Tooltip = "Jump speed")] public float JumpSpeed = 600.0f;

    static readonly Vector3[] s_Palette =
    {
        new(1.0f, 0.85f, 0.2f), new(0.2f, 0.6f, 1.0f), new(1.0f, 0.3f, 0.3f), new(0.3f, 0.9f, 0.4f),
        new(0.8f, 0.4f, 1.0f), new(1.0f, 0.55f, 0.1f), new(0.2f, 0.9f, 0.9f), new(0.95f, 0.95f, 0.95f)
    };

    Mesh? m_Mesh;
    bool m_JumpHeld;
    Entity m_Camera;

    protected override void OnCreate()
    {
        if (HasAuthority)
            Color = s_Palette[Owner % (uint)s_Palette.Length];
        m_Mesh = NetDemoMesh.Box(new Vector3(68.0f, 176.0f, 68.0f), Color);
        Self.SetMesh(m_Mesh);
    }

    void OnColorChanged() => m_Mesh?.SetColor(Color.X, Color.Y, Color.Z, new Vector3(0.0f, 0.0f, 0.0f));

    protected override void OnUpdate(float deltaTime)
    {
        if (!IsOwner)
            return; // someone else's player: the server moves it, the engine draws it

        // Moved here and now, as in a single-player game (the engine sends the same inputs to the server).
        Vector3 move = new(0.0f, 0.0f, 0.0f);
        if (Input.IsKeyDown(KeyCode.W)) move.Z -= 1.0f;
        if (Input.IsKeyDown(KeyCode.S)) move.Z += 1.0f;
        if (Input.IsKeyDown(KeyCode.A)) move.X -= 1.0f;
        if (Input.IsKeyDown(KeyCode.D)) move.X += 1.0f;
        float length = MathF.Sqrt(move.X * move.X + move.Z * move.Z);
        if (length > 1.0f)
            move = new Vector3(move.X / length, 0.0f, move.Z / length);
        CharacterControllerComponent character = GetComponent<CharacterControllerComponent>();
        character.SetMoveVelocity(new Vector3(move.X * Speed, 0.0f, move.Z * Speed));

        // A jump per press, not per frame held.
        bool jump = Input.IsKeyDown(KeyCode.Space);
        if (jump && !m_JumpHeld && character.IsGrounded)
        {
            character.Jump(JumpSpeed);
            CallServerRpc(nameof(ServerJumped));
        }
        m_JumpHeld = jump;

        // The camera behind this machine's own player (found once: the camera is in the scene on every side).
        if (!m_Camera.IsValid)
            m_Camera = Entity.FindByName("Camera");
        if (m_Camera.IsValid)
        {
            TransformComponent camera = m_Camera.GetComponent<TransformComponent>();
            Vector3d position = GetComponent<TransformComponent>().WorldPosition;
            camera.WorldPosition = new Vector3d(position.X, position.Y + 350.0, position.Z + 700.0);
            camera.LocalRotation = new Vector3(-0.4f, 0.0f, 0.0f);
        }
    }

    // On the server: the owner says it jumped (the jump itself came with its inputs): counted and announced.
    [ServerRpc]
    void ServerJumped()
    {
        Jumps++;
        CallMulticastRpc(nameof(Jumped), Jumps);
    }

    // Everyone: the server and every client.
    [MulticastRpc]
    void Jumped(int count) => Log.Info($"[NetDemo] the player of {(Owner == 0 ? "the server" : $"client {Owner}")} jumped ({count} jumps)");

    protected override void OnDestroy()
    {
        m_Mesh?.Dispose();
        m_Mesh = null;
    }
}

/// <summary>A platform the server turns (a replicated level entity: it is in the scene on every side; the server's rotation wins).</summary>
public sealed class NetSpinner : EntityBehaviour
{
    [Expose(Tooltip = "Edge lengths in world units")] public Vector3 Size = new(600.0f, 40.0f, 600.0f);
    [Expose(Tooltip = "Radians per second")] public float SpinSpeed = 0.6f;

    Mesh? m_Mesh;

    protected override void OnCreate()
    {
        m_Mesh = NetDemoMesh.Box(Size, new Vector3(0.9f, 0.45f, 0.1f));
        Self.SetMesh(m_Mesh);
    }

    protected override void OnUpdate(float deltaTime)
    {
        if (!HasAuthority)
            return;
        TransformComponent transform = GetComponent<TransformComponent>();
        Vector3 rotation = transform.LocalRotation;
        rotation.Y += SpinSpeed * deltaTime;
        transform.LocalRotation = rotation;
    }

    protected override void OnDestroy()
    {
        m_Mesh?.Dispose();
        m_Mesh = null;
    }
}

static class NetDemoMesh
{
    // A colored box built at run time (no mesh asset): six faces with their own vertices, counter-clockwise seen from outside.
    internal static Mesh Box(Vector3 size, Vector3 color)
    {
        Vector3 half = new(size.X * 0.5f, size.Y * 0.5f, size.Z * 0.5f);
        MeshBuilder builder = new();
        AddFace(builder, new Vector3(1, 0, 0), new Vector3(0, 0, -1), new Vector3(0, 1, 0), half);
        AddFace(builder, new Vector3(-1, 0, 0), new Vector3(0, 0, 1), new Vector3(0, 1, 0), half);
        AddFace(builder, new Vector3(0, 1, 0), new Vector3(1, 0, 0), new Vector3(0, 0, -1), half);
        AddFace(builder, new Vector3(0, -1, 0), new Vector3(1, 0, 0), new Vector3(0, 0, 1), half);
        AddFace(builder, new Vector3(0, 0, 1), new Vector3(1, 0, 0), new Vector3(0, 1, 0), half);
        AddFace(builder, new Vector3(0, 0, -1), new Vector3(-1, 0, 0), new Vector3(0, 1, 0), half);
        Mesh mesh = new();
        mesh.SetData(builder);
        mesh.SetColor(color.X, color.Y, color.Z, new Vector3(0.0f, 0.0f, 0.0f));
        return mesh;
    }

    static void AddFace(MeshBuilder builder, Vector3 normal, Vector3 right, Vector3 up, Vector3 half)
    {
        Vector3 center = Scale(normal, half);
        Vector3 r = Scale(right, half);
        Vector3 u = Scale(up, half);
        uint first = builder.AddVertex(center - r - u, normal, new Vector2(0, 1));
        builder.AddVertex(center + r - u, normal, new Vector2(1, 1));
        builder.AddVertex(center + r + u, normal, new Vector2(1, 0));
        builder.AddVertex(center - r + u, normal, new Vector2(0, 0));
        builder.Indices.AddRange(new[] { first, first + 1, first + 2, first, first + 2, first + 3 });
    }

    static Vector3 Scale(Vector3 a, Vector3 b) => new(a.X * b.X, a.Y * b.Y, a.Z * b.Z);
}
