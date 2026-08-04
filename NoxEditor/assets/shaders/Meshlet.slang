#include "shaderIO.h"

struct VSOutput
{
    float4 pos : SV_Position;
    float2 texCooord;
    float4 color;
    nointerpolation uint drawID;

    int entityID;
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(MESH_SHADER_DISPATCH_X, 1, 1)]
void meshMain
(
    in payload MeshletPayload sharedPayload,
    out indices uint3 triangles[MAX_PRIMITIVES], out vertices VSOutput vertices[MAX_VERTICES],
    uint3 groupId : SV_GroupID, uint3 gtid : SV_GroupThreadID,
    uniform PushConstantMeshlets pushConstant
)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    InstanceData* instances = (InstanceData*)pushConstant.instanceReference;

    // Retrieve model matrix using drawID stored in payload
    InstanceData instance = instances[sharedPayload.drawID];
    float4x4 modelMatrix = instance.modelMatrix;

    uint64_t* drawsPageTable  = (uint64_t*)pushConstant.meshletDrawsPageTableReference;
    uint64_t* vertsPageTable  = (uint64_t*)pushConstant.vertexPageTableReference;
    uint64_t* mVertsPageTable = (uint64_t*)pushConstant.meshletVerticesPageTableReference;
    uint64_t* mTrisPageTable  = (uint64_t*)pushConstant.meshletTrianglesPageTableReference;

    MeshletDraw* meshletDrawPage = (MeshletDraw*)drawsPageTable[instance.drawsPageIndex];
    Vertex* vertexPage = (Vertex*)vertsPageTable[instance.verticesPageIndex];
    uint32_t* meshletVerticesPage = (uint32_t*)mVertsPageTable[instance.meshletVerticesPageIndex];
    uint8_t* meshletTrianglePage = (uint8_t*)mTrisPageTable[instance.meshletTrianglesPageIndex];
    
    // Fetch compacted meshlet index from task payload
    uint8_t localMeshletOffset = sharedPayload.meshletIndices[groupId.x];
    uint globalMeshletIdx = sharedPayload.groupMeshletOffset + localMeshletOffset;
    
    MeshletDraw meshlet = meshletDrawPage[globalMeshletIdx];
    float4x4 viewProj = mul(ubo.proj, ubo.view);

    uint hash = globalMeshletIdx * 747796405u + 2891336453u;
    float3 color = float3(
        (hash & 0xFF) / 255.0,
        ((hash >> 8) & 0xFF) / 255.0,
        ((hash >> 16) & 0xFF) / 255.0
    );

    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

    // Process vertices
    for(uint i = gtid.x; i < meshlet.vertexCount; i += MESH_SHADER_DISPATCH_X)
    {
        uint localVertexIndex = meshletVerticesPage[meshlet.vertexOffset + i];
        Vertex v = vertexPage[meshlet.globalVertexOffset + localVertexIndex];

        float4 localPos = float4(v.pos, 1.0);

        if(instance.boneMatrixOffset != 0xFFFFFFFF)
        {
            float4x4* boneMatrices = (float4x4*)pushConstant.boneMatrixReference;
            uint baseOffset = instance.boneMatrixOffset;

            // Blend bone matrices using vertex weights
            float4x4 skinTransform =
                boneMatrices[baseOffset + v.boneIDs.x] * v.boneWeights.x +
                boneMatrices[baseOffset + v.boneIDs.y] * v.boneWeights.y +
                boneMatrices[baseOffset + v.boneIDs.z] * v.boneWeights.z +
                boneMatrices[baseOffset + v.boneIDs.w] * v.boneWeights.w;

            localPos = mul(skinTransform, localPos);
        }

        float4 worldPos = mul(modelMatrix, localPos);
        float4 clipPos = mul(viewProj, worldPos);

        vertices[i].pos = clipPos;
        vertices[i].texCooord = v.texCoord;
        vertices[i].color = float4(color, 1.0);
        vertices[i].entityID = instance.entityID;
        vertices[i].drawID = sharedPayload.drawID;
    }

    // Process triangles
    for(uint i = gtid.x; i < meshlet.triangleCount; i += MESH_SHADER_DISPATCH_X)
    {
        uint triOffset = meshlet.triangleOffset + i * 3;

        uint idx0 = meshletTrianglePage[triOffset + 0];
        uint idx1 = meshletTrianglePage[triOffset + 1];
        uint idx2 = meshletTrianglePage[triOffset + 2];

        triangles[i] = uint3(idx0, idx1, idx2);
    }
}

struct FSOutput
{
    float4 color : SV_TARGET0; // Final color output (RGBA)
    int entityID : SV_TARGET1;
};

[shader("fragment")]
FSOutput fragMain(VSOutput vertIn, uniform PushConstantMeshlets pushConstant)
{
    FSOutput output;

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    InstanceData* instances = (InstanceData*)pushConstant.instanceReference;
    InstanceData instance = instances[vertIn.drawID];

    Texture2D texHandle = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.albedoTextureIndex, 0));
    SamplerState sampHandle = SamplerState.Handle(uint2(0, 0));
    float4 img = texHandle.Sample(sampHandle, vertIn.texCooord);

    if(instance.albedoTextureIndex == -1)
        output.color = instance.albedoColor * vertIn.color;
    else
        output.color = img * instance.albedoColor/* * vertIn.color */;

    output.entityID = vertIn.entityID;

    return output;
}