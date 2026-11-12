#include "shaderIO.h"

// Depth pyramids. For occlusion culling (§5.6.4): reverse-Z (near is 1, far is 0), a level keeps the smallest (farthest) depth
// of the texels it covers, so it never claims an area is closer than what was drawn there. For the screen-space reflections'
// Hi-Z tracing (keepClosest, SSRCommon.slang): the largest (closest), so a tile the ray passes in front of holds nothing it could
// hit. Odd sizes include the extra row/column either way.

float hiz_reduce(float a, float b, uint keepClosest)
{
    return keepClosest != 0 ? max(a, b) : min(a, b);
}

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
        farthest = hiz_reduce(farthest, depth.Load(int3(min(source + int2(1, 0), maxTexel), 0)), pushConstant.keepClosest);
        farthest = hiz_reduce(farthest, depth.Load(int3(min(source + int2(0, 1), maxTexel), 0)), pushConstant.keepClosest);
        farthest = hiz_reduce(farthest, depth.Load(int3(min(source + int2(1, 1), maxTexel), 0)), pushConstant.keepClosest);

        // Odd source size: the level also covers the leftover row/column.
        if (source.x + 2 == maxTexel.x)
            farthest = hiz_reduce(farthest, hiz_reduce(depth.Load(int3(maxTexel.x, min(source.y, maxTexel.y), 0)), depth.Load(int3(maxTexel.x, min(source.y + 1, maxTexel.y), 0)), pushConstant.keepClosest), pushConstant.keepClosest);
        if (source.y + 2 == maxTexel.y)
            farthest = hiz_reduce(farthest, hiz_reduce(depth.Load(int3(min(source.x, maxTexel.x), maxTexel.y, 0)), depth.Load(int3(min(source.x + 1, maxTexel.x), maxTexel.y, 0)), pushConstant.keepClosest), pushConstant.keepClosest);
    }
    else
    {
        Texture2D<float> pyramid = Texture2D<float>.Handle(uint2(pushConstant.sourceTextureIndex, 0));
        uint2 levelSize;
        uint levelMips;
        pyramid.GetDimensions(uint(sourceMip), levelSize.x, levelSize.y, levelMips);
        int2 maxTexel = int2(levelSize) - 1;

        farthest = pyramid.Load(int3(min(source, maxTexel), sourceMip));
        farthest = hiz_reduce(farthest, pyramid.Load(int3(min(source + int2(1, 0), maxTexel), sourceMip)), pushConstant.keepClosest);
        farthest = hiz_reduce(farthest, pyramid.Load(int3(min(source + int2(0, 1), maxTexel), sourceMip)), pushConstant.keepClosest);
        farthest = hiz_reduce(farthest, pyramid.Load(int3(min(source + int2(1, 1), maxTexel), sourceMip)), pushConstant.keepClosest);

        if (source.x + 2 == maxTexel.x)
            farthest = hiz_reduce(farthest, hiz_reduce(pyramid.Load(int3(maxTexel.x, min(source.y, maxTexel.y), sourceMip)), pyramid.Load(int3(maxTexel.x, min(source.y + 1, maxTexel.y), sourceMip)), pushConstant.keepClosest), pushConstant.keepClosest);
        if (source.y + 2 == maxTexel.y)
            farthest = hiz_reduce(farthest, hiz_reduce(pyramid.Load(int3(min(source.x, maxTexel.x), maxTexel.y, sourceMip)), pyramid.Load(int3(min(source.x + 1, maxTexel.x), maxTexel.y, sourceMip)), pushConstant.keepClosest), pushConstant.keepClosest);
    }

    RWTexture2D<float> output = RWTexture2D<float>.Handle(uint2(pushConstant.outputStorageIndex, 0));
    output[target] = farthest;
}
