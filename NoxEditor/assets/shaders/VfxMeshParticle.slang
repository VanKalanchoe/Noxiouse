// GPU-Instanced Mesh Particles (docs/VFX_And_Volumes_Plan_2026-11.md step V3b):
// Draws arbitrary 3D meshes (or a built-in 3D crystal shard when no mesh is assigned) directly from GPU particle
// pools via DrawMeshTasksIndirect (groupID.x = 32-triangle mesh chunk, groupID.y = alive particle index).
#include "shaderIO.h"

static const uint VfxMeshTrisPerGroup = 32u;

struct MeshParticleVertex
{
    float4 pos : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    nointerpolation float4 color : TEXCOORD3;
    nointerpolation float age : TEXCOORD4;
    nointerpolation float randomVal : TEXCOORD5;
    nointerpolation float3 velocity : TEXCOORD6;
};

// Built-in 8-triangle 3D crystal/octahedron shard (used when ParticleMesh == 0).
static const float3 kShardPos[6] = {
    float3( 0.0,  1.2,  0.0), // top tip
    float3( 0.5,  0.0,  0.0),
    float3( 0.0,  0.0,  0.5),
    float3(-0.5,  0.0,  0.0),
    float3( 0.0,  0.0, -0.5),
    float3( 0.0, -0.8,  0.0)  // bottom tip
};
static const uint3 kShardTris[8] = {
    uint3(0, 1, 2), uint3(0, 2, 3), uint3(0, 3, 4), uint3(0, 4, 1),
    uint3(5, 2, 1), uint3(5, 3, 2), uint3(5, 4, 3), uint3(5, 1, 4)
};

