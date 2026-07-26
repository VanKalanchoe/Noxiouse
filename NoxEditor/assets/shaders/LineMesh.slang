#include "shaderIO.h"

struct LinePayLoad
{
    uint lineID;
};

[shader("task")]
[numthreads(64, 1, 1)]
void taskMain(
    uint3 gid : SV_GroupID, uint3 groupThreadID: SV_GroupThreadID, uniform PushConstantLine pushConstant
)
{
    // globalThreadID = thread index in group + offset from group
    uint globalThreadID = gid.x;

    // safety check
    if (globalThreadID >= pushConstant.numOfElements) return;

    // create payload for this line
    LinePayLoad payload;
    payload.lineID = globalThreadID;

    // dispatch exactly 1 mesh per line
    DispatchMesh(1, 1, 1, payload);
}

struct VSOutput
{
    float4 pos : SV_Position;
    float4 color;
    int entityID;
};

[shader("mesh")]
[outputtopology("line")]
[numthreads(1, 1, 1)]
void meshMain
(
    in payload LinePayLoad sharedPayload,
    out indices uint2 lines[1], out vertices VSOutput vertices[2],
    uniform PushConstantLine pushConstant
)
{
    uint lineID = sharedPayload.lineID;
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    LineData* lineData = (LineData*)pushConstant.lineDataReference;
    LineData line = lineData[lineID];

	SetMeshOutputCounts(2, 1); // 2 vertices, 1 line

	// Vertex 0
    vertices[0].pos = mul(ubo.proj, mul(ubo.view, float4(line.p0, 1.0)));
    vertices[0].color = line.color;
    vertices[0].entityID = line.entityID;

    // Vertex 1
    vertices[1].pos = mul(ubo.proj, mul(ubo.view, float4(line.p1, 1.0)));
    vertices[1].color = line.color;
    vertices[1].entityID = line.entityID;

    // Line uses first 2 vertices
    lines[0] = uint2(0, 1); // third index ignored
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

    output.color = vertIn.color;

    output.entityID = vertIn.entityID;

    return output;
}