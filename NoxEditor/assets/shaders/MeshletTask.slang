#include "shaderIO.h"

groupshared MeshletPayload sharedPayload;
groupshared Atomic<uint32_t> visibleMeshletCount;

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
    uint drawIndex = drawID;

    // 1. Fetch instance data for this draw call via SV_DrawIndex
    InstanceData* instances = (InstanceData*)pushConstant.instanceReference;
    InstanceData instance = instances[drawIndex];

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

    uint32_t totalMeshlets = instance.meshletCount;
    // Base global meshlet index for this Task workgroup
    uint32_t groupBaseMeshlet = instance.meshletOffset + (gid.x * TASK_SHADER_DISPATCH_X);

    // Thread 0 initializes payload metadata
    if (gtid.x == 0)
    {
        visibleMeshletCount.store(0);
        sharedPayload.groupMeshletOffset = groupBaseMeshlet;
    }

    GroupMemoryBarrierWithGroupSync();

    // 2. Determine if this thread points to a valid meshlet for this model
    uint8_t threadOffset = (uint8_t)gtid.x;
    uint localMeshletIdx = (gid.x * TASK_SHADER_DISPATCH_X) + threadOffset;

    if (localMeshletIdx < totalMeshlets)
    {
        uint32_t globalMeshletIndex = groupBaseMeshlet + threadOffset;
        MeshletBounds* meshletBoundsPtr = (MeshletBounds*)pushConstant.meshletBoundsReference;
        MeshletBounds meshletBounds = meshletBoundsPtr[globalMeshletIndex];

        // Transform bounding sphere center to world space
        float3 worldCenter = mul(instance.modelMatrix, float4(meshletBounds.center, 1.0)).xyz;

        // Account for non-uniform model scaling
        float maxScale = max(length(instance.modelMatrix[0].xyz), 
                         max(length(instance.modelMatrix[1].xyz), 
                             length(instance.modelMatrix[2].xyz)));

        float worldRadius = meshletBounds.radius * maxScale;
        float4 worldBounds = float4(worldCenter, worldRadius);

        bool visible = true;

        // Frustum Cull
        bool frustumVisible = true;
        for(uint i = 0; i < 6; i++)
        {
            float dist = dot(ubo.frozenFrustum.planes[i].xyz, worldBounds.xyz) + ubo.frozenFrustum.planes[i].w;
            frustumVisible &= (dist >= -worldBounds.w);
        }
        visible &= frustumVisible;
        if(!frustumVisible)
        {
           /*  pushConstant.culledData.frustumCulled.add(1); */
        }

        // Backface cull from meshoptimizer.h
        // visible = dot(center - camera_position, cone_axis) >= cone_cutoff * length(center - camera_position) + radius
    /*     float3 worldConeAxis = normalize(mul((float3x3)pushConstant.modelMatrix, meshletBounds.coneAxis));
        float3 cameraToCenter = worldCenter - ubo.frozenCameraWorldPos.xyz;
        float centerDist = length(cameraToCenter);
        bool backfaceVisible = dot(cameraToCenter, worldConeAxis) < meshletBounds.coneCutoff * centerDist + meshletBounds.radius;
        visible &= backfaceVisible;
        if(!backfaceVisible)
        { */
            /* pushConstant.culledData.backfaceCulled.add(1); */
      /*   } */

        if (visible)
        {
            // Compact visible meshlets into payload array
            uint index = visibleMeshletCount.add(1);
            sharedPayload.drawID = drawIndex;
            sharedPayload.meshletIndices[index] = threadOffset;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // 3. Dispatch only the visible meshlets to the Mesh Shader
    if (gtid.x == 0)
    {
        uint count = visibleMeshletCount.load();
        DispatchMesh(count, 1, 1, sharedPayload);
    }
}