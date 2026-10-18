using System;
using System.Collections.Generic;
using Nox;

namespace Facerun;

/// <summary>
/// A Minecraft-style terrain built with the run-time mesh API (Mesh, MeshBuilder, Jobs, Entity.SetMeshCollider,
/// Physics.Raycast): put it on an empty entity and press Play. Chunks of 16 x 16 columns are generated and meshed on the job
/// threads (only the block faces next to air), then shown and made solid on the main thread. Left click digs the block the
/// camera looks at, right click places one against it.
/// </summary>
public sealed class VoxelTerrain : EntityBehaviour
{
    [Expose] public int ChunksPerSide = 4;
    [Expose(Unit = "m")] public float BlockSize = 100.0f; // world units (the project's are cm: 100 = 1 m)
    [Expose] public string CameraName = "Camera";
    [Expose] public string MaterialPath = "";             // optional .nmat; empty: a plain grass color
    [Expose] public float NoiseScale = 0.04f;              // per block
    [Expose] public float HillHeight = 10.0f;              // blocks
    [Expose] public int Seed = 1337;

    private const int ChunkSize = 16;
    private const int ChunkHeight = 48;
    private const float ReachInBlocks = 8.0f;

    private sealed class Chunk
    {
        public int X;
        public int Z;
        public byte[] Blocks = Array.Empty<byte>(); // 0 = air, x + ChunkSize * (z + ChunkSize * y)
        public Entity Entity;
        public Mesh? Mesh;
        public JobHandle<MeshBuilder> Job;
        public bool Built;
    }

    private readonly Dictionary<(int, int), Chunk> _chunks = new();
    private bool _ready;
    private bool _leftWasDown;
    private bool _rightWasDown;

    protected override void OnCreate()
    {
        Vector3 origin = GetComponent<TransformComponent>().World.Position;
        for (int cz = 0; cz < ChunksPerSide; ++cz)
        {
            for (int cx = 0; cx < ChunksPerSide; ++cx)
            {
                var chunk = new Chunk { X = cx, Z = cz };
                chunk.Entity = Scene.CreateEntity($"Chunk {cx},{cz}", origin + new Vector3(cx * ChunkSize * BlockSize, 0.0f, cz * ChunkSize * BlockSize));
                chunk.Mesh = new Mesh();
                if (MaterialPath.Length == 0 || !chunk.Mesh.SetMaterial(MaterialPath))
                    chunk.Mesh.SetColor(0.30f, 0.55f, 0.22f);
                chunk.Entity.SetMesh(chunk.Mesh);
                _chunks[(cx, cz)] = chunk;

                // Generation and meshing touch only the chunk's own data and pure functions: safe on a job thread. The heights
                // outside the chunk come from the same noise, so the faces at chunk borders match.
                Chunk captured = chunk;
                chunk.Job = Jobs.Run(() =>
                {
                    captured.Blocks = Generate(captured.X, captured.Z);
                    return BuildMesh(captured, null);
                });
            }
        }
    }

    protected override void OnUpdate(float deltaTime)
    {
        if (!_ready)
        {
            // Shown as each job finishes; edits wait until every chunk is there.
            bool all = true;
            foreach (Chunk chunk in _chunks.Values)
            {
                if (chunk.Built)
                    continue;
                if (chunk.Job.TryGetResult(out MeshBuilder builder))
                    Apply(chunk, builder);
                else
                    all = false;
            }
            _ready = all;
            return;
        }

        bool left = Input.IsMouseButtonDown(MouseButton.Left);
        bool right = Input.IsMouseButtonDown(MouseButton.Right);
        if (left && !_leftWasDown)
            Edit(dig: true);
        if (right && !_rightWasDown)
            Edit(dig: false);
        _leftWasDown = left;
        _rightWasDown = right;
    }

    protected override void OnDestroy()
    {
        foreach (Chunk chunk in _chunks.Values)
            chunk.Mesh?.Dispose();
        _chunks.Clear();
    }

    private void Apply(Chunk chunk, MeshBuilder builder)
    {
        chunk.Built = true;
        if (chunk.Mesh == null)
            return;
        chunk.Mesh.SetData(builder);
        chunk.Entity.SetMeshCollider(chunk.Mesh);
    }