// Build orthonormal 3x3 basis for a mesh particle (either velocity-aligned or tumbling 3D spin).
float3x3 vfx_particle_basis(float3 vel, float angle, uint seed, bool alignToVel)
{
    if (alignToVel && dot(vel, vel) > 1e-6)
    {
        const float3 up = normalize(vel);
        const float3 ref = abs(up.y) < 0.95 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
        const float3 right = normalize(cross(ref, up));
        const float3 fwd = cross(up, right);
        const float c = cos(angle);
        const float s = sin(angle);
        return float3x3(right * c + fwd * s, up, fwd * c - right * s);
    }

    // Deterministic per-particle tumble axis from seed + Rodrigues rotation by angle.
    const float3 rawAxis = float3(
        float((seed) & 0xFFu) / 127.5 - 1.0,
        float((seed >> 8u) & 0xFFu) / 127.5 - 1.0,
        float((seed >> 16u) & 0xFFu) / 127.5 - 1.0);
    const float3 axis = dot(rawAxis, rawAxis) > 1e-4 ? normalize(rawAxis) : float3(0.0, 1.0, 0.0);
    const float c = cos(angle);
    const float s = sin(angle);
    const float t = 1.0 - c;
    return float3x3(
        float3(t * axis.x * axis.x + c,          t * axis.x * axis.y - s * axis.z, t * axis.x * axis.z + s * axis.y),
        float3(t * axis.x * axis.y + s * axis.z, t * axis.y * axis.y + c,          t * axis.y * axis.z - s * axis.x),
        float3(t * axis.x * axis.z - s * axis.y, t * axis.y * axis.z + s * axis.x, t * axis.z * axis.z + c));
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void meshMain(
    out indices uint3 triangles[32],
    out vertices MeshParticleVertex vertices[96],
    uint threadID : SV_GroupIndex,
    uint3 groupID : SV_GroupID,
    uniform PushConstantVfxMeshParticle pushConstant
)
{
    const uint aliveCount = BufferRef<GpuVfxCounters>(pushConstant.countersReference)[0].aliveInCount;
    const uint particleIdx = groupID.y;
    const bool useCustomMesh = pushConstant.verticesReference != 0 && pushConstant.indicesReference != 0 && pushConstant.triangleCount > 0u;
    const uint totalTris = useCustomMesh ? pushConstant.triangleCount : 8u;
    const uint chunkFirstTri = groupID.x * VfxMeshTrisPerGroup;
    const uint trisInGroup = (particleIdx < aliveCount && totalTris > chunkFirstTri)
        ? min(totalTris - chunkFirstTri, VfxMeshTrisPerGroup)
        : 0u;

    SetMeshOutputCounts(trisInGroup * 3u, trisInGroup);
    if (threadID >= trisInGroup)
        return;

    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    const GpuParticle gp = BufferRef<GpuParticle>(pushConstant.renderParticlesReference)[particleIdx];
    const uint poolIdx = BufferRef<uint>(pushConstant.aliveListReference)[particleIdx];
    const GpuVfxParticle vp = BufferRef<GpuVfxParticle>(pushConstant.particlesReference)[poolIdx];

    const bool alignVel = (pushConstant.flags & VFX_MESH_ALIGN_VELOCITY) != 0u;
    const float3x3 rot = vfx_particle_basis(vp.velocity, gp.rotation, vp.seed, alignVel);
    // Custom meshes are already authored in world units (e.g. 100 cm = 1 m), whereas the procedural shard is unit-meter sized.
    const float scale = max(gp.size, 0.0) / (useCustomMesh ? max(ubo.unitsPerMeter, 1e-4) : 1.0);
    const float ageNorm = saturate(vp.age / max(vp.lifetime, 1e-4));
    const float rand01 = float(vp.seed & 0xFFFFu) / 65535.0;

    const uint triIdx = chunkFirstTri + threadID;
    float3 pLocal[3];
    float3 nLocal[3];
    float2 uvLocal[3];

    if (useCustomMesh)
    {
        BufferRef<uint> indices = BufferRef<uint>(pushConstant.indicesReference);
        BufferRef<PackedVertex> verts = BufferRef<PackedVertex>(pushConstant.verticesReference);
        for (uint k = 0u; k < 3u; ++k)
        {
            const uint vi = pushConstant.verticesOffset + indices[pushConstant.indicesOffset + triIdx * 3u + k];
            const Vertex v = unpackVertex(verts[vi]);
            pLocal[k] = v.pos;
            nLocal[k] = v.normal;
            uvLocal[k] = v.uv0;
        }
    }
    else
    {
        const uint3 idx = kShardTris[triIdx];
        pLocal[0] = kShardPos[idx.x];
        pLocal[1] = kShardPos[idx.y];
        pLocal[2] = kShardPos[idx.z];
        const float3 faceN = normalize(cross(pLocal[1] - pLocal[0], pLocal[2] - pLocal[0]));
        nLocal[0] = faceN;
        nLocal[1] = faceN;
        nLocal[2] = faceN;
        uvLocal[0] = float2(0.5, 0.0);
        uvLocal[1] = float2(0.0, 1.0);
        uvLocal[2] = float2(1.0, 1.0);
    }

    for (uint k = 0u; k < 3u; ++k)
    {
        const float3 worldPos = gp.position + mul(rot, pLocal[k] * scale);
        const float3 worldN = normalize(mul(rot, nLocal[k]));
        MeshParticleVertex outV;
        outV.pos = mul(ubo.proj, mul(ubo.view, float4(worldPos, 1.0)));
        outV.worldPos = worldPos;
        outV.worldNormal = worldN;
        outV.uv = uvLocal[k];
        outV.color = gp.color;
        outV.age = ageNorm;
        outV.randomVal = rand01;
        outV.velocity = vp.velocity;
        vertices[threadID * 3u + k] = outV;
    }
    triangles[threadID] = uint3(threadID * 3u, threadID * 3u + 1u, threadID * 3u + 2u);
}

[shader("fragment")]
float4 fragMain(MeshParticleVertex inVert, uniform PushConstantVfxMeshParticle pushConstant) : SV_Target0
{
    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);

    // Reverse-Z scene depth test against opaque scene.
    if (pushConstant.depthTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D depthTex = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
        const float sceneDepth = depthTex.Load(int3(int2(inVert.pos.xy), 0)).r;
        if (inVert.pos.z < sceneDepth)
            discard;
    }

    float4 texCol = float4(1.0);
    if (pushConstant.textureIndex != 0xFFFFFFFFu)
    {
        Texture2D tex = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + pushConstant.textureIndex, 0));
        SamplerState samp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
        texCol = tex.Sample(samp, inVert.uv);
    }

    const float3 N = normalize(inVert.worldNormal);
    const float3 V = normalize(ubo.cameraWorldPos.xyz - inVert.worldPos);
    float3 baseCol = inVert.color.rgb * texCol.rgb;
    float3 emissiveCol = float3(0.0);
    float rough = pushConstant.roughness;
    float alpha = saturate(inVert.color.a * texCol.a);

