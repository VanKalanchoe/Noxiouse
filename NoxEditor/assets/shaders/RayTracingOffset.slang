// World Position Offset in the ray tracing scene (material graph step 4c part 4; Unreal's "Evaluate World Position Offset" for ray
// tracing): each vertex of an instance whose material graph offsets its vertices, moved as the raster passes move it
// (material_graph_offset: the same graph, time and inputs), written into a buffer only ray tracing reads. The instance's own BLAS is
// refit from it every frame (Renderer::updateAnimatedBLAS) and its hits read it (GpuRayTracingInstance::skinnedVertexBufferAddress), so
// ray-traced shadows, reflections, GI and the path tracer see the moved surface. A skinned (or morphed) instance starts from this frame's
// compute-skinned vertices. Compiled only as its generated variant with the project's graphs (RayTracingOffsetMaterialGraph.slang,
// Renderer/CustomLitShaders.h): the graphs' World Position Offset functions come from MaterialGraphs.slang.
#include "shaderIO.h"
#include "MaterialGraph.slang"

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID, uniform PushConstantRayTracingOffset pushConstant)
{
    UniformBufferObject ubo = loadUbo(pushConstant.uniformsReference);
    const RayTracingOffsetWorkItem work = BufferRef<RayTracingOffsetWorkItem>(pushConstant.workItemsReference)[groupID.x];
    const GpuRayTracingInstance instance = BufferRef<GpuRayTracingInstance>(ubo.sceneRayTracingInstancesReference)[work.instanceSlot];
    const GpuMaterial material = BufferRef<GpuMaterial>(ubo.sceneMaterialsReference)[instance.materialIndex];
    const float4x4 world = BufferRef<GpuTransform>(ubo.sceneTransformsReference)[work.instanceSlot].world;
    const uint meshIndex = BufferRef<GpuInstance>(ubo.sceneInstancesReference)[work.instanceSlot].meshIndex;
    const GpuMesh gpuMesh = BufferRef<GpuMesh>(ubo.sceneMeshesReference)[meshIndex];
    RWBufferRef<SkinnedVertex> output = RWBufferRef<SkinnedVertex>(pushConstant.outputReference);
    BufferRef<PackedVertex> packedVertices = BufferRef<PackedVertex>(instance.vertexBufferAddress);
    BufferRef<SkinnedVertex> skinnedVertices = BufferRef<SkinnedVertex>(pushConstant.skinnedVerticesReference);
    BufferRef<uint32_t> vertexColors = BufferRef<uint32_t>(instance.colorBufferAddress);
    for (uint v = groupThreadID.x; v < work.vertexCount; v += 64)
    {
        const uint vertexIndex = work.firstVertex + v;
        Vertex vertex = unpackVertex(packedVertices[vertexIndex]);
        const float3 bindPosition = vertex.pos; // (a graph's Local Position: before skinning and morphs)
        if (work.skinnedVertexOffset != NoSkinnedVertices && pushConstant.skinnedVerticesReference != 0)
        {
            const SkinnedVertex skinned = skinnedVertices[work.skinnedVertexOffset + vertexIndex];
            vertex.pos = skinned.position.xyz;
            vertex.normal = skinned.normal.xyz;
            vertex.tangent = skinned.tangent;
        }
        const float3 renderPos = mul(world, float4(vertex.pos, 1.0)).xyz;
        const float3 worldNormal = mul((float3x3)instance.normalMatrix, vertex.normal);
        const float4 color = instance.colorBufferAddress != 0 ? unpackVertexColor(vertexColors[vertexIndex]) : float4(1.0);
        const float3 offset = material_graph_offset(material, renderPos, worldNormal, vertex.uv0, vertex.uv1, color, ubo.timeSeconds, ubo,
                                                    bindPosition, float4(mul((float3x3)world, vertex.tangent.xyz), vertex.tangent.w),
                                                    world, instance.normalMatrix, gpuMesh.boundsMin, gpuMesh.boundsMax);
        // World units into the mesh's own space (the BLAS's): the inverse of the instance's 3x3 is the transpose of its normal matrix
        // (transpose(inverse(world))).
        const float3 localOffset = mul(transpose((float3x3)instance.normalMatrix), offset);
        SkinnedVertex moved;
        moved.position = float4(vertex.pos + localOffset, 1.0);
        moved.normal = float4(vertex.normal, 0.0);
        moved.tangent = vertex.tangent;
        output.Store(work.destinationVertexOffset + vertexIndex, moved);
    }
}
