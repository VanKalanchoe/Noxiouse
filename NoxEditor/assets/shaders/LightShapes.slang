// The rect lights' emitting faces, drawn into the lit scene (Forward 3D, after the opaque unlit meshes): a rect light is not
// geometry, so without this it lit the room but was not there to see. One mesh workgroup per light of the light buffer; anything
// but an area light outputs nothing. Depth-tested against the scene, not written. The front shows the light's radiance (its
// intensity is a radiance: no conversion), the back is a dark panel. Fogged like every forward surface.
#include "shaderIO.h"
#include "HeightFog.slang"

struct LightShapesPayload
{
    uint dummy;
};
groupshared LightShapesPayload gLightShapesPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain(uniform PushConstantLightShapes pushConstant)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    DispatchMesh(max(ubo->lightCount, 1u), 1, 1, gLightShapesPayload);
}

struct LightShapeVertex
{
    float4 pos : SV_Position;
    float3 worldPos;
    nointerpolation uint lightIndex;
};

static const float2 LightShapeCorners[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0) };
static const uint DiscSegments = 24;

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void meshMain(out indices uint3 triangles[DiscSegments], out vertices LightShapeVertex vertices[DiscSegments + 1], uint threadID : SV_GroupIndex,
              uint3 groupID : SV_GroupID, uniform PushConstantLightShapes pushConstant)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    LightData* lights = (LightData*)ubo->lightDataReference;
    const uint lightIndex = groupID.x;
    const uint type = lightIndex < ubo->lightCount ? (uint)lights[lightIndex].position.w : 0u;
    // A rect: 4 corners, 2 triangles. A disc: its centre and DiscSegments points of its rim, a triangle per segment.
    const uint vertexCount = type == 3u ? 4u : (type == 4u ? DiscSegments + 1 : 0u);
    const uint triangleCount = type == 3u ? 2u : (type == 4u ? DiscSegments : 0u);
    SetMeshOutputCounts(vertexCount, triangleCount);
    if (vertexCount == 0)
        return;

    const LightData light = lights[lightIndex];
    if (threadID < vertexCount)
    {
        float2 corner = float2(0.0);
        if (type == 3u)
            corner = LightShapeCorners[threadID];
        else if (threadID > 0)
        {
            const float angle = 6.28318530718 * float(threadID - 1) / float(DiscSegments);
            corner = float2(cos(angle), sin(angle));
        }
        const float3 worldPos = light.position.xyz + rect_light_axis_x(light) * corner.x + rect_light_axis_y(light) * corner.y;
        vertices[threadID].pos = mul(ubo->proj, mul(ubo->view, float4(worldPos, 1.0)));
        vertices[threadID].worldPos = worldPos;
        vertices[threadID].lightIndex = lightIndex;
    }
    if (threadID < triangleCount)
    {
        if (type == 3u)
            triangles[threadID] = threadID == 0 ? uint3(0, 1, 2) : uint3(0, 2, 3);
        else
            triangles[threadID] = uint3(0, threadID + 1, threadID + 1 < DiscSegments ? threadID + 2 : 1);
    }
}

struct LightShapeOutput
{
    float4 color : SV_Target0;
    int entityID : SV_Target1;
};

[shader("fragment")]
LightShapeOutput fragMain(LightShapeVertex input, uniform PushConstantLightShapes pushConstant)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    LightData* lights = (LightData*)ubo->lightDataReference;
    const LightData light = lights[input.lightIndex];

    const float3 toCamera = ubo->cameraWorldPos.xyz - input.worldPos;
    const bool front = dot(light.direction.xyz, toCamera) > 0.0;
    float3 color = front ? light.color.rgb * light.color.w : float3(0.02);
    const float4 fog = sceneFogTerms(ubo, input.worldPos);
    color = color * fog.a + fog.rgb;

    LightShapeOutput output;
    output.color = float4(color, 1.0);
    output.entityID = -1;
    return output;
}
