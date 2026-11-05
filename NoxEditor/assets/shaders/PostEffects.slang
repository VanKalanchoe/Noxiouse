#include "shaderIO.h"

// The post effects before the tonemapper (PP1, Settings > Post Effects; Renderer::addPostEffectPasses), on the HDR image at the
// display resolution:
//   dofMain            -- depth of field: a thin lens (focus distance, f-number, the focal length the camera's field of view gives on
//                         a 24 mm-tall sensor) gives every pixel a circle of confusion; each pixel gathers the neighbours whose circles
//                         reach it (a background neighbour no blurrier than the pixel itself: no halo of blur over a sharp foreground).
//   motionBlurMain     -- along each pixel's motion vector (the G-buffer's, uv offset to last frame) scaled by the shutter.
//   bloomDownsampleMain / bloomUpsampleMain -- Call of Duty's bloom (Jimenez 2014): 13-tap downsamples (the first with Karis'
//                         average against fireflies), then 3x3 tent upsamples added back up the chain; the tonemap pass lerps it in.

static const float GOLDEN_ANGLE = 2.399963229728653;

float2 vogel(uint i, uint count, float rotation)
{
    const float r = sqrt((float(i) + 0.5) / float(count));
    const float theta = float(i) * GOLDEN_ANGLE + rotation;
    return r * float2(cos(theta), sin(theta));
}

float luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// --- Depth of field ---------------------------------------------------------------------------------------------------------------
// params: x focus distance (world units), y aperture diameter x focal length / sensor height (world units, the CoC scale), z the
// largest radius (pixels), w output height (pixels). params2.x: near plane (the depth's view distance = near / depth).
float dof_coc(float viewDistance, float4 params)
{
    // Thin lens: diameter on the sensor A f |z - S| / (z (S - f)), here already over the sensor height; f << S.
    const float coc = params.y * abs(viewDistance - params.x) / max(viewDistance * params.x, 1e-6);
    return min(0.5 * coc * params.w, params.z); // radius in pixels
}

[shader("compute")]
[numthreads(8, 8, 1)]
void dofMain(uint3 id : SV_DispatchThreadID, uniform PushConstantPostEffect pushConstant)
{
    if (id.x >= pushConstant.width || id.y >= pushConstant.height)
        return;
    Texture2D source = Texture2D.Handle(uint2(pushConstant.sourceIndex, 0));
    Texture2D<float> depth = Texture2D<float>.Handle(uint2(pushConstant.auxIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    SamplerState pointClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_NEAREST_CLAMP, 0));
    const float2 size = float2(pushConstant.width, pushConstant.height);
    const float2 uv = (float2(id.xy) + 0.5) / size;
    const float nearPlane = pushConstant.params2.x;
    const float centerDepth = depth.SampleLevel(pointClamp, uv, 0.0);
    const float centerZ = centerDepth > 0.0 ? nearPlane / centerDepth : 1e30; // the sky: infinitely far
    const float centerCoc = dof_coc(centerZ, pushConstant.params);
    const float3 centerColor = source.SampleLevel(pointClamp, uv, 0.0).rgb;
    RWTexture2D<float4> output = RWTexture2D<float4>.Handle(uint2(pushConstant.outputIndex, 0));
    const float maxRadius = pushConstant.params.z;
    if (maxRadius < 0.5)
    {
        output[id.xy] = float4(centerColor, 1.0);
        return;
    }

    const float rotation = frac(52.9829189 * frac(0.06711056 * float(id.x) + 0.00583715 * float(id.y))) * 6.283185307;
    const uint taps = 48;
    float3 sum = centerColor / max(3.14159 * max(centerCoc, 0.5) * max(centerCoc, 0.5), 1.0);
    float weightSum = 1.0 / max(3.14159 * max(centerCoc, 0.5) * max(centerCoc, 0.5), 1.0);
    for (uint i = 0; i < taps; ++i)
    {
        const float2 offset = vogel(i, taps, rotation) * maxRadius;
        const float distance = length(offset);
        const float2 tapUV = uv + offset / size;
        const float tapDepth = depth.SampleLevel(pointClamp, tapUV, 0.0);
        const float tapZ = tapDepth > 0.0 ? nearPlane / tapDepth : 1e30;
        float tapCoc = dof_coc(tapZ, pushConstant.params);
        if (tapZ > centerZ)
            tapCoc = min(tapCoc, centerCoc); // a farther neighbour blurs no more than this pixel (no background over a sharp edge)
        // Reaches this pixel (soft edge over a pixel), its light spread over its circle.
        const float reach = saturate(tapCoc - distance + 1.0);
        const float w = reach / max(3.14159 * max(tapCoc, 0.5) * max(tapCoc, 0.5), 1.0);
        sum += source.SampleLevel(linearClamp, tapUV, 0.0).rgb * w;
        weightSum += w;
    }
    output[id.xy] = float4(sum / max(weightSum, 1e-6), 1.0);
}

