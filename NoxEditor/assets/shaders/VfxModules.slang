// Modular GPU VFX Simulation Library (docs/VFX_And_Volumes_Plan_2026-11.md step V2):
// Curl Noise / Turbulence, Vortex, Point Attractor, Reverse-Z Depth Buffer Collisions,
// 16-stop Color & Size Curves over Life, and Static / Skinned Mesh Surface Sampling.
#pragma once

// Fast 3D hash -> [0, 1) for procedural curl noise.
float3 vfx_hash33(float3 p)
{
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx);
}

// Smooth C2 quintic 3D value noise with analytic gradient (returns float4(value, d/dx, d/dy, d/dz)).
float4 vfx_noise3d_grad(float3 x)
{
    float3 i = floor(x);
    float3 f = frac(x);
    float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float3 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);

    float a = vfx_hash33(i + float3(0, 0, 0)).x;
    float b = vfx_hash33(i + float3(1, 0, 0)).x;
    float c = vfx_hash33(i + float3(0, 1, 0)).x;
    float d = vfx_hash33(i + float3(1, 1, 0)).x;
    float e = vfx_hash33(i + float3(0, 0, 1)).x;
    float f1 = vfx_hash33(i + float3(1, 0, 1)).x;
    float g = vfx_hash33(i + float3(0, 1, 1)).x;
    float h = vfx_hash33(i + float3(1, 1, 1)).x;

    float k0 = a;
    float k1 = b - a;
    float k2 = c - a;
    float k3 = e - a;
    float k4 = a - b - c + d;
    float k5 = a - c - e + g;
    float k6 = a - b - e + f1;
    float k7 = -a + b + c - d + e - f1 - g + h;

    float val = k0 + k1 * u.x + k2 * u.y + k3 * u.z +
                k4 * u.x * u.y + k5 * u.y * u.z + k6 * u.z * u.x +
                k7 * u.x * u.y * u.z;
    float3 grad = du * float3(
        k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z,
        k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x,
        k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y);
    return float4(val, grad);
}

// Divergence-free 3D Curl Noise velocity field using analytic gradients of 3 offset noise potentials.
float3 vfx_curl_noise(float3 pos, float time, float frequency, float scrollSpeed)
{
    const float3 p = pos * frequency + float3(0.0, -time * scrollSpeed, time * scrollSpeed * 0.37);
    const float3 g0 = vfx_noise3d_grad(p).yzw;
    const float3 g1 = vfx_noise3d_grad(p + float3(31.416, -47.853, 12.793)).yzw;
    const float3 g2 = vfx_noise3d_grad(p + float3(-233.145, -113.408, -185.31)).yzw;
    // curl(F) = (dFz/dy - dFy/dz, dFx/dz - dFz/dx, dFy/dx - dFx/dy)
    return float3(g2.y - g1.z, g0.z - g2.x, g1.x - g0.y);
}

// Tangential vortex swirl + inward radial pull around an axis passing through origin.
void vfx_apply_vortex(inout float3 vel, float3 pos, float3 origin, float3 axis, float strength, float radialPull, float radius, float dt)
{
    const float3 rel = pos - origin;
    const float3 axial = axis * dot(rel, axis);
    const float3 radial = rel - axial;
    const float dist = length(radial);
    if (dist < 1e-4)
        return;
    const float falloff = radius > 1e-4 ? saturate(1.0 - dist / radius) : 1.0;
    const float3 radialDir = radial / dist;
    const float3 tangent = cross(axis, radialDir);
    vel += (tangent * strength - radialDir * radialPull) * (falloff * dt);
}

// Point attractor pulling particles toward target (with optional kill radius on arrival).
void vfx_apply_attractor(inout float3 vel, float3 pos, float3 target, float strength, float radius, float killRadius, float dt, out bool killed)
{
    killed = false;
    const float3 toTarget = target - pos;
    const float dist = length(toTarget);
    if (killRadius > 0.0 && dist <= killRadius)
    {
        killed = true;
        return;
    }
    if (dist < 1e-4)
        return;
    const float falloff = radius > 1e-4 ? saturate(1.0 - dist / radius) : 1.0;
    vel += (toTarget / dist) * (strength * falloff * dt);
}

