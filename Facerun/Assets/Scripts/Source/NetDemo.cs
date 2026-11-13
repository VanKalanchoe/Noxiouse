using Nox;

namespace Facerun;

// The multiplayer demo (docs/Networking_Plan_2026-11.md, NET2 - NET5; scene Scenes/NetDemo.nox): a server that spawns boxes and a
// player for every client, players that look with the mouse, walk with WASD, jump with Space (predicted on their own machine) and shoot
// with the left button (lag compensated on the server), and north of the start a
// field of boxes with bots wandering in it -- each client gets only what is near its player (relevancy: boxes within 50 m, bots within
// 80 m, players within 150 m). Run NoxRuntime --server --scene=Scenes/NetDemo.nox and one or more
// NoxRuntime --windowed --connect=localhost:7777 --scene=Scenes/NetDemo.nox (or --listen to play on the server too).

/// <summary>
/// The game's rules (Unreal's GameMode): only the authority acts -- it spawns a player for each client that joins (and for the one
/// playing on a listen server or standalone), removes it when the client leaves, and drops a box every few seconds.
/// </summary>
public sealed class NetGameMode : EntityBehaviour
{
    [Expose(Tooltip = "The player prefab, under the asset directory")] public string PlayerPrefab = "Prefabs/NetPlayer.nprefab";
    [Expose(Tooltip = "Players are the animated UAL1 character (Prefabs/NetPlayerAnimated.nprefab; its model and graph must be in the project)")]
    public bool AnimatedPlayers = false;
    [Expose(Tooltip = "The animated player prefab, under the asset directory")] public string AnimatedPlayerPrefab = "Prefabs/NetPlayerAnimated.nprefab";
    [Expose(Tooltip = "The box prefab, under the asset directory")] public string BoxPrefab = "Prefabs/NetBox.nprefab";
    [Expose(Tooltip = "Seconds between two boxes", Min = 0.1f, Max = 30.0f)] public float BoxInterval = 1.5f;
    [Expose(Tooltip = "Boxes at once; the oldest goes when another falls", Min = 1, Max = 500)] public int MaxBoxes = 30;
    [Expose(Tooltip = "Boxes in the field north of the start (a grid; each client gets the ones within 50 m of its player)", Min = 0, Max = 4000)]
    public int FieldBoxes = 400;
    [Expose(Tooltip = "World units between two boxes of the field")] public float FieldSpacing = 800.0f;
    [Expose(Tooltip = "How far north of the start the field's middle is (world units)")] public float FieldDistance = 12000.0f;
    [Expose(Tooltip = "The bot prefab, under the asset directory")] public string BotPrefab = "Prefabs/NetBot.nprefab";
    [Expose(Tooltip = "Server-controlled characters wandering in the field", Min = 0, Max = 256)] public int Bots = 16;

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
        SpawnField();
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
        string prefab = AnimatedPlayers ? AnimatedPlayerPrefab : PlayerPrefab;
        Entity player = Network.Spawn(prefab, position, owner);
        if (!player.IsValid)
        {
            Log.Info($"[NetDemo] could not spawn '{prefab}' (is the prefab under Facerun/Assets?)");
            return;
        }
        m_Players[owner] = player;
        Log.Info($"[NetDemo] spawned the player of {(owner == 0 ? "the server" : $"client {owner}")}");
    }

    // The field: a grid of boxes resting on the ground and bots among them, all on the server; a client is sent the ones near its player.
    void SpawnField()
    {
        int side = (int)MathF.Ceiling(MathF.Sqrt(FieldBoxes));
        double half = (side - 1) * FieldSpacing * 0.5;
        for (int index = 0; index < FieldBoxes; ++index)
        {
            double x = (index % side) * FieldSpacing - half;
            double z = -FieldDistance + (index / side) * FieldSpacing - half;
            Network.Spawn(BoxPrefab, new Vector3d(x, 60.0, z));
        }
        for (int index = 0; index < Bots; ++index)
        {
            Vector3d position = new(m_Random.NextDouble() * 2.0 * half - half, 200.0, -FieldDistance + m_Random.NextDouble() * 2.0 * half - half);
            Network.Spawn(BotPrefab, position);
        }
        Log.Info($"[NetDemo] a field of {FieldBoxes} boxes and {Bots} bots {FieldDistance / 100.0f:0} m north of the start");
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

/// <summary>
/// A box: the server picks its color; physics runs on the server and on every client (each client simulates its copy and follows the
/// server's -- what a player pushes moves at once on that player's screen). A shot knocks it away.
/// </summary>
public sealed class NetBox : EntityBehaviour, INetShootable
{
    [Replicated(OnRep = nameof(OnColorChanged))] public Vector3 Color = new(0.8f, 0.8f, 0.8f);
    [Expose(Tooltip = "Edge length in world units")] public float Size = 100.0f;

    Mesh? m_Mesh;
    NetFlash m_Flash;

    protected override void OnCreate()
    {
        if (HasAuthority)
        {
            var random = new Random();
            Color = new Vector3(0.3f + 0.7f * random.NextSingle(), 0.3f + 0.7f * random.NextSingle(), 0.3f + 0.7f * random.NextSingle());
        }
        m_Mesh = NetDemoMesh.Box(new Vector3(Size, Size, Size), Color);
        Self.SetMesh(m_Mesh);
        NetDemoTargets.Register(Self, this);
    }

    // On a client, when the server's color arrives (right after the box appears).
    void OnColorChanged() => m_Mesh?.SetColor(Color.X, Color.Y, Color.Z, new Vector3(0.0f, 0.0f, 0.0f));

    // On the server: a shot hit it -- pushed along the shot (physics replication carries it to every client) and flashed everywhere.
    public void OnShot(Vector3 direction, uint shooter)
    {
        GetComponent<RigidBody3DComponent>().AddImpulse(new Vector3(direction.X * 700.0f, direction.Y * 700.0f + 300.0f, direction.Z * 700.0f));
        CallMulticastRpc(nameof(Hit));
    }

    [MulticastRpc(Reliable = false)]
    void Hit() => m_Flash.Start(0.25f);

    protected override void OnUpdate(float deltaTime) => m_Flash.Update(deltaTime, m_Mesh, Color);

    protected override void OnDestroy()
    {
        NetDemoTargets.Unregister(Self);
        m_Mesh?.Dispose();
        m_Mesh = null;
    }
}

/// <summary>
/// A player, third person (Unreal's template): the mouse turns the view (click into the window to capture it, Tab to let it go), WASD
/// walks relative to it and the character turns toward where it walks, Space jumps, the left button shoots. With an animator (the
/// NetPlayerAnimated prefab: the UAL1 character and its locomotion graph) it feeds the graph Speed / VerticalSpeed / IsGrounded -- on the
/// machine that owns it (predicted) and on the server, which replicates them to everyone else (NET5 animation replication). The machine that owns it moves it at once (NET3 prediction); a shot is checked on the
/// server against where the targets were on the shooter's screen (NET5 lag compensation) -- the shooter's own flash plays at once, the
/// others get it from the server (a multicast that skips the owner). Jumps are counted on the server and announced to everyone.
/// </summary>
public sealed class NetPlayer : EntityBehaviour, INetShootable
{
    [Replicated(OnRep = nameof(OnColorChanged))] public Vector3 Color = new(1.0f, 1.0f, 1.0f);
    [Replicated] public int Jumps;
    [Expose(Unit = "m/s", Tooltip = "Walking speed")] public float Speed = 500.0f;
    [Expose(Unit = "m/s", Tooltip = "Jump speed")] public float JumpSpeed = 600.0f;
    [Expose(Tooltip = "Radians the view turns per pixel the mouse moves")] public float LookSpeed = 0.0025f;
    [Expose(Tooltip = "How far a shot reaches (world units)")] public float Range = 30000.0f;
    [Expose(Tooltip = "Degrees a second it turns toward where it walks (Unreal's Rotation Rate)")] public float TurnRate = 540.0f;
    [Expose(Tooltip = "Radians: the model's forward axis relative to +Z")] public float ModelYawOffset = 0.0f;

    static readonly Vector3[] s_Palette =
    {
        new(1.0f, 0.85f, 0.2f), new(0.2f, 0.6f, 1.0f), new(1.0f, 0.3f, 0.3f), new(0.3f, 0.9f, 0.4f),
        new(0.8f, 0.4f, 1.0f), new(1.0f, 0.55f, 0.1f), new(0.2f, 0.9f, 0.9f), new(0.95f, 0.95f, 0.95f)
    };

    Mesh? m_Mesh;
    NetFlash m_Flash;
    bool m_JumpHeld, m_FireHeld;
    float m_Yaw, m_Pitch = -0.2f; // the view
    float m_BodyYaw;              // where the character faces
    Entity m_Camera;

    protected override void OnCreate()
    {
        if (HasAuthority)
            Color = s_Palette[Owner % (uint)s_Palette.Length];
        // A model of its own (the animated prefab): kept; else a colored box.
        if (!HasComponent<MeshComponent>())
        {
            m_Mesh = NetDemoMesh.Box(new Vector3(68.0f, 176.0f, 68.0f), Color);
            Self.SetMesh(m_Mesh);
        }
        NetDemoTargets.Register(Self, this);
    }

    // The animation graph's inputs from the character's movement (the server and the owning machine; the others get them replicated).
    void UpdateAnimation(CharacterControllerComponent character)
    {
        if (!TryGetComponent(out AnimatorComponent? animator) || animator == null)
            return;
        Vector3 velocity = character.Velocity;
        animator.SetFloat("Speed", WorldUnits.ToMeters(MathF.Sqrt(velocity.X * velocity.X + velocity.Z * velocity.Z)));
        animator.SetFloat("VerticalSpeed", WorldUnits.ToMeters(velocity.Y));
        animator.SetBool("IsGrounded", character.IsGrounded);
    }

    void OnColorChanged() => m_Mesh?.SetColor(Color.X, Color.Y, Color.Z, new Vector3(0.0f, 0.0f, 0.0f));

    // Where the camera looks (yaw around up, pitch up/down; -Z is ahead at yaw 0).
    Vector3 ViewDirection() =>
        new(-MathF.Cos(m_Pitch) * MathF.Sin(m_Yaw), MathF.Sin(m_Pitch), -MathF.Cos(m_Pitch) * MathF.Cos(m_Yaw));

    protected override void OnUpdate(float deltaTime)
    {
        m_Flash.Update(deltaTime, m_Mesh, Color);
        if (HasAuthority && !IsOwner)
            UpdateAnimation(GetComponent<CharacterControllerComponent>()); // (the server: a client's player, moved by that client's inputs)
        if (!IsOwner)
            return; // someone else's player: the server moves it, the engine draws it

        // The view: captured mouse (a click captures it, Tab lets it go).
        if (!Input.CursorLocked && Input.IsMouseButtonDown(MouseButton.Left))
        {
            Input.CursorLocked = true;
            m_FireHeld = true; // (that click only captures)
        }
        if (Input.IsKeyDown(KeyCode.Tab))
            Input.CursorLocked = false;
        if (Input.CursorLocked)
        {
            Vector2 look = Input.MouseDelta;
            m_Yaw -= look.X * LookSpeed;
            m_Pitch = Math.Clamp(m_Pitch - look.Y * LookSpeed, -1.2f, 0.3f); // (higher: the camera behind would go under the ground)
        }
        // Walking relative to the view, here and now as in a single-player game (the engine sends the same inputs to the server).
        Vector3 forward = new(-MathF.Sin(m_Yaw), 0.0f, -MathF.Cos(m_Yaw));
        Vector3 right = new(MathF.Cos(m_Yaw), 0.0f, -MathF.Sin(m_Yaw));
        float ahead = (Input.IsKeyDown(KeyCode.W) ? 1.0f : 0.0f) - (Input.IsKeyDown(KeyCode.S) ? 1.0f : 0.0f);
        float side = (Input.IsKeyDown(KeyCode.D) ? 1.0f : 0.0f) - (Input.IsKeyDown(KeyCode.A) ? 1.0f : 0.0f);
        Vector3 move = new(forward.X * ahead + right.X * side, 0.0f, forward.Z * ahead + right.Z * side);
        float length = MathF.Sqrt(move.X * move.X + move.Z * move.Z);
        if (length > 1.0f)
            move = new Vector3(move.X / length, 0.0f, move.Z / length);
        CharacterControllerComponent character = GetComponent<CharacterControllerComponent>();
        character.SetMoveVelocity(new Vector3(move.X * Speed, 0.0f, move.Z * Speed));

        // Turned toward where it walks, at TurnRate (Unreal's Orient Rotation To Movement); sent with the inputs -- the server turns it too.
        // From where it faces now: a platform it stands on turns it (the engine adds the ground's yaw).
        m_BodyYaw = GetComponent<TransformComponent>().LocalRotation.Y;
        if (length > 0.01f)
        {
            float target = MathF.Atan2(move.X, move.Z) + ModelYawOffset;
            float delta = MathF.IEEERemainder(target - m_BodyYaw, 2.0f * MathF.PI);
            float step = TurnRate * (MathF.PI / 180.0f) * deltaTime;
            m_BodyYaw += Math.Clamp(delta, -step, step);
        }
        GetComponent<TransformComponent>().LocalRotation = new Vector3(0.0f, m_BodyYaw, 0.0f);
        UpdateAnimation(character);

        // A jump per press, not per frame held.
        bool jump = Input.IsKeyDown(KeyCode.Space);
        if (jump && !m_JumpHeld && character.IsGrounded)
        {
            character.Jump(JumpSpeed);
            CallServerRpc(nameof(ServerJumped));
        }
        m_JumpHeld = jump;

        // A shot per click: the flash here at once, the hit decided by the server with the tick this screen shows the others at.
        bool fire = Input.CursorLocked && Input.IsMouseButtonDown(MouseButton.Left);
        if (fire && !m_FireHeld && m_Camera.IsValid)
        {
            m_Flash.Start(0.08f);
            CallServerRpc(nameof(ServerFire), m_Camera.GetComponent<TransformComponent>().WorldPosition, ViewDirection(), Network.ViewTick);
        }
        m_FireHeld = fire;
    }

    // The camera behind this machine's own player, after physics moved it this frame (OnUpdate runs before: it would trail a frame).
    protected override void OnLateUpdate(float deltaTime)
    {
        if (!IsOwner)
            return;
        if (!m_Camera.IsValid) // (found once: the camera is in the scene on every side)
            m_Camera = Entity.FindByName("Camera");
        if (!m_Camera.IsValid)
            return;
        Vector3 view = ViewDirection();
        Vector3d position = GetComponent<TransformComponent>().WorldPosition;
        TransformComponent camera = m_Camera.GetComponent<TransformComponent>();
        camera.WorldPosition = new Vector3d(position.X - view.X * 600.0, position.Y + 220.0 - view.Y * 600.0, position.Z - view.Z * 600.0);
        camera.LocalRotation = new Vector3(m_Pitch, m_Yaw, 0.0f);
    }

    // On the server: the shot, against where the targets were on the shooter's screen. The origin it claims must be near its player
    // (never trust the client).
    [ServerRpc]
    void ServerFire(Vector3d origin, Vector3 direction, double viewTick)
    {
        Vector3d me = GetComponent<TransformComponent>().WorldPosition;
        double dx = origin.X - me.X, dy = origin.Y - me.Y, dz = origin.Z - me.Z;
        if (dx * dx + dy * dy + dz * dz > 1000.0 * 1000.0)
            origin = new Vector3d(me.X, me.Y + 150.0, me.Z);
        string who = Owner == 0 ? "the server's player" : $"client {Owner}";
        if (Network.RaycastLagCompensated(Owner, viewTick, origin, direction, Range, out RaycastHit hit, out double rewoundMs))
        {
            bool target = NetDemoTargets.Shoot(hit.Entity, direction, Owner, out string what);
            Log.Info($"[NetDemo] {who} hit {(target ? what : "the level")} {hit.Distance / 100.0f:0.0} m away (looked {rewoundMs:0} ms back)");
        }
        else
            Log.Info($"[NetDemo] {who} missed (looked {rewoundMs:0} ms back)");
        CallMulticastRpc(nameof(Fired));
    }

    // Everyone but the shooter (it flashed already): the shooter's flash.
    [MulticastRpc(Reliable = false, SkipOwner = true)]
    void Fired() => m_Flash.Start(0.08f);

    // On the server: shot by someone.
    public void OnShot(Vector3 direction, uint shooter) => CallMulticastRpc(nameof(Hit));

    [MulticastRpc(Reliable = false)]
    void Hit() => m_Flash.Start(0.25f, red: true);

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
        NetDemoTargets.Unregister(Self);
        if (IsOwner)
            Input.CursorLocked = false;
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

/// <summary>
/// A bot: a character the server owns and moves (it wanders around where it was spawned); clients draw it between snapshots -- only
/// the clients whose player is within 80 m (its Network Identity's Cull Distance).
/// </summary>
public sealed class NetBot : EntityBehaviour, INetShootable
{
    [Replicated(OnRep = nameof(OnColorChanged))] public Vector3 Color = new(0.6f, 0.6f, 0.6f);
    [Expose(Tooltip = "Walking speed (world units a second)")] public float Speed = 250.0f;
    [Expose(Tooltip = "How far it wanders from where it was spawned (world units)")] public float Range = 3000.0f;

    Mesh? m_Mesh;
    NetFlash m_Flash;
    Vector3d m_Home;
    bool m_HomeSet;
    Vector3 m_Direction;
    float m_Timer;
    readonly Random m_Random = new();

    protected override void OnCreate()
    {
        if (HasAuthority)
        {
            Color = new Vector3(0.2f + 0.3f * m_Random.NextSingle(), 0.2f + 0.3f * m_Random.NextSingle(), 0.6f + 0.4f * m_Random.NextSingle());
        }
        m_Mesh = NetDemoMesh.Box(new Vector3(68.0f, 150.0f, 68.0f), Color);
        Self.SetMesh(m_Mesh);
        NetDemoTargets.Register(Self, this);
    }

    // On the server: shot -- it hops, and flashes red everywhere.
    public void OnShot(Vector3 direction, uint shooter)
    {
        CharacterControllerComponent character = GetComponent<CharacterControllerComponent>();
        if (character.IsGrounded)
            character.Jump(450.0f);
        CallMulticastRpc(nameof(Hit));
    }

    [MulticastRpc(Reliable = false)]
    void Hit() => m_Flash.Start(0.3f, red: true);

    void OnColorChanged() => m_Mesh?.SetColor(Color.X, Color.Y, Color.Z, new Vector3(0.0f, 0.0f, 0.0f));

    protected override void OnUpdate(float deltaTime)
    {
        m_Flash.Update(deltaTime, m_Mesh, Color);
        if (!HasAuthority)
            return; // a client's copy: the server moves it
        if (!m_HomeSet)
        {
            m_Home = GetComponent<TransformComponent>().WorldPosition; // (where it was spawned, once its transform is in place)
            m_HomeSet = true;
        }
        m_Timer -= deltaTime;
        if (m_Timer <= 0.0f)
        {
            // A new direction every few seconds (sometimes a pause); back home when it wandered too far.
            m_Timer = 1.5f + 3.0f * m_Random.NextSingle();
            Vector3d position = GetComponent<TransformComponent>().WorldPosition;
            double dx = m_Home.X - position.X, dz = m_Home.Z - position.Z;
            double away = Math.Sqrt(dx * dx + dz * dz);
            if (away > Range)
                m_Direction = new Vector3((float)(dx / away), 0.0f, (float)(dz / away));
            else if (m_Random.NextSingle() < 0.2f)
                m_Direction = new Vector3(0.0f, 0.0f, 0.0f);
            else
            {
                float angle = m_Random.NextSingle() * MathF.PI * 2.0f;
                m_Direction = new Vector3(MathF.Cos(angle), 0.0f, MathF.Sin(angle));
            }
        }
        GetComponent<CharacterControllerComponent>().SetMoveVelocity(new Vector3(m_Direction.X * Speed, 0.0f, m_Direction.Z * Speed));
    }

    protected override void OnDestroy()
    {
        NetDemoTargets.Unregister(Self);
        m_Mesh?.Dispose();
        m_Mesh = null;
    }
}

/// <summary>Something a shot can hit (the demo's targets; the server calls it).</summary>
interface INetShootable
{
    void OnShot(Vector3 direction, uint shooter);
}

// The demo's targets by entity, so the server finds the script a shot hit.
static class NetDemoTargets
{
    static readonly Dictionary<ulong, INetShootable> s_Targets = new();

    internal static void Register(Entity entity, INetShootable target) => s_Targets[entity.ID] = target;
    internal static void Unregister(Entity entity) => s_Targets.Remove(entity.ID);

    internal static bool Shoot(Entity entity, Vector3 direction, uint shooter, out string what)
    {
        if (!s_Targets.TryGetValue(entity.ID, out INetShootable? target))
        {
            what = "";
            return false;
        }
        what = target.GetType().Name;
        target.OnShot(direction, shooter);
        return true;
    }
}

// A short flash of a mesh's color (a shot: bright; a hit: red), back to its own color after.
struct NetFlash
{
    float m_Time, m_Duration;
    bool m_Red;

    public void Start(float seconds, bool red = false)
    {
        m_Time = m_Duration = seconds;
        m_Red = red;
    }

    public void Update(float deltaTime, Mesh? mesh, Vector3 color)
    {
        if (m_Time <= 0.0f || mesh == null)
            return;
        m_Time -= deltaTime;
        if (m_Time <= 0.0f)
        {
            mesh.SetColor(color.X, color.Y, color.Z, new Vector3(0.0f, 0.0f, 0.0f));
            return;
        }
        float k = m_Time / m_Duration;
        Vector3 flash = m_Red ? new Vector3(1.0f, 0.1f, 0.1f) : new Vector3(1.0f, 1.0f, 0.8f);
        mesh.SetColor(color.X + (flash.X - color.X) * k, color.Y + (flash.Y - color.Y) * k, color.Z + (flash.Z - color.Z) * k,
                      new Vector3(flash.X * k * 2.0f, flash.Y * k * 2.0f, flash.Z * k * 2.0f));
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
