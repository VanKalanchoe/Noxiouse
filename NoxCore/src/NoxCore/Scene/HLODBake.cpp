#include "HLODBake.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <numeric>
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

        // --- Source triangles and a BVH over them (the bake's ray casts) ---

        struct SourceTriangle
        {
            glm::vec3 P0, E1, E2;   // corner and edges, cell space
            glm::vec3 N[3];         // vertex normals, world orientation
            glm::vec4 T[3];         // vertex tangents
            glm::vec2 UV[3];
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

        Surface Evaluate(const Material& material, const SourceTriangle& tri, float u, float v)
        {
            const float w = 1.0f - u - v;
            const glm::vec2 uv = tri.UV[0] * w + tri.UV[1] * u + tri.UV[2] * v;
            glm::vec3 normal = glm::normalize(tri.N[0] * w + tri.N[1] * u + tri.N[2] * v);
            const glm::vec4 tangent = tri.T[0] * w + tri.T[1] * u + tri.T[2] * v;

            Surface surface;
            glm::vec4 base = material.BaseColorFactor;
            if (material.BaseColor && material.BaseColor->Valid())
            {
                const glm::vec4 texel = material.BaseColor->Sample(uv);
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
                    const glm::vec4 texel = material.MetallicRoughness->Sample(uv);
                    surface.Roughness *= texel.g;
                    surface.Metallic *= texel.b;
                }
            }

            surface.Normal = normal;
            if (material.Normal && material.Normal->Valid() && glm::dot(glm::vec3(tangent), glm::vec3(tangent)) > 1e-12f)
            {
                const glm::vec4 texel = material.Normal->Sample(uv);
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
                const glm::vec4 texel = material.Emissive->Sample(uv);
                emissive *= glm::vec3(SrgbToLinear(texel.r), SrgbToLinear(texel.g), SrgbToLinear(texel.b));
            }
            surface.Emissive = emissive;
            return surface;
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
        glm::vec3 worldLow(FLT_MAX), worldHigh(-FLT_MAX);
        for (const SourceMesh& mesh : meshes)
        {
            for (const glm::vec3& position : mesh.Positions)
            {
                const glm::vec3 world = glm::vec3(mesh.Transform * glm::vec4(position, 1.0f));
                worldLow = glm::min(worldLow, world);
                worldHigh = glm::max(worldHigh, world);
            }
        }
        if (worldLow.x > worldHigh.x)
        {
            result.Error = "no geometry";
            return result;
        }
        const glm::vec3 origin = (worldLow + worldHigh) * 0.5f;
        result.Origin = origin;

        const float minPart = settings.MinPartSize > 0.0f ? settings.MinPartSize : 2.0f * settings.Error;
        std::vector<SourceTriangle> sources;
        std::vector<glm::vec3> mergedPositions;
        std::vector<uint32_t> mergedIndices;
        for (const SourceMesh& mesh : meshes)
        {
            if (mesh.Positions.empty() || mesh.Indices.size() < 3 || mesh.Material >= materials.size())
                continue;
            const glm::mat3 linear(mesh.Transform);
            const glm::mat3 normalMatrix = glm::transpose(glm::inverse(linear));
            const float determinant = glm::determinant(linear);
            const bool mirrored = determinant < 0.0f;

            std::vector<glm::vec3> positions(mesh.Positions.size());
            glm::vec3 low(FLT_MAX), high(-FLT_MAX);
            for (size_t index = 0; index < positions.size(); ++index)
            {
                positions[index] = glm::vec3(mesh.Transform * glm::vec4(mesh.Positions[index], 1.0f)) - origin;
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
                }
                if (!std::isfinite(tri.N[0].x) || glm::dot(glm::cross(tri.E1, tri.E2), glm::cross(tri.E1, tri.E2)) <= 0.0f)
                    continue; // degenerate
                tri.Material = mesh.Material;
                sources.push_back(tri);
            }
        }
        result.SourceTriangles = static_cast<uint32_t>(sources.size());
        if (mergedIndices.empty())
        {
            result.Error = "every part is smaller than the HLOD's error";
            return result;
        }

        // 2. Welded by position, simplified to the error; then, over the triangle budget, cut down to it (sloppy: joins parts).
        std::vector<uint32_t> remap(mergedPositions.size());
        const size_t uniqueCount = meshopt_generateVertexRemap(remap.data(), mergedIndices.data(), mergedIndices.size(), mergedPositions.data(),
                                                               mergedPositions.size(), sizeof(glm::vec3));
        std::vector<glm::vec3> welded(uniqueCount);
        meshopt_remapVertexBuffer(welded.data(), mergedPositions.data(), mergedPositions.size(), sizeof(glm::vec3), remap.data());
        std::vector<uint32_t> indices(mergedIndices.size());
        meshopt_remapIndexBuffer(indices.data(), mergedIndices.data(), mergedIndices.size(), remap.data());

        std::vector<uint32_t> simplified(indices.size());
        float resultError = 0.0f;
        size_t simplifiedCount = meshopt_simplify(simplified.data(), indices.data(), indices.size(), &welded[0].x, welded.size(), sizeof(glm::vec3),
                                                  0, settings.Error, meshopt_SimplifyErrorAbsolute, &resultError);
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
        if (atlas->meshCount != 1 || std::max(atlas->width, atlas->height) > size || atlas->width == 0)
        {
            xatlas::Destroy(atlas);
            result.Error = "the UV atlas does not fit";
            return result;
        }
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

        // 6. The bake: every texel the atlas covers is a point and frame on the HLOD; the source surface it stands for is found along
        //    the normal, from outside in first (the visible surface), else from inside out, within the simplification error. Per texel
        //    only the HLOD triangle covering it is kept (2048^2 texels of full frames would be hundreds of MB).
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
        const float search = std::max(settings.Error * 2.0f, 1e-3f);
        parallelFor(size, [&](uint32_t row)
        {
            for (uint32_t column = 0; column < size; ++column)
            {
                const size_t texel = size_t(row) * size + column;
                const uint32_t triangle = texelTriangle[texel];
                float w0, w1, w2;
                if (triangle == NoTriangle || !barycentrics(triangle, float(column) + 0.5f, float(row) + 0.5f, w0, w1, w2))
                    continue;
                const uint32_t i0 = result.Indices[triangle * 3 + 0], i1 = result.Indices[triangle * 3 + 1], i2 = result.Indices[triangle * 3 + 2];
                const glm::vec3 position = result.Positions[i0] * w0 + result.Positions[i1] * w1 + result.Positions[i2] * w2;
                const glm::vec3 frameNormal = glm::normalize(result.Normals[i0] * w0 + result.Normals[i1] * w1 + result.Normals[i2] * w2);
                const glm::vec4 frameTangent = result.Tangents[i0] * w0 + result.Tangents[i1] * w1 + result.Tangents[i2] * w2;

                // From outside in, then from inside out; alpha-masked hits that are cut away let the ray go on.
                bool cut = false;
                for (int pass = 0; pass < 2 && states[texel] != TexelState::Baked; ++pass)
                {
                    const glm::vec3 direction = pass == 0 ? -frameNormal : frameNormal;
                    const glm::vec3 rayOrigin = position - direction * search;
                    float tMin = 0.0f;
                    for (int step = 0; step < 8; ++step)
                    {
                        uint32_t hitTriangle = 0;
                        float t = 0.0f, u = 0.0f, v = 0.0f;
                        if (!bvh.Intersect(rayOrigin, direction, tMin, search * 2.0f, hitTriangle, t, u, v))
                            break;
                        const SourceTriangle& tri = sources[hitTriangle];
                        const Material& material = materials[tri.Material];
                        const Surface surface = Evaluate(material, tri, u, v);
                        if (material.AlphaMask && surface.Alpha < material.AlphaCutoff)
                        {
                            cut = true;
                            tMin = t + search * 1e-4f;
                            continue;
                        }
                        states[texel] = TexelState::Baked;
                        uint8_t* color = &result.BaseColor[texel * 4];
                        color[0] = ToByte(LinearToSrgb(surface.BaseColor.r));
                        color[1] = ToByte(LinearToSrgb(surface.BaseColor.g));
                        color[2] = ToByte(LinearToSrgb(surface.BaseColor.b));
                        color[3] = 255;
                        uint8_t* mr = &result.MetallicRoughness[texel * 4];
                        mr[0] = 255;
                        mr[1] = ToByte(surface.Roughness);
                        mr[2] = ToByte(surface.Metallic);
                        mr[3] = 255;
                        emissive[texel] = surface.Emissive;
                        // The source's shading normal in the HLOD's tangent frame at this texel.
                        glm::vec3 tangent = glm::vec3(frameTangent) - frameNormal * glm::dot(frameNormal, glm::vec3(frameTangent));
                        tangent = glm::dot(tangent, tangent) > 1e-20f ? glm::normalize(tangent) : glm::vec3(1, 0, 0);
                        const glm::vec3 bitangent = glm::cross(frameNormal, tangent) * (frameTangent.w < 0.0f ? -1.0f : 1.0f);
                        glm::vec3 local(glm::dot(surface.Normal, tangent), glm::dot(surface.Normal, bitangent), glm::dot(surface.Normal, frameNormal));
                        local.z = std::max(local.z, 0.0f); // a back-facing source normal: flat on the HLOD rather than inverted
                        local = glm::dot(local, local) > 1e-12f ? glm::normalize(local) : glm::vec3(0, 0, 1);
                        uint8_t* normal = &result.Normal[texel * 4];
                        normal[0] = ToByte(local.x * 0.5f + 0.5f);
                        normal[1] = ToByte(local.y * 0.5f + 0.5f);
                        normal[2] = ToByte(local.z * 0.5f + 0.5f);
                        normal[3] = 255;
                        break;
                    }
                }
                if (states[texel] != TexelState::Baked && cut)
                    states[texel] = TexelState::CutOut;
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

        result.BoundsMin = glm::vec3(FLT_MAX);
        result.BoundsMax = glm::vec3(-FLT_MAX);
        for (const glm::vec3& position : result.Positions)
        {
            result.BoundsMin = glm::min(result.BoundsMin, position + origin);
            result.BoundsMax = glm::max(result.BoundsMax, position + origin);
        }
        result.Ok = true;
        return result;
    }
}
