#include "PathTracerSignals.slang"

// After NRD: the path tracer's pixel is its primary emission plus the denoised diffuse and specular light, modulated back
// with the same G-buffer factors the path tracer divided by (PathTracerSignals.slang).
[shader("compute")]
[numthreads(8, 8, 1)]
void compositeMain(uint3 dispatchThreadID : SV_DispatchThreadID, uniform PushConstantPTComposite pushConstant)
{
    if (dispatchThreadID.x >= pushConstant.width || dispatchThreadID.y >= pushConstant.height)
        return;

    int2 pixel = int2(dispatchThreadID.xy);
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

    float3 diffFactor, specFactor;
    float roughness;
    PT_SurfaceFactors(ubo, pixel, pushConstant.depthTextureIndex, pushConstant.gbufferAlbedoIndex, pushConstant.gbufferNormalIndex,
                      pushConstant.gbufferMaterialIndex, diffFactor, specFactor, roughness);

    Texture2D emissionTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.emissionTextureIndex, 0));
    Texture2D diffuseTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.diffuseTextureIndex, 0));
    Texture2D specularTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.specularTextureIndex, 0));
    float3 color = emissionTex.Load(int3(pixel, 0)).rgb +
                   NRD_UnpackSignal(pushConstant.denoiserMode, diffuseTex.Load(int3(pixel, 0))) * diffFactor +
                   NRD_UnpackSignal(pushConstant.denoiserMode, specularTex.Load(int3(pixel, 0))) * specFactor;

    RWTexture2D<float4> output = RWTexture2D<float4>.Handle(uint2(pushConstant.outputStorageIndex, 0));
    output[pixel] = float4(color, 1.0);
}
