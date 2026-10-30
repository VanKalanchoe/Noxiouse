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
    PackedVertex* sourceVertices = (PackedVertex*)pushConstant.sourceVerticesReference;
    SkinVertex* sourceSkin = (SkinVertex*)pushConstant.sourceSkinReference;
    SkinnedVertex* skinnedVertices = (SkinnedVertex*)pushConstant.skinnedVerticesReference;
    float4x4* boneMatrices = (float4x4*)pushConstant.boneMatricesReference;
    SkinningWorkItem* workItems = (SkinningWorkItem*)pushConstant.workItemsReference;

    SkinningWorkItem work = workItems[groupID.x];
    for (uint vertexIndex = groupThreadID.x; vertexIndex < work.vertexCount; vertexIndex += 64)
    {
        const uint sourceIndex = work.sourceVertexOffset + vertexIndex;
        Vertex vertex = unpackVertex(sourceVertices[sourceIndex]);

        // Morph targets first, in the bind pose (Unreal's order): the vertex's deltas, each by its target's weight.
        if (work.morphWeightOffset != NoMorphWeights && pushConstant.morphRangesReference != 0)
        {
            const GpuMorphRange range = ((GpuMorphRange*)pushConstant.morphRangesReference)[sourceIndex];
            GpuMorphDelta* deltas = (GpuMorphDelta*)pushConstant.morphDeltasReference;
            float* weights = (float*)pushConstant.morphWeightsReference;
            float3 normal = vertex.normal;
            for (uint i = 0; i < range.count; ++i)
            {
                const GpuMorphDelta delta = deltas[range.first + i];
                const float weight = weights[work.morphWeightOffset + delta.target];
                vertex.pos += delta.position * weight;
                normal += delta.normal * weight;
            }
            vertex.normal = dot(normal, normal) > 1e-12 ? normalize(normal) : vertex.normal;
        }

        float4x4 skinTransform = float4x4(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);
        if (work.boneMatrixOffset != NoSkinningBones)
        {
            unpackSkin(sourceSkin[sourceIndex], vertex);
            skinTransform =
                boneMatrices[work.boneMatrixOffset + vertex.boneIDs.x] * vertex.boneWeights.x +
                boneMatrices[work.boneMatrixOffset + vertex.boneIDs.y] * vertex.boneWeights.y +
                boneMatrices[work.boneMatrixOffset + vertex.boneIDs.z] * vertex.boneWeights.z +
                boneMatrices[work.boneMatrixOffset + vertex.boneIDs.w] * vertex.boneWeights.w;
        }

        SkinnedVertex output;
        output.position = mul(skinTransform, float4(vertex.pos, 1.0));
        output.normal = float4(mul((float3x3)skinTransform, vertex.normal), 0.0);
        // Handedness (w) is a fixed sign describing the tangent/bitangent/normal triple's winding, not a direction:
        // skin it like the normal (rotation only, no translation) and carry the sign through unchanged.
        output.tangent = float4(mul((float3x3)skinTransform, vertex.tangent.xyz), vertex.tangent.w);
        skinnedVertices[work.destinationVertexOffset + vertexIndex] = output;
    }
}
