struct VSInput 
{
    float3 inPosition;
    float3 inColor;
    float2 inTexCoord;
};

struct UniformBuffer 
{
    float4x4 model;
    float4x4 view;
    float4x4 proj;
    uint samplerIndex;
    uint imageHeapIndexOffset;
};

struct PushConstant 
{
    UniformBuffer* uboBuffer;
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
VSOutput vertMain(VSInput input) 
{
    VSOutput output;
    UniformBuffer ubo = *pushConst.uboBuffer;
    output.pos = mul(ubo.proj, mul(ubo.view, mul(ubo.model, float4(input.inPosition, 1.0))));
    output.color = input.inColor;
    output.fragTexCoord = input.inTexCoord;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_TARGET 
{
    UniformBuffer ubo = *pushConst.uboBuffer;
   
    DescriptorHandle<Texture2D> texHandle = (DescriptorHandle<Texture2D>)ubo.imageHeapIndexOffset;
    DescriptorHandle<SamplerState> sampHandle = (DescriptorHandle<SamplerState>)ubo.samplerIndex;

    float4 img = texHandle.Sample(sampHandle, vertIn.fragTexCoord);
    return img;
}

[shader("compute")]
[numthreads(256,1,1)]
void compMain(uint3 threadId : SV_DispatchThreadID)
{

}