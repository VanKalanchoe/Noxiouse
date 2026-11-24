#include "shaderIO.h"

// GPU instance culling (§5.6.1): one view's draw list -> visible instance slots + indirect commands + draw counts per
// bucket, plus the candidates for phase 2. See CullView in shaderIO.h.
//
// Phase 1 (cullMain, countMain, offsetMain, writeMain): frustum test, then the occlusion test against the depth pyramid
// of the previous frame, using each instance's previous transform and the previous view (exactly what produced that
// pyramid). Entries that only failed the occlusion test go to the late list.
// Phase 2 (lateMain, after this frame's pyramid is built): re-tests the late list against it, so an instance that was
// wrongly rejected still draws in the same frame.

static const uint EntryCulled = 0;
static const uint EntryVisible = 1;
static const uint EntryOccluded = 2;

// `grow`: world units the instance's vertices may be moved past its mesh's bounds (SceneInstances::offsetBounds: a material graph's World
// Position Offset).
bool IsInsideFrustum(CullView view, GpuMesh mesh, GpuTransform transform, float grow)
{
    float3 center = mul(transform.world, float4(mesh.boundsSphere.xyz, 1.0)).xyz;
    float scale = max(length(transform.world[0].xyz), max(length(transform.world[1].xyz), length(transform.world[2].xyz)));
    float radius = mesh.boundsSphere.w * scale + grow;

    // Left, right, bottom, top (reverse-Z without a far plane: the near plane culls little and the far plane is none).
    for (uint i = 0; i < 4; i++)
    {
        float4 plane = view.frustum.planes[i];
        if (dot(plane.xyz, center) + plane.w < -radius)
            return false;
    }
    return true;
}

// The instance is behind everything drawn in its screen area: its nearest point is farther than the pyramid's farthest
// depth there (reverse-Z, so nearest is the largest value).
bool IsOccluded(PushConstantInstanceCulling pushConstant, GpuMesh mesh, float4x4 world, float4x4 viewProj, float grow)
{
    if (pushConstant.hiZTextureIndex == 0xFFFFFFFF)
        return false;

    // The box grown by `grow` world units: in the mesh's space, by it over the smallest axis scale (at least that far in every direction).
    float minScale = min(length(world[0].xyz), min(length(world[1].xyz), length(world[2].xyz)));
    float3 localGrow = float3(grow > 0.0 ? grow / max(minScale, 1e-6) : 0.0);
    float3 growMin = mesh.boundsMin - localGrow;
    float3 growMax = mesh.boundsMax + localGrow;
    float3 boundsMin = float3(1e30), boundsMax = float3(-1e30);
    float nearest = 0.0;
    for (uint corner = 0; corner < 8; corner++)
    {
        float3 local = float3((corner & 1) != 0 ? growMax.x : growMin.x,
                              (corner & 2) != 0 ? growMax.y : growMin.y,
                              (corner & 4) != 0 ? growMax.z : growMin.z);
        float4 clip = mul(viewProj, mul(world, float4(local, 1.0)));

        // Crosses the near plane: the screen rectangle is not meaningful, so keep the instance.
        if (clip.w <= 1e-5)
            return false;

        float3 ndc = clip.xyz / clip.w;
        boundsMin = min(boundsMin, ndc);
        boundsMax = max(boundsMax, ndc);
        nearest = max(nearest, ndc.z);
    }

    // The scene is rasterized through a flipped viewport (negative height), so NDC -1 in y is the bottom row of the
    // depth the pyramid reduced: y is negated here, which swaps its minimum and maximum.
    float2 uvMin = saturate(float2(boundsMin.x, -boundsMax.y) * 0.5 + 0.5);
    float2 uvMax = saturate(float2(boundsMax.x, -boundsMin.y) * 0.5 + 0.5);

    // The rectangle in level 0 texels, and the level where it covers at most 2x2 of them.
    int2 baseSize = int2(int(pushConstant.hiZWidth), int(pushConstant.hiZHeight));
    int2 baseMin = clamp(int2(uvMin * float2(baseSize)), int2(0, 0), baseSize - 1);
    int2 baseMax = clamp(int2(uvMax * float2(baseSize)), baseMin, baseSize - 1);
    int2 span = baseMax - baseMin + 1;
    int mip = int(clamp(ceil(log2(float(max(span.x, span.y)))), 0.0, float(pushConstant.hiZMipCount - 1)));
    int2 levelSize = int2(max(pushConstant.hiZWidth >> uint(mip), 1u), max(pushConstant.hiZHeight >> uint(mip), 1u));

    // A texel of level m reduced the level 0 texels [i << m, (i + 1) << m), the last one also the remainder the build
    // folded into it, so the level 0 index shifted down is exactly the texel that covers it -- scaling the uv by the
    // level size instead misses it by up to a texel on the coarse levels, where one texel is a large part of the screen.
    int2 texelMin = min(baseMin >> mip, levelSize - 1);
    int2 texelMax = min(baseMax >> mip, levelSize - 1);

    Texture2D<float> pyramid = Texture2D<float>.Handle(uint2(pushConstant.hiZTextureIndex, 0));
    float farthest = 1.0;
    for (int y = texelMin.y; y <= texelMax.y; y++)
    {
        for (int x = texelMin.x; x <= texelMax.x; x++)
            farthest = min(farthest, pyramid.Load(int3(x, y, mip)));
    }

    return nearest < farthest;
}

