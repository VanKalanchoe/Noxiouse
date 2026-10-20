#include "shaderIO.h"

// Temporal anti-aliasing (Renderer::setTAAEnabled; DLSS / DLAA replaces it where it runs). The camera is jittered by a sub-pixel
// R2 sequence each frame; each pixel blends its new sample into the history reprojected with the motion vectors:
//   - the motion of the nearest surface of the 3 x 3 neighbourhood (edges keep the foreground's motion, no ghost outline);
//   - the history read with a 5-tap Catmull-Rom filter (sharp under motion, unlike bilinear);
//   - clipped to the box of the neighbourhood's colors (variance clipping in YCoCg): what the history holds that the pixel
//     cannot show anymore (disocclusion, moving light) is pulled to the nearest plausible color instead of ghosting;
//   - blended in a tonemapped space (1 / (1 + luma)) so a few very bright samples do not flicker.

float3 rgb_to_ycocg(float3 c)
{
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 ycocg_to_rgb(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

float luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Catmull-Rom over 4 x 4 texels with 5 bilinear taps (the corners dropped: they weigh almost nothing).
float3 sample_history_catmull_rom(Texture2D<float4> history, SamplerState linearClamp, float2 uv, float2 size)
{
    const float2 position = uv * size;
    const float2 center = floor(position - 0.5) + 0.5;
    const float2 f = position - center;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 offset12 = w2 / w12;
    const float2 texel = 1.0 / size;
    const float2 uv0 = (center - 1.0) * texel;
    const float2 uv3 = (center + 2.0) * texel;
    const float2 uv12 = (center + offset12) * texel;

    float3 result = float3(0.0);
    float weight = 0.0;
    float w;
    w = w12.x * w0.y; result += history.SampleLevel(linearClamp, float2(uv12.x, uv0.y), 0).rgb * w; weight += w;
    w = w0.x * w12.y; result += history.SampleLevel(linearClamp, float2(uv0.x, uv12.y), 0).rgb * w; weight += w;
    w = w12.x * w12.y; result += history.SampleLevel(linearClamp, float2(uv12.x, uv12.y), 0).rgb * w; weight += w;
    w = w3.x * w12.y; result += history.SampleLevel(linearClamp, float2(uv3.x, uv12.y), 0).rgb * w; weight += w;
    w = w12.x * w3.y; result += history.SampleLevel(linearClamp, float2(uv12.x, uv3.y), 0).rgb * w; weight += w;
    return max(result / max(weight, 1e-5), 0.0);
}

// Pulls a color towards the box's centre until it is inside the box.
float3 clip_to_box(float3 color, float3 boxMin, float3 boxMax)
{
    const float3 center = 0.5 * (boxMax + boxMin);
    const float3 extent = 0.5 * (boxMax - boxMin) + 1e-5;
    const float3 offset = color - center;
    const float3 units = abs(offset / extent);
    const float largest = max(units.x, max(units.y, units.z));
    return largest > 1.0 ? center + offset / largest : color;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID, uniform PushConstantTAA pc)
{
    if (any(tid.xy >= uint2(pc.width, pc.height)))
        return;

    Texture2D<float4> color = Texture2D<float4>.Handle(uint2(pc.colorIndex, 0));
    Texture2D<float> depth = Texture2D<float>.Handle(uint2(pc.depthIndex, 0));
    Texture2D<float2> velocity = Texture2D<float2>.Handle(uint2(pc.velocityIndex, 0));
    RWTexture2D<float4> output = RWTexture2D<float4>.Handle(uint2(pc.outputIndex, 0));

    const int2 pixel = int2(tid.xy);
    const int2 last = int2(pc.width, pc.height) - 1;

    // The neighbourhood: its color moments and its nearest surface (reverse Z: the largest depth).
    float3 m1 = float3(0.0);
    float3 m2 = float3(0.0);
    float nearestDepth = -1.0;
    int2 nearestPixel = pixel;
    float3 current = float3(0.0);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const int2 p = clamp(pixel + int2(x, y), int2(0, 0), last);
            float3 c = max(color.Load(int3(p, 0)).rgb, 0.0);
            if (any(isnan(c)) || any(isinf(c)))
                c = float3(0.0);
            c = min(c, 60000.0);
            if (x == 0 && y == 0)
                current = c;
            const float3 ycocg = rgb_to_ycocg(c / (1.0 + luma(c)));
            m1 += ycocg;
            m2 += ycocg * ycocg;
            const float d = depth.Load(int3(p, 0));
            if (d > nearestDepth)
            {
                nearestDepth = d;
                nearestPixel = p;
            }
        }
    }

    const float2 size = float2(pc.width, pc.height);
    const float2 uv = (float2(pixel) + 0.5) / size;
    const float2 previousUV = uv + velocity.Load(int3(nearestPixel, 0));
    const bool offscreen = any(previousUV < 0.0) || any(previousUV > 1.0);
    if (pc.reset != 0 || pc.historyIndex == 0xFFFFFFFF || offscreen)
    {
        output[tid.xy] = float4(current, 1.0);
        return;
    }

    Texture2D<float4> history = Texture2D<float4>.Handle(uint2(pc.historyIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    float3 previous = sample_history_catmull_rom(history, linearClamp, previousUV, size);
    if (any(isnan(previous)) || any(isinf(previous)))
        previous = current;

    // Variance clipping in the tonemapped YCoCg space (gamma 1.25).
    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float3 boxMin = mean - 1.25 * sigma;
    const float3 boxMax = mean + 1.25 * sigma;
    const float3 currentMapped = current / (1.0 + luma(current));
    float3 previousMapped = previous / (1.0 + luma(previous));
    previousMapped = ycocg_to_rgb(clip_to_box(rgb_to_ycocg(previousMapped), boxMin, boxMax));

    // More of the new sample where the pixel moved a lot (the history is resampled, softer each time).
    const float2 motionPixels = abs(previousUV - uv) * size;
    const float blend = lerp(saturate(pc.blend), 0.25, saturate(max(motionPixels.x, motionPixels.y) / 32.0));
    const float3 resultMapped = lerp(previousMapped, currentMapped, blend);
    const float3 result = resultMapped / max(1.0 - luma(resultMapped), 1e-4);
    output[tid.xy] = float4(max(result, 0.0), 1.0);
}
