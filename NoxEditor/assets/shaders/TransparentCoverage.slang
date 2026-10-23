#pragma once

#include "shaderIO.h"

// Transparent surfaces drawn over DLSS-RR's output (MESHLET_AFTER_UPSCALE, Renderer::addTransparentAfterUpscalePass): the share of
// this display pixel where they are in front of the scene, from its depth at 2x2 samples per pixel (Renderer::addCoverageDepthPass)
// -- the depth test, by share. RR draws a thin bar in front of a window soft; a pane tested against one depth per pixel was all
// there or not at all along its edges, and the bar crawled as the camera moved. 1 when not drawn after RR.
float after_upscale_coverage(UniformBufferObject* ubo, PushConstantMeshlets pushConstant, float4 position)
{
    // Its own depth at each sample, along its plane: a surface lying on another is not cut in half by the offsets.
    const float depthDx = ddx(position.z);
    const float depthDy = ddy(position.z);
    if ((pushConstant.meshletCulling & MESHLET_AFTER_UPSCALE) == 0 || pushConstant.coverageDepthTextureIndex == 0xFFFFFFFF)
        return 1.0;

    Texture2D<float> sceneDepth = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.coverageDepthTextureIndex, 0));
    const int2 base = int2(position.xy) * 2;
    float covered = 0.0;
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 2; ++x)
        {
            const float2 offset = float2(x, y) * 0.5 - 0.25;
            const float depth = position.z + depthDx * offset.x + depthDy * offset.y;
            if (depth >= sceneDepth.Load(int3(base + int2(x, y), 0))) // reverse Z, the hardware test's GreaterOrEqual
                covered += 0.25;
        }
    }
    return covered;
}