    private void Edit(bool dig)
    {
        Entity camera = Entity.FindByName(CameraName);
        if (!camera.IsValid)
            return;
        Transform view = camera.GetComponent<TransformComponent>().World;
        float pitch = view.Rotation.X;
        float yaw = view.Rotation.Y;
        Vector3 forward = new(-MathF.Sin(yaw) * MathF.Cos(pitch), MathF.Sin(pitch), -MathF.Cos(yaw) * MathF.Cos(pitch));

        if (!Physics.Raycast(view.Position, forward, ReachInBlocks * BlockSize, out RaycastHit hit))
            return;
        // Half a block into the surface is the block hit, half a block out the empty cell in front of it.
        Vector3 inside = hit.Point + hit.Normal * (dig ? -0.5f * BlockSize : 0.5f * BlockSize);
        Vector3 local = inside - GetComponent<TransformComponent>().World.Position;
        int wx = (int)MathF.Floor(local.X / BlockSize);
        int wy = (int)MathF.Floor(local.Y / BlockSize);
        int wz = (int)MathF.Floor(local.Z / BlockSize);
        if (wy < 0 || wy >= ChunkHeight)
            return;

        (int cx, int cz) = (FloorDiv(wx, ChunkSize), FloorDiv(wz, ChunkSize));
        if (!_chunks.TryGetValue((cx, cz), out Chunk? chunk))
            return;
        int x = wx - cx * ChunkSize;
        int z = wz - cz * ChunkSize;
        chunk.Blocks[Index(x, wy, z)] = dig ? (byte)0 : (byte)1;

        // A small chunk meshes in about a millisecond: rebuilt right here, with the neighbours a border edit uncovers.
        Rebuild(chunk);
        if (x == 0) RebuildAt(cx - 1, cz);
        if (x == ChunkSize - 1) RebuildAt(cx + 1, cz);
        if (z == 0) RebuildAt(cx, cz - 1);
        if (z == ChunkSize - 1) RebuildAt(cx, cz + 1);
    }

    private void RebuildAt(int cx, int cz)
    {
        if (_chunks.TryGetValue((cx, cz), out Chunk? chunk))
            Rebuild(chunk);
    }

    private void Rebuild(Chunk chunk) => Apply(chunk, BuildMesh(chunk, _chunks));

    // Terrain: grass-topped hills from fractal value noise over the column's world position.
    private byte[] Generate(int cx, int cz)
    {
        var blocks = new byte[ChunkSize * ChunkSize * ChunkHeight];
        for (int z = 0; z < ChunkSize; ++z)
        {
            for (int x = 0; x < ChunkSize; ++x)
            {
                int height = Height(cx * ChunkSize + x, cz * ChunkSize + z);
                for (int y = 0; y < height; ++y)
                    blocks[Index(x, y, z)] = 1;
            }
        }
        return blocks;
    }

    private int Height(int wx, int wz)
    {
        float noise = 0.0f;
        float amplitude = 1.0f;
        float frequency = NoiseScale;
        for (int octave = 0; octave < 4; ++octave)
        {
            noise += ValueNoise(wx * frequency, wz * frequency, Seed + octave * 101) * amplitude;
            amplitude *= 0.5f;
            frequency *= 2.0f;
        }
        int height = 8 + (int)(noise * HillHeight);
        return Math.Clamp(height, 1, ChunkHeight - 1);
    }

    // Solid or not, from the chunk's blocks, a neighbour chunk's (when given, after edits) or the generated terrain.
    private bool IsSolid(Chunk chunk, Dictionary<(int, int), Chunk>? chunks, int x, int y, int z)
    {
        if (y < 0)
            return true;
        if (y >= ChunkHeight)
            return false;
        if (x >= 0 && x < ChunkSize && z >= 0 && z < ChunkSize)
            return chunk.Blocks[Index(x, y, z)] != 0;

        int wx = chunk.X * ChunkSize + x;
        int wz = chunk.Z * ChunkSize + z;
        if (chunks != null && chunks.TryGetValue((FloorDiv(wx, ChunkSize), FloorDiv(wz, ChunkSize)), out Chunk? neighbour) && neighbour.Blocks.Length > 0)
            return neighbour.Blocks[Index(wx - neighbour.X * ChunkSize, y, wz - neighbour.Z * ChunkSize)] != 0;
        return y < Height(wx, wz);
    }

