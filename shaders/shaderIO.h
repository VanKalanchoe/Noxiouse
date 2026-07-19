
#ifndef HOST_DEVICE_H
#define HOST_DEVICE_H

#ifdef __SLANG__
typealias vec2 = float2;
typealias vec3 = float3;
typealias vec4 = float4;
typealias mat4 = float4x4;
#define STATIC_CONST static const
#else
#define STATIC_CONST const
#endif

struct UniformBufferObject 
{
    mat4 model;
    mat4 view;
    mat4 proj;
    uint samplerIndex;
    uint imageHeapIndexOffset;
    uint finalImageIndex;
};

struct Vertex
{
    vec3 pos;
    vec3 color;
    vec2 texCoord;
};

#endif  // HOST_DEVICE_H