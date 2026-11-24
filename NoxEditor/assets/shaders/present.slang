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
VSOutput vertMain(uint vertexID : SV_VertexID) 
{
    VSOutput output;
    output.pos = noxFullscreenTriangle(vertexID, output.fragTexCoord);
    output.color = float3(1.0, 1.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn, uniform PushConstant pushConst) : SV_TARGET 
{
    UniformBufferObject ubo = loadUbo(pushConst.uboBuffer);
   
    Texture2D texHandle = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + ubo.finalImageIndex, 0));
    SamplerState sampHandle = SamplerState.Handle(uint2(0, 0));

    float4 img = texHandle.Sample(sampHandle, vertIn.fragTexCoord);
    return img;
}