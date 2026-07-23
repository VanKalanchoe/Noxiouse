#include "shaderIO.h"

struct InstanceBuffer
{
    float4x4 model;
};

struct PushConstant 
{
    UniformBufferObject* uboBuffer;
    Vertex* vertexBuffer;
    InstanceBuffer* instanceBuffer;
};
[[vk::push_constant]]
ConstantBuffer<PushConstant> pushConst;

struct VSOutput
{
    float4 pos : SV_Position;
    float3 color;
    float2 fragTexCoord;
};

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) 
{
    VSOutput output;
    UniformBufferObject ubo = *pushConst.uboBuffer;
    Vertex vertex = pushConst.vertexBuffer[vertexID];
    InstanceBuffer instance = pushConst.instanceBuffer[instanceID];
    output.pos = mul(ubo.proj, mul(ubo.view, mul(instance.model, float4(vertex.pos, 1.0))));
    output.color = vertex.color;
    output.fragTexCoord = vertex.texCoord;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_TARGET 
{
    UniformBufferObject ubo = *pushConst.uboBuffer;
   
    Texture2D texHandle = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + 0, 0));
    SamplerState sampHandle = SamplerState.Handle(uint2(0, 0));

    float4 img = texHandle.Sample(sampHandle, vertIn.fragTexCoord);
    return img;
}