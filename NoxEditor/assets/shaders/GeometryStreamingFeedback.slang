// Geometry streaming feedback (§5.7.2): which pages of the streamed meshes the LOD wants. One workgroup per draw list entry (an
// instance), its threads over the mesh's pages: a page is wanted when its conservative LOD bounds (all its groups' simplified bounds,
// their largest error) project above the threshold from the main view's culling camera -- the task shader's rule, so every page the
// cut would refine into is asked for, a little more at most. Every instance, not only the visible ones: ray tracing sees what is
// behind the camera, so detail streams by distance (the camera turning shows what was already there).
//
// A missing page is requested every frame it is wanted; a resident one reports once per GeometryTouchInterval frames (the
// streamer evicts what stopped reporting first). One report per page and frame (GpuGeometryPage::requestFrame).
#include "shaderIO.h"

// As the task shader's lodBoundsError (Material_PBR_MeshTask.slang).
float pageBoundsError(float3 center, float radius, float error, float3 cameraPosition, float cameraProj, float cameraNear)
{
    float d = length(center - cameraPosition) - radius;
    return error / (d > cameraNear ? d : cameraNear) * (cameraProj * 0.5);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uniform PushConstantGeometryFeedback pc)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pc.matrixReference;
    uint32_t* drawList = (uint32_t*)pc.drawListReference;
    GeometryStreamingFeedback* feedback = (GeometryStreamingFeedback*)pc.feedbackReference;
    GpuGeometryPage* pages = (GpuGeometryPage*)ubo->geometryPagesReference;
    GpuInstance* instances = (GpuInstance*)ubo->sceneInstancesReference;
    GpuTransform* transforms = (GpuTransform*)ubo->sceneTransformsReference;
    GpuMesh* meshes = (GpuMesh*)ubo->sceneMeshesReference;

    const float3 cameraPosition = ubo->frozenCameraWorldPos.xyz;
    const float cameraProj = ubo->frozenProj[1][1];
    const float threshold = max(ubo->lodErrorThreshold, 1e-6);

    for (uint entry = gid.x; entry < pc.drawCount; entry += pc.groupCount)
    {
        const uint slot = drawList[entry];
        GpuMesh* mesh = &meshes[instances[slot].meshIndex];
        const uint pageOffset = mesh.pageOffset;
        if (pageOffset == NoGeometryRange)
            continue;
        const uint pageCount = mesh.pageCount;
        const float4x4 world = transforms[slot].world;
        const float maxScale = max(length(world[0].xyz), max(length(world[1].xyz), length(world[2].xyz)));

        for (uint local = gtid.x; local < pageCount; local += 64)
        {
            const uint index = pageOffset + local;
            GpuGeometryPage* page = &pages[index];
            const float lodError = page.lodError;
            if (lodError >= ClusterTerminalError)
                continue; // a root page

            // Full detail (or an orthographic view, which the cut does not measure): every page, nearest first.
            float priority;
            if (ubo->lodFullDetail != 0)
            {
                const float3 center = mul(world, float4(page.lodCenter, 1.0)).xyz;
                priority = 1.0 / max(length(center - cameraPosition), 1e-3);
            }
            else
            {
                const float3 center = mul(world, float4(page.lodCenter, 1.0)).xyz;
                const float error = pageBoundsError(center, page.lodRadius * maxScale, lodError * maxScale, cameraPosition, cameraProj, ubo->lodCameraNear);
                if (!(error > ubo->lodErrorThreshold * GeometryPrefetchFactor))
                    continue;
                priority = error / threshold;
            }

            uint previous;
            InterlockedExchange(page.requestFrame, pc.frame, previous);
            if (previous == pc.frame)
                continue; // another instance reported it this frame
            const bool resident = page.resident != 0;
            if (resident && previous != 0xFFFFFFFF && previous / GeometryTouchInterval == pc.frame / GeometryTouchInterval)
                continue; // reported in use this interval already

            uint request;
            InterlockedAdd(feedback.count, 1, request);
            if (request < GeometryRequestCapacity)
            {
                GeometryPageRequest record;
                record.page = index | (resident ? GeometryRequestResident : 0u);
                record.priority = priority;
                feedback.requests[request] = record;
            }
        }
    }
}
