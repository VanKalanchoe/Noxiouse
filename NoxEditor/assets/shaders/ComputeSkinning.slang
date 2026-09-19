#include "shaderIO.h"

// One group per bounded vertex chunk. Multiple groups therefore skin a large character in parallel while each group
// still amortizes its work-item load over several vertices per lane.
[shader("compute")]
[numthreads(64, 1, 1)]
void compMain(
    uint3 groupID : SV_GroupID,
    uint3 groupThreadID : SV_GroupThreadID,
    uniform PushConstantSkinning pushConstant)
{
    Vertex* sourceVertices = (Vertex*)pushConstant.sourceVerticesReference;
    SkinnedVertex* skinnedVertices = (SkinnedVertex*)pushConstant.skinnedVerticesReference;
    float4x4* boneMatrices = (float4x4*)pushConstant.boneMatricesReference;
    SkinningWorkItem* workItems = (SkinningWorkItem*)pushConstant.workItemsReference;

    SkinningWorkItem work = workItems[groupID.x];
    for (uint vertexIndex = groupThreadID.x; vertexIndex < work.vertexCount; vertexIndex += 64)
    {
        Vertex vertex = sourceVertices[work.sourceVertexOffset + vertexIndex];
        float4x4 skinTransform =
            boneMatrices[work.boneMatrixOffset + vertex.boneIDs.x] * vertex.boneWeights.x +
            boneMatrices[work.boneMatrixOffset + vertex.boneIDs.y] * vertex.boneWeights.y +
            boneMatrices[work.boneMatrixOffset + vertex.boneIDs.z] * vertex.boneWeights.z +
            boneMatrices[work.boneMatrixOffset + vertex.boneIDs.w] * vertex.boneWeights.w;

        SkinnedVertex output;
        output.position = mul(skinTransform, float4(vertex.pos, 1.0));
        output.normal = float4(mul((float3x3)skinTransform, vertex.normal), 0.0);
        skinnedVertices[work.destinationVertexOffset + vertexIndex] = output;
    }
}