uint BucketOfEntry(CullView view, uint entry, uint bucket)
{
    while (bucket + 1 < CULL_BUCKET_COUNT && entry >= view.bucketStart[bucket + 1])
        bucket++;
    return bucket;
}

// 1. Frustum and occlusion test per entry (parallel).
[shader("compute")]
[numthreads(256, 1, 1)]
void cullMain(uint3 id: SV_DispatchThreadID, uniform PushConstantInstanceCulling pushConstant)
{
    CullView view = BufferRef<CullView>(pushConstant.viewReference).Load(0);
    uint entry = id.x;
    if (entry >= view.entryCount)
        return;

    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    uint slot = BufferRef<uint32_t>(pushConstant.drawListReference)[entry];
    GpuInstance instance = BufferRef<GpuInstance>(ubo.sceneInstancesReference)[slot];
    GpuMesh mesh = BufferRef<GpuMesh>(ubo.sceneMeshesReference)[instance.meshIndex];
    GpuTransform transform = BufferRef<GpuTransform>(ubo.sceneTransformsReference)[slot];

    uint state = EntryVisible;
    // Skinned vertices leave the bind pose bounds.
    bool testBounds = instance.boneMatrixOffset == 0xFFFFFFFF;
    // A material graph's World Position Offset moves them by up to its Max Displacement.
    GpuMaterial material = BufferRef<GpuMaterial>(ubo.sceneMaterialsReference)[instance.materialIndex];
    float grow = material.graphSlot != 0xFFFFFFFFu && (material.graphOutputs & MaterialGraphOutputWorldPositionOffset) != 0u ? material.graphMaxDisplacement : 0.0;
    if (testBounds && (view.flags & CULL_INSTANCES) != 0 && !IsInsideFrustum(view, mesh, transform, grow))
        state = EntryCulled;
    else if (testBounds && (view.flags & CULL_OCCLUSION) != 0 &&
             IsOccluded(pushConstant, mesh, transform.previousWorld, view.previousViewProj, grow))
        state = EntryOccluded;

    RWBufferRef<uint32_t>(pushConstant.flagsReference).Store(entry, state);
}

// 2. Visible and occluded entries per block (one invocation per block walks its entries in order).
[shader("compute")]
[numthreads(1, 1, 1)]
void countMain(uint3 id: SV_DispatchThreadID, uniform PushConstantInstanceCulling pushConstant)
{
    CullView view = BufferRef<CullView>(pushConstant.viewReference).Load(0);
    uint block = id.x;
    if (block >= view.blockCount)
        return;

    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    BufferRef<uint32_t> drawList = BufferRef<uint32_t>(pushConstant.drawListReference);
    RWBufferRef<uint32_t> flags = RWBufferRef<uint32_t>(pushConstant.flagsReference);
    BufferRef<GpuInstance> sceneInstances = BufferRef<GpuInstance>(ubo.sceneInstancesReference);
    BufferRef<GpuMesh> sceneMeshes = BufferRef<GpuMesh>(ubo.sceneMeshesReference);
    uint begin = block * CULL_BLOCK_SIZE;
    uint end = min(begin + CULL_BLOCK_SIZE, view.entryCount);
    uint visible = 0;
    uint occluded = 0;
    uint triangles = 0;
    for (uint entry = begin; entry < end; entry++)
    {
        uint f = flags[entry];
        occluded += f == EntryOccluded ? 1 : 0;
        if (f != EntryVisible)
            continue;

        visible++;
        GpuInstance instance = sceneInstances[drawList[entry]];
        triangles += sceneMeshes[instance.meshIndex].triangleCount;
    }

    RWBufferRef<CullBlock> blocks = RWBufferRef<CullBlock>(pushConstant.blocksReference);
    CullBlock cb = blocks.Load(block);
    cb.visibleCount = visible;
    cb.occludedCount = occluded;
    cb.visibleTriangles = triangles;
    blocks.Store(block, cb);
}