    // The faces of solid blocks next to air, each a quad with its own vertices (hard edges), in the chunk's local space.
    private MeshBuilder BuildMesh(Chunk chunk, Dictionary<(int, int), Chunk>? chunks)
    {
        var builder = new MeshBuilder();
        float s = BlockSize;
        for (int y = 0; y < ChunkHeight; ++y)
        {
            for (int z = 0; z < ChunkSize; ++z)
            {
                for (int x = 0; x < ChunkSize; ++x)
                {
                    if (chunk.Blocks[Index(x, y, z)] == 0)
                        continue;
                    float x0 = x * s, x1 = (x + 1) * s;
                    float y0 = y * s, y1 = (y + 1) * s;
                    float z0 = z * s, z1 = (z + 1) * s;
                    Vector2 uv0 = new(0.0f, 0.0f), uv1 = new(1.0f, 1.0f);
                    // Corners counter-clockwise seen from outside the block.
                    if (!IsSolid(chunk, chunks, x, y + 1, z))
                        builder.AddQuad(new(x0, y1, z1), new(x1, y1, z1), new(x1, y1, z0), new(x0, y1, z0), new(0, 1, 0), uv0, uv1);
                    if (!IsSolid(chunk, chunks, x, y - 1, z))
                        builder.AddQuad(new(x0, y0, z0), new(x1, y0, z0), new(x1, y0, z1), new(x0, y0, z1), new(0, -1, 0), uv0, uv1);
                    if (!IsSolid(chunk, chunks, x + 1, y, z))
                        builder.AddQuad(new(x1, y0, z1), new(x1, y0, z0), new(x1, y1, z0), new(x1, y1, z1), new(1, 0, 0), uv0, uv1);
                    if (!IsSolid(chunk, chunks, x - 1, y, z))
                        builder.AddQuad(new(x0, y0, z0), new(x0, y0, z1), new(x0, y1, z1), new(x0, y1, z0), new(-1, 0, 0), uv0, uv1);
                    if (!IsSolid(chunk, chunks, x, y, z + 1))
                        builder.AddQuad(new(x0, y0, z1), new(x1, y0, z1), new(x1, y1, z1), new(x0, y1, z1), new(0, 0, 1), uv0, uv1);
                    if (!IsSolid(chunk, chunks, x, y, z - 1))
                        builder.AddQuad(new(x1, y0, z0), new(x0, y0, z0), new(x0, y1, z0), new(x1, y1, z0), new(0, 0, -1), uv0, uv1);
                }
            }
        }
        return builder;
    }

    private static int Index(int x, int y, int z) => x + ChunkSize * (z + ChunkSize * y);

    private static int FloorDiv(int value, int divisor) => (int)MathF.Floor(value / (float)divisor);

    // Smoothly interpolated hashed lattice values in [-1, 1].
    private static float ValueNoise(float x, float z, int seed)
    {
        int x0 = (int)MathF.Floor(x), z0 = (int)MathF.Floor(z);
        float tx = x - x0, tz = z - z0;
        tx = tx * tx * (3.0f - 2.0f * tx);
        tz = tz * tz * (3.0f - 2.0f * tz);
        float a = Hash(x0, z0, seed), b = Hash(x0 + 1, z0, seed);
        float c = Hash(x0, z0 + 1, seed), d = Hash(x0 + 1, z0 + 1, seed);
        return Lerp(Lerp(a, b, tx), Lerp(c, d, tx), tz);
    }

    private static float Lerp(float a, float b, float t) => a + (b - a) * t;

    private static float Hash(int x, int z, int seed)
    {
        unchecked
        {
            uint h = (uint)(x * 374761393 + z * 668265263 + seed * 144665);
            h = (h ^ (h >> 13)) * 1274126177u;
            h ^= h >> 16;
            return (h & 0xFFFFFF) / (float)0x7FFFFF - 1.0f;
        }
    }
}