// Reverse-Z screen-space depth buffer collision using scene Depth + GBufferNormal.
void vfx_depth_collision(inout float3 pos, inout float3 vel, float3 prevPos, UniformBufferObject ubo, GpuVfxEmitterParams params,
                         uint depthTexIdx, uint normalTexIdx, out bool killed)
{
    killed = false;
    if (depthTexIdx == 0xFFFFFFFFu)
        return;

    const float4 viewPos = mul(ubo.view, float4(pos, 1.0));
    const float4 clip = mul(ubo.proj, viewPos);
    if (clip.w <= 1e-4)
        return;
    const float3 ndc = clip.xyz / clip.w;
    // Clip check (NDC x,y in [-1,1], Reverse-Z z in (0,1]).
    if (ndc.x < -1.0 || ndc.x > 1.0 || ndc.y < -1.0 || ndc.y > 1.0 || ndc.z <= 0.0 || ndc.z > 1.0)
        return;

    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    const int2 dims = int2(max(int(params.collisionParams.w), 1), max(int(params.collisionParams2.x), 1));
    const int2 px = clamp(int2(uv * float2(dims)), int2(0, 0), dims - int2(1, 1));

    Texture2D depthTex = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + depthTexIdx, 0));
    const float sceneDepthNdc = depthTex.Load(int3(px, 0)).r;
    if (sceneDepthNdc <= 1e-6) // sky / far plane
        return;

    // Reverse-Z: larger NDC z is closer to the camera. Particle is behind the surface when ndc.z < sceneDepthNdc.
    if (ndc.z >= sceneDepthNdc)
        return;

    // Reconstruct scene surface world position to measure actual penetration thickness in world units.
    float4 sceneClip = float4(ndc.xy, sceneDepthNdc, 1.0);
    float4 sceneWorldH = mul(ubo.invViewProj, sceneClip);
    if (abs(sceneWorldH.w) < 1e-6)
        return;
    const float3 sceneWorld = sceneWorldH.xyz / sceneWorldH.w;

    const float particleViewZ = abs(viewPos.z);
    const float sceneViewZ = abs(mul(ubo.view, float4(sceneWorld, 1.0)).z);
    const float penetration = particleViewZ - sceneViewZ;
    const float thickness = max(params.collisionParams.z, 1e-3);
    if (penetration <= 0.0 || penetration > thickness)
        return;

    if ((params.moduleFlags & VFX_MOD_KILL_ON_COLLIDE) != 0u)
    {
        killed = true;
        return;
    }

    float3 surfaceNormal = float3(0.0, 1.0, 0.0);
    if (normalTexIdx != 0xFFFFFFFFu)
    {
        Texture2D normalTex = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + normalTexIdx, 0));
        const float3 n = normalTex.Load(int3(px, 0)).xyz;
        if (dot(n, n) > 1e-4)
            surfaceNormal = normalize(n);
    }

    // Only bounce if moving into the surface.
    const float vn = dot(vel, surfaceNormal);
    if (vn < 0.0)
    {
        const float restitution = saturate(params.collisionParams.x);
        const float friction = saturate(params.collisionParams.y);
        const float3 vNormal = surfaceNormal * vn;
        const float3 vTangent = vel - vNormal;
        vel = vTangent * (1.0 - friction) - vNormal * restitution;
        // Push particle just outside the surface along the normal to prevent re-penetration.
        pos = sceneWorld + surfaceNormal * (thickness * 0.08);
    }
}

// 16-entry GPU RGBA color curve LUT over normalized life t in [0, 1].
float4 vfx_sample_color_curve(GpuVfxEmitterParams params, float t)
{
    const float scaled = saturate(t) * 15.0;
    const uint i0 = min(uint(scaled), 14u);
    const uint i1 = i0 + 1u;
    const float f = frac(scaled);
    return lerp(params.colorCurveLut[i0], params.colorCurveLut[i1], f);
}

// 16-entry GPU float size curve LUT (packed into 4 vec4s) over normalized life t in [0, 1].
float vfx_sample_size_curve(GpuVfxEmitterParams params, float t)
{
    const float scaled = saturate(t) * 15.0;
    const uint i0 = min(uint(scaled), 14u);
    const uint i1 = i0 + 1u;
    const float f = frac(scaled);
    const float s0 = params.sizeCurveLut[i0 >> 2u][i0 & 3u];
    const float s1 = params.sizeCurveLut[i1 >> 2u][i1 & 3u];
    return lerp(s0, s1, f);
}

// Sample a random vertex from a static Vertex[] or animated SkinnedVertex[] buffer in render space.
bool vfx_sample_mesh_surface(GpuVfxEmitterParams params, inout uint rng, out float3 outPos, out float3 outNormal)
{
    outPos = params.origin;
    outNormal = float3(0.0, 1.0, 0.0);
    if (params.meshVertexReference == 0 || params.meshVertexCount == 0u)
        return false;

    const uint vIdx = params.meshVertexOffset + (vfx_pcg(rng) % params.meshVertexCount);
    float3 localPos;
    float3 localNormal;
    if (params.meshIsSkinned != 0u)
    {
        SkinnedVertex sv = BufferRef<SkinnedVertex>(params.meshVertexReference)[vIdx];
        localPos = sv.position.xyz;
        localNormal = sv.normal.xyz;
    }
    else
    {
        Vertex v = unpackVertex(BufferRef<PackedVertex>(params.meshVertexReference)[vIdx]);
        localPos = v.pos;
        localNormal = v.normal;
    }

    outPos = mul(params.meshModelMatrix, float4(localPos, 1.0)).xyz;
    const float3 wn = mul((float3x3)params.meshModelMatrix, localNormal);
    outNormal = dot(wn, wn) > 1e-6 ? normalize(wn) : float3(0.0, 1.0, 0.0);
    return true;
}
