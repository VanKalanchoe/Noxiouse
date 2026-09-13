#include "shaderIO.h"

// Writes mip 0 of the local-light PDF texture that ReSTIRDIPresample.slang's RTXDI_PresampleLocalLights
// call samples (Rtxdi/LightSampling/PresamplingFunctions.hlsli): one thread per texel of a square,
// power-of-two texture. Each light's importance is written at its Z-curve-encoded texel position
// (RTXDI_LinearIndexToZCurve) so RTXDI_SamplePdfMipmap can recover the light index later via
// RTXDI_ZCurveToLinearIndex. Texels beyond numLocalLights are written 0 (dead weight, never selected).

#include "Rtxdi/Utils/Math.hlsli"

float LightImportance(LightData light)
{
    return max(dot(light.color.rgb, float3(0.2126, 0.7152, 0.0722)) * light.color.a, 1e-6);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compMain(uint3 dispatchThreadID : SV_DispatchThreadID, uniform PushConstantReSTIRDIWriteLightPDF pushConstant)
{
    if (dispatchThreadID.x >= pushConstant.pdfTextureSize || dispatchThreadID.y >= pushConstant.pdfTextureSize)
        return;

    RWTexture2D<float> pdfMip0 = RWTexture2D<float>.Handle(uint2(pushConstant.pdfTextureStorageIndex, 0));

    uint linearIndex = RTXDI_ZCurveToLinearIndex(dispatchThreadID.xy);

    float pdf = 0.0;
    if (linearIndex < pushConstant.numLocalLights)
    {
        LightData* lightData = (LightData*)pushConstant.lightDataReference;
        pdf = LightImportance(lightData[pushConstant.firstLocalLightIndex + linearIndex]);
    }

    pdfMip0[dispatchThreadID.xy] = pdf;
}
