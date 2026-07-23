#include "shaderIO.h"

const static int32_t TASK_SHADER_DISPATCH_X = 64;
const static int32_t MESH_SHADER_DISPATCH_X = 32;

struct QuadPayLoad
{
    uint quadID;
};

[shader("task")]
[numthreads(TASK_SHADER_DISPATCH_X, 1, 1)]
void taskMain(
    uint3 gid : SV_GroupID, uint3 groupThreadID: SV_GroupThreadID
)
{
    // globalThreadID = thread index in group + offset from group
    uint globalThreadID = gid.x;

    // create payload for this quad
    QuadPayLoad payload;
    payload.quadID = globalThreadID;

    // dispatch exactly 1 mesh per quad
    DispatchMesh(1, 1, 1, payload);
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
    float4 position : SV_Position;
    float2 texcoords : TEXCOORD0;
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void meshMain
(
    in payload QuadPayLoad sharedPayload,
    out indices uint3 triangles[2], out vertices VSOutput vertices[4],
    uniform PushConstantQuad pushConstant
)
{
    uint quadID = sharedPayload.quadID;
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

	SetMeshOutputCounts(4, 2); // 4 vertices, 2 triangles

	for (uint i = 0; i < 4; i++) 
    {
		vertices[i].position = mul(ubo.proj, mul(ubo.view, positions[i]));
        vertices[i].texcoords = uv[i];
	}

	triangles[0] = uint3(0, 1, 2);
    triangles[1] = uint3(0, 2, 3);
}

struct FSOutput
{
    float4 color : SV_TARGET0; // Final color output (RGBA)
};

[shader("fragment")]
FSOutput fragMain(VSOutput vertexInput)
{
    FSOutput output;

    output.color = float4(1.0, 0.0, 1.0, 1.0); // Default pink color

    return output;
}