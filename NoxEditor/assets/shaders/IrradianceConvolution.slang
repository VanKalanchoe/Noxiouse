#include "shaderIO.h"

static const float PI = 3.14159265359;

// Maps cubemap face coordinates [-1, 1] to a 3D world direction vector (Vulkan spec)
float3 getCubemapVector(uint face, float2 uv)
    {
        float x = uv.x;
        float y = uv.y;

        float3 dir = float3(0.0);

        switch (face)
        {
            case 0: dir = float3( 1.0,  -y,  -x); break; // +X (Right)
            case 1: dir = float3(-1.0,  -y,   x); break; // -X (Left)
            case 2: dir = float3(   x, 1.0,   y); break; // +Y (Top/Sky)
            case 3: dir = float3(   x,-1.0,  -y); break; // -Y (Bottom/Floor)
            case 4: dir = float3(   x,  -y, 1.0); break; // +Z (Back)
            case 5: dir = float3(  -x,  -y, -1.0); break; // -Z (Front)
        }

        return normalize(dir);
    }

[shader("compute")]
[numthreads(16, 16, 1)]
void compMain(
uint3 dispatchThreadID : SV_DispatchThreadID,
uniform PushConstantEquirect pushConstant
)
{
if (dispatchThreadID.x >= pushConstant.cubemapSize || dispatchThreadID.y >= pushConstant.cubemapSize)
return;

uint faceIndex = dispatchThreadID.z;

// Normalize pixel position to [-1.0, 1.0] range
float2 pixelCenter = float2(dispatchThreadID.xy) + 0.5;
float2 st = (pixelCenter / float(pushConstant.cubemapSize)) * 2.0 - 1.0;

// 1. Calculate normal vector (N) from the cube face direction
float3 N = getCubemapVector(faceIndex, st);

// 2. Build World-to-Tangent basis vectors around Normal N
float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0);
float3 right = normalize(cross(up, N));
up = normalize(cross(N, right));

// 3. Obtain bindless handles for resources
TextureCube envCubemap = TextureCube.Handle(uint2(pushConstant.hdrTextureIndex, 0));
SamplerState linearSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));
RWTexture2DArray<float4> irradianceOutput = RWTexture2DArray<float4>.Handle(uint2(pushConstant.cubemapStorageIndex, 0));

// 4. Riemann sum integral over the hemisphere
float3 irradiance = float3(0.0);
float sampleDelta = 0.05;
uint nrSamples = 0;

for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
{
    for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
    {
        // Spherical to Cartesian (in Tangent Space)
        float3 tangentSample = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
        
        // Tangent Space to World Space
        float3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;

        // Sample input HDR environment cubemap using direction vector
        irradiance += envCubemap.SampleLevel(linearSampler, sampleVec, 0.0).rgb * cos(theta) * sin(theta);
        nrSamples++;
    }
}

irradiance = PI * irradiance * (1.0 / float(nrSamples));

// 5. Write color output to the corresponding layer face in the irradiance cubemap storage array
uint3 writePos = uint3(dispatchThreadID.x, dispatchThreadID.y, faceIndex);
irradianceOutput[writePos] = float4(irradiance, 1.0);
}