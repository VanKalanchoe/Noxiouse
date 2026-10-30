#include "HLODBake.h"

#include "NoxCore/Math/Math.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <functional>
#include <numeric>
#include <optional>
#include <unordered_map>

#include <meshoptimizer.h>
#include <xatlas.h>

namespace Nox::HLOD
{
    namespace
    {
        float SrgbToLinear(float value)
        {
            return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
        }

        float LinearToSrgb(float value)
        {
            value = std::clamp(value, 0.0f, 1.0f);
            return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
        }

        uint8_t ToByte(float value)
        {
            return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }

        // The point of triangle abc closest to p, as a + u (b - a) + v (c - a) (Ericson, Real-Time Collision Detection 5.1.5).
        glm::vec3 ClosestPointOnTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, float& u, float& v)
        {
            const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
            const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
            if (d1 <= 0.0f && d2 <= 0.0f)
            {
                u = 0.0f, v = 0.0f;
                return a;
            }
            const glm::vec3 bp = p - b;
            const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
            if (d3 >= 0.0f && d4 <= d3)
            {
                u = 1.0f, v = 0.0f;
                return b;
            }
            const float vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
            {
                u = d1 / (d1 - d3), v = 0.0f;
                return a + ab * u;
            }
            const glm::vec3 cp = p - c;
            const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
            if (d6 >= 0.0f && d5 <= d6)
            {
                u = 0.0f, v = 1.0f;
                return c;
            }
            const float vb = d5 * d2 - d1 * d6;
            if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
            {
                u = 0.0f, v = d2 / (d2 - d6);
                return a + ac * v;
            }
            const float va = d3 * d6 - d5 * d4;
            if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f)
            {
                v = (d4 - d3) / ((d4 - d3) + (d5 - d6)), u = 1.0f - v;
                return b + (c - b) * v;
            }
            const float sum = va + vb + vc;
            if (!(sum > 0.0f)) // degenerate (no area): its nearest corner
            {
                const float da = glm::dot(ap, ap), db = glm::dot(bp, bp), dc = glm::dot(cp, cp);
                u = db < da && db <= dc ? 1.0f : 0.0f;
                v = dc < da && dc < db ? 1.0f : 0.0f;
                return a + ab * u + ac * v;
            }
            u = vb / sum, v = vc / sum;
            return a + ab * u + ac * v;
        }

        float DistanceSquaredToTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
        {
            float u, v;
            const glm::vec3 offset = p - ClosestPointOnTriangle(p, a, b, c, u, v);
            return glm::dot(offset, offset);
        }

        // --- Source triangles and a BVH over them (the bake's ray casts) ---

        struct SourceTriangle
        {
            glm::vec3 P0, E1, E2;   // corner and edges, cell space
            glm::vec3 N[3];         // vertex normals, world orientation
            glm::vec4 T[3];         // vertex tangents
            glm::vec2 UV[3];
            glm::vec4 Color[3] = { glm::vec4(1.0f), glm::vec4(1.0f), glm::vec4(1.0f) }; // vertex colors
            float UVPerUnit = 0.0f; // texture coordinates per world unit (which mip a sample of a given size reads)
            uint32_t Material = 0;
        };

        struct BvhNode
        {
            glm::vec3 Min, Max;
            uint32_t First = 0;  // leaf: first triangle (into the ordered list); inner: right child
            uint32_t Count = 0;  // leaf: triangles; inner: 0
        };

        class Bvh
        {
        public:
            void Build(std::vector<SourceTriangle>& triangles)
            {
                m_Order.resize(triangles.size());
                std::iota(m_Order.begin(), m_Order.end(), 0u);
                m_Centroids.resize(triangles.size());
                for (size_t index = 0; index < triangles.size(); ++index)
                {
                    const SourceTriangle& tri = triangles[index];
                    m_Centroids[index] = tri.P0 + (tri.E1 + tri.E2) * (1.0f / 3.0f);
                }
                m_Triangles = &triangles;
                m_Nodes.clear();
                m_Nodes.reserve(triangles.size() * 2 / LeafSize + 1);
                if (!triangles.empty())
                    BuildNode(0, static_cast<uint32_t>(triangles.size()));
                m_Centroids.clear();
            }

            // The nearest hit in (tMin, tMax): the triangle and its barycentrics (u, v), or false.
            bool Intersect(const glm::vec3& origin, const glm::vec3& direction, float tMin, float tMax, uint32_t& outTriangle, float& outT,
                           float& outU, float& outV) const
            {
                if (m_Nodes.empty())
                    return false;
                const glm::vec3 inverse(1.0f / (std::abs(direction.x) > 1e-12f ? direction.x : 1e-12f),
                                        1.0f / (std::abs(direction.y) > 1e-12f ? direction.y : 1e-12f),
                                        1.0f / (std::abs(direction.z) > 1e-12f ? direction.z : 1e-12f));
                std::array<uint32_t, 64> stack;
                uint32_t depth = 0;
                stack[depth++] = 0;
                bool hit = false;
                float closest = tMax;
                while (depth > 0)
                {
                    const BvhNode& node = m_Nodes[stack[--depth]];
                    if (!HitsBox(node, origin, inverse, tMin, closest))
                        continue;
                    if (node.Count > 0)
                    {
                        for (uint32_t index = node.First; index < node.First + node.Count; ++index)
                        {
                            const uint32_t triangle = m_Order[index];
                            float t, u, v;
                            if (IntersectTriangle((*m_Triangles)[triangle], origin, direction, t, u, v) && t > tMin && t < closest)
                            {
                                closest = t;
                                outTriangle = triangle;
                                outT = t;
                                outU = u;
                                outV = v;
                                hit = true;
                            }
                        }
                        continue;
                    }
                    const uint32_t self = static_cast<uint32_t>(&node - m_Nodes.data());
                    if (depth + 2 <= stack.size())
                    {
                        stack[depth++] = node.First;  // right
                        stack[depth++] = self + 1;    // left
                    }
                }
                return hit;
            }

            // The source point nearest to p within maxDistance: its triangle and barycentrics (u, v), or false.
            bool Closest(const glm::vec3& p, float maxDistance, uint32_t& outTriangle, float& outU, float& outV) const
            {
                if (m_Nodes.empty())
                    return false;
                std::array<uint32_t, 64> stack;
                uint32_t depth = 0;
                stack[depth++] = 0;
                float best = maxDistance * maxDistance;
                bool found = false;
                while (depth > 0)
                {
                    const BvhNode& node = m_Nodes[stack[--depth]];
                    const glm::vec3 outside = glm::max(glm::max(node.Min - p, p - node.Max), glm::vec3(0.0f));
                    if (glm::dot(outside, outside) > best)
                        continue;
                    if (node.Count > 0)
                    {
                        for (uint32_t index = node.First; index < node.First + node.Count; ++index)
                        {
                            const uint32_t triangle = m_Order[index];
                            const SourceTriangle& tri = (*m_Triangles)[triangle];
                            float u, v;
                            const glm::vec3 offset = p - ClosestPointOnTriangle(p, tri.P0, tri.P0 + tri.E1, tri.P0 + tri.E2, u, v);
                            const float squared = glm::dot(offset, offset);
                            if (squared < best)
                            {
                                best = squared;
                                outTriangle = triangle;
                                outU = u;
                                outV = v;
                                found = true;
                            }
                        }
                        continue;
                    }
                    const uint32_t self = static_cast<uint32_t>(&node - m_Nodes.data());
                    if (depth + 2 <= stack.size())
                    {
                        stack[depth++] = node.First;  // right
                        stack[depth++] = self + 1;    // left
                    }
                }
                return found;
            }

        private:
            static constexpr uint32_t LeafSize = 4;

            uint32_t BuildNode(uint32_t first, uint32_t count)
            {
                const uint32_t self = static_cast<uint32_t>(m_Nodes.size());
                m_Nodes.push_back({});
                glm::vec3 low(FLT_MAX), high(-FLT_MAX), centroidLow(FLT_MAX), centroidHigh(-FLT_MAX);
                for (uint32_t index = first; index < first + count; ++index)
                {
                    const SourceTriangle& tri = (*m_Triangles)[m_Order[index]];
                    for (const glm::vec3& corner : { tri.P0, tri.P0 + tri.E1, tri.P0 + tri.E2 })
                    {
                        low = glm::min(low, corner);
                        high = glm::max(high, corner);
                    }
                    centroidLow = glm::min(centroidLow, m_Centroids[m_Order[index]]);
                    centroidHigh = glm::max(centroidHigh, m_Centroids[m_Order[index]]);
                }
                m_Nodes[self].Min = low;
                m_Nodes[self].Max = high;
                const glm::vec3 extent = centroidHigh - centroidLow;
                const int axis = extent.x > extent.y ? (extent.x > extent.z ? 0 : 2) : (extent.y > extent.z ? 1 : 2);
                if (count <= LeafSize || extent[axis] <= 0.0f)
                {
                    m_Nodes[self].First = first;
                    m_Nodes[self].Count = count;
                    return self;
                }
                // Split at the median centroid along the longest axis (balanced: the stack depth stays small).
                const uint32_t middle = first + count / 2;
                std::nth_element(m_Order.begin() + first, m_Order.begin() + middle, m_Order.begin() + first + count,
                                 [&](uint32_t a, uint32_t b) { return m_Centroids[a][axis] < m_Centroids[b][axis]; });
                BuildNode(first, middle - first);              // left: self + 1
                const uint32_t right = BuildNode(middle, first + count - middle);
                m_Nodes[self].First = right;
                m_Nodes[self].Count = 0;
                return self;
            }

            static bool HitsBox(const BvhNode& node, const glm::vec3& origin, const glm::vec3& inverse, float tMin, float tMax)
            {
                const glm::vec3 t0 = (node.Min - origin) * inverse;
                const glm::vec3 t1 = (node.Max - origin) * inverse;
                const glm::vec3 near = glm::min(t0, t1), far = glm::max(t0, t1);
                const float enter = std::max({ near.x, near.y, near.z, tMin });
                const float exit = std::min({ far.x, far.y, far.z, tMax });
                return enter <= exit;
            }

            // Möller-Trumbore, both faces.
            static bool IntersectTriangle(const SourceTriangle& tri, const glm::vec3& origin, const glm::vec3& direction, float& t, float& u, float& v)
            {
                const glm::vec3 p = glm::cross(direction, tri.E2);
                const float determinant = glm::dot(tri.E1, p);
                if (std::abs(determinant) < 1e-12f)
                    return false;
                const float inverse = 1.0f / determinant;
                const glm::vec3 s = origin - tri.P0;
                u = glm::dot(s, p) * inverse;
                if (u < 0.0f || u > 1.0f)
                    return false;
                const glm::vec3 q = glm::cross(s, tri.E1);
                v = glm::dot(direction, q) * inverse;
                if (v < 0.0f || u + v > 1.0f)
                    return false;
                t = glm::dot(tri.E2, q) * inverse;
                return true;
            }

            std::vector<BvhNode> m_Nodes;
            std::vector<uint32_t> m_Order;
            std::vector<glm::vec3> m_Centroids;
            const std::vector<SourceTriangle>* m_Triangles = nullptr;
        };

        // A material evaluated at a surface point.
        struct Surface
        {
            glm::vec3 BaseColor{ 0.0f }; // linear
            float Alpha = 1.0f;
            float Roughness = 1.0f;
            float Metallic = 0.0f;
            glm::vec3 Normal{ 0.0f, 0.0f, 1.0f }; // world
            glm::vec3 Emissive{ 0.0f };           // linear
        };

        // footprint: the world size a sample stands for (0: the full-resolution texture).
        Surface Evaluate(const Material& material, const SourceTriangle& tri, float u, float v, float footprint = 0.0f)
        {
            auto sample = [&](const Texture& texture, glm::vec2 uv)
            {
                const float texels = footprint * tri.UVPerUnit * float(std::max(texture.Width, texture.Height));
                return texels > 1.0f ? texture.SampleLevel(uv, std::log2(texels)) : texture.Sample(uv);
            };
            const float w = 1.0f - u - v;
            const glm::vec2 uv = tri.UV[0] * w + tri.UV[1] * u + tri.UV[2] * v;
            glm::vec3 normal = glm::normalize(tri.N[0] * w + tri.N[1] * u + tri.N[2] * v);
            const glm::vec4 tangent = tri.T[0] * w + tri.T[1] * u + tri.T[2] * v;

            Surface surface;
            glm::vec4 base = material.BaseColorFactor * (tri.Color[0] * w + tri.Color[1] * u + tri.Color[2] * v);
            if (material.BaseColor && material.BaseColor->Valid())
            {
                const glm::vec4 texel = sample(*material.BaseColor, uv);
                base *= glm::vec4(SrgbToLinear(texel.r), SrgbToLinear(texel.g), SrgbToLinear(texel.b), texel.a);
            }
            surface.BaseColor = glm::vec3(base);
            surface.Alpha = base.a;

            if (material.SpecularGlossiness)
            {
                surface.Roughness = 1.0f - std::clamp(material.Glossiness, 0.0f, 1.0f);
                surface.Metallic = 0.0f;
            }
            else
            {
                surface.Roughness = material.RoughnessFactor;
                surface.Metallic = material.MetallicFactor;
                if (material.MetallicRoughness && material.MetallicRoughness->Valid())
                {
                    const glm::vec4 texel = sample(*material.MetallicRoughness, uv);
                    surface.Roughness *= texel.g;
                    surface.Metallic *= texel.b;
                }
            }

            surface.Normal = normal;
            if (material.Normal && material.Normal->Valid() && glm::dot(glm::vec3(tangent), glm::vec3(tangent)) > 1e-12f)
            {
                const glm::vec4 texel = sample(*material.Normal, uv);
                glm::vec3 local(texel.r * 2.0f - 1.0f, texel.g * 2.0f - 1.0f, texel.b * 2.0f - 1.0f);
                if (material.NormalTwoChannel)
                    local.z = std::sqrt(std::max(0.0f, 1.0f - local.x * local.x - local.y * local.y));
                // The tangent frame as the renderer builds it (Vertex.tangent: w the bitangent's sign).
                glm::vec3 t = glm::vec3(tangent);
                t = glm::normalize(t - normal * glm::dot(normal, t));
                const glm::vec3 b = glm::cross(normal, t) * (tangent.w < 0.0f ? -1.0f : 1.0f);
                const glm::vec3 shading = t * local.x + b * local.y + normal * local.z;
                if (glm::dot(shading, shading) > 1e-12f)
                    surface.Normal = glm::normalize(shading);
            }

            glm::vec3 emissive = material.EmissiveFactor;
            if (material.Emissive && material.Emissive->Valid())
            {
                const glm::vec4 texel = sample(*material.Emissive, uv);
                emissive *= glm::vec3(SrgbToLinear(texel.r), SrgbToLinear(texel.g), SrgbToLinear(texel.b));
            }
            surface.Emissive = emissive;
            return surface;
        }

        // --- The shell: Unreal's "Approximated Mesh" (its mesh approximation and ProxyLOD voxelise the same way) ---

        struct Shell
        {
            std::vector<glm::vec3> Positions;
            std::vector<uint32_t> Indices;
            float VoxelSize = 0.0f;
            float Offset = 0.0f;
        };

        // The parts as one closed surface. Their distances are sampled on a grid near them; everything the outside cannot reach is
        // solid (a building's walls enclose its inside); the surface is taken at `offset` off the parts by surface nets (a vertex
        // in each cell it passes, a quad across each grid edge it crosses). Closed and welded by construction: what the
        // simplification needs -- the parts' own open, overlapping triangles tore into shards and holes under it. Over maxPoints
        // the voxels grow, and the offset with them: it stays over half a voxel's diagonal, or the outside would leak between two
        // samples through a surface.
        Shell VoxelShell(const std::vector<glm::vec3>& positions, const std::vector<uint32_t>& indices, float offset, uint64_t maxPoints,
                         const std::function<void(uint32_t, const std::function<void(uint32_t)>&)>& parallelFor)
        {
            Shell shell;
            glm::vec3 low(FLT_MAX), high(-FLT_MAX);
            for (uint32_t index : indices)
            {
                low = glm::min(low, positions[index]);
                high = glm::max(high, positions[index]);
            }
            if (indices.size() < 3 || !(offset > 0.0f))
                return shell;

            constexpr float OffsetPerVoxel = 0.875f; // over half the diagonal (0.866)
            float h = offset / OffsetPerVoxel;
            float band = 0.0f;
            int64_t pad = 0, nx = 0, ny = 0, nz = 0;
            for (;;)
            {
                band = offset + 1.5f * h; // distances are kept up to here: the outside samples next to the solid ones need theirs
                pad = static_cast<int64_t>(std::ceil(band / h)) + 2;
                nx = static_cast<int64_t>(std::ceil((high.x - low.x) / h)) + 1 + 2 * pad;
                ny = static_cast<int64_t>(std::ceil((high.y - low.y) / h)) + 1 + 2 * pad;
                nz = static_cast<int64_t>(std::ceil((high.z - low.z) / h)) + 1 + 2 * pad;
                if (static_cast<uint64_t>(nx * ny * nz) <= maxPoints)
                    break;
                h *= 1.15f;
                offset = std::max(offset, h * OffsetPerVoxel);
            }
            shell.VoxelSize = h;
            shell.Offset = offset;
            const glm::vec3 gridOrigin = low - glm::vec3(static_cast<float>(pad) * h);
            const size_t pointCount = static_cast<size_t>(nx * ny * nz);
            auto at = [&](int64_t x, int64_t y, int64_t z) { return static_cast<size_t>(x + nx * (y + ny * z)); };
            auto point = [&](int64_t x, int64_t y, int64_t z) { return gridOrigin + glm::vec3(float(x), float(y), float(z)) * h; };

            // 1. Distances near the parts, quantised to a byte over the band (255: farther). Slabs of layers on workers, each
            //    writing only its own layers.
            std::vector<uint8_t> distance(pointCount, 255);
            const float toByte = 254.0f / band;
            const uint8_t solidByte = static_cast<uint8_t>(std::floor(offset * toByte));
            constexpr int64_t SlabLayers = 8;
            const int64_t slabCount = (nz + SlabLayers - 1) / SlabLayers;
            std::vector<std::vector<uint32_t>> slabs(static_cast<size_t>(slabCount));
            const uint32_t triangleCount = static_cast<uint32_t>(indices.size() / 3);
            for (uint32_t triangle = 0; triangle < triangleCount; ++triangle)
            {
                const float zMin = std::min({ positions[indices[triangle * 3]].z, positions[indices[triangle * 3 + 1]].z, positions[indices[triangle * 3 + 2]].z });
                const float zMax = std::max({ positions[indices[triangle * 3]].z, positions[indices[triangle * 3 + 1]].z, positions[indices[triangle * 3 + 2]].z });
                const int64_t first = std::max<int64_t>(0, static_cast<int64_t>(std::floor((zMin - band - gridOrigin.z) / h)));
                const int64_t last = std::min<int64_t>(nz - 1, static_cast<int64_t>(std::ceil((zMax + band - gridOrigin.z) / h)));
                for (int64_t slab = first / SlabLayers; slab <= last / SlabLayers; ++slab)
                    slabs[static_cast<size_t>(slab)].push_back(triangle);
            }
            parallelFor(static_cast<uint32_t>(slabCount), [&](uint32_t slab)
            {
                const int64_t slabFirst = int64_t(slab) * SlabLayers, slabLast = std::min(nz - 1, slabFirst + SlabLayers - 1);
                for (uint32_t triangle : slabs[slab])
                {
                    const glm::vec3& a = positions[indices[triangle * 3]];
                    const glm::vec3& b = positions[indices[triangle * 3 + 1]];
                    const glm::vec3& c = positions[indices[triangle * 3 + 2]];
                    const glm::vec3 lo = (glm::min(a, glm::min(b, c)) - glm::vec3(band) - gridOrigin) / h;
                    const glm::vec3 hi = (glm::max(a, glm::max(b, c)) + glm::vec3(band) - gridOrigin) / h;
                    const int64_t x0 = std::max<int64_t>(0, int64_t(std::floor(lo.x))), x1 = std::min<int64_t>(nx - 1, int64_t(std::ceil(hi.x)));
                    const int64_t y0 = std::max<int64_t>(0, int64_t(std::floor(lo.y))), y1 = std::min<int64_t>(ny - 1, int64_t(std::ceil(hi.y)));
                    const int64_t z0 = std::max<int64_t>(slabFirst, int64_t(std::floor(lo.z))), z1 = std::min<int64_t>(slabLast, int64_t(std::ceil(hi.z)));
                    for (int64_t z = z0; z <= z1; ++z)
                        for (int64_t y = y0; y <= y1; ++y)
                            for (int64_t x = x0; x <= x1; ++x)
                            {
                                const float squared = DistanceSquaredToTriangle(point(x, y, z), a, b, c);
                                if (squared >= band * band)
                                    continue;
                                const uint8_t value = static_cast<uint8_t>(std::min(254.0f, std::sqrt(squared) * toByte));
                                uint8_t& stored = distance[at(x, y, z)];
                                stored = std::min(stored, value);
                            }
                }
            });
            slabs = {};

            // 2. The outside: flood filled from the grid's corner (the padding is empty all round), a run along x at a time.
            std::vector<uint8_t> outside(pointCount, 0);
            auto open = [&](size_t index) { return !outside[index] && distance[index] > solidByte; };
            std::vector<std::array<int64_t, 3>> stack = { { 0, 0, 0 } };
            while (!stack.empty())
            {
                const auto [x, y, z] = stack.back();
                stack.pop_back();
                if (!open(at(x, y, z)))
                    continue;
                int64_t left = x, right = x;
                while (left > 0 && open(at(left - 1, y, z)))
                    --left;
                while (right < nx - 1 && open(at(right + 1, y, z)))
                    ++right;
                for (int64_t run = left; run <= right; ++run)
                    outside[at(run, y, z)] = 1;
                const int64_t neighbours[4][2] = { { y - 1, z }, { y + 1, z }, { y, z - 1 }, { y, z + 1 } };
                for (const auto& [ny2, nz2] : neighbours)
                {
                    if (ny2 < 0 || ny2 >= ny || nz2 < 0 || nz2 >= nz)
                        continue;
                    bool inRun = false;
                    for (int64_t run = left; run <= right; ++run)
                    {
                        const bool fillable = open(at(run, ny2, nz2));
                        if (fillable && !inRun)
                            stack.push_back({ run, ny2, nz2 });
                        inRun = fillable;
                    }
                }
            }

            // The field the surface is taken from: distance minus the offset, positive outside, negative in the solid.
            const float epsilon = h * 1e-4f;
            auto value = [&](size_t index)
            {
                const float d = float(distance[index]) / toByte - offset;
                return outside[index] ? std::max(d, epsilon) : std::min(d, -epsilon);
            };

            // 3. Surface nets: a vertex in each cell with corners on both sides, at the mean of its edges' crossings. Per layer on
            //    workers; joined in layer order, so the cells stay sorted (their vertex is found by binary search).
            static constexpr int Edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
                                                  { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
            std::vector<std::vector<uint64_t>> layerCells(static_cast<size_t>(nz - 1));
            std::vector<std::vector<glm::vec3>> layerVertices(static_cast<size_t>(nz - 1));
            parallelFor(static_cast<uint32_t>(nz - 1), [&](uint32_t layer)
            {
                const int64_t z = layer;
                for (int64_t y = 0; y + 1 < ny; ++y)
                    for (int64_t x = 0; x + 1 < nx; ++x)
                    {
                        size_t corners[8];
                        int outsideCount = 0;
                        for (int corner = 0; corner < 8; ++corner)
                        {
                            corners[corner] = at(x + (corner & 1), y + ((corner >> 1) & 1), z + ((corner >> 2) & 1));
                            outsideCount += outside[corners[corner]];
                        }
                        if (outsideCount == 0 || outsideCount == 8)
                            continue;
                        glm::vec3 sum(0.0f);
                        int crossings = 0;
                        for (const auto& edge : Edges)
                        {
                            if (outside[corners[edge[0]]] == outside[corners[edge[1]]])
                                continue;
                            const float a = value(corners[edge[0]]), b = value(corners[edge[1]]);
                            const float t = a / (a - b);
                            const glm::vec3 pa(float(edge[0] & 1), float((edge[0] >> 1) & 1), float((edge[0] >> 2) & 1));
                            const glm::vec3 pb(float(edge[1] & 1), float((edge[1] >> 1) & 1), float((edge[1] >> 2) & 1));
                            sum += pa + (pb - pa) * t;
                            ++crossings;
                        }
                        layerCells[layer].push_back(at(x, y, z));
                        layerVertices[layer].push_back(point(x, y, z) + sum / float(crossings) * h);
                    }
            });
            std::vector<uint64_t> cells;
            for (size_t layer = 0; layer < layerCells.size(); ++layer)
            {
                cells.insert(cells.end(), layerCells[layer].begin(), layerCells[layer].end());
                shell.Positions.insert(shell.Positions.end(), layerVertices[layer].begin(), layerVertices[layer].end());
            }
            layerCells = {};
            layerVertices = {};
            auto vertexOf = [&](int64_t x, int64_t y, int64_t z) -> int64_t
            {
                if (x < 0 || y < 0 || z < 0 || x + 1 >= nx || y + 1 >= ny || z + 1 >= nz)
                    return -1;
                const uint64_t cell = at(x, y, z);
                const auto found = std::lower_bound(cells.begin(), cells.end(), cell);
                return found != cells.end() && *found == cell ? int64_t(found - cells.begin()) : -1;
            };

            // 4. A quad across each grid edge between the outside and the solid, from the four cells around it, facing the outside
            //    (counter-clockwise seen from there); split along its shorter diagonal.
            std::vector<std::vector<uint32_t>> layerIndices(static_cast<size_t>(nz));
            parallelFor(static_cast<uint32_t>(nz), [&](uint32_t layer)
            {
                const int64_t z = layer;
                std::vector<uint32_t>& out = layerIndices[layer];
                for (int64_t y = 0; y < ny; ++y)
                    for (int64_t x = 0; x < nx; ++x)
                    {
                        const int64_t p[3] = { x, y, z };
                        const bool here = outside[at(x, y, z)] != 0;
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            int64_t q[3] = { x, y, z };
                            ++q[axis];
                            if (q[axis] >= (axis == 0 ? nx : axis == 1 ? ny : nz) || (outside[at(q[0], q[1], q[2])] != 0) == here)
                                continue;
                            const int u = (axis + 1) % 3, v = (axis + 2) % 3; // u x v = axis
                            int64_t c[4][3];
                            for (int k = 0; k < 4; ++k)
                                for (int i = 0; i < 3; ++i)
                                    c[k][i] = p[i];
                            --c[0][u]; --c[0][v];
                            --c[1][v];
                            --c[3][u];
                            int64_t quad[4];
                            bool complete = true;
                            for (int k = 0; k < 4; ++k)
                            {
                                quad[k] = vertexOf(c[k][0], c[k][1], c[k][2]);
                                complete &= quad[k] >= 0;
                            }
                            if (!complete)
                                continue;
                            if (here) // the solid is on the +axis side: the surface faces -axis
                                std::swap(quad[1], quad[3]);
                            const float diagonal02 = glm::length(shell.Positions[quad[0]] - shell.Positions[quad[2]]);
                            const float diagonal13 = glm::length(shell.Positions[quad[1]] - shell.Positions[quad[3]]);
                            const uint32_t i0 = uint32_t(quad[0]), i1 = uint32_t(quad[1]), i2 = uint32_t(quad[2]), i3 = uint32_t(quad[3]);
                            if (diagonal02 <= diagonal13)
                                out.insert(out.end(), { i0, i1, i2, i0, i2, i3 });
                            else
                                out.insert(out.end(), { i0, i1, i3, i1, i2, i3 });
                        }
                    }
            });
            for (const std::vector<uint32_t>& layer : layerIndices)
                shell.Indices.insert(shell.Indices.end(), layer.begin(), layer.end());
            return shell;
        }

        uint32_t NextPowerOfTwo(uint32_t value)
        {
            uint32_t power = 1;
            while (power < value && power < (1u << 30))
                power <<= 1;
            return power;
        }
    }

    glm::vec4 Texture::Sample(glm::vec2 uv) const
    {
        if (!Valid())
            return glm::vec4(1.0f);
        const float x = (uv.x - std::floor(uv.x)) * static_cast<float>(Width) - 0.5f;
        const float y = (uv.y - std::floor(uv.y)) * static_cast<float>(Height) - 0.5f;
        const float fx = std::floor(x), fy = std::floor(y);
        const float ax = x - fx, ay = y - fy;
        auto texel = [&](int32_t tx, int32_t ty)
        {
            tx = ((tx % static_cast<int32_t>(Width)) + static_cast<int32_t>(Width)) % static_cast<int32_t>(Width);
            ty = ((ty % static_cast<int32_t>(Height)) + static_cast<int32_t>(Height)) % static_cast<int32_t>(Height);
            const uint8_t* p = &Texels[(size_t(ty) * Width + size_t(tx)) * 4];
            return glm::vec4(p[0], p[1], p[2], p[3]) * (1.0f / 255.0f);
        };
        const int32_t x0 = static_cast<int32_t>(fx), y0 = static_cast<int32_t>(fy);
        const glm::vec4 top = glm::mix(texel(x0, y0), texel(x0 + 1, y0), ax);
        const glm::vec4 bottom = glm::mix(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), ax);
        return glm::mix(top, bottom, ay);
    }

    glm::vec4 Texture::SampleLevel(glm::vec2 uv, float lod) const
    {
        lod = std::clamp(lod, 0.0f, float(Mips.size()));
        const uint32_t level = static_cast<uint32_t>(lod);
        const float blend = lod - float(level);
        auto at = [&](uint32_t index) -> const Texture& { return index == 0 ? *this : Mips[std::min<size_t>(index, Mips.size()) - 1]; };
        const glm::vec4 fine = at(level).Sample(uv);
        return blend > 0.0f && level < Mips.size() ? glm::mix(fine, at(level + 1).Sample(uv), blend) : fine;
    }

    void Texture::BuildMips()
    {
        Mips.clear();
        if (!Valid())
            return;
        size_t levels = 0;
        for (uint32_t extent = std::max(Width, Height); extent > 1; extent /= 2)
            ++levels;
        Mips.reserve(levels); // (from points into it while it grows)
        const Texture* from = this;
        while (from->Width > 1 || from->Height > 1)
        {
            Texture next;
            next.Width = std::max(1u, from->Width / 2);
            next.Height = std::max(1u, from->Height / 2);
            next.Texels.resize(size_t(next.Width) * next.Height * 4);
            for (uint32_t y = 0; y < next.Height; ++y)
                for (uint32_t x = 0; x < next.Width; ++x)
                    for (uint32_t channel = 0; channel < 4; ++channel)
                    {
                        uint32_t sum = 0;
                        for (uint32_t dy = 0; dy < 2; ++dy)
                            for (uint32_t dx = 0; dx < 2; ++dx)
                            {
                                const uint32_t sx = std::min(from->Width - 1, x * 2 + dx), sy = std::min(from->Height - 1, y * 2 + dy);
                                sum += from->Texels[(size_t(sy) * from->Width + sx) * 4 + channel];
                            }
                        next.Texels[(size_t(y) * next.Width + x) * 4 + channel] = static_cast<uint8_t>((sum + 2) / 4);
                    }
            Mips.push_back(std::move(next));
            from = &Mips.back();
        }
    }

    Result Build(const std::vector<SourceMesh>& meshes, const std::vector<Material>& materials, const Settings& settings)
    {
        Result result;
        auto parallelFor = [&](uint32_t count, const std::function<void(uint32_t)>& body)
        {
            if (settings.ParallelFor)
                settings.ParallelFor(count, body);
            else
                for (uint32_t index = 0; index < count; ++index)
                    body(index);
        };

        // 1. The sources in world space; the cell's centre is the origin (floats keep their precision far from the world's).
        glm::dvec3 worldLow(DBL_MAX), worldHigh(-DBL_MAX);
        for (const SourceMesh& mesh : meshes)
        {
            for (const glm::vec3& position : mesh.Positions)
            {
                const glm::dvec3 world = glm::dvec3(mesh.Transform * glm::dvec4(glm::dvec3(position), 1.0));
                worldLow = glm::min(worldLow, world);
                worldHigh = glm::max(worldHigh, world);
            }
        }
        if (worldLow.x > worldHigh.x)
        {
            result.Error = "no geometry";
            return result;
        }
        const glm::dvec3 origin = (worldLow + worldHigh) * 0.5;
        result.Origin = origin;

        const float minPart = settings.MinPartSize > 0.0f ? settings.MinPartSize : 2.0f * settings.Error;
        std::vector<SourceTriangle> sources;
        std::vector<glm::vec3> mergedPositions;
        std::vector<uint32_t> mergedIndices;
        for (const SourceMesh& mesh : meshes)
        {
            if (mesh.Positions.empty() || mesh.Indices.size() < 3 || mesh.Material >= materials.size())
                continue;
            const glm::mat3 linear = Math::Basis(mesh.Transform);
            const glm::mat4 relative = Math::RelativeTo(mesh.Transform, origin); // (the cell's own space: exact in float)
            const glm::mat3 normalMatrix = glm::transpose(glm::inverse(linear));
            const float determinant = glm::determinant(linear);
            const bool mirrored = determinant < 0.0f;

            std::vector<glm::vec3> positions(mesh.Positions.size());
            glm::vec3 low(FLT_MAX), high(-FLT_MAX);
            for (size_t index = 0; index < positions.size(); ++index)
            {
                positions[index] = glm::vec3(relative * glm::vec4(mesh.Positions[index], 1.0f));
                low = glm::min(low, positions[index]);
                high = glm::max(high, positions[index]);
            }
            const glm::vec3 extent = high - low;
            const bool inGeometry = std::max({ extent.x, extent.y, extent.z }) >= minPart;
            const uint32_t base = static_cast<uint32_t>(mergedPositions.size());
            if (inGeometry)
                mergedPositions.insert(mergedPositions.end(), positions.begin(), positions.end());

            for (size_t index = 0; index + 2 < mesh.Indices.size(); index += 3)
            {
                uint32_t corner[3] = { mesh.Indices[index], mesh.Indices[index + 1], mesh.Indices[index + 2] };
                if (corner[0] >= positions.size() || corner[1] >= positions.size() || corner[2] >= positions.size())
                    continue;
                if (mirrored)
                    std::swap(corner[1], corner[2]); // a mirroring transform turns the winding over
                if (inGeometry)
                    for (uint32_t c : corner)
                        mergedIndices.push_back(base + c);

                SourceTriangle tri;
                tri.P0 = positions[corner[0]];
                tri.E1 = positions[corner[1]] - tri.P0;
                tri.E2 = positions[corner[2]] - tri.P0;
                for (int c = 0; c < 3; ++c)
                {
                    const uint32_t vertex = corner[c];
                    const glm::vec3 normal = vertex < mesh.Normals.size() ? mesh.Normals[vertex] : glm::cross(tri.E1, tri.E2);
                    tri.N[c] = glm::normalize(normalMatrix * normal);
                    if (vertex < mesh.Tangents.size())
                    {
                        const glm::vec4 tangent = mesh.Tangents[vertex];
                        tri.T[c] = glm::vec4(linear * glm::vec3(tangent), (mirrored ? -1.0f : 1.0f) * (tangent.w < 0.0f ? -1.0f : 1.0f));
                    }
                    else
                        tri.T[c] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                    tri.UV[c] = vertex < mesh.UVs.size() ? mesh.UVs[vertex] : glm::vec2(0.0f);
                    tri.Color[c] = vertex < mesh.Colors.size() ? mesh.Colors[vertex] : glm::vec4(1.0f);
                }
                if (!std::isfinite(tri.N[0].x) || glm::dot(glm::cross(tri.E1, tri.E2), glm::cross(tri.E1, tri.E2)) <= 0.0f)
                    continue; // degenerate
                tri.Material = mesh.Material;
                {
                    const glm::vec2 du = tri.UV[1] - tri.UV[0], dv = tri.UV[2] - tri.UV[0];
                    const float uvArea = std::abs(du.x * dv.y - dv.x * du.y);
                    const float worldArea = glm::length(glm::cross(tri.E1, tri.E2));
                    tri.UVPerUnit = worldArea > 0.0f ? std::sqrt(uvArea / worldArea) : 0.0f;
                }
                sources.push_back(tri);
            }
        }
        result.SourceTriangles = static_cast<uint32_t>(sources.size());
        if (mergedIndices.empty())
        {
            result.Error = "every part is smaller than the HLOD's error";
            return result;
        }

        // 2. One closed shell around the parts (Remesh: Unreal's Approximated Mesh), else the parts welded by position; simplified to
        //    what is left of the error, then, over the triangle budget, cut down to it (sloppy: joins parts).
        std::vector<glm::vec3> welded;
        std::vector<uint32_t> indices;
        float simplifyError = settings.Error;
        float shellOffset = 0.0f; // how far the shell stands off the parts (0: the parts themselves)
        if (settings.Remesh)
        {
            // Half the error is the shell's distance off the parts, half the simplification's.
            Shell shell = VoxelShell(mergedPositions, mergedIndices, settings.Error * 0.5f, settings.MaxVoxels, parallelFor);
            result.VoxelSize = shell.VoxelSize;
            simplifyError = std::max(settings.Error - shell.Offset, settings.Error * 0.25f);
            shellOffset = shell.Offset + shell.VoxelSize; // (surface nets place a vertex anywhere in its voxel)
            welded = std::move(shell.Positions);
            indices = std::move(shell.Indices);
            if (indices.size() < 3)
            {
                result.Error = "the voxel shell is empty";
                return result;
            }
        }
        else
        {
            std::vector<uint32_t> remap(mergedPositions.size());
            const size_t uniqueCount = meshopt_generateVertexRemap(remap.data(), mergedIndices.data(), mergedIndices.size(), mergedPositions.data(),
                                                                   mergedPositions.size(), sizeof(glm::vec3));
            welded.resize(uniqueCount);
            meshopt_remapVertexBuffer(welded.data(), mergedPositions.data(), mergedPositions.size(), sizeof(glm::vec3), remap.data());
            indices.resize(mergedIndices.size());
            meshopt_remapIndexBuffer(indices.data(), mergedIndices.data(), mergedIndices.size(), remap.data());
        }
        mergedPositions = {};
        mergedIndices = {};

        std::vector<uint32_t> simplified(indices.size());
        float resultError = 0.0f;
        size_t simplifiedCount = meshopt_simplify(simplified.data(), indices.data(), indices.size(), &welded[0].x, welded.size(), sizeof(glm::vec3),
                                                  0, simplifyError, meshopt_SimplifyErrorAbsolute, &resultError);
        simplified.resize(simplifiedCount);
        const size_t budget = size_t(std::max(settings.MaxTriangles, 16u)) * 3;
        if (simplified.size() > budget)
        {
            std::vector<uint32_t> sloppy(simplified.size());
            const size_t sloppyCount = meshopt_simplifySloppy(sloppy.data(), simplified.data(), simplified.size(), &welded[0].x, welded.size(),
                                                              sizeof(glm::vec3), budget, FLT_MAX, &resultError);
            sloppy.resize(sloppyCount);
            if (!sloppy.empty())
                simplified = std::move(sloppy);
        }
        if (simplified.size() < 3)
        {
            result.Error = "nothing left after simplification";
            return result;
        }

        // 3. Hard edges: each corner takes the normals of the faces around its vertex within HardEdgeAngle of its own face.
        const size_t faceCount = simplified.size() / 3;
        std::vector<glm::vec3> faceNormals(faceCount); // area weighted
        for (size_t face = 0; face < faceCount; ++face)
        {
            const glm::vec3& a = welded[simplified[face * 3 + 0]];
            const glm::vec3& b = welded[simplified[face * 3 + 1]];
            const glm::vec3& c = welded[simplified[face * 3 + 2]];
            faceNormals[face] = glm::cross(b - a, c - a) * 0.5f;
        }
        std::vector<std::vector<uint32_t>> facesOfVertex(welded.size());
        for (size_t face = 0; face < faceCount; ++face)
            for (int c = 0; c < 3; ++c)
                facesOfVertex[simplified[face * 3 + c]].push_back(static_cast<uint32_t>(face));
        const float cosLimit = std::cos(glm::radians(settings.HardEdgeAngle));
        std::vector<glm::vec3> splitPositions, splitNormals;
        std::vector<uint32_t> splitIndices(simplified.size());
        {
            struct Key
            {
                uint32_t Vertex;
                int32_t X, Y, Z;
                bool operator==(const Key& other) const { return Vertex == other.Vertex && X == other.X && Y == other.Y && Z == other.Z; }
            };
            struct KeyHash
            {
                size_t operator()(const Key& key) const
                {
                    return (size_t(key.Vertex) * 73856093u) ^ (size_t(uint32_t(key.X)) * 19349663u) ^ (size_t(uint32_t(key.Y)) * 83492791u) ^ size_t(uint32_t(key.Z));
                }
            };
            std::unordered_map<Key, uint32_t, KeyHash> vertexOf;
            for (size_t face = 0; face < faceCount; ++face)
            {
                const glm::vec3 own = faceNormals[face];
                const float ownLength = glm::length(own);
                for (int c = 0; c < 3; ++c)
                {
                    const uint32_t vertex = simplified[face * 3 + c];
                    glm::vec3 sum(0.0f);
                    for (uint32_t other : facesOfVertex[vertex])
                    {
                        const glm::vec3 candidate = faceNormals[other];
                        const float length = glm::length(candidate);
                        if (ownLength > 0.0f && length > 0.0f && glm::dot(own, candidate) >= cosLimit * ownLength * length)
                            sum += candidate;
                    }
                    const glm::vec3 normal = glm::dot(sum, sum) > 0.0f ? glm::normalize(sum) : (ownLength > 0.0f ? own / ownLength : glm::vec3(0, 1, 0));
                    const Key key{ vertex, int32_t(std::lround(normal.x * 1024.0f)), int32_t(std::lround(normal.y * 1024.0f)), int32_t(std::lround(normal.z * 1024.0f)) };
                    auto [entry, added] = vertexOf.try_emplace(key, static_cast<uint32_t>(splitPositions.size()));
                    if (added)
                    {
                        splitPositions.push_back(welded[vertex]);
                        splitNormals.push_back(normal);
                    }
                    splitIndices[face * 3 + c] = entry->second;
                }
            }
        }

        // 4. The UV atlas (xatlas), one atlas: its scale from the surface and the texel density, shrunk until it fits the largest texture.
        double area = 0.0;
        for (const glm::vec3& normal : faceNormals)
            area += glm::length(normal);
        if (area <= 0.0)
        {
            result.Error = "no surface";
            return result;
        }
        const float wanted = static_cast<float>(std::sqrt(area)) * settings.TexelsPerUnit / 0.7f; // ~70 % of the atlas is used
        uint32_t size = std::clamp(NextPowerOfTwo(static_cast<uint32_t>(std::ceil(wanted))), settings.MinTextureSize, settings.MaxTextureSize);

        xatlas::Atlas* atlas = xatlas::Create();
        xatlas::MeshDecl declaration;
        declaration.vertexPositionData = splitPositions.data();
        declaration.vertexPositionStride = sizeof(glm::vec3);
        declaration.vertexNormalData = splitNormals.data();
        declaration.vertexNormalStride = sizeof(glm::vec3);
        declaration.vertexCount = static_cast<uint32_t>(splitPositions.size());
        declaration.indexData = splitIndices.data();
        declaration.indexCount = static_cast<uint32_t>(splitIndices.size());
        declaration.indexFormat = xatlas::IndexFormat::UInt32;
        if (xatlas::AddMesh(atlas, declaration) != xatlas::AddMeshError::Success)
        {
            xatlas::Destroy(atlas);
            result.Error = "xatlas could not take the mesh";
            return result;
        }
        xatlas::ComputeCharts(atlas);
        xatlas::PackOptions pack;
        pack.padding = 2;
        pack.bilinear = true;
        pack.blockAlign = true;
        // The texture's size comes from the density; the charts are then scaled to fill it (at least the density asked for, the
        // power of two rounding's room used rather than left empty): packed, measured, scaled, until the atlas spans 90-100 %.
        // Every chart also costs its padding and block alignment whatever its scale: a cell simplified down to its budget is mostly
        // small separate pieces (thousands of charts), whose overhead alone can outgrow the size the surface asks for. Then the
        // texture grows (up to the largest size) until they fit.
        for (;;)
        {
            pack.texelsPerUnit = static_cast<float>(std::sqrt(0.7 * double(size) * size / area));
            for (int attempt = 0; attempt < 12; ++attempt)
            {
                xatlas::PackCharts(atlas, pack);
                const uint32_t largest = std::max(atlas->width, atlas->height);
                if (largest <= size && largest * 10 >= size * 9)
                    break;
                pack.texelsPerUnit *= 0.97f * static_cast<float>(size) / static_cast<float>(std::max(largest, 1u));
            }
            if (std::max(atlas->width, atlas->height) > size)
            {
                pack.texelsPerUnit *= 0.9f * static_cast<float>(size) / static_cast<float>(std::max(atlas->width, atlas->height));
                xatlas::PackCharts(atlas, pack);
            }
            if (std::max(atlas->width, atlas->height) <= size || size >= settings.MaxTextureSize)
                break;
            size *= 2;
        }
        if (atlas->meshCount != 1 || std::max(atlas->width, atlas->height) > size || atlas->width == 0)
        {
            xatlas::Destroy(atlas);
            result.Error = "the UV atlas does not fit";
            return result;
        }
        const float texelsPerUnit = pack.texelsPerUnit; // the atlas' density: what a texel of the HLOD stands for
        const xatlas::Mesh& laidOut = atlas->meshes[0];
        result.Positions.resize(laidOut.vertexCount);
        result.Normals.resize(laidOut.vertexCount);
        result.UVs.resize(laidOut.vertexCount);
        for (uint32_t index = 0; index < laidOut.vertexCount; ++index)
        {
            const xatlas::Vertex& vertex = laidOut.vertexArray[index];
            result.Positions[index] = splitPositions[vertex.xref];
            result.Normals[index] = splitNormals[vertex.xref];
            result.UVs[index] = glm::vec2(vertex.uv[0], vertex.uv[1]) / static_cast<float>(size);
        }
        result.Indices.assign(laidOut.indexArray, laidOut.indexArray + laidOut.indexCount);
        xatlas::Destroy(atlas);
        result.TextureSize = size;

        // 5. Tangents from the atlas (Lengyel): written with the mesh, so the baked normal map is read in exactly this frame.
        {
            std::vector<glm::vec3> tangents(result.Positions.size(), glm::vec3(0.0f)), bitangents(result.Positions.size(), glm::vec3(0.0f));
            for (size_t index = 0; index + 2 < result.Indices.size(); index += 3)
            {
                const uint32_t i0 = result.Indices[index], i1 = result.Indices[index + 1], i2 = result.Indices[index + 2];
                const glm::vec3 e1 = result.Positions[i1] - result.Positions[i0], e2 = result.Positions[i2] - result.Positions[i0];
                const glm::vec2 d1 = result.UVs[i1] - result.UVs[i0], d2 = result.UVs[i2] - result.UVs[i0];
                const float determinant = d1.x * d2.y - d2.x * d1.y;
                if (std::abs(determinant) < 1e-20f)
                    continue;
                const float r = 1.0f / determinant;
                const glm::vec3 t = (e1 * d2.y - e2 * d1.y) * r;
                const glm::vec3 b = (e2 * d1.x - e1 * d2.x) * r;
                for (uint32_t vertex : { i0, i1, i2 })
                {
                    tangents[vertex] += t;
                    bitangents[vertex] += b;
                }
            }
            result.Tangents.resize(result.Positions.size());
            for (size_t index = 0; index < result.Positions.size(); ++index)
            {
                const glm::vec3 n = result.Normals[index];
                glm::vec3 t = tangents[index] - n * glm::dot(n, tangents[index]);
                if (glm::dot(t, t) < 1e-20f)
                {
                    // No UV gradient: any frame around the normal.
                    t = std::abs(n.x) < 0.9f ? glm::cross(n, glm::vec3(1, 0, 0)) : glm::cross(n, glm::vec3(0, 1, 0));
                }
                t = glm::normalize(t);
                const float sign = glm::dot(glm::cross(n, t), bitangents[index]) < 0.0f ? -1.0f : 1.0f;
                result.Tangents[index] = glm::vec4(t, sign);
            }
        }

        // 6. The bake: every texel the atlas covers is a patch of the HLOD, sampled n x n; each sample finds the source surface it
        //    stands for (bakeSample). Per texel only the HLOD triangle covering its centre is kept (2048^2 texels of full frames would
        //    be hundreds of MB).
        Bvh bvh;
        bvh.Build(sources);
        const size_t texelCount = size_t(size) * size;
        constexpr uint32_t NoTriangle = UINT32_MAX;
        std::vector<uint32_t> texelTriangle(texelCount, NoTriangle);
        auto barycentrics = [&](uint32_t triangle, float px, float py, float& w0, float& w1, float& w2)
        {
            const glm::vec2 a = result.UVs[result.Indices[triangle * 3 + 0]] * float(size);
            const glm::vec2 b = result.UVs[result.Indices[triangle * 3 + 1]] * float(size);
            const glm::vec2 c = result.UVs[result.Indices[triangle * 3 + 2]] * float(size);
            const float denominator = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
            if (std::abs(denominator) < 1e-12f)
                return false;
            w0 = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / denominator;
            w1 = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / denominator;
            w2 = 1.0f - w0 - w1;
            return true;
        };
        for (uint32_t triangle = 0; triangle * 3 + 2 < result.Indices.size(); ++triangle)
        {
            const glm::vec2 a = result.UVs[result.Indices[triangle * 3 + 0]] * float(size);
            const glm::vec2 b = result.UVs[result.Indices[triangle * 3 + 1]] * float(size);
            const glm::vec2 c = result.UVs[result.Indices[triangle * 3 + 2]] * float(size);
            const int32_t x0 = std::max(0, int32_t(std::floor(std::min({ a.x, b.x, c.x }))));
            const int32_t x1 = std::min(int32_t(size) - 1, int32_t(std::ceil(std::max({ a.x, b.x, c.x }))));
            const int32_t y0 = std::max(0, int32_t(std::floor(std::min({ a.y, b.y, c.y }))));
            const int32_t y1 = std::min(int32_t(size) - 1, int32_t(std::ceil(std::max({ a.y, b.y, c.y }))));
            for (int32_t y = y0; y <= y1; ++y)
            {
                for (int32_t x = x0; x <= x1; ++x)
                {
                    float w0, w1, w2;
                    if (!barycentrics(triangle, float(x) + 0.5f, float(y) + 0.5f, w0, w1, w2) || w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
                        continue;
                    texelTriangle[size_t(y) * size + size_t(x)] = triangle;
                }
            }
        }

        enum class TexelState : uint8_t { Empty, Baked, CutOut };
        std::vector<TexelState> states(texelCount, TexelState::Empty);
        result.BaseColor.assign(texelCount * 4, 0);
        result.Normal.assign(texelCount * 4, 0);
        result.MetallicRoughness.assign(texelCount * 4, 0);
        std::vector<glm::vec3> emissive(texelCount, glm::vec3(0.0f)); // linear, encoded once its brightest texel is known
        // How far from the HLOD's surface its source can be: twice the error, or more where the shell stands farther off (voxels grown
        // for a large cell) -- searching only the error left most of such a shell without its source.
        const float search = std::max({ settings.Error * 2.0f, (shellOffset + simplifyError) * 1.5f, 1e-3f });
        // n x n samples per texel, each the size of its share of the texel (the mip level its textures are read at).
        const uint32_t samplesPerAxis = std::max(1u, settings.SamplesPerTexel);
        const float footprint = 1.0f / std::max(texelsPerUnit * float(samplesPerAxis), 1e-12f);
        enum class SampleKind { None, Hit, Cut };

        // One point of the HLOD: the source surface it stands for, found along the normal from outside in first (the visible
        // surface), else from inside out. Alpha-masked hits that are cut away let the ray go on, and so do see-through ones (glass),
        // adding what they cover of what is behind them. Where neither way finds a surface (a thin part's rim faces along the part,
        // a ray there runs past it): the source point nearest to it.
        auto bakeSample = [&](const glm::vec3& position, const glm::vec3& frameNormal, Surface& out) -> SampleKind
        {
            bool cut = false;
            for (int pass = 0; pass < 2; ++pass)
            {
                const glm::vec3 direction = pass == 0 ? -frameNormal : frameNormal;
                const glm::vec3 rayOrigin = position - direction * search;
                float tMin = 0.0f;
                glm::vec3 seenColor(0.0f), seenEmissive(0.0f);
                float through = 1.0f; // what the glass in front lets through
                std::optional<Surface> front;
                for (int step = 0; step < 8; ++step)
                {
                    uint32_t hitTriangle = 0;
                    float t = 0.0f, u = 0.0f, v = 0.0f;
                    const bool hit = bvh.Intersect(rayOrigin, direction, tMin, search * 2.0f, hitTriangle, t, u, v);
                    if (!hit && !front)
                        break;
                    Surface surface;
                    if (hit)
                    {
                        const SourceTriangle& tri = sources[hitTriangle];
                        const Material& material = materials[tri.Material];
                        surface = Evaluate(material, tri, u, v, footprint);
                        if (material.AlphaMask && surface.Alpha < material.AlphaCutoff)
                        {
                            cut = true;
                            tMin = t + search * 1e-4f;
                            continue;
                        }
                        if (material.Translucent && step + 1 < 8)
                        {
                            const float coverage = std::clamp(surface.Alpha * (1.0f - material.Transmission), 0.0f, 1.0f);
                            seenColor += surface.BaseColor * (through * coverage);
                            seenEmissive += surface.Emissive * through;
                            through *= 1.0f - coverage;
                            if (!front)
                                front = surface;
                            tMin = t + search * 1e-4f;
                            continue;
                        }
                        seenColor += surface.BaseColor * through;
                        seenEmissive += surface.Emissive * through;
                    }
                    // (no hit behind the glass: nothing more to see through it)
                    if (front)
                    {
                        surface.BaseColor = seenColor;
                        surface.Emissive = seenEmissive;
                        surface.Roughness = front->Roughness;
                        surface.Metallic = front->Metallic;
                        surface.Normal = front->Normal;
                    }
                    out = surface;
                    return SampleKind::Hit;
                }
            }
            if (cut)
                return SampleKind::Cut;
            uint32_t nearest = 0;
            float u = 0.0f, v = 0.0f;
            if (!bvh.Closest(position, search, nearest, u, v))
                return SampleKind::None;
            const SourceTriangle& tri = sources[nearest];
            const Material& material = materials[tri.Material];
            out = Evaluate(material, tri, u, v, footprint);
            if (material.AlphaMask && out.Alpha < material.AlphaCutoff)
                return SampleKind::Cut;
            if (material.Translucent) // nothing behind it is looked for here: what it covers of the dark
                out.BaseColor *= std::clamp(out.Alpha * (1.0f - material.Transmission), 0.0f, 1.0f);
            return SampleKind::Hit;
        };

        parallelFor(size, [&](uint32_t row)
        {
            for (uint32_t column = 0; column < size; ++column)
            {
                const size_t texel = size_t(row) * size + column;
                const uint32_t triangle = texelTriangle[texel];
                if (triangle == NoTriangle)
                    continue;
                const uint32_t i0 = result.Indices[triangle * 3 + 0], i1 = result.Indices[triangle * 3 + 1], i2 = result.Indices[triangle * 3 + 2];

                // The texel's samples (those inside its triangle; at least its centre, which is): averaged, the source normal in the
                // HLOD's tangent frame at each sample. Coverage: the share of samples not cut away.
                glm::vec3 color(0.0f), glow(0.0f), local(0.0f);
                float roughness = 0.0f, metallic = 0.0f;
                uint32_t hits = 0, cuts = 0, taken = 0;
                auto take = [&](float px, float py)
                {
                    float w0, w1, w2;
                    if (!barycentrics(triangle, px, py, w0, w1, w2) || w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
                        return;
                    ++taken;
                    const glm::vec3 position = result.Positions[i0] * w0 + result.Positions[i1] * w1 + result.Positions[i2] * w2;
                    const glm::vec3 frameNormal = glm::normalize(result.Normals[i0] * w0 + result.Normals[i1] * w1 + result.Normals[i2] * w2);
                    const glm::vec4 frameTangent = result.Tangents[i0] * w0 + result.Tangents[i1] * w1 + result.Tangents[i2] * w2;
                    Surface surface;
                    const SampleKind kind = bakeSample(position, frameNormal, surface);
                    if (kind == SampleKind::Cut)
                        ++cuts;
                    if (kind != SampleKind::Hit)
                        return;
                    ++hits;
                    color += surface.BaseColor;
                    glow += surface.Emissive;
                    roughness += surface.Roughness;
                    metallic += surface.Metallic;
                    glm::vec3 tangent = glm::vec3(frameTangent) - frameNormal * glm::dot(frameNormal, glm::vec3(frameTangent));
                    tangent = glm::dot(tangent, tangent) > 1e-20f ? glm::normalize(tangent) : glm::vec3(1, 0, 0);
                    const glm::vec3 bitangent = glm::cross(frameNormal, tangent) * (frameTangent.w < 0.0f ? -1.0f : 1.0f);
                    glm::vec3 normal(glm::dot(surface.Normal, tangent), glm::dot(surface.Normal, bitangent), glm::dot(surface.Normal, frameNormal));
                    normal.z = std::max(normal.z, 0.0f); // a back-facing source normal: flat on the HLOD rather than inverted
                    local += glm::dot(normal, normal) > 1e-12f ? glm::normalize(normal) : glm::vec3(0, 0, 1);
                };
                for (uint32_t sy = 0; sy < samplesPerAxis; ++sy)
                    for (uint32_t sx = 0; sx < samplesPerAxis; ++sx)
                        take(float(column) + (float(sx) + 0.5f) / float(samplesPerAxis), float(row) + (float(sy) + 0.5f) / float(samplesPerAxis));
                if (taken == 0)
                    take(float(column) + 0.5f, float(row) + 0.5f);

                if (hits == 0 || cuts * 2 > hits + cuts)
                {
                    if (cuts > 0)
                        states[texel] = TexelState::CutOut;
                    continue;
                }
                const float scale = 1.0f / float(hits);
                color *= scale;
                states[texel] = TexelState::Baked;
                uint8_t* out = &result.BaseColor[texel * 4];
                out[0] = ToByte(LinearToSrgb(color.r));
                out[1] = ToByte(LinearToSrgb(color.g));
                out[2] = ToByte(LinearToSrgb(color.b));
                out[3] = 255;
                uint8_t* mr = &result.MetallicRoughness[texel * 4];
                mr[0] = 255;
                mr[1] = ToByte(roughness * scale);
                mr[2] = ToByte(metallic * scale);
                mr[3] = 255;
                emissive[texel] = glow * scale;
                local = glm::dot(local, local) > 1e-12f ? glm::normalize(local) : glm::vec3(0, 0, 1);
                uint8_t* normal = &result.Normal[texel * 4];
                normal[0] = ToByte(local.x * 0.5f + 0.5f);
                normal[1] = ToByte(local.y * 0.5f + 0.5f);
                normal[2] = ToByte(local.z * 0.5f + 0.5f);
                normal[3] = 255;
            }
        });
        size_t covered = 0;
        for (uint32_t triangle : texelTriangle)
            covered += triangle != NoTriangle ? 1 : 0;
        texelTriangle = {};

        // 7. Texels without a source take their neighbours' values (the atlas' padding, gaps of the simplification), so filtering and
        //    mips never pull in black; cut-out texels keep their colour that way too and only lose their coverage.
        size_t baked = 0, cutTexels = 0;
        for (size_t texel = 0; texel < texelCount; ++texel)
        {
            baked += states[texel] == TexelState::Baked ? 1 : 0;
            cutTexels += states[texel] == TexelState::CutOut ? 1 : 0;
        }
        result.BakedTexels = static_cast<uint32_t>(baked);
        result.CutTexels = static_cast<uint32_t>(cutTexels);
        result.SurfaceTexels = static_cast<uint32_t>(covered);
        std::vector<uint8_t> filled(texelCount);
        for (size_t texel = 0; texel < texelCount; ++texel)
            filled[texel] = states[texel] == TexelState::Baked ? 1 : 0;
        std::vector<size_t> grown;
        for (int pass = 0; pass < 64; ++pass)
        {
            grown.clear();
            for (uint32_t y = 0; y < size; ++y)
            {
                for (uint32_t x = 0; x < size; ++x)
                {
                    const size_t texel = size_t(y) * size + x;
                    if (filled[texel])
                        continue;
                    uint32_t color[4] = {}, normal[3] = {}, mr[2] = {};
                    glm::vec3 glow(0.0f);
                    uint32_t count = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int32_t nx = int32_t(x) + dx, ny = int32_t(y) + dy;
                            if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= int32_t(size) || ny >= int32_t(size))
                                continue;
                            const size_t neighbour = size_t(ny) * size + size_t(nx);
                            if (filled[neighbour] != 1)
                                continue;
                            for (int k = 0; k < 3; ++k)
                            {
                                color[k] += result.BaseColor[neighbour * 4 + k];
                                normal[k] += result.Normal[neighbour * 4 + k];
                            }
                            mr[0] += result.MetallicRoughness[neighbour * 4 + 1];
                            mr[1] += result.MetallicRoughness[neighbour * 4 + 2];
                            glow += emissive[neighbour];
                            ++count;
                        }
                    }
                    if (count == 0)
                        continue;
                    for (int k = 0; k < 3; ++k)
                    {
                        result.BaseColor[texel * 4 + k] = static_cast<uint8_t>((color[k] + count / 2) / count);
                        result.Normal[texel * 4 + k] = static_cast<uint8_t>((normal[k] + count / 2) / count);
                    }
                    result.BaseColor[texel * 4 + 3] = 255;
                    result.Normal[texel * 4 + 3] = 255;
                    result.MetallicRoughness[texel * 4 + 0] = 255;
                    result.MetallicRoughness[texel * 4 + 1] = static_cast<uint8_t>((mr[0] + count / 2) / count);
                    result.MetallicRoughness[texel * 4 + 2] = static_cast<uint8_t>((mr[1] + count / 2) / count);
                    result.MetallicRoughness[texel * 4 + 3] = 255;
                    emissive[texel] = glow / float(count);
                    grown.push_back(texel);
                }
            }
            if (grown.empty())
                break;
            for (size_t texel : grown)
                filled[texel] = 1; // a source for the next pass (not this one: the scan above has finished)
        }
        // What no pass reached (nothing baked at all): mid grey, flat, rough.
        for (size_t texel = 0; texel < texelCount; ++texel)
        {
            if (filled[texel])
                continue;
            const uint8_t flat[4] = { 128, 128, 255, 255 }, grey[4] = { 128, 128, 128, 255 }, rough[4] = { 255, 255, 0, 255 };
            std::copy(grey, grey + 4, &result.BaseColor[texel * 4]);
            std::copy(flat, flat + 4, &result.Normal[texel * 4]);
            std::copy(rough, rough + 4, &result.MetallicRoughness[texel * 4]);
        }
        // Coverage: alpha-masked sources cut away there (an HLOD of foliage keeps its holes).
        // Alpha testing costs (no early depth): only when the cut-outs are more than stray texels -- over 64 and 0.2 % of the surface.
        result.Masked = cutTexels > 64 && cutTexels * 500 > result.SurfaceTexels;
        if (result.Masked)
            for (size_t texel = 0; texel < texelCount; ++texel)
                if (states[texel] == TexelState::CutOut)
                    result.BaseColor[texel * 4 + 3] = 0;

        // 8. Emissive in sRGB over its brightest texel.
        float brightest = 0.0f;
        for (const glm::vec3& glow : emissive)
            brightest = std::max({ brightest, glow.r, glow.g, glow.b });
        result.EmissiveScale = brightest;
        if (brightest > 0.0f)
        {
            result.Emissive.resize(texelCount * 4);
            for (size_t texel = 0; texel < texelCount; ++texel)
            {
                uint8_t* glow = &result.Emissive[texel * 4];
                glow[0] = ToByte(LinearToSrgb(emissive[texel].r / brightest));
                glow[1] = ToByte(LinearToSrgb(emissive[texel].g / brightest));
                glow[2] = ToByte(LinearToSrgb(emissive[texel].b / brightest));
                glow[3] = 255;
            }
        }

        result.BoundsMin = glm::dvec3(DBL_MAX);
        result.BoundsMax = glm::dvec3(-DBL_MAX);
        for (const glm::vec3& position : result.Positions)
        {
            result.BoundsMin = glm::min(result.BoundsMin, glm::dvec3(position) + origin);
            result.BoundsMax = glm::max(result.BoundsMax, glm::dvec3(position) + origin);
        }
        result.Ok = true;
        return result;
    }
}
