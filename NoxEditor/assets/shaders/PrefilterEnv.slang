#include "shaderIO.h"

static const float PI = 3.14159265359;

struct PrefilterPushConstant
{
uint32_t envTextureIndex;
uint32_t cubemapStorageIndex;
uint32_t cubemapSize;
float roughness;
};

// Based on http://byteblacksmith.com/improvements-to-the-canonical-one-liner-glsl-rand-for-opengl-es-2-0/
float random(float2 co)
{
float a = 12.9898;
float b = 78.233;
float c = 43758.5453;
float dt = dot(co.xy, float2(a, b));
float sn = fmod(dt, 3.14);
return frac(sin(sn) * c);
}

float2 Hammersley(uint i, uint N)
{
// Radical inverse based on http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
uint bits = (i << 16u) | (i >> 16u);
bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
float rdi = float(bits) * 2.3283064365386963e-10;
return float2(float(i) / float(N), rdi);
}

// Based on http://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_slides.pdf
float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness)
{
float alpha = roughness * roughness;
float phi = 2.0 * PI * Xi.x + random(N.xz) * 0.1;
float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (alpha * alpha - 1.0) * Xi.y));
float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
float3 H = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
float3 tangentX = normalize(cross(up, N));
float3 tangentY = normalize(cross(N, tangentX));

return normalize(tangentX * H.x + tangentY * H.y + N * H.z);
}

float D_GGX(float dotNH, float roughness)
{
float alpha = roughness * roughness;
float alpha2 = alpha * alpha;
float denom = dotNH * dotNH * (alpha2 - 1.0) + 1.0;
return alpha2 / (PI * denom * denom);
}

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
uniform PrefilterPushConstant pushConstant
)
{
if (dispatchThreadID.x >= pushConstant.cubemapSize || dispatchThreadID.y >= pushConstant.cubemapSize)
return;

uint faceIndex = dispatchThreadID.z;
float2 pixelCenter = float2(dispatchThreadID.xy) + 0.5;
float2 st = (pixelCenter / float(pushConstant.cubemapSize)) * 2.0 - 1.0;

float3 N = getCubemapVector(faceIndex, st);
float3 R = N;
float3 V = R;

TextureCube envMap = TextureCube.Handle(uint2(pushConstant.envTextureIndex, 0));
SamplerState linearSampler = SamplerState.Handle(uint2(0, 0));
RWTexture2DArray<float4> prefilteredOutput = RWTexture2DArray<float4>.Handle(uint2(pushConstant.cubemapStorageIndex, 0));

const uint SAMPLE_COUNT = 1024u;
float totalWeight = 0.0;
float3 prefilteredColor = float3(0.0);

for (uint i = 0u; i < SAMPLE_COUNT; ++i)
{
    float2 Xi = Hammersley(i, SAMPLE_COUNT);
    float3 H = ImportanceSampleGGX(Xi, N, pushConstant.roughness);
    float3 L = normalize(2.0 * dot(V, H) * H - V);

    float dotNL = clamp(dot(N, L), 0.0, 1.0);
    if (dotNL > 0.0)
    {
        float dotNH = clamp(dot(N, H), 0.0, 1.0);
        float dotVH = clamp(dot(V, H), 0.0, 1.0);

        float pdf = D_GGX(dotNH, pushConstant.roughness) * dotNH / (4.0 * dotVH) + 0.0001;
        float omegaS = 1.0 / (float(SAMPLE_COUNT) * pdf);
        float omegaP = 4.0 * PI / (6.0 * float(pushConstant.cubemapSize * pushConstant.cubemapSize));
        float mipLevel = pushConstant.roughness == 0.0 ? 0.0 : max(0.5 * log2(omegaS / omegaP) + 1.0, 0.0);

        prefilteredColor += envMap.SampleLevel(linearSampler, L, mipLevel).rgb * dotNL;
        totalWeight += dotNL;
    }
}

prefilteredColor = totalWeight > 0.0 ? prefilteredColor / totalWeight : float3(0.0);

uint3 writePos = uint3(dispatchThreadID.x, dispatchThreadID.y, faceIndex);
prefilteredOutput[writePos] = float4(prefilteredColor, 1.0);
}