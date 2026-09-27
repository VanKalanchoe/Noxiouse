#include "shaderIO.h"

// The editor's world grid, a port of Godot's (editor/scene/3d/node_3d_editor_plugin.cpp, Node3DEditor::_init_grid and its grid shader):
// real one pixel wide lines on a plane through the origin, (2 x 200 + 1) in each direction around the camera, every steps-th one a
// primary line. The CPU only works out the few numbers Godot works out (Renderer2D::DrawGrid); the line positions and colors are made
// here, one mesh shader group per line. The fade with distance and viewing angle is Godot's fragment shader. The plane is the ground
// (y = 0) in the perspective view and the plane the ortho view looks at (Top / Bottom: y = 0, Front / Back: z = 0, Left / Right: x = 0).

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

// The plane's normal is world axis c; a and b are the other two (Godot: a = (c + 1) % 3, b = (c + 2) % 3). A grid point is (u, v) in them.
float3 planePoint(int c, float u, float v)
{
    float3 point = float3(0.0, 0.0, 0.0);
    point[(c + 1) % 3] = u;
    point[(c + 2) % 3] = v;
    return point;
}

float3 axisColor(int axis)
{
    if (axis == 0)
        return float3(0.9, 0.25, 0.25);
    if (axis == 1)
        return float3(0.5, 0.85, 0.25);
    return float3(0.25, 0.45, 0.95);
}

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
    const int c = pushConstant.planeAxis;
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
        const bool alongB = (id % 2) == 0; // a line at a = position, running along b; otherwise at b = position, running along a

        // Godot: primary lines get the primary color blended toward the secondary one by the division level's decimals, the others fade out.
        if (i % steps == 0)
            color = lerp(primaryColor, secondaryColor, pushConstant.decimals);
        else
        {
            color = secondaryColor;
            color.a *= 1.0 - 0.6 * pushConstant.decimals; // Godot fades them out completely; we keep 40% so the small grid never disappears
        }

        const float extent = gridSize * smallStep;
        if (alongB)
        {
            const float position = pushConstant.centerA + i * smallStep;
            from = planePoint(c, position, pushConstant.centerB - extent);
            to = planePoint(c, position, pushConstant.centerB + extent);
            draw = abs(position) > smallStep * 1e-3; // the origin has its own colored line
        }
        else
        {
            const float position = pushConstant.centerB + i * smallStep;
            from = planePoint(c, pushConstant.centerA - extent, position);
            to = planePoint(c, pushConstant.centerA + extent, position);
            draw = abs(position) > smallStep * 1e-3;
        }
    }
    else
    {
        // The axes through the origin, as long as the grid reaches, colored by the world axis they run along.
        const float reach = pushConstant.gridFadeSize * 2.0;
        if (id == gridLineCount)
        {
            from = planePoint(c, pushConstant.centerA - reach, 0.0);
            to = planePoint(c, pushConstant.centerA + reach, 0.0);
            color = float4(axisColor((c + 1) % 3), 0.9);
        }
        else
        {
            from = planePoint(c, 0.0, pushConstant.centerB - reach);
            to = planePoint(c, 0.0, pushConstant.centerB + reach);
            color = float4(axisColor((c + 2) % 3), 0.9);
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
    const int c = pushConstant.planeAxis;

    // Godot's shader: fade with the angle between the view and the plane, and with the distance to the camera (on the plane).
    const float3 viewDirection = normalize(camera - vertIn.world);
    float angleFade = smoothstep(0.05, 0.2, abs(viewDirection[c]));

    float3 cameraOnPlane = camera;
    cameraOnPlane[c] = 0.0;
    float distanceFade = 1.0 - distance(vertIn.world, cameraOnPlane) / pushConstant.gridFadeSize;
    distanceFade = smoothstep(0.02, 0.3, distanceFade);

    FSOutput output;
    output.color = float4(vertIn.color.rgb, vertIn.color.a * distanceFade * angleFade);
    output.entityID = -1;
    return output;
}
