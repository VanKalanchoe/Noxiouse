#include "shaderIO.h"

struct InstanceBuffer
{
    float4x4 model;
};

struct PushConstant 
{
    uint64_t uboBuffer;
    uint64_t vertexBuffer;
    uint64_t instanceBuffer;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float3 color : TEXCOORD0;
    float2 fragTexCoord : TEXCOORD1;
};

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID, uniform PushConstant pushConst) 
{
    VSOutput output;
    UniformBufferObject ubo = loadUbo(pushConst.uboBuffer);
    Vertex vertex = BufferRef<Vertex>(pushConst.vertexBuffer)[vertexID];
    InstanceBuffer instance = BufferRef<InstanceBuffer>(pushConst.instanceBuffer)[instanceID];
    output.pos = mul(ubo.proj, mul(ubo.view, mul(instance.model, float4(vertex.pos, 1.0))));
    output.color = float3(1.0, 1.0, 1.0); // Set the color to white
    output.fragTexCoord = vertex.texCoord;
    return output;
}

struct FSOutput
{
    float4 color : SV_TARGET0; // Final color output (RGBA)
    int entityID : SV_TARGET1;
};

[shader("fragment")]
FSOutput fragMain(VSOutput vertIn, uniform PushConstant pushConst) : SV_TARGET 
{
    FSOutput output;
    
    UniformBufferObject ubo = loadUbo(pushConst.uboBuffer);
   
    Texture2D texHandle = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + 0, 0));
    SamplerState sampHandle = SamplerState.Handle(uint2(0, 0));

    float4 img = texHandle.Sample(sampHandle, vertIn.fragTexCoord);

    output.color = img;
    output.entityID = -1; // Set entityID to -1 as a placeholder

    return output;
}