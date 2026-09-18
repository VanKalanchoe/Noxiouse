#include "shaderIO.h"

groupshared MeshletPayload sharedPayload;
groupshared Atomic<uint32_t> visibleMeshletCount;
groupshared Atomic<uint32_t> visibleTriangleCount;

// Screen-space error of a cluster LOD bound (fraction of the screen height), meshoptimizer demo/nanite.cpp boundsError:
// perspective distortion ignored (rotation invariant), so it is monotonic along the DAG and the cut is consistent.
float lodBoundsError(float3 center, float radius, float error, float3 cameraPosition, float cameraProj, float cameraNear)
{
    float d = length(center - cameraPosition) - radius;
    return error / (d > cameraNear ? d : cameraNear) * (cameraProj * 0.5);
}

[shader("task")]
[numthreads(TASK_SHADER_DISPATCH_X, 1, 1)]
void taskMain
(
    uint3 gid : SV_GroupID, 
    uint3 gtid : SV_GroupThreadID,
    uint drawID : SV_DrawIndex,
    uniform PushConstantMeshlets pushConstant
)
{
    // 1. The draw's instance slot (draw lists hold one GPU scene instance slot per indirect draw)
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    uint32_t* drawInstances = (uint32_t*)pushConstant.drawInstancesReference;
    uint instanceSlot = drawInstances[pushConstant.instanceBaseIndex + drawID];

    SceneInstances instances = SceneInstances(ubo);
    InstanceData instance = instances[instanceSlot];

    uint32_t totalMeshlets = instance.meshletCount;
    // Base global meshlet index for this Task workgroup
    uint32_t groupBaseMeshlet = instance.drawsOffset + (gid.x * TASK_SHADER_DISPATCH_X);

    // Thread 0 initializes payload metadata
    if (gtid.x == 0)
    {
        visibleMeshletCount.store(0);
        visibleTriangleCount.store(0);
        sharedPayload.groupMeshletOffset = groupBaseMeshlet;
    }

    GroupMemoryBarrierWithGroupSync();

    // 2. Determine if this thread points to a valid meshlet for this model
    uint8_t threadOffset = (uint8_t)gtid.x;
    uint localMeshletIdx = (gid.x * TASK_SHADER_DISPATCH_X) + threadOffset;

    if (localMeshletIdx < totalMeshlets)
    {
        // The bounds stream holds this mesh's meshlets in the same order as its draws, from its own offset.
        MeshletBounds* meshletBounds = &((MeshletBounds*)ubo->geometryMeshletBoundsReference)[instance.boundsOffset + localMeshletIdx];
        float maxScale = max(length(instance.modelMatrix[0].xyz),
                         max(length(instance.modelMatrix[1].xyz), length(instance.modelMatrix[2].xyz)));

        // 1. The LOD cut (clusterlod's rule): the coarser version is too coarse, this cluster is fine enough. The bounds go
        //    to world space with the instance's largest scale; the culling view's camera (frozen with it).
        bool visible;
        if (ubo->lodFullDetail != 0)
        {
            visible = meshletBounds.lodLevel == 0;
        }
        else
        {
            float3 cameraPosition = ubo->frozenCameraWorldPos.xyz;
            float cameraProj = ubo->frozenProj[1][1];
            float3 parentCenter = mul(instance.modelMatrix, float4(meshletBounds.parentCenter, 1.0)).xyz;
            float3 lodCenter = mul(instance.modelMatrix, float4(meshletBounds.lodCenter, 1.0)).xyz;
            float parentError = meshletBounds.parentError >= ClusterTerminalError
                ? ClusterTerminalError
                : lodBoundsError(parentCenter, meshletBounds.parentRadius * maxScale, meshletBounds.parentError * maxScale, cameraPosition, cameraProj, ubo->lodCameraNear);
            float selfError = lodBoundsError(lodCenter, meshletBounds.lodRadius * maxScale, meshletBounds.lodError * maxScale, cameraPosition, cameraProj, ubo->lodCameraNear);
            visible = parentError > ubo->lodErrorThreshold && selfError <= ubo->lodErrorThreshold;
        }

        // 2. Optional meshlet frustum culling (the instance already passed GPU instance culling).
        if (visible && (pushConstant.meshletCulling & MESHLET_CULL_FRUSTUM) != 0)
        {
            float3 worldCenter = mul(instance.modelMatrix, float4(meshletBounds.center, 1.0)).xyz;
            float worldRadius = meshletBounds.radius * maxScale;

            for (uint i = 0; i < 4; i++)
            {
                float4 plane = ubo->frozenFrustum.planes[i];
                visible = visible && dot(plane.xyz, worldCenter) + plane.w >= -worldRadius;
            }
        }

        if (visible)
        {
            // Compact visible meshlets into payload array
            uint index = visibleMeshletCount.add(1);
            sharedPayload.drawID = instanceSlot;
            sharedPayload.meshletIndices[index] = threadOffset;
            if ((pushConstant.meshletCulling & MESHLET_COUNT_TRIANGLES) != 0)
                visibleTriangleCount.add(meshletBounds.triangleCount);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // 3. Dispatch only the visible meshlets to the Mesh Shader
    if (gtid.x == 0)
    {
        uint count = visibleMeshletCount.load();
        if ((pushConstant.meshletCulling & MESHLET_COUNT_TRIANGLES) != 0 && count > 0)
        {
            ClusterStats* stats = (ClusterStats*)ubo->clusterStatsReference;
            InterlockedAdd(stats->drawnClusters, count);
            InterlockedAdd(stats->drawnTriangles, visibleTriangleCount.load());
        }
        DispatchMesh(count, 1, 1, sharedPayload);
    }
}