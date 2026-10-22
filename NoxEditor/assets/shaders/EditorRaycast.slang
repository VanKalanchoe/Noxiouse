#include "shaderIO.h"

// Rays the editor asks about (Renderer::RequestRaycast: Snap to Floor's surface below an object): the distance to the first
// opaque, non-blended surface, -1 for none. Transmissive and blended surfaces are skipped (a floor is what you stand on).
[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID, uniform PushConstantEditorRaycast pc)
{
    if (tid.x >= pc.rayCount)
        return;
    UniformBufferObject* ubo = (UniformBufferObject*)pc.matrixReference;
    EditorRay* rays = (EditorRay*)pc.raysReference;
    EditorRay ray = rays[tid.x];
    if (ubo->tlasDeviceAddress == 0)
    {
        rays[tid.x].hitDistance = -1.0;
        return;
    }

    RayDesc desc;
    desc.Origin = ray.origin;
    desc.Direction = normalize(ray.direction);
    desc.TMin = 0.0;
    desc.TMax = ray.maxDistance;
    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(tlas, RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, desc);
    GpuRayTracingInstance* instances = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
    while (query.Proceed())
    {
        const GpuRayTracingInstance candidate = instances[query.CandidateRayInstanceCustomIndex()];
        if (candidate.alphaMode != 2 && !ray_see_through(candidate.transmissionFactor, candidate.alphaMode))
            query.CommitNonOpaqueTriangleHit();
    }
    rays[tid.x].hitDistance = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? query.CommittedRayT() : -1.0;
}
