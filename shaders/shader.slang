/* struct VSInput 
{
    float3 inPosition;
    float3 inColor;
    float2 inTexCoord;
};
 */
struct UniformBuffer 
{
    float4x4 model;
    float4x4 view;
    float4x4 proj;
    uint samplerIndex;
    uint imageHeapIndexOffset;
};

struct VertexBuffer
{
    float3 inPosition;
    float3 inColor;
    float2 inTexCoord;
};

struct PushConstant 
{
    UniformBuffer* uboBuffer;
    VertexBuffer* vertexBuffer;
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
VSOutput vertMain(uint vertexID : SV_VertexID) 
{
    VSOutput output;
    UniformBuffer ubo = *pushConst.uboBuffer;
    VertexBuffer vertex = pushConst.vertexBuffer[vertexID];
    output.pos = mul(ubo.proj, mul(ubo.view, mul(ubo.model, float4(vertex.inPosition, 1.0))));
    output.color = vertex.inColor;
    output.fragTexCoord = vertex.inTexCoord;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_TARGET 
{
    UniformBuffer ubo = *pushConst.uboBuffer;
   
    Texture2D texHandle = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset, 0));
    SamplerState sampHandle = SamplerState.Handle(uint2(0, 0));

    float4 img = texHandle.Sample(sampHandle, vertIn.fragTexCoord);
    return img;
}