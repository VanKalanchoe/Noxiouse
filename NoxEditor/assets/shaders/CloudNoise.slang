#pragma once

// Tileable noise for the procedural sky's clouds, after Schneider, "The Real-time Volumetric Cloudscapes of Horizon Zero
// Dawn" (2015) and the Nubis papers: Perlin-Worley for the cloud shapes, Worley FBM to erode their edges, Perlin FBM for
// where clouds are (coverage). Every function repeats every `period` lattice cells, so the textures made from them tile.

uint3 cloudHashPcg3d(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

// A random point in [0, 1)^3 per lattice cell, the cell wrapped to the period first (that is what makes it tile).
float3 cloudHash(int3 cell, int period, uint seed)
{
    const int3 wrapped = ((cell % period) + period) % period;
    return float3(cloudHashPcg3d(uint3(wrapped) + uint3(seed, seed * 3u, seed * 7u)) & 0xFFFFFFu) / 16777216.0;
}

// Worley (cellular) noise, inverted: 1 at a cell's feature point, 0 a cell away. p in cells.
float cloudWorley(float3 p, int period, uint seed)
{
    const int3 cell = int3(floor(p));
    const float3 local = p - float3(cell);
    float nearest = 1e9;
    for (int z = -1; z <= 1; ++z)
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                const int3 offset = int3(x, y, z);
                const float3 toFeature = float3(offset) + cloudHash(cell + offset, period, seed) - local;
                nearest = min(nearest, dot(toFeature, toFeature));
            }
        }
    }
    return 1.0 - saturate(sqrt(nearest));
}

// Gradient (Perlin) noise, about [-1, 1]. p in cells.
float cloudPerlin(float3 p, int period, uint seed)
{
    const int3 cell = int3(floor(p));
    const float3 f = p - float3(cell);
    const float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float corners[8];
    for (int i = 0; i < 8; ++i)
    {
        const int3 corner = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const float3 gradient = cloudHash(cell + corner, period, seed) * 2.0 - 1.0;
        corners[i] = dot(gradient, f - float3(corner));
    }
    const float x00 = lerp(corners[0], corners[1], u.x);
    const float x10 = lerp(corners[2], corners[3], u.x);
    const float x01 = lerp(corners[4], corners[5], u.x);
    const float x11 = lerp(corners[6], corners[7], u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z) * 1.5;
}

// Octaves at doubling frequency (and period), halving amplitude; uvw in [0, 1) tiles.
float cloudPerlinFbm(float3 uvw, int baseCells, int octaves, uint seed)
{
    float sum = 0.0;
    float amplitude = 1.0;
    float total = 0.0;
    int cells = baseCells;
    for (int octave = 0; octave < octaves; ++octave)
    {
        sum += amplitude * cloudPerlin(uvw * float(cells), cells, seed + uint(octave));
        total += amplitude;
        amplitude *= 0.5;
        cells *= 2;
    }
    return sum / total;
}

float cloudWorleyFbm(float3 uvw, int baseCells, uint seed)
{
    return cloudWorley(uvw * float(baseCells), baseCells, seed) * 0.625 +
           cloudWorley(uvw * float(baseCells * 2), baseCells * 2, seed + 1u) * 0.25 +
           cloudWorley(uvw * float(baseCells * 4), baseCells * 4, seed + 2u) * 0.125;
}

float cloudRemap(float value, float oldMin, float oldMax, float newMin, float newMax)
{
    return newMin + (value - oldMin) / max(oldMax - oldMin, 1e-6) * (newMax - newMin);
}
