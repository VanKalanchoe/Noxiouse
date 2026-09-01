#include "shaderIO.h"

static const float2 invAtan = float2(0.15915494309, 0.31830988618); // 1 / (2 * PI), 1 / PI

// Converts a 3D unit direction vector to 2D equirectangular UV coordinates
float2 sampleSphericalMap(float3 v)
{
    // -Z is Forward, y+up
    float phi = atan2(-v.x, v.z);
    
    // Clamp v.y to avoid NaN from floating point precision issues at extremes
    float theta = asin(clamp(v.y, -1.0, 1.0));

    float2 uv = float2(phi, theta) * invAtan + 0.5;
    
    // Invert V coordinate to match STB's top-left origin (V=0 is Top/Sky, V=1 is Bottom/Floor)
    uv.y = 1.0 - uv.y;
    return uv;
}

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

    uint faceIndex = dispatchThreadID.z; // SV_DispatchThreadID.z: faces 0 to 5

    // Normalize pixel position to [-1.0, 1.0] range
    float2 pixelCenter = float2(dispatchThreadID.xy) + 0.5;
    float2 st = (pixelCenter / float(pushConstant.cubemapSize)) * 2.0 - 1.0;

    // 1. Calculate 3D direction vector from center of cube face
    float3 dir = getCubemapVector(faceIndex, st);

    // 2. Convert 3D direction vector into 2D equirectangular UV coordinates
    float2 equirectUV = sampleSphericalMap(dir);

    // 3. Obtain bindless handles for resources
    Texture2D hdrTexture = Texture2D.Handle(uint2(pushConstant.hdrTextureIndex, 0));
    SamplerState linearSampler = SamplerState.Handle(uint2(0, 0));
    RWTexture2DArray<float4> cubemapOutput = RWTexture2DArray<float4>.Handle(uint2(pushConstant.cubemapStorageIndex, 0));

    // 4. Sample the input HDR panorama using linear filtering
    float4 hdrColor = hdrTexture.SampleLevel(linearSampler, equirectUV, 0.0);

    // 5. Write color output to the corresponding layer face in the cubemap storage array
    uint3 writePos = uint3(dispatchThreadID.x, dispatchThreadID.y, faceIndex);
    cubemapOutput[writePos] = float4(hdrColor.rgb, 1.0);
}