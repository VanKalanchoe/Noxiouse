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

bool IsInsideFrustum(CullView* view, GpuMesh* mesh, GpuTransform* transform)
{
    float3 center = mul(transform.world, float4(mesh.boundsSphere.xyz, 1.0)).xyz;
    float scale = max(length(transform.world[0].xyz), max(length(transform.world[1].xyz), length(transform.world[2].xyz)));
    float radius = mesh.boundsSphere.w * scale;

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
bool IsOccluded(PushConstantInstanceCulling pushConstant, GpuMesh* mesh, float4x4 world, float4x4 viewProj)
{
    if (pushConstant.hiZTextureIndex == 0xFFFFFFFF)
        return false;

    float3 boundsMin = float3(1e30), boundsMax = float3(-1e30);
    float nearest = 0.0;
    for (uint corner = 0; corner < 8; corner++)
    {
        float3 local = float3((corner & 1) != 0 ? mesh.boundsMax.x : mesh.boundsMin.x,
                              (corner & 2) != 0 ? mesh.boundsMax.y : mesh.boundsMin.y,
                              (corner & 4) != 0 ? mesh.boundsMax.z : mesh.boundsMin.z);
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

uint BucketOfEntry(CullView* view, uint entry, uint bucket)
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
    CullView* view = (CullView*)pushConstant.viewReference;
    uint entry = id.x;
    if (entry >= view.entryCount)
        return;

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    uint slot = ((uint32_t*)pushConstant.drawListReference)[entry];
    GpuInstance* instance = &((GpuInstance*)ubo->sceneInstancesReference)[slot];
    GpuMesh* mesh = &((GpuMesh*)ubo->sceneMeshesReference)[instance.meshIndex];
    GpuTransform* transform = &((GpuTransform*)ubo->sceneTransformsReference)[slot];

    uint state = EntryVisible;
    // Skinned vertices leave the bind pose bounds.
    bool testBounds = instance.boneMatrixOffset == 0xFFFFFFFF;
    if (testBounds && (view.flags & CULL_INSTANCES) != 0 && !IsInsideFrustum(view, mesh, transform))
        state = EntryCulled;
    else if (testBounds && (view.flags & CULL_OCCLUSION) != 0 &&
             IsOccluded(pushConstant, mesh, transform.previousWorld, view.previousViewProj))
        state = EntryOccluded;

    ((uint32_t*)pushConstant.flagsReference)[entry] = state;
}

// 2. Visible and occluded entries per block (one invocation per block walks its entries in order).
[shader("compute")]
[numthreads(1, 1, 1)]
void countMain(uint3 id: SV_DispatchThreadID, uniform PushConstantInstanceCulling pushConstant)
{
    CullView* view = (CullView*)pushConstant.viewReference;
    uint block = id.x;
    if (block >= view.blockCount)
        return;

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    uint32_t* drawList = (uint32_t*)pushConstant.drawListReference;
    uint32_t* flags = (uint32_t*)pushConstant.flagsReference;
    uint begin = block * CULL_BLOCK_SIZE;
    uint end = min(begin + CULL_BLOCK_SIZE, view.entryCount);
    uint visible = 0;
    uint occluded = 0;
    uint triangles = 0;
    for (uint entry = begin; entry < end; entry++)
    {
        occluded += flags[entry] == EntryOccluded ? 1 : 0;
        if (flags[entry] != EntryVisible)
            continue;

        visible++;
        GpuInstance* instance = &((GpuInstance*)ubo->sceneInstancesReference)[drawList[entry]];
        triangles += ((GpuMesh*)ubo->sceneMeshesReference)[instance.meshIndex].triangleCount;
    }

    CullBlock* blocks = (CullBlock*)pushConstant.blocksReference;
    blocks[block].visibleCount = visible;
    blocks[block].occludedCount = occluded;
    blocks[block].visibleTriangles = triangles;
}

// 3. Block offsets, the entries before each bucket start and the draw counts per bucket (one invocation).
[shader("compute")]
[numthreads(1, 1, 1)]
void offsetMain(uniform PushConstantInstanceCulling pushConstant)
{
    CullView* view = (CullView*)pushConstant.viewReference;
    CullBlock* blocks = (CullBlock*)pushConstant.blocksReference;
    uint32_t* flags = (uint32_t*)pushConstant.flagsReference;
    CullCounts* counts = (CullCounts*)pushConstant.countsReference;

    uint visible = 0;
    uint occluded = 0;
    uint triangles = 0;
    for (uint block = 0; block < view.blockCount; block++)
    {
        blocks[block].visibleBefore = visible;
        blocks[block].occludedBefore = occluded;
        visible += blocks[block].visibleCount;
        occluded += blocks[block].occludedCount;
        triangles += blocks[block].visibleTriangles;
    }
    counts.visibleTriangles = triangles;
    // Phase 2 adds to these while it re-tests (this pass runs before it).
    counts.lateDrawn = 0;
    counts.lateTriangles = 0;

    for (uint bucket = 0; bucket <= CULL_BUCKET_COUNT; bucket++)
    {
        uint start = view.bucketStart[bucket];
        uint block = start / CULL_BLOCK_SIZE;
        uint visibleBefore = block < view.blockCount ? blocks[block].visibleBefore : visible;
        uint occludedBefore = block < view.blockCount ? blocks[block].occludedBefore : occluded;
        for (uint entry = block * CULL_BLOCK_SIZE; entry < start; entry++)
        {
            visibleBefore += flags[entry] == EntryVisible ? 1 : 0;
            occludedBefore += flags[entry] == EntryOccluded ? 1 : 0;
        }
        counts.visibleBefore[bucket] = visibleBefore;
        counts.occludedBefore[bucket] = occludedBefore;
    }

    for (uint bucket = 0; bucket < CULL_BUCKET_COUNT; bucket++)
    {
        counts.drawCount[bucket] = counts.visibleBefore[bucket + 1] - counts.visibleBefore[bucket];
        counts.lateDrawCount[bucket] = counts.occludedBefore[bucket + 1] - counts.occludedBefore[bucket];
    }
}

// 4. Visible entries and phase 2 candidates written from their bucket's start, in entry order (one invocation per block).
[shader("compute")]
[numthreads(1, 1, 1)]
void writeMain(uint3 id: SV_DispatchThreadID, uniform PushConstantInstanceCulling pushConstant)
{
    CullView* view = (CullView*)pushConstant.viewReference;
    uint block = id.x;
    if (block >= view.blockCount)
        return;

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    uint32_t* drawList = (uint32_t*)pushConstant.drawListReference;
    uint32_t* flags = (uint32_t*)pushConstant.flagsReference;
    CullCounts* counts = (CullCounts*)pushConstant.countsReference;
    uint32_t* visibleInstances = (uint32_t*)pushConstant.visibleInstancesReference;
    uint32_t* lateInstances = (uint32_t*)pushConstant.lateInstancesReference;
    MeshTasksIndirectCommand* commands = (MeshTasksIndirectCommand*)pushConstant.commandsReference;

    uint begin = block * CULL_BLOCK_SIZE;
    uint end = min(begin + CULL_BLOCK_SIZE, view.entryCount);
    CullBlock* blocks = (CullBlock*)pushConstant.blocksReference;
    uint visibleBefore = blocks[block].visibleBefore;
    uint occludedBefore = blocks[block].occludedBefore;

    uint bucket = BucketOfEntry(view, begin, 0);
    for (uint entry = begin; entry < end; entry++)
    {
        bucket = BucketOfEntry(view, entry, bucket);
        uint slot = drawList[entry];

        if (flags[entry] == EntryVisible)
        {
            uint target = view.bucketStart[bucket] + visibleBefore - counts.visibleBefore[bucket];
            visibleBefore++;

            GpuInstance* instance = &((GpuInstance*)ubo->sceneInstancesReference)[slot];
            uint meshletCount = ((GpuMesh*)ubo->sceneMeshesReference)[instance.meshIndex].meshletCount;
            visibleInstances[target] = slot;
            commands[target].groupCountX = (meshletCount + TASK_SHADER_DISPATCH_X - 1) / TASK_SHADER_DISPATCH_X;
            commands[target].groupCountY = 1;
            commands[target].groupCountZ = 1;
        }
        else if (flags[entry] == EntryOccluded)
        {
            uint target = view.bucketStart[bucket] + occludedBefore - counts.occludedBefore[bucket];
            occludedBefore++;
            lateInstances[target] = slot;
        }
    }
}

// Phase 2: the candidates against this frame's pyramid, with their current transform and view. Entries outside a bucket's
// candidate range and instances that are still occluded get an empty command.
[shader("compute")]
[numthreads(256, 1, 1)]
void lateMain(uint3 id: SV_DispatchThreadID, uniform PushConstantInstanceCulling pushConstant)
{
    CullView* view = (CullView*)pushConstant.viewReference;
    uint entry = id.x;
    if (entry >= view.entryCount)
        return;

    CullCounts* counts = (CullCounts*)pushConstant.countsReference;
    MeshTasksIndirectCommand* commands = (MeshTasksIndirectCommand*)pushConstant.lateCommandsReference;
    commands[entry].groupCountX = 0;
    commands[entry].groupCountY = 1;
    commands[entry].groupCountZ = 1;

    uint bucket = BucketOfEntry(view, entry, 0);
    if (entry >= view.bucketStart[bucket] + counts.lateDrawCount[bucket])
        return;

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    uint slot = ((uint32_t*)pushConstant.lateInstancesReference)[entry];
    GpuInstance* instance = &((GpuInstance*)ubo->sceneInstancesReference)[slot];
    GpuMesh* mesh = &((GpuMesh*)ubo->sceneMeshesReference)[instance.meshIndex];
    GpuTransform* transform = &((GpuTransform*)ubo->sceneTransformsReference)[slot];

    if (IsOccluded(pushConstant, mesh, transform.world, view.viewProj))
        return;

    commands[entry].groupCountX = (mesh.meshletCount + TASK_SHADER_DISPATCH_X - 1) / TASK_SHADER_DISPATCH_X;

    // What phase 2 adds to the frame, so the counters cover every instance drawn and not only phase 1.
    uint32_t previous;
    InterlockedAdd(counts.lateDrawn, 1u, previous);
    InterlockedAdd(counts.lateTriangles, mesh.triangleCount, previous);
}
