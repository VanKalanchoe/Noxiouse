#pragma once

#include "PathPayload.slang"
#include "PathTracerBridge.slang"
#include "PathTracerPushConstants.slang"

// Whether an alpha-tested surface (alpha mode Mask or Blend: the TLAS's hit group 1) stops the path's ray at this candidate hit --
// its alpha test with a random number per path, vertex, frame and triangle (blended surfaces stochastically, unbiased). One function
// for both ways of tracing: the ray tracing pipeline's any hit (anyHitMain below) and the compute path tracer's ray query
// (PathTracerCompute.slang). `payload`: the path's packed state, whose id and vertex seed the number.
bool pathTracerAlphaAccepts(PathPayload payload, uint instanceID, uint primitiveIndex, float2 barycentrics, UniformBufferObject ubo)
{
    uint seed = payload.packed[0].w ^ (pushConstant.frameIndex * 19937u) ^ (instanceID * 747796405u) ^
        (primitiveIndex * 2891336453u) ^ payload.packed[4].w;
    return Bridge::AlphaTest(instanceID, primitiveIndex, barycentrics, ubo, Bridge::rand(seed));
}

[shader("anyhit")]
void anyHitMain(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attrib)
{
    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    if (!pathTracerAlphaAccepts(payload, InstanceID(), PrimitiveIndex(), attrib.barycentrics, ubo))
    {
        IgnoreHit();
    }
}
