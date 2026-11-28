// The path tracer as a compute shader: the same work as its ray tracing pipeline (PathTracerRayGen / Hit / Miss / AnyHit.slang),
// with the hits found by inline ray queries (DXR 1.1 / VK_KHR_ray_query, as every other ray traced pass here) instead of TraceRay
// and the shader binding table. One thread per pixel; the closest hit, miss and alpha test run as the functions those stages call
// (pathTracerClosestHit, pathTracerMiss, pathTracerAlphaAccepts), the path packed and unpacked around each ray as a payload is, so
// both give the same image. Chosen in the Renderer Settings (Path Tracer Dispatch); the default on D3D12, whose ray tracing
// pipeline hung the GPU at the path tracer's DispatchRays (docs/Backlog_2026.md).
#pragma once

#define NOX_PATH_TRACER_COMPUTE 1

#include "PathPayload.slang"
#include "PathTracerBridge.slang"
#include "PathTracerPushConstants.slang"
#include "PathTracerAnyHit.slang"
#include "PathTracerHit.slang"
#include "PathTracerMiss.slang"

// TraceRay(as, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload) of the ray tracing pipeline, as a ray query. Candidates the query hands
// over are the non-opaque instances' triangles (GpuScene::WriteTlasInstance: FORCE_NON_OPAQUE for alpha-tested and transmissive
// materials): an alpha-tested one (alpha mode not Opaque: the TLAS's shader table offset 1, hit group 1 with the any hit) is accepted
// when its alpha test passes, any other (glass with alpha mode Opaque: hit group 0, no any hit) always. Then the closest hit or the
// miss on the committed result, with the closest hit's built-ins from the query.
void pathTracerTraceInline(RaytracingAccelerationStructure as, RayDesc ray, inout PathPayload payload)
{
    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    BufferRef<GpuRayTracingInstance> instances = BufferRef<GpuRayTracingInstance>(ubo.sceneRayTracingInstancesReference);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(as, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);
    while (query.Proceed())
    {
        const uint slot = query.CandidateInstanceID();
        if (instances[slot].alphaMode == 0 ||
            pathTracerAlphaAccepts(payload, slot, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), ubo))
            query.CommitNonOpaqueTriangleHit();
    }

    PathState path = PathPayload::unpack(payload);
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        PathTracerHitInfo hit;
        hit.instanceID = query.CommittedInstanceID();
        hit.primitiveIndex = query.CommittedPrimitiveIndex();
        hit.barycentrics = query.CommittedTriangleBarycentrics();
        hit.rayT = query.CommittedRayT();
        hit.rayOrigin = ray.Origin;
        hit.rayDir = ray.Direction;
        hit.objectToWorld = query.CommittedObjectToWorld3x4();
        pathTracerClosestHit(path, hit);
    }
    else
    {
        pathTracerMiss(path, ray.Origin, ray.Direction);
    }
    payload = PathPayload::pack(path);
}

#include "PathTracerRayGen.slang"

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    pathTracePixel(threadId.xy);
}
