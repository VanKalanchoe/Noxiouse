#include "shaderIO.h"

struct VSOutput
{
    float4 pos : SV_Position;
    float4 color;
    float2 fragTexCoord;
    uint32_t materialIndex;
    int entityID;
};

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID, uniform PushConstantQuad pushConstant) 
{
    VSOutput output;

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    Vertex* vertexPTR = (Vertex*)pushConstant.vertexReference;
    Vertex vertex = vertexPTR[vertexID];
    QuadData* quadData = (QuadData*)pushConstant.quadDataReference;
    QuadData quad = quadData[instanceID];

    output.pos = mul(ubo.proj, mul(ubo.view, mul(quad.modelMatrix, float4(vertex.pos, 1.0))));
    output.color = quad.color /* * float4(vertex.color, 1) */;
    output.fragTexCoord = vertex.texCoord;
    output.materialIndex = quad.materialIndex;
    output.entityID = quad.entityID;

    return output;
}

struct FSOutput
{
    float4 color : SV_TARGET0; // Final color output (RGBA)
    int entityID : SV_TARGET1;
};

[shader("fragment")]
FSOutput fragMain(VSOutput vertIn, uniform PushConstantQuad pushConstant)
{
    FSOutput output;

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
   
    Texture2D texHandle = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + vertIn.materialIndex, 0));
    SamplerState sampHandle = SamplerState.Handle(uint2(0, 0));
    float4 img = texHandle.Sample(sampHandle, vertIn.fragTexCoord);

    output.color = img * vertIn.color;

    output.entityID = vertIn.entityID;

    return output;
}