// 3. Block offsets, the entries before each bucket start and the draw counts per bucket (one invocation).
[shader("compute")]
[numthreads(1, 1, 1)]
void offsetMain(uniform PushConstantInstanceCulling pushConstant)
{
    CullView view = BufferRef<CullView>(pushConstant.viewReference).Load(0);
    RWBufferRef<CullBlock> blocks = RWBufferRef<CullBlock>(pushConstant.blocksReference);
    RWBufferRef<uint32_t> flags = RWBufferRef<uint32_t>(pushConstant.flagsReference);
    RWBufferRef<CullCounts> countsBuf = RWBufferRef<CullCounts>(pushConstant.countsReference);
    CullCounts counts = countsBuf.Load(0);

    uint visible = 0;
    uint occluded = 0;
    uint triangles = 0;
    for (uint block = 0; block < view.blockCount; block++)
    {
        CullBlock cb = blocks.Load(block);
        cb.visibleBefore = visible;
        cb.occludedBefore = occluded;
        visible += cb.visibleCount;
        occluded += cb.occludedCount;
        triangles += cb.visibleTriangles;
        blocks.Store(block, cb);
    }
    counts.visibleTriangles = triangles;
    // Phase 2 adds to these while it re-tests (this pass runs before it).
    counts.lateDrawn = 0;
    counts.lateTriangles = 0;

    for (uint bucket = 0; bucket <= CULL_BUCKET_COUNT; bucket++)
    {
        uint start = view.bucketStart[bucket];
        uint block = start / CULL_BLOCK_SIZE;
        uint visibleBefore = visible;
        uint occludedBefore = occluded;
        if (block < view.blockCount)
        {
            CullBlock cb = blocks.Load(block);
            visibleBefore = cb.visibleBefore;
            occludedBefore = cb.occludedBefore;
        }
        for (uint entry = block * CULL_BLOCK_SIZE; entry < start; entry++)
        {
            uint f = flags[entry];
            visibleBefore += f == EntryVisible ? 1 : 0;
            occludedBefore += f == EntryOccluded ? 1 : 0;
        }
        counts.visibleBefore[bucket] = visibleBefore;
        counts.occludedBefore[bucket] = occludedBefore;
    }

    for (uint bucket = 0; bucket < CULL_BUCKET_COUNT; bucket++)
    {
        counts.drawCount[bucket] = counts.visibleBefore[bucket + 1] - counts.visibleBefore[bucket];
        counts.lateDrawCount[bucket] = counts.occludedBefore[bucket + 1] - counts.occludedBefore[bucket];
    }
    countsBuf.Store(0, counts);
}

// 4. Visible entries and phase 2 candidates written from their bucket's start, in entry order (one invocation per block).
[shader("compute")]
[numthreads(1, 1, 1)]
void writeMain(uint3 id: SV_DispatchThreadID, uniform PushConstantInstanceCulling pushConstant)
{
    CullView view = BufferRef<CullView>(pushConstant.viewReference).Load(0);
    uint block = id.x;
    if (block >= view.blockCount)
        return;

    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    BufferRef<uint32_t> drawList = BufferRef<uint32_t>(pushConstant.drawListReference);
    RWBufferRef<uint32_t> flags = RWBufferRef<uint32_t>(pushConstant.flagsReference);
    CullCounts counts = RWBufferRef<CullCounts>(pushConstant.countsReference).Load(0);
    RWBufferRef<uint32_t> visibleInstances = RWBufferRef<uint32_t>(pushConstant.visibleInstancesReference);
    RWBufferRef<uint32_t> lateInstances = RWBufferRef<uint32_t>(pushConstant.lateInstancesReference);
    RWBufferRef<MeshTasksIndirectCommand> commands = RWBufferRef<MeshTasksIndirectCommand>(pushConstant.commandsReference);
    BufferRef<GpuInstance> sceneInstances = BufferRef<GpuInstance>(ubo.sceneInstancesReference);
    BufferRef<GpuMesh> sceneMeshes = BufferRef<GpuMesh>(ubo.sceneMeshesReference);

    uint begin = block * CULL_BLOCK_SIZE;
    uint end = min(begin + CULL_BLOCK_SIZE, view.entryCount);
    CullBlock cb = RWBufferRef<CullBlock>(pushConstant.blocksReference).Load(block);
    uint visibleBefore = cb.visibleBefore;
    uint occludedBefore = cb.occludedBefore;

    uint bucket = BucketOfEntry(view, begin, 0);
    for (uint entry = begin; entry < end; entry++)
    {
        bucket = BucketOfEntry(view, entry, bucket);
        uint slot = drawList[entry];
        uint f = flags[entry];

        if (f == EntryVisible)
        {
            uint rank = visibleBefore - counts.visibleBefore[bucket];
            uint target = view.bucketStart[bucket] + rank;
            visibleBefore++;

            GpuInstance instance = sceneInstances[slot];
            uint meshletCount = sceneMeshes[instance.meshIndex].meshletCount;
            visibleInstances.Store(target, slot);
            MeshTasksIndirectCommand cmd;
            cmd.drawID = rank;
            cmd.groupCountX = (meshletCount + TASK_SHADER_DISPATCH_X - 1) / TASK_SHADER_DISPATCH_X;
            cmd.groupCountY = 1;
            cmd.groupCountZ = 1;
            commands.Store(target, cmd);
        }
        else if (f == EntryOccluded)
        {
            uint target = view.bucketStart[bucket] + occludedBefore - counts.occludedBefore[bucket];
            occludedBefore++;
            lateInstances.Store(target, slot);
        }
    }
}

