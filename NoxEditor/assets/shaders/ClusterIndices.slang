// A streamed mesh's ray tracing triangles (§5.7.2): its BLAS is built from the finest resident clusters (the leaf cut), whose pages
// lie anywhere in the geometry streams. One job per cluster: its triangles as vertex stream indices (the page's vertex block plus
// the cluster's local indices) into the RT index stream, where the BLAS build and the hit shaders read them. Workgroups walk the
// jobs groupCount apart, threads the cluster's triangles.
#include "shaderIO.h"

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uniform PushConstantClusterIndices pc)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pc.matrixReference;
    ClusterIndexJob* jobs = (ClusterIndexJob*)pc.jobsReference;
    uint32_t* indices = (uint32_t*)pc.indicesReference;
    MeshletDraw* draws = (MeshletDraw*)ubo->geometryMeshletDrawsReference;
    uint32_t* meshletVertices = (uint32_t*)ubo->geometryMeshletVerticesReference;
    uint8_t* meshletTriangles = (uint8_t*)ubo->geometryMeshletTrianglesReference;

    for (uint jobIndex = gid.x; jobIndex < pc.jobCount; jobIndex += pc.groupCount)
    {
        const ClusterIndexJob job = jobs[jobIndex];
        const MeshletDraw draw = draws[job.draw];
        for (uint corner = gtid.x; corner < draw.triangleCount * 3; corner += 64)
        {
            const uint local = meshletTriangles[draw.triangleOffset + corner];
            indices[job.firstIndex + corner] = draw.globalVertexOffset + meshletVertices[draw.vertexOffset + local];
        }
    }
}
