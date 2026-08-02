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

    uint32_t* meshletVerticesPtr = (uint32_t*)pushConstant.meshletVerticesReference;
    Vertex* vertexPtr = (Vertex*)pushConstant.verticesReference;
    uint8_t* meshletTrianglePtr = (uint8_t*)pushConstant.meshletTrianglesReference;

    MeshletDraw* meshletDrawPtr = (MeshletDraw*)pushConstant.meshletDrawsReference;

    // Fetch compacted meshlet index from task payload
    uint8_t localMeshletOffset = sharedPayload.meshletIndices[groupId.x];
    uint globalMeshletIdx = sharedPayload.groupMeshletOffset + localMeshletOffset;
    
    MeshletDraw meshlet = meshletDrawPtr[globalMeshletIdx];
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
        uint localVertexIndex = meshletVerticesPtr[meshlet.vertexOffset + i];
        Vertex v = vertexPtr[meshlet.globalVertexOffset + localVertexIndex];

        float4 worldPos = mul(modelMatrix, float4(v.pos, 1.0));
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

        uint idx0 = meshletTrianglePtr[triOffset + 0];
        uint idx1 = meshletTrianglePtr[triOffset + 1];
        uint idx2 = meshletTrianglePtr[triOffset + 2];

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