#ifdef NOX_MATERIAL_GRAPH
    if (pushConstant.materialSlot != 0xFFFFFFFFu && ubo.materialDataReference != 0)
    {
        const GpuMaterial mat = BufferRef<GpuMaterial>(ubo.materialDataReference)[pushConstant.materialSlot];
        if (mat.graphSlot != 0xFFFFFFFFu)
        {
            MaterialGraphInputs gi = (MaterialGraphInputs)0;
            gi.uv0 = inVert.uv;
            gi.uv0Dx = ddx(inVert.uv);
            gi.uv0Dy = ddy(inVert.uv);
            gi.position = inVert.worldPos + ubo.renderOrigin;
            gi.positionDx = ddx(inVert.worldPos);
            gi.positionDy = ddy(inVert.worldPos);
            gi.normal = N;
            gi.view = V;
            gi.time = ubo.timeSeconds;
            gi.cameraPosition = ubo.cameraWorldPos.xyz + ubo.renderOrigin;
            gi.vertexColor = inVert.color;
            gi.hasParticleData = 1u;
            gi.particleAge = inVert.age;
            gi.particleRandom = inVert.randomVal;
            gi.particleVelocity = inVert.velocity;
            gi.particleColor = inVert.color;
            gi.ribbonUV = inVert.uv;
            gi.material = mat;
            gi.ubo = ubo;

            MaterialGraphOutputs go = (MaterialGraphOutputs)0;
            go.baseColor = baseCol;
            go.roughness = rough;
            go.emissive = float3(0.0);
            go.opacity = alpha;
            material_graph_surface(mat.graphSlot, gi, go);
            baseCol = go.baseColor;
            emissiveCol = go.emissive;
            rough = saturate(go.roughness);
            alpha = saturate(go.opacity);
        }
    }
#endif

    float3 shaded = baseCol;

    if ((pushConstant.flags & PARTICLE_LIT) != 0u)
    {
        // Fast directional + hemisphere ambient + specular rim shading for 3D mesh particles.
        float3 ambient = float3(0.22, 0.25, 0.30);
        if (ubo.irradianceMapIndex != 0xFFFFFFFFu)
        {
            TextureCube irr = TextureCube.Handle(uint2(ubo.imageHeapIndexOffset + ubo.irradianceMapIndex, 0));
            SamplerState irrSamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
            ambient = irr.SampleLevel(irrSamp, N, 0).rgb * ubo.scaleIBLAmbient;
        }
        float3 direct = float3(0.0);
        BufferRef<LightData> lights = BufferRef<LightData>(ubo.lightDataReference);
        const uint lightCount = min(ubo.lightCount, 8u);
        for (uint i = 0u; i < lightCount; ++i)
        {
            const LightData light = lights[i];
            const uint lightType = uint(light.position.w);
            const float3 L = lightType == 0u ? normalize(light.direction.xyz) : normalize(light.position.xyz - inVert.worldPos);
            const float ndotl = saturate(dot(N, L));
            if (ndotl > 0.0)
            {
                const float3 H = normalize(L + V);
                const float spec = pow(saturate(dot(N, H)), lerp(64.0, 6.0, rough)) * (1.0 - rough * 0.7);
                direct += light.color.rgb * light.color.a * (ndotl / 3.14159265 + spec * 0.35);
            }
        }
        shaded = shaded * (ambient + direct) + emissiveCol;
    }
    else
    {
        // Unlit/emissive 3D shards get a subtle normal-based facet contrast + Fresnel edge boost so 3D form reads crisply.
        const float fresnel = pow(1.0 - saturate(abs(dot(N, V))), 2.5);
        const float facet = 0.65 + 0.35 * saturate(N.y * 0.5 + 0.5);
        shaded = shaded * (facet + fresnel * 0.6) + emissiveCol;
    }

    if (alpha <= 1e-4)
        discard;

    return float4(shaded * alpha, (pushConstant.flags & PARTICLE_ADDITIVE) != 0u ? 0.0 : alpha);
}
