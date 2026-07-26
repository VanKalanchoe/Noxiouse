#include "shaderIO.h"

struct CirclePayLoad
{
    uint circleID;
};

[shader("task")]
[numthreads(64, 1, 1)]
void taskMain(
    uint3 gid : SV_GroupID, uint3 groupThreadID: SV_GroupThreadID, uniform PushConstantCircle pushConstant
)
{
    // globalThreadID = thread index in group + offset from group
    uint globalThreadID = gid.x;

    // safety check
    if (globalThreadID >= pushConstant.numOfElements) return;

    // create payload for this circle
    CirclePayLoad payload;
    payload.circleID = globalThreadID;

    // dispatch exactly 1 mesh per circle
    DispatchMesh(1, 1, 1, payload);
}

static const float4 positions[4] = 
{
    float4(-0.5, -0.5, 0.0, 1.0),  // bottom-left
    float4( 0.5, -0.5, 0.0, 1.0),  // bottom-right
    float4( 0.5,  0.5, 0.0, 1.0),  // top-right
    float4(-0.5,  0.5, 0.0, 1.0)   // top-left
};

struct VSOutput
{
    float4 pos : SV_Position;
    float4 color;
    float thickness;
    float3 localPosition;
    float fade;
    int entityID;
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void meshMain
(
    in payload CirclePayLoad sharedPayload,
    out indices uint3 triangles[2], out vertices VSOutput vertices[4],
    uniform PushConstantCircle pushConstant
)
{
    uint circleID = sharedPayload.circleID;
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    CircleData* circleData = (CircleData*)pushConstant.circleDataReference;
    CircleData circle = circleData[circleID];

	SetMeshOutputCounts(4, 2); // 4 vertices, 2 triangles

	for (uint i = 0; i < 4; i++) 
    {
		vertices[i].pos = mul(ubo.proj, mul(ubo.view, mul(circle.worldPosition, positions[i])));
        vertices[i].color = circle.color;
        vertices[i].thickness = circle.thickness;
        float2 lp = positions[i].xy * 2.0;
        vertices[i].localPosition = float3(lp, 0.0);
        vertices[i].fade = circle.fade;
        vertices[i].entityID = circle.entityID;
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
FSOutput fragMain(VSOutput vertIn)
{
    FSOutput output;

    // Calculate distance and fill circle with white
    float distance = 1.0 - length(vertIn.localPosition);
    float circle = smoothstep(0.0, vertIn.fade, distance);
    circle *= smoothstep(vertIn.thickness + vertIn.fade, vertIn.thickness, distance);

    if(circle == 0.0)
        discard;

    // Set output color
    float4 color = vertIn.color;
    color.a *= circle; 

    output.color = color;

    output.entityID = vertIn.entityID;

    return output;
}