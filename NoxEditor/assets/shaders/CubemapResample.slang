#include "shaderIO.h"

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

// Re-samples a source cubemap (any format/resolution the sampler can read, including a
// block-compressed one like BC6H -- sampling decodes it in hardware, unlike a blit) into a
// smaller, uncompressed, mip-chainable destination. A block-compressed cubemap loaded straight
// from a DDS file has no mips of its own (RTXPT's environment DDS assets ship with mipmapcount=1)
// and can't have them blitted in afterward (compressed formats generally don't support
// VK_FORMAT_FEATURE_BLIT_SRC/DST_BIT), so IrradianceConvolution.slang and PrefilterEnv.slang's
// "read the environment through an already-blurred mip so a coarse sample grid doesn't need to be
// enormous" trick silently read the full-resolution, unfiltered image at every sample instead --
// a handful of discrete samples over a sharp 4K HDRI aliases badly, which is exactly the
// faceted/"disco ball" look this pass exists to fix.
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

    float2 pixelCenter = float2(dispatchThreadID.xy) + 0.5;
    float2 st = (pixelCenter / float(pushConstant.cubemapSize)) * 2.0 - 1.0;

    float3 dir = getCubemapVector(faceIndex, st);

    TextureCube sourceCubemap = TextureCube.Handle(uint2(pushConstant.hdrTextureIndex, 0));
    SamplerState linearSampler = SamplerState.Handle(uint2(0, 0));
    RWTexture2DArray<float4> cubemapOutput = RWTexture2DArray<float4>.Handle(uint2(pushConstant.cubemapStorageIndex, 0));

    float4 srcColor = sourceCubemap.SampleLevel(linearSampler, dir, 0.0);

    uint3 writePos = uint3(dispatchThreadID.x, dispatchThreadID.y, faceIndex);
    cubemapOutput[writePos] = float4(srcColor.rgb, 1.0);
}
