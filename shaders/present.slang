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

struct InstanceBuffer
{
    float4x4 model;
};


struct PushConstant 
{
    UniformBuffer* uboBuffer;
    VertexBuffer* vertexBuffer;
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
VSOutput vertMain(uint vertexID : SV_VertexID) 
{
    static const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    static const float2 uvs[3] =
    {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0)
    };

    VSOutput output;
    output.pos = float4(positions[vertexID], 0.0, 1.0);
    output.fragTexCoord = uvs[vertexID];
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_TARGET 
{
    UniformBuffer ubo = *pushConst.uboBuffer;
   
    Texture2D texHandle = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + 1, 0));
    SamplerState sampHandle = SamplerState.Handle(uint2(0, 0));

    float4 img = texHandle.Sample(sampHandle, vertIn.fragTexCoord);
    return img;
}