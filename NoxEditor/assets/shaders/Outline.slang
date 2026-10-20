#include "shaderIO.h"

struct OutlinePayload
{
    uint dummy;
};
groupshared OutlinePayload gPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gPayload);
}

struct MSOutput
{
    float4 pos : SV_Position;
    float2 texCoord : TEXCOORD0;
};

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

    [shader("mesh")]
    [outputtopology("triangle")]
    [numthreads(3, 1, 1)]
    void meshMain(
        out indices uint3 triangles[1],
        out vertices MSOutput vertices[3],
        uint threadID : SV_GroupIndex,
        uniform PushConstantOutline pushConstant
    )
    {
        SetMeshOutputCounts(3, 1);

        if (threadID < 3)
        {
            vertices[threadID].pos = float4(positions[threadID], 0.0, 1.0);
            vertices[threadID].texCoord = uvs[threadID];
        }

        if (threadID == 0)
        {
            triangles[0] = uint3(0, 1, 2);
        }
    }

bool IsSelected(
    int entityID,
    int* selectedIDs,
    uint selectedCount)
{
    for (uint i = 0; i < selectedCount; ++i)
    {
        if (selectedIDs[i] == entityID)
            return true;
    }

    return false;
}

 [shader("fragment")]
float4 fragMain(
    MSOutput input,
    uniform PushConstantOutline pushConstant
) : SV_Target
{
    UniformBufferObject* ubo =
        (UniformBufferObject*)pushConstant.matrixReference;

    if (pushConstant.selectedEntityCount == 0)
        discard;

    int2 maskPixel = int2(input.pos.xy);

    // The selection mask (SelectionMask.slang): drawn without the camera jitter, so the outline stays still with DLSS on.
    // An outline pixel is outside the selection with a selected pixel within `radius`.
    if (pushConstant.selectionMaskTextureIndex != 0xFFFFFFFF)
    {
        Texture2D<float> maskTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.selectionMaskTextureIndex, 0));
        uint maskWidth, maskHeight;
        maskTex.GetDimensions(maskWidth, maskHeight);

        if (maskTex.Load(int3(maskPixel, 0)) > 0.5)
            discard;

        const int maskRadius = 2;
        for (int y = -maskRadius; y <= maskRadius; ++y)
        {
            for (int x = -maskRadius; x <= maskRadius; ++x)
            {
                int2 p = maskPixel + int2(x, y);
                if (p.x < 0 || p.y < 0 || p.x >= int(maskWidth) || p.y >= int(maskHeight))
                    continue;
                if (maskTex.Load(int3(p, 0)) > 0.5)
                    return float4(1.0, 0.5, 0.0, 1.0);
            }
        }
        discard;
    }

    Texture2D<int4> entityTex =
        Texture2D<int4>.Handle(
            uint2(
                ubo->imageHeapIndexOffset +
                (pushConstant.entityTextureIndex != 0xFFFFFFFF ? pushConstant.entityTextureIndex : ubo->entityTextureIndex),
                0
            )
        );

    int* selectedIDs =
        (int*)pushConstant.selectedEntityIDsReference;

    int2 pixelPos = int2(input.pos.xy);

    uint width, height;
    entityTex.GetDimensions(width, height);

    int centerID =
        entityTex.Load(int3(pixelPos, 0)).r;

    if (IsSelected(
        centerID,
        selectedIDs,
        pushConstant.selectedEntityCount))
    {
        discard;
    }

    const int radius = 2;

    bool neighborSelected = false;

    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            if (x == 0 && y == 0)
                continue;

            int2 p = pixelPos + int2(x, y);

            if (p.x < 0 || p.y < 0 ||
                p.x >= int(width) || p.y >= int(height))
            {
                continue;
            }

            int neighborID =
                entityTex.Load(int3(p, 0)).r;

            if (IsSelected(
                neighborID,
                selectedIDs,
                pushConstant.selectedEntityCount))
            {
                neighborSelected = true;
                break;
            }
        }

        if (neighborSelected)
            break;
    }

    if (!neighborSelected)
        discard;

    return float4(1.0, 0.5, 0.0, 1.0);
}