// --- Motion blur ------------------------------------------------------------------------------------------------------------------
// params: x shutter (fraction of the frame), y the longest blur (pixels), z samples.
[shader("compute")]
[numthreads(8, 8, 1)]
void motionBlurMain(uint3 id : SV_DispatchThreadID, uniform PushConstantPostEffect pushConstant)
{
    if (id.x >= pushConstant.width || id.y >= pushConstant.height)
        return;
    Texture2D source = Texture2D.Handle(uint2(pushConstant.sourceIndex, 0));
    Texture2D<float2> motion = Texture2D<float2>.Handle(uint2(pushConstant.auxIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    SamplerState pointClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_NEAREST_CLAMP, 0));
    const float2 size = float2(pushConstant.width, pushConstant.height);
    const float2 uv = (float2(id.xy) + 0.5) / size;
    // The motion over the open shutter (last frame's position is uv + motion), at most the longest blur.
    float2 blur = motion.SampleLevel(pointClamp, uv, 0.0) * pushConstant.params.x;
    const float pixels = length(blur * size);
    if (pixels > pushConstant.params.y)
        blur *= pushConstant.params.y / pixels;
    RWTexture2D<float4> output = RWTexture2D<float4>.Handle(uint2(pushConstant.outputIndex, 0));
    if (pixels < 0.5)
    {
        output[id.xy] = float4(source.SampleLevel(pointClamp, uv, 0.0).rgb, 1.0);
        return;
    }
    const uint samples = clamp(uint(pushConstant.params.z), 4u, 32u);
    const float jitter = frac(52.9829189 * frac(0.06711056 * float(id.x) + 0.00583715 * float(id.y)));
    float3 sum = float3(0.0);
    for (uint i = 0; i < samples; ++i)
    {
        const float t = (float(i) + jitter) / float(samples) - 0.5;
        sum += source.SampleLevel(linearClamp, uv + blur * t, 0.0).rgb;
    }
    output[id.xy] = float4(sum / float(samples), 1.0);
}

// --- Bloom ------------------------------------------------------------------------------------------------------------------------
// The 13 taps (Jimenez 2014) around uv in the source's texels: four overlapping 2x2 boxes and the centre one, weighted 0.5 / 0.125 x 4.
float3 bloom_downsample13(Texture2D source, SamplerState s, float2 uv, float2 texel, float mip, bool karis)
{
    const float3 a = source.SampleLevel(s, uv + texel * float2(-2, -2), mip).rgb;
    const float3 b = source.SampleLevel(s, uv + texel * float2(0, -2), mip).rgb;
    const float3 c = source.SampleLevel(s, uv + texel * float2(2, -2), mip).rgb;
    const float3 d = source.SampleLevel(s, uv + texel * float2(-1, -1), mip).rgb;
    const float3 e = source.SampleLevel(s, uv + texel * float2(1, -1), mip).rgb;
    const float3 f = source.SampleLevel(s, uv + texel * float2(-2, 0), mip).rgb;
    const float3 g = source.SampleLevel(s, uv, mip).rgb;
    const float3 h = source.SampleLevel(s, uv + texel * float2(2, 0), mip).rgb;
    const float3 i = source.SampleLevel(s, uv + texel * float2(-1, 1), mip).rgb;
    const float3 j = source.SampleLevel(s, uv + texel * float2(1, 1), mip).rgb;
    const float3 k = source.SampleLevel(s, uv + texel * float2(-2, 2), mip).rgb;
    const float3 l = source.SampleLevel(s, uv + texel * float2(0, 2), mip).rgb;
    const float3 m = source.SampleLevel(s, uv + texel * float2(2, 2), mip).rgb;
    const float3 box0 = (d + e + i + j) * 0.25;
    const float3 box1 = (a + b + f + g) * 0.25;
    const float3 box2 = (b + c + g + h) * 0.25;
    const float3 box3 = (f + g + k + l) * 0.25;
    const float3 box4 = (g + h + l + m) * 0.25;
    if (!karis)
        return box0 * 0.5 + (box1 + box2 + box3 + box4) * 0.125;
    // Karis' average: each box weighted by 1 / (1 + its luminance), a single blazing pixel no longer floods its surroundings.
    const float w0 = 0.5 / (1.0 + luminance(box0));
    const float w1 = 0.125 / (1.0 + luminance(box1));
    const float w2 = 0.125 / (1.0 + luminance(box2));
    const float w3 = 0.125 / (1.0 + luminance(box3));
    const float w4 = 0.125 / (1.0 + luminance(box4));
    return (box0 * w0 + box1 * w1 + box2 * w2 + box3 * w3 + box4 * w4) / (w0 + w1 + w2 + w3 + w4);
}

