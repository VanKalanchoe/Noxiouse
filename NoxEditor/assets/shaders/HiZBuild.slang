#include "shaderIO.h"

// Depth pyramid for occlusion culling (§5.6.4). Reverse-Z: near is 1, far is 0, so a level keeps the smallest (farthest)
// depth of the texels it covers. Odd sizes include the extra row/column, so a level never claims an area is closer than
// what was drawn there.

[shader("compute")]
[numthreads(8, 8, 1)]
void buildMain(uint3 dispatchThreadID: SV_DispatchThreadID, uniform PushConstantHiZBuild pushConstant)
{
    if (dispatchThreadID.x >= pushConstant.width || dispatchThreadID.y >= pushConstant.height)
        return;

    int2 target = int2(dispatchThreadID.xy);
    int2 source = target * 2;
    int sourceMip = int(pushConstant.sourceMip);

    float farthest;
    if (pushConstant.sourceIsDepth != 0)
    {
        Texture2D<float> depth = Texture2D<float>.Handle(uint2(pushConstant.sourceTextureIndex, 0));
        uint2 depthSize;
        uint depthMips;
        depth.GetDimensions(0, depthSize.x, depthSize.y, depthMips);
        int2 maxTexel = int2(depthSize) - 1;

        farthest = depth.Load(int3(min(source, maxTexel), 0));
        farthest = min(farthest, depth.Load(int3(min(source + int2(1, 0), maxTexel), 0)));
        farthest = min(farthest, depth.Load(int3(min(source + int2(0, 1), maxTexel), 0)));
        farthest = min(farthest, depth.Load(int3(min(source + int2(1, 1), maxTexel), 0)));

        // Odd source size: the level also covers the leftover row/column.
        if (source.x + 2 == maxTexel.x)
            farthest = min(farthest, min(depth.Load(int3(maxTexel.x, min(source.y, maxTexel.y), 0)), depth.Load(int3(maxTexel.x, min(source.y + 1, maxTexel.y), 0))));
        if (source.y + 2 == maxTexel.y)
            farthest = min(farthest, min(depth.Load(int3(min(source.x, maxTexel.x), maxTexel.y, 0)), depth.Load(int3(min(source.x + 1, maxTexel.x), maxTexel.y, 0))));
    }
    else
    {
        Texture2D<float> pyramid = Texture2D<float>.Handle(uint2(pushConstant.sourceTextureIndex, 0));
        uint2 levelSize;
        uint levelMips;
        pyramid.GetDimensions(uint(sourceMip), levelSize.x, levelSize.y, levelMips);
        int2 maxTexel = int2(levelSize) - 1;

        farthest = pyramid.Load(int3(min(source, maxTexel), sourceMip));
        farthest = min(farthest, pyramid.Load(int3(min(source + int2(1, 0), maxTexel), sourceMip)));
        farthest = min(farthest, pyramid.Load(int3(min(source + int2(0, 1), maxTexel), sourceMip)));
        farthest = min(farthest, pyramid.Load(int3(min(source + int2(1, 1), maxTexel), sourceMip)));

        if (source.x + 2 == maxTexel.x)
            farthest = min(farthest, min(pyramid.Load(int3(maxTexel.x, min(source.y, maxTexel.y), sourceMip)), pyramid.Load(int3(maxTexel.x, min(source.y + 1, maxTexel.y), sourceMip))));
        if (source.y + 2 == maxTexel.y)
            farthest = min(farthest, min(pyramid.Load(int3(min(source.x, maxTexel.x), maxTexel.y, sourceMip)), pyramid.Load(int3(min(source.x + 1, maxTexel.x), maxTexel.y, sourceMip))));
    }

    RWTexture2D<float> output = RWTexture2D<float>.Handle(uint2(pushConstant.outputStorageIndex, 0));
    output[target] = farthest;
}