// Phase 2: the candidates against this frame's pyramid, with their current transform and view. Entries outside a bucket's
// candidate range and instances that are still occluded get an empty command.
[shader("compute")]
[numthreads(256, 1, 1)]
void lateMain(uint3 id: SV_DispatchThreadID, uniform PushConstantInstanceCulling pushConstant)
{
    CullView view = BufferRef<CullView>(pushConstant.viewReference).Load(0);
    uint entry = id.x;
    if (entry >= view.entryCount)
        return;

    CullCounts counts = RWBufferRef<CullCounts>(pushConstant.countsReference).Load(0);
    RWBufferRef<MeshTasksIndirectCommand> commands = RWBufferRef<MeshTasksIndirectCommand>(pushConstant.lateCommandsReference);
    uint bucket = BucketOfEntry(view, entry, 0);
    uint rank = entry - view.bucketStart[bucket];

    MeshTasksIndirectCommand cmd;
    cmd.drawID = rank;
    cmd.groupCountX = 0;
    cmd.groupCountY = 1;
    cmd.groupCountZ = 1;
    commands.Store(entry, cmd);

    if (entry >= view.bucketStart[bucket] + counts.lateDrawCount[bucket])
        return;

    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    uint slot = BufferRef<uint32_t>(pushConstant.lateInstancesReference)[entry];
    GpuInstance instance = BufferRef<GpuInstance>(ubo.sceneInstancesReference)[slot];
    GpuMesh mesh = BufferRef<GpuMesh>(ubo.sceneMeshesReference)[instance.meshIndex];
    GpuTransform transform = BufferRef<GpuTransform>(ubo.sceneTransformsReference)[slot];

    GpuMaterial material = BufferRef<GpuMaterial>(ubo.sceneMaterialsReference)[instance.materialIndex];
    float grow = material.graphSlot != 0xFFFFFFFFu && (material.graphOutputs & MaterialGraphOutputWorldPositionOffset) != 0u ? material.graphMaxDisplacement : 0.0;
    if (IsOccluded(pushConstant, mesh, transform.world, view.viewProj, grow))
        return;

    cmd.groupCountX = (mesh.meshletCount + TASK_SHADER_DISPATCH_X - 1) / TASK_SHADER_DISPATCH_X;
    commands.Store(entry, cmd);

    // What phase 2 adds to the frame, so the counters cover every instance drawn and not only phase 1.
    // CullCounts layout: drawCount[10], lateDrawCount[10], visibleBefore[11], occludedBefore[11], visibleTriangles, lateDrawn, lateTriangles.
    const uint kLateDrawnOffset = (CULL_BUCKET_COUNT * 2 + (CULL_BUCKET_COUNT + 1) * 2 + 1) * 4u;
    const uint kLateTrianglesOffset = kLateDrawnOffset + 4u;
    AtomicAddRaw(pushConstant.countsReference, kLateDrawnOffset, 1u);
    AtomicAddRaw(pushConstant.countsReference, kLateTrianglesOffset, mesh.triangleCount);
}
