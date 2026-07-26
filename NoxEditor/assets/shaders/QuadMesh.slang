#include "shaderIO.h"

struct QuadPayLoad
{
    uint quadID;
};
groupshared QuadPayLoad dummyPayLoad;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain(uniform PushConstantQuad pushConstant)
{
    DispatchMesh(pushConstant.numOfElements, 1, 1, dummyPayLoad);
}

static const float4 positions[4] = 
{
    float4(-0.5, -0.5, 0.0, 1.0),  // bottom-left
    float4( 0.5, -0.5, 0.0, 1.0),  // bottom-right
    float4( 0.5,  0.5, 0.0, 1.0),  // top-right
    float4(-0.5,  0.5, 0.0, 1.0)   // top-left
};

static const float2 uv[4] = 
{
    float2(0.0, 0.0), // bottom-left
    float2(1.0, 0.0), // bottom-right
    float2(1.0, 1.0), // top-right
    float2(0.0, 1.0)  // top-left
};

struct VSOutput
{
    float4 pos : SV_Position;
    float4 color;
    float2 texCooord;
    uint32_t materialIndex;
    int entityID;
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void meshMain
(
    out indices uint3 triangles[2], out vertices VSOutput vertices[4],
    uint3 DispatchThreadID : SV_DispatchThreadID,
    uniform PushConstantQuad pushConstant
)
{
    uint quadID = DispatchThreadID.x;
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    QuadData* quadData = (QuadData*)pushConstant.quadDataReference;
    QuadData quad = quadData[quadID];

	SetMeshOutputCounts(4, 2); // 4 vertices, 2 triangles

	for (uint i = 0; i < 4; i++) 
    {
		vertices[i].pos = mul(ubo.proj, mul(ubo.view, mul(quad.modelMatrix, positions[i])));
        vertices[i].color = quad.color;
        vertices[i].texCooord = uv[i];
        vertices[i].materialIndex = quad.materialIndex;
        vertices[i].entityID = quad.entityID;
	}

	triangles[0] = uint3(0, 1, 2);
    triangles[1] = uint3(0, 2, 3);
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
    float4 img = texHandle.Sample(sampHandle, vertIn.texCooord);

    output.color = img * vertIn.color;

    output.entityID = vertIn.entityID;

    return output;
}