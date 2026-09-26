#include "shaderIO.h"

// The editor's world grid, a port of Godot's (editor/scene/3d/node_3d_editor_plugin.cpp, Node3DEditor::_init_grid and its grid shader):
// real one pixel wide lines on y = 0, (2 x 200 + 1) in each direction around the camera, every steps-th one a primary line. The CPU
// only works out the few numbers Godot works out (Renderer2D::DrawGrid); the line positions and colors are made here, one mesh
// shader group per line. The fade with distance and viewing angle is Godot's fragment shader.

struct GridPayLoad
{
    uint unused;
};
groupshared GridPayLoad dummyPayLoad;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain(uniform PushConstantGrid pushConstant)
{
    // All lines of both directions, plus the two axes through the origin.
    DispatchMesh(2 * (2 * pushConstant.gridSize + 1) + 2, 1, 1, dummyPayLoad);
}

struct VSOutput
{
    float4 pos : SV_Position;
    float4 color;
    float3 world;
};

[shader("mesh")]
[outputtopology("line")]
[numthreads(1, 1, 1)]
void meshMain
(
    out indices uint2 lines[1], out vertices VSOutput vertices[2],
    uint3 DispatchThreadID : SV_DispatchThreadID,
    uniform PushConstantGrid pushConstant
)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

    // Godot's colors are (0.56, 0.5) and (0.38, 0.5) on its dark background; on a lit floor they vanish, so ours are brighter and stronger.
    const float4 primaryColor = float4(0.78, 0.78, 0.80, 0.75);
    const float4 secondaryColor = float4(0.62, 0.62, 0.64, 0.55);

    const int gridSize = pushConstant.gridSize;
    const int steps = pushConstant.steps;
    const float smallStep = pushConstant.smallStep;
    const uint id = DispatchThreadID.x;
    const uint gridLineCount = 2 * (2 * gridSize + 1);

    float3 from;
    float3 to;
    float4 color;
    bool draw = true;

    if (id < gridLineCount)
    {
        const int i = (int)(id / 2) - gridSize;
        const bool alongZ = (id % 2) == 0; // a line at x = position, running along z; otherwise at z = position, running along x

        // Godot: primary lines get the primary color blended toward the secondary one by the division level's decimals, the others fade out.
        if (i % steps == 0)
            color = lerp(primaryColor, secondaryColor, pushConstant.decimals);
        else
        {
            color = secondaryColor;
            color.a *= 1.0 - 0.6 * pushConstant.decimals; // Godot fades them out completely; we keep 40% so the small grid never disappears
        }

        const float extent = gridSize * smallStep;
        if (alongZ)
        {
            const float x = pushConstant.centerX + i * smallStep;
            from = float3(x, 0.0, pushConstant.centerZ - extent);
            to = float3(x, 0.0, pushConstant.centerZ + extent);
            draw = abs(x) > smallStep * 1e-3; // the origin has its own colored line
        }
        else
        {
            const float z = pushConstant.centerZ + i * smallStep;
            from = float3(pushConstant.centerX - extent, 0.0, z);
            to = float3(pushConstant.centerX + extent, 0.0, z);
            draw = abs(z) > smallStep * 1e-3;
        }
    }
    else
    {
        // The axes: X red, Z blue, as long as the grid reaches.
        const float reach = pushConstant.gridFadeSize * 2.0;
        if (id == gridLineCount)
        {
            from = float3(pushConstant.centerX - reach, 0.0, 0.0);
            to = float3(pushConstant.centerX + reach, 0.0, 0.0);
            color = float4(0.9, 0.25, 0.25, 0.9);
        }
        else
        {
            from = float3(0.0, 0.0, pushConstant.centerZ - reach);
            to = float3(0.0, 0.0, pushConstant.centerZ + reach);
            color = float4(0.25, 0.45, 0.95, 0.9);
        }
    }

    SetMeshOutputCounts(draw ? 2 : 0, draw ? 1 : 0);
    if (!draw)
        return;

    vertices[0].pos = mul(ubo.nonJitteredProj, mul(ubo.view, float4(from, 1.0)));
    vertices[0].color = color;
    vertices[0].world = from;
    vertices[1].pos = mul(ubo.nonJitteredProj, mul(ubo.view, float4(to, 1.0)));
    vertices[1].color = color;
    vertices[1].world = to;
    lines[0] = uint2(0, 1);
}

struct FSOutput
{
    float4 color : SV_TARGET0;
    int entityID : SV_TARGET1;
};

[shader("fragment")]
FSOutput fragMain(VSOutput vertIn, uniform PushConstantGrid pushConstant)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    const float3 camera = ubo.cameraWorldPos.xyz;

    // Godot's shader: fade with the angle between the view and the plane, and with the distance to the camera (on the plane).
    const float3 viewDirection = normalize(camera - vertIn.world);
    float angleFade = smoothstep(0.05, 0.2, abs(viewDirection.y));

    const float3 cameraOnPlane = float3(camera.x, 0.0, camera.z);
    float distanceFade = 1.0 - distance(vertIn.world, cameraOnPlane) / pushConstant.gridFadeSize;
    distanceFade = smoothstep(0.02, 0.3, distanceFade);

    FSOutput output;
    output.color = float4(vertIn.color.rgb, vertIn.color.a * distanceFade * angleFade);
    output.entityID = -1;
    return output;
}
