// GPU Compute Particle Simulation (docs/VFX_And_Volumes_Plan_2026-11.md step V1):
// DeadList / ping-pong AliveList particle pool simulated entirely on the GPU, writing compact GpuParticle render records
// and MeshTasksIndirectCommand arguments for Particles.slang's indirect mesh-shader draw.
#include "shaderIO.h"

static const uint VfxGroupSize = 64u;
static const float VfxTwoPi = 6.28318530718;

// Fast PCG hash -> [0, 1) float.
uint vfx_pcg(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float vfx_rand01(inout uint state)
{
    return float(vfx_pcg(state) & 0x00FFFFFFu) / 16777216.0;
}

float vfx_rand_signed(inout uint state)
{
    return vfx_rand01(state) * 2.0 - 1.0;
}

#include "VfxModules.slang"

// 1. Initialize DeadList and counters once when a pool is created or restarted.
[shader("compute")]
[numthreads(VfxGroupSize, 1, 1)]
void initMain(uint3 tid : SV_DispatchThreadID, uniform PushConstantVfxSimulate pushConstant)
{
    GpuVfxEmitterParams params = BufferRef<GpuVfxEmitterParams>(pushConstant.paramsReference)[0];
    const uint idx = tid.x;
    if (idx < params.maxParticles)
    {
        RWBufferRef<uint> deadList = RWBufferRef<uint>(pushConstant.deadListReference);
        deadList[idx] = idx;
    }
    if (idx == 0u)
    {
        RWBufferRef<GpuVfxCounters> counters = RWBufferRef<GpuVfxCounters>(pushConstant.countersReference);
        GpuVfxCounters initCounters;
        initCounters.aliveInCount = 0u;
        initCounters.aliveOutCount = 0u;
        initCounters.deadCount = params.maxParticles;
        initCounters.spawnActual = 0u;
        counters[0] = initCounters;

        RWBufferRef<GpuVfxIndirectCommand> indirect = RWBufferRef<GpuVfxIndirectCommand>(pushConstant.indirectArgsReference);
        GpuVfxIndirectCommand initCmd;
        initCmd.groupCountX = 0u;
        initCmd.groupCountY = 1u;
        initCmd.groupCountZ = 1u;
        initCmd.pad0 = 0u;
        indirect[0] = initCmd;
    }
}

// 2. Spawn up to params.spawnCount new particles from DeadList into AliveListIn.
[shader("compute")]
[numthreads(VfxGroupSize, 1, 1)]
void spawnMain(uint3 tid : SV_DispatchThreadID, uniform PushConstantVfxSimulate pushConstant)
{
    GpuVfxEmitterParams params = BufferRef<GpuVfxEmitterParams>(pushConstant.paramsReference)[0];
    const uint idx = tid.x;
    if (idx >= params.spawnCount)
        return;

    const int prevDead = int(AtomicAddRaw(pushConstant.countersReference, 8u, 0xFFFFFFFFu));
    if (prevDead <= 0)
    {
        AtomicAddRaw(pushConstant.countersReference, 8u, 1u);
        return;
    }
    const uint deadSlot = uint(prevDead - 1);
    if (deadSlot >= params.maxParticles)
        return;
    RWBufferRef<uint> deadList = RWBufferRef<uint>(pushConstant.deadListReference);
    const uint particleIndex = deadList[deadSlot];
    if (particleIndex >= params.maxParticles)
        return;

    uint rng = (particleIndex * 1973u) ^ (params.frameSeed * 9277u) ^ (idx * 26699u) ^ 0xA3C59AC3u;
    vfx_pcg(rng);

    // Shape position & direction in emitter space (or render-space directly for MeshSurface).
    float3 pos = params.origin;
    float3 vel = params.velocity;
    bool alreadyWorldSpace = false;

    if (params.shape == 4u && (params.moduleFlags & VFX_MOD_MESH_SURFACE) != 0u)
    {
        float3 surfPos;
        float3 surfNormal;
        if (vfx_sample_mesh_surface(params, rng, surfPos, surfNormal))
        {
            pos = surfPos;
            const float3 baseVel = params.basisX * params.velocity.x + params.basisY * params.velocity.y + params.basisZ * params.velocity.z;
            const float3 randVel = float3(vfx_rand_signed(rng), vfx_rand_signed(rng), vfx_rand_signed(rng)) * params.velocityRandom;
            vel = baseVel + surfNormal * params.collisionParams2.y + randVel;
            alreadyWorldSpace = true;
        }
    }

    if (!alreadyWorldSpace)
    {
        float3 localPos = float3(0.0);
        float3 localVel = params.velocity;
        if (params.shape == 1u) // Sphere
        {
            const float z = vfx_rand_signed(rng);
            const float phi = vfx_rand01(rng) * VfxTwoPi;
            const float r = pow(vfx_rand01(rng), 0.3333333);
            const float xy = sqrt(max(1.0 - z * z, 0.0));
            localPos = float3(xy * cos(phi), z, xy * sin(phi)) * (params.shapeSize * r);
        }
        else if (params.shape == 2u) // Box
        {
            localPos = float3(vfx_rand_signed(rng), vfx_rand_signed(rng), vfx_rand_signed(rng)) * (params.shapeSize * 0.5);
        }
        else if (params.shape == 3u) // Cone (shoots along +Y with half-angle coneAngleRad)
        {
            const float speed = length(params.velocity);
            const float cosMax = cos(clamp(params.coneAngleRad, 0.0, 1.5707963));
            const float cosTheta = lerp(cosMax, 1.0, vfx_rand01(rng));
            const float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
            const float phi = vfx_rand01(rng) * VfxTwoPi;
            const float3 dir = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
            localVel = dir * speed;
            const float baseR = sqrt(vfx_rand01(rng)) * params.shapeSize.x;
            localPos = float3(cos(phi) * baseR, 0.0, sin(phi) * baseR);
        }

        localVel += float3(vfx_rand_signed(rng), vfx_rand_signed(rng), vfx_rand_signed(rng)) * params.velocityRandom;

        pos = localPos;
        vel = localVel;
        if (params.localSpace == 0u)
        {
            pos = params.origin + params.basisX * localPos.x + params.basisY * localPos.y + params.basisZ * localPos.z;
            vel = params.basisX * localVel.x + params.basisY * localVel.y + params.basisZ * localVel.z;
        }
    }

    GpuVfxParticle p;
    p.position = pos;
    p.previousPosition = pos;
    p.age = 0.0;
    p.velocity = vel;
    p.lifetime = max(lerp(params.lifetimeMin, params.lifetimeMax, vfx_rand01(rng)), 0.01);
    p.sizeRandom = 1.0 - params.sizeRandom * vfx_rand01(rng);
    p.rotation = vfx_rand01(rng) * VfxTwoPi;
    p.spin = lerp(params.spinMinRad, params.spinMaxRad, vfx_rand01(rng));
    p.seed = rng;
    p.flags = 0u;

    RWBufferRef<GpuVfxParticle> particles = RWBufferRef<GpuVfxParticle>(pushConstant.particlesReference);
    particles[particleIndex] = p;

    const uint aliveSlot = AtomicAddRaw(pushConstant.countersReference, 0u, 1u);
    if (aliveSlot < params.maxParticles)
    {
        RWBufferRef<uint> aliveIn = RWBufferRef<uint>(pushConstant.aliveInReference);
        aliveIn[aliveSlot] = particleIndex;
    }
}

// 3. Simulate all alive particles, retire dead ones back to DeadList, and write compact GpuParticle render records.
[shader("compute")]
[numthreads(VfxGroupSize, 1, 1)]
void updateMain(uint3 tid : SV_DispatchThreadID, uniform PushConstantVfxSimulate pushConstant)
{
    GpuVfxEmitterParams params = BufferRef<GpuVfxEmitterParams>(pushConstant.paramsReference)[0];
    RWBufferRef<GpuVfxCounters> counters = RWBufferRef<GpuVfxCounters>(pushConstant.countersReference);
    const uint idx = tid.x;
    const uint aliveCount = min(counters[0].aliveInCount, params.maxParticles);
    if (idx >= aliveCount)
        return;

    RWBufferRef<uint> aliveIn = RWBufferRef<uint>(pushConstant.aliveInReference);
    const uint particleIndex = aliveIn[idx];
    if (particleIndex >= params.maxParticles)
        return;

    RWBufferRef<GpuVfxParticle> particles = RWBufferRef<GpuVfxParticle>(pushConstant.particlesReference);
    GpuVfxParticle p = particles[particleIndex];

    p.age += params.dt;
    if (p.age >= p.lifetime)
    {
        const uint deadSlot = AtomicAddRaw(pushConstant.countersReference, 8u, 1u);
        if (deadSlot < params.maxParticles)
        {
            RWBufferRef<uint> deadList = RWBufferRef<uint>(pushConstant.deadListReference);
            deadList[deadSlot] = particleIndex;
        }
        return;
    }

    p.previousPosition = p.position;
    bool killedByModule = false;
    if (params.dt > 0.0)
    {
        p.velocity.y -= params.gravity * params.dt;

        // Step V2 GPU Force Modules: Curl Noise, Vortex, Point Attractor.
        if ((params.moduleFlags & VFX_MOD_CURL_NOISE) != 0u)
        {
            const float3 curlForce = vfx_curl_noise(p.position, params.time, params.curlParams.y, params.curlParams.z);
            p.velocity += curlForce * (params.curlParams.x * params.dt);
        }
        if ((params.moduleFlags & VFX_MOD_VORTEX) != 0u)
        {
            const float3 vortexOrigin = params.localSpace != 0u ? float3(0.0) : params.origin;
            vfx_apply_vortex(p.velocity, p.position, vortexOrigin, params.vortexAxisAndStrength.xyz,
                             params.vortexAxisAndStrength.w, params.vortexParams.x, params.vortexParams.y, params.dt);
        }
        if ((params.moduleFlags & VFX_MOD_ATTRACTOR) != 0u)
        {
            bool attrKilled = false;
            vfx_apply_attractor(p.velocity, p.position, params.attractorTargetAndStrength.xyz,
                                params.attractorTargetAndStrength.w, params.attractorParams.x, params.attractorParams.y, params.dt, attrKilled);
            killedByModule = killedByModule || attrKilled;
        }

        if (params.drag > 0.0)
            p.velocity *= max(1.0 - params.drag * params.dt, 0.0);
        p.position += p.velocity * params.dt;
        p.rotation += p.spin * params.dt;

        // Step V2 Reverse-Z Screen-Space Depth Collisions (world-space particles).
        if (!killedByModule && params.localSpace == 0u && (params.moduleFlags & VFX_MOD_DEPTH_COLLISION) != 0u && params.matrixReference != 0)
        {
            UniformBufferObject ubo = loadUbo(params.matrixReference);
            bool colKilled = false;
            vfx_depth_collision(p.position, p.velocity, p.previousPosition, ubo, params,
                                pushConstant.depthTextureIndex, pushConstant.normalTextureIndex, colKilled);
            killedByModule = killedByModule || colKilled;
        }
    }

    if (killedByModule)
    {
        const uint deadSlot = AtomicAddRaw(pushConstant.countersReference, 8u, 1u);
        if (deadSlot < params.maxParticles)
        {
            RWBufferRef<uint> deadList = RWBufferRef<uint>(pushConstant.deadListReference);
            deadList[deadSlot] = particleIndex;
        }
        return;
    }

    particles[particleIndex] = p;

    const uint outSlot = AtomicAddRaw(pushConstant.countersReference, 4u, 1u);
    if (outSlot >= params.maxParticles)
        return;

    RWBufferRef<uint> aliveOut = RWBufferRef<uint>(pushConstant.aliveOutReference);
    aliveOut[outSlot] = particleIndex;

    const float t = saturate(p.age / max(p.lifetime, 1e-4));
    const bool useCurves = (params.moduleFlags & VFX_MOD_CURVES) != 0u;
    const float baseSize = useCurves ? vfx_sample_size_curve(params, t) : lerp(params.sizeStart, params.sizeEnd, t);
    const float size = max(baseSize * p.sizeRandom, 0.0);
    float4 color = useCurves ? vfx_sample_color_curve(params, t) : lerp(params.colorStart, params.colorEnd, t);
    color.rgb *= params.intensity;

    const uint totalFrames = max(params.flipbookCols * params.flipbookRows, 1u);
    uint frame = 0u;
    if (totalFrames > 1u)
    {
        frame = params.flipbookFPS > 0.0
            ? (uint(p.age * params.flipbookFPS) % totalFrames)
            : min(uint(t * float(totalFrames)), totalFrames - 1u);
    }

    float3 renderPos = p.position;
    float3 renderPrevPos = p.previousPosition;
    if (params.localSpace != 0u)
    {
        renderPos = params.origin + params.basisX * p.position.x + params.basisY * p.position.y + params.basisZ * p.position.z;
        renderPrevPos = params.origin + params.basisX * p.previousPosition.x + params.basisY * p.previousPosition.y + params.basisZ * p.previousPosition.z;
    }

    GpuParticle gp;
    gp.position = renderPos;
    gp.size = size;
    gp.color = color;
    gp.rotation = p.rotation;
    gp.frame = frame;
    gp.padding0 = 0.0;
    gp.padding1 = 0.0;
    gp.previousPosition = renderPrevPos;
    gp.padding2 = 0.0;

    RWBufferRef<GpuParticle> renderParticles = RWBufferRef<GpuParticle>(pushConstant.renderParticlesReference);
    renderParticles[outSlot] = gp;
}

// 4. Write indirect mesh-task dispatch count and swap counters for next frame.
[shader("compute")]
[numthreads(1, 1, 1)]
void finalizeMain(uniform PushConstantVfxSimulate pushConstant)
{
    GpuVfxEmitterParams params = BufferRef<GpuVfxEmitterParams>(pushConstant.paramsReference)[0];
    RWBufferRef<GpuVfxCounters> counters = RWBufferRef<GpuVfxCounters>(pushConstant.countersReference);
    GpuVfxCounters c = counters[0];
    const uint alive = min(c.aliveOutCount, params.maxParticles);
    c.aliveInCount = alive;
    c.aliveOutCount = 0u;
    counters[0] = c;

    RWBufferRef<GpuVfxIndirectCommand> indirect = RWBufferRef<GpuVfxIndirectCommand>(pushConstant.indirectArgsReference);
    GpuVfxIndirectCommand cmd;
    if (params.meshRenderChunkCount > 0u)
    {
        cmd.groupCountX = params.meshRenderChunkCount;
        cmd.groupCountY = min(alive, 65535u);
        cmd.groupCountZ = 1u;
    }
    else
    {
        cmd.groupCountX = (alive + 31u) / 32u;
        cmd.groupCountY = 1u;
        cmd.groupCountZ = 1u;
    }
    cmd.pad0 = 0u;
    indirect[0] = cmd;
}
