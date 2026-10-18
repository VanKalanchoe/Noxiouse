// Particles (docs/Hybrid_Renderer_Plan_2026-10.md §6 V1 / V2): one camera-facing quad per particle of a ParticleEmitterComponent
// (simulated on the CPU, Scene/ParticleSystem.cpp), 32 particles per mesh workgroup, blended over the lit HDR scene after the
// forward pass.
//
// Lit particles are shaded once per particle, here in the mesh stage: the environment (DDGI where it runs, else the irradiance
// cubemap) and every light as if the particle were a small diffuse puff facing it, the first directional light behind a shadow
// ray (ray query) when RT shadows are on. Unlit particles are their color (HDR: above 1 glows).
//
// No depth attachment: each fragment compares itself with the scene depth (it is behind geometry: discarded) and, soft, fades
// out over the last softDistance before it, so a puff of smoke meets the ground without a hard line.

#include "shaderIO.h"
#include "DDGICommon.slang"

static const uint ParticlesPerGroup = 32;
static const uint LitLightCount = 8; // the first lights of the scene light a particle (scenes can have hundreds)

struct ParticleVertex
{
    float4 pos : SV_Position;
    float3 worldPos : WORLD_POS;
    float2 uv : TEXCOORD0;
    nointerpolation float4 color : COLOR0;
    nointerpolation uint frame : FRAME;
};

// The sun hidden from the particle? Blended and transmissive occluders let it through (forwardShadowed in
// Material_TransparentLit_Mesh.slang decides the same way; alpha-masked ones count as solid here).
bool particleShadowed(UniformBufferObject* ubo, float3 origin, float3 direction)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.01 * ubo->unitsPerMeter;
    ray.TMax = 100000.0 * ubo->unitsPerMeter;

    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(tlas, RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, ray);

    GpuRayTracingInstance* rtInstances = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
    while (query.Proceed())
    {
        GpuRayTracingInstance occluder = rtInstances[query.CandidateRayInstanceCustomIndex()];
        if (occluder.alphaMode == 2 || occluder.transmissionFactor > 0.0)
            continue;
        query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// What a particle of this color reflects towards the camera.
float3 litParticle(UniformBufferObject* ubo, float3 albedo, float3 position, float3 toCamera, bool traceShadow)
{
    // The environment: probes where DDGI runs, the irradiance cubemap otherwise (seen from the camera's side and from above:
    // a puff is lit all around, not like a wall).
    float3 ambient = float3(0.0);
    if (ubo->enableDDGI != 0 && ubo->ddgiIrradianceTextureIndex != 0xFFFFFFFF && ubo->ddgiDistanceTextureIndex != 0xFFFFFFFF)
    {
        Texture2D ddgiIrradiance = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiIrradianceTextureIndex, 0));
        Texture2D ddgiDistance = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiDistanceTextureIndex, 0));
        SamplerState ddgiSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
        ambient = sample_ddgi_diffuse(ubo, position, toCamera, ddgiIrradiance, ddgiDistance, ddgiSampler);
    }
    else if (ubo->irradianceMapIndex != 0xFFFFFFFF)
    {
        TextureCube irradiance = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
        SamplerState irradianceSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
        ambient = 0.5 * (irradiance.SampleLevel(irradianceSampler, toCamera, 0).rgb +
                         irradiance.SampleLevel(irradianceSampler, float3(0.0, 1.0, 0.0), 0).rgb) * ubo->scaleIBLAmbient;
    }

    float3 direct = float3(0.0);
    LightData* lights = (LightData*)ubo->lightDataReference;
    bool sunTested = false;
    for (uint i = 0; i < min(ubo->lightCount, LitLightCount); ++i)
    {
        LightData light = lights[i];
        const uint lightType = (uint)light.position.w;
        float3 l;
        float attenuation = 1.0;
        if (lightType == 0)
        {
            l = normalize(light.direction.xyz);
            if (traceShadow && !sunTested)
            {
                sunTested = true;
                if (particleShadowed(ubo, position, l))
                    continue;
            }
        }
        else
        {
            const float3 toLight = light.position.xyz - position;
            const float distance = length(toLight);
            l = distance > 0.0001 ? toLight / distance : float3(0.0, 1.0, 0.0);
            const float range = light.direction.w;
            attenuation = range > 0.0 ? max(1.0 - pow(distance / range, 4.0), 0.0) / (distance * distance) : 1.0 / (distance * distance);
            if (lightType == 2)
            {
                const float angular = saturate(dot(normalize(light.direction.xyz), -l) * light.spotParams.x + light.spotParams.y);
                attenuation *= angular * angular;
            }
        }
        // A diffuse puff facing the light; a little brighter seen against it (light shining through the particle's edge).
        const float through = 0.75 + 0.25 * saturate(dot(-l, toCamera));
        direct += light.color.rgb * light.color.w * attenuation * through;
    }

    return albedo * (ambient + direct / 3.14159265);
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void meshMain(
    out indices uint3 triangles[64],
    out vertices ParticleVertex vertices[128],
    uint threadID : SV_GroupIndex,
    uint3 groupID : SV_GroupID,
    uniform PushConstantParticles pushConstant
)
{
    const uint groupFirst = groupID.x * ParticlesPerGroup;
    const uint inGroup = pushConstant.particleCount > groupFirst ? min(pushConstant.particleCount - groupFirst, ParticlesPerGroup) : 0;
    SetMeshOutputCounts(inGroup * 4, inGroup * 2);
    if (threadID >= inGroup)
        return;

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    GpuParticle* particles = (GpuParticle*)pushConstant.particlesReference;
    const GpuParticle particle = particles[pushConstant.firstParticle + groupFirst + threadID];

    // Facing the camera's position (round from every side, also at the edges of a wide view).
    const float3 camera = ubo->cameraWorldPos.xyz;
    float3 forward = particle.position - camera;
    const float distance = length(forward);
    forward = distance > 1e-5 ? forward / distance : float3(0.0, 0.0, -1.0);
    float3 right = cross(forward, float3(0.0, 1.0, 0.0));
    right = dot(right, right) > 1e-8 ? normalize(right) : float3(1.0, 0.0, 0.0);
    float3 up = cross(right, forward);
    const float c = cos(particle.rotation);
    const float s = sin(particle.rotation);
    const float3 rotatedRight = right * c + up * s;
    const float3 rotatedUp = up * c - right * s;
    const float halfSize = 0.5 * particle.size;

    float4 color = particle.color;
    if ((pushConstant.flags & PARTICLE_LIT) != 0)
    {
        const bool shadow = (pushConstant.flags & PARTICLE_SHADOW) != 0 && ubo->enableRTShadows != 0 && ubo->tlasDeviceAddress != 0;
        color.rgb = litParticle(ubo, color.rgb, particle.position, -forward, shadow);
    }

    const float2 corners[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0) };
    for (uint corner = 0; corner < 4; ++corner)
    {
        const float3 worldPos = particle.position + (rotatedRight * corners[corner].x + rotatedUp * corners[corner].y) * halfSize;
        ParticleVertex vertex;
        vertex.pos = mul(ubo->proj, mul(ubo->view, float4(worldPos, 1.0)));
        vertex.worldPos = worldPos;
        vertex.uv = float2(corners[corner].x * 0.5 + 0.5, 0.5 - corners[corner].y * 0.5);
        vertex.color = color;
        vertex.frame = particle.frame;
        vertices[threadID * 4 + corner] = vertex;
    }
    triangles[threadID * 2 + 0] = uint3(threadID * 4 + 0, threadID * 4 + 1, threadID * 4 + 2);
    triangles[threadID * 2 + 1] = uint3(threadID * 4 + 0, threadID * 4 + 2, threadID * 4 + 3);
}

