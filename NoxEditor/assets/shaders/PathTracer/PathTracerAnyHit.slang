#pragma once

#include "PathPayload.slang"
#include "PathTracerBridge.slang"

[[vk::push_constant]]
ConstantBuffer<PushConstantPathTracer> pushConstant;

[shader("anyhit")]
void anyHitMain(inout PathPayload payload : SV_RayPayload, in BuiltInTriangleIntersectionAttributes attrib)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

    uint seed = payload.packed[0].w ^ (pushConstant.frameIndex * 19937u) ^ (InstanceID() * 747796405u) ^
        (PrimitiveIndex() * 2891336453u) ^ payload.packed[4].w;
    if (!Bridge::AlphaTest(InstanceID(), PrimitiveIndex(), attrib.barycentrics, ubo, Bridge::rand(seed)))
    {
        IgnoreHit();
    }
}
