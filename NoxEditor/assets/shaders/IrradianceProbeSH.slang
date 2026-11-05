#include "shaderIO.h"

// Irradiance volume capture (RS5a; Renderer::addIrradianceVolumePasses): each captured probe's six rasterized faces (one row of the
// atlas: +X, -X, +Y, -Y, +Z, -Z, IrradianceProbeFaceSize texels each) projected onto L1 spherical harmonics of the radiance around it,
// every texel weighted by the solid angle it covers. One group per probe, one thread per texel position of all six faces.

// A face's view (Renderer: a flipped right-handed look-at along its axis, 90 degrees): world direction of a texel's ndc.
static const float3 kFaceDirections[6] = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1) };
static const float3 kFaceUps[6] = { float3(0, 1, 0), float3(0, 1, 0), float3(0, 0, 1), float3(0, 0, -1), float3(0, 1, 0), float3(0, 1, 0) };

groupshared float4 gSH[256][3];

[shader("compute")]
[numthreads(16, 16, 1)]
void compMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID, uint threadIndex : SV_GroupIndex,
              uniform PushConstantIrradianceProbeSH pushConstant)
{
    const uint row = groupId.x;
    Texture2D atlas = Texture2D.Handle(uint2(pushConstant.atlasTextureIndex, 0));
    const float faceSize = float(IrradianceProbeFaceSize);
    float4 shR = float4(0.0), shG = float4(0.0), shB = float4(0.0);
    for (uint face = 0; face < 6; ++face)
    {
        const float2 ndc = float2((float(threadId.x) + 0.5) / faceSize * 2.0 - 1.0, 1.0 - (float(threadId.y) + 0.5) / faceSize * 2.0);
        const float3 forward = kFaceDirections[face];
        const float3 side = normalize(cross(forward, kFaceUps[face]));
        const float3 up = cross(side, forward);
        const float3 direction = normalize(ndc.x * side + ndc.y * up + forward);
        // The texel's solid angle: (2 / size)^2 / (1 + x^2 + y^2)^(3/2).
        const float solidAngle = (4.0 / (faceSize * faceSize)) / pow(1.0 + dot(ndc, ndc), 1.5);
        const float3 radiance = atlas.Load(int3(int(face * IrradianceProbeFaceSize + threadId.x), int(row * IrradianceProbeFaceSize + threadId.y), 0)).rgb;
        const float4 basis = float4(0.282095, 0.488603 * direction.y, 0.488603 * direction.z, 0.488603 * direction.x) * solidAngle;
        shR += basis * radiance.r;
        shG += basis * radiance.g;
        shB += basis * radiance.b;
    }
    gSH[threadIndex][0] = shR;
    gSH[threadIndex][1] = shG;
    gSH[threadIndex][2] = shB;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1)
    {
        if (threadIndex < stride)
        {
            gSH[threadIndex][0] += gSH[threadIndex + stride][0];
            gSH[threadIndex][1] += gSH[threadIndex + stride][1];
            gSH[threadIndex][2] += gSH[threadIndex + stride][2];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (threadIndex == 0)
    {
        const uint probe = ((uint*)pushConstant.probeIndicesReference)[row];
        float4* sh = (float4*)pushConstant.probeSHReference;
        sh[probe * 4 + 0] = gSH[0][0];
        sh[probe * 4 + 1] = gSH[0][1];
        sh[probe * 4 + 2] = gSH[0][2];
        sh[probe * 4 + 3] = float4(1.0, 0.0, 0.0, 0.0); // captured
    }
}