// params: x 1 for the first downsample (from the image, Karis' average), y the source's mip, zw the source mip's texel size.
[shader("compute")]
[numthreads(8, 8, 1)]
void bloomDownsampleMain(uint3 id : SV_DispatchThreadID, uniform PushConstantPostEffect pushConstant)
{
    if (id.x >= pushConstant.width || id.y >= pushConstant.height)
        return;
    Texture2D source = Texture2D.Handle(uint2(pushConstant.sourceIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    const float2 uv = (float2(id.xy) + 0.5) / float2(pushConstant.width, pushConstant.height);
    const float3 color = bloom_downsample13(source, linearClamp, uv, pushConstant.params.zw, pushConstant.params.y, pushConstant.params.x > 0.5);
    RWTexture2D<float4> output = RWTexture2D<float4>.Handle(uint2(pushConstant.outputIndex, 0));
    output[id.xy] = float4(max(color, float3(0.0)), 1.0);
}

// The down chain's mip (source, sourceMip) averaged with the up chain's next mip (aux, sourceMip + 1) through a 3x3 tent of params.x texels
// (the bloom's radius); params.y 1: the last mip (copied). params.zw: the next mip's texel size.
[shader("compute")]
[numthreads(8, 8, 1)]
void bloomUpsampleMain(uint3 id : SV_DispatchThreadID, uniform PushConstantPostEffect pushConstant)
{
    if (id.x >= pushConstant.width || id.y >= pushConstant.height)
        return;
    Texture2D down = Texture2D.Handle(uint2(pushConstant.sourceIndex, 0));
    Texture2D up = Texture2D.Handle(uint2(pushConstant.auxIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    const float2 uv = (float2(id.xy) + 0.5) / float2(pushConstant.width, pushConstant.height);
    const float mip = float(pushConstant.sourceMip);
    const float3 own = down.SampleLevel(linearClamp, uv, mip).rgb;
    RWTexture2D<float4> output = RWTexture2D<float4>.Handle(uint2(pushConstant.outputIndex, 0));
    if (pushConstant.params.y > 0.5) // the chain's last mip: nothing below to add
    {
        output[id.xy] = float4(own, 1.0);
        return;
    }
    const float2 t = pushConstant.params.zw * pushConstant.params.x;
    float3 tent = up.SampleLevel(linearClamp, uv, mip + 1.0).rgb * 4.0;
    tent += (up.SampleLevel(linearClamp, uv + float2(-t.x, 0), mip + 1.0).rgb + up.SampleLevel(linearClamp, uv + float2(t.x, 0), mip + 1.0).rgb +
             up.SampleLevel(linearClamp, uv + float2(0, -t.y), mip + 1.0).rgb + up.SampleLevel(linearClamp, uv + float2(0, t.y), mip + 1.0).rgb) * 2.0;
    tent += up.SampleLevel(linearClamp, uv + float2(-t.x, -t.y), mip + 1.0).rgb + up.SampleLevel(linearClamp, uv + float2(t.x, -t.y), mip + 1.0).rgb +
            up.SampleLevel(linearClamp, uv + float2(-t.x, t.y), mip + 1.0).rgb + up.SampleLevel(linearClamp, uv + float2(t.x, t.y), mip + 1.0).rgb;
    // Halved at each step: mip k weighs 1 / 2^(k+1), the chain sums to the image's energy (no brighter than what it blurs).
    output[id.xy] = float4(0.5 * (own + tent / 16.0), 1.0);
}