[shader("fragment")]
float4 fragMain(ParticleVertex input, uniform PushConstantParticles pushConstant) : SV_Target0
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

    // Behind the scene's geometry: hidden. Soft: fading over the last softDistance in front of it.
    float fade = 1.0;
    if (pushConstant.depthTextureIndex != 0xFFFFFFFF)
    {
        Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
        const float sceneDepth = depthTex.Load(int3(int2(input.pos.xy), 0));
        if (sceneDepth > 0.0) // reverse Z: 0 is the sky
        {
            uint width, height;
            depthTex.GetDimensions(width, height);
            const float2 uv = (input.pos.xy) / float2(width, height);
            const float4 world = mul(ubo->invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, sceneDepth, 1.0));
            const float sceneDistance = length(world.xyz / world.w - ubo->cameraWorldPos.xyz);
            const float particleDistance = length(input.worldPos - ubo->cameraWorldPos.xyz);
            if (particleDistance > sceneDistance)
                discard;
            if ((pushConstant.flags & PARTICLE_SOFT) != 0 && pushConstant.softDistance > 0.0)
                fade = saturate((sceneDistance - particleDistance) / pushConstant.softDistance);
        }
    }

    float4 texel = float4(1.0);
    if (pushConstant.textureIndex != 0xFFFFFFFF)
    {
        // The flipbook sheet: frames left to right, top to bottom.
        const uint columns = max(pushConstant.flipbookColumns, 1u);
        const uint rows = max(pushConstant.flipbookRows, 1u);
        const uint frame = input.frame % (columns * rows);
        const float2 cell = float2(float(frame % columns), float(frame / columns));
        const float2 uv = (cell + input.uv) / float2(columns, rows);
        Texture2D sheet = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.textureIndex, 0));
        SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
        texel = sheet.Sample(linearClamp, uv);
    }
    else
    {
        // A soft round dot.
        const float radius = length(input.uv * 2.0 - 1.0);
        const float dot = saturate(1.0 - radius);
        texel.a = dot * dot * (3.0 - 2.0 * dot);
    }

    const float alpha = saturate(input.color.a * texel.a * fade);
    const float3 rgb = input.color.rgb * texel.rgb * alpha; // premultiplied
    // Additive: the light is added and nothing behind is covered (the blend keeps the destination's alpha).
    return float4(rgb, (pushConstant.flags & PARTICLE_ADDITIVE) != 0 ? 0.0 : alpha);
}
