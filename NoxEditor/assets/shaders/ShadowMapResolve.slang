// The sun's cascaded shadow maps resolved into the screen's shadow mask (RS1; Renderer::addShadowMapPasses): the RT shadow mask's
// format and meaning (ShadowMask.slang: R the penumbra NRD reads, G the visibility the deferred lighting reads without NRD), so the
// deferred lighting shades the sun the same way whichever made it. Per G-buffer pixel, as ShadowMask.slang: a double-sided surface
// lit from behind is looked up from the side the sun is on; a surface facing away from the sun is in its own shadow.
#include "shaderIO.h"
#include "ShadowMaps.slang"

struct ResolvePayload
{
    uint dummy;
};
groupshared ResolvePayload gPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gPayload);
}

struct MSOutput
{
    float4 pos : SV_Position;
};

static const float2 positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(3, 1, 1)]
void meshMain(out indices uint3 triangles[1], out vertices MSOutput vertices[3], uint threadID : SV_GroupIndex)
{
    SetMeshOutputCounts(3, 1);
    if (threadID < 3)
        vertices[threadID].pos = float4(positions[threadID], 0.0, 1.0);
    if (threadID == 0)
        triangles[0] = uint3(0, 1, 2);
}

static const float NRD_FP16_MAX = 65504.0;

[shader("fragment")]
float2 fragMain(MSOutput input, uniform PushConstantShadowMapResolve pushConstant) : SV_Target0
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    GpuShadowCascades* cascades = (GpuShadowCascades*)pushConstant.cascadesReference;
    const int2 pixelCoord = int2(input.pos.xy);

    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    const float rawDepth = depthTex.Load(int3(pixelCoord, 0)).r;
    if (rawDepth <= 0.0 || ubo->shadowMaskLightIndex >= ubo->lightCount) // the sky (reverse Z), or no sun
        return float2(NRD_FP16_MAX, 1.0);

    const float2 uv = (float2(pixelCoord) + 0.5) / pushConstant.viewportSize;
    const float4 worldPosH = mul(pushConstant.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, rawDepth, 1.0));
    const float3 worldPos = worldPosH.xyz / worldPosH.w;

    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferNormalIndex, 0));
    const float3 normalSample = normalTex.Load(int3(pixelCoord, 0)).rgb;
    const float lenSqN = dot(normalSample, normalSample);
    float3 normal = lenSqN > 1e-6 ? normalSample * rsqrt(lenSqN) : float3(0.0, 1.0, 0.0);

    const float3 L = cascades->lightDirection.xyz;
    Texture2D materialTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferMaterialIndex, 0));
    if (gbuffer_double_sided(materialTex.Load(int3(pixelCoord, 0)).a) && dot(normal, L) < 0.0)
        normal = -normal;
    if (dot(normal, L) <= 0.0)
        return float2(0.0, 0.0);

    // Interleaved gradient noise (Jimenez 2014), moving with the frame: the kernel's rotation, averaged by the temporal upscale.
    const float2 p = float2(pixelCoord) + float(pushConstant.frameIndex % 64u) * float2(5.588238, 3.45367);
    const float noise = frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
    return float2(NRD_FP16_MAX, sun_shadow_map(ubo, cascades, worldPos, normal, noise));
}
