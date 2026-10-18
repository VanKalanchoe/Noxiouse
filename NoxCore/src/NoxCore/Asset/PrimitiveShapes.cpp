#include "PrimitiveShapes.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

#include <glm/gtc/constants.hpp>

namespace Nox
{
    namespace
    {
        constexpr float Half = 0.5f;

        // a, b, c, d counter-clockwise seen from the side the normal points to.
        void AddQuad(PrimitiveGeometry& geometry, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d, const glm::vec3& normal)
        {
            const uint32_t base = static_cast<uint32_t>(geometry.Vertices.size());
            geometry.Vertices.push_back({ a, normal, { 0.0f, 1.0f } });
            geometry.Vertices.push_back({ b, normal, { 1.0f, 1.0f } });
            geometry.Vertices.push_back({ c, normal, { 1.0f, 0.0f } });
            geometry.Vertices.push_back({ d, normal, { 0.0f, 0.0f } });
            geometry.Indices.insert(geometry.Indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
        }

        // Counter-clockwise from outside; the face normal comes from the winding.
        void AddTriangle(PrimitiveGeometry& geometry, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
        {
            const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
            const uint32_t base = static_cast<uint32_t>(geometry.Vertices.size());
            geometry.Vertices.push_back({ a, normal, { 0.0f, 1.0f } });
            geometry.Vertices.push_back({ b, normal, { 1.0f, 1.0f } });
            geometry.Vertices.push_back({ c, normal, { 0.5f, 0.0f } });
            geometry.Indices.insert(geometry.Indices.end(), { base, base + 1, base + 2 });
        }

        // A grid of rows x (segments + 1) vertices (the seam column is doubled for the UVs), row r above row r - 1, the angle
        // growing counter-clockwise seen from +Y. With p(row, seg), the quad p0 = (r, s), p1 = (r, s + 1), p2 = (r + 1, s),
        // p3 = (r + 1, s + 1) faces outward as (p0, p2, p1) and (p1, p2, p3): d(angle) x d(up) points inward.
        void ConnectRows(PrimitiveGeometry& geometry, uint32_t lowerRow, uint32_t upperRow, uint32_t segments, bool skipLowerTriangle, bool skipUpperTriangle)
        {
            for (uint32_t seg = 0; seg < segments; ++seg)
            {
                const uint32_t p0 = lowerRow + seg, p1 = lowerRow + seg + 1, p2 = upperRow + seg, p3 = upperRow + seg + 1;
                if (!skipLowerTriangle)
                    geometry.Indices.insert(geometry.Indices.end(), { p0, p2, p1 });
                if (!skipUpperTriangle)
                    geometry.Indices.insert(geometry.Indices.end(), { p1, p2, p3 });
            }
        }

        // Latitude band of a sphere around `center` from latStart to latEnd (radians, -pi/2 = south pole). v runs from
        // vBottom at latStart to vTop at latEnd. A band that reaches a pole leaves out the triangles that collapse there.
        void AddSphereBand(PrimitiveGeometry& geometry, const glm::vec3& center, float radius, float latStart, float latEnd,
                           uint32_t segments, uint32_t rings, float vBottom, float vTop)
        {
            const uint32_t first = static_cast<uint32_t>(geometry.Vertices.size());
            for (uint32_t ring = 0; ring <= rings; ++ring)
            {
                const float t = static_cast<float>(ring) / static_cast<float>(rings);
                const float latitude = latStart + (latEnd - latStart) * t;
                const float y = std::sin(latitude);
                const float r = std::cos(latitude);
                for (uint32_t seg = 0; seg <= segments; ++seg)
                {
                    const float u = static_cast<float>(seg) / static_cast<float>(segments);
                    const float angle = u * glm::two_pi<float>();
                    glm::vec3 normal(r * std::cos(angle), y, r * std::sin(angle));
                    normal = glm::normalize(normal);
                    geometry.Vertices.push_back({ center + normal * radius, normal, { u, vBottom + (vTop - vBottom) * t } });
                }
            }

            constexpr float PoleEpsilon = 1e-4f;
            const bool southPole = latStart <= -glm::half_pi<float>() + PoleEpsilon;
            const bool northPole = latEnd >= glm::half_pi<float>() - PoleEpsilon;
            const uint32_t stride = segments + 1;
            for (uint32_t ring = 0; ring < rings; ++ring)
                ConnectRows(geometry, first + ring * stride, first + (ring + 1) * stride, segments,
                            southPole && ring == 0, northPole && ring + 1 == rings);
        }

        // One row of a vertical surface: radius r at height y; normalY tilts the normal up (0 = a vertical wall).
        uint32_t AddRow(PrimitiveGeometry& geometry, float y, float r, float normalY, float v, uint32_t segments)
        {
            const uint32_t first = static_cast<uint32_t>(geometry.Vertices.size());
            for (uint32_t seg = 0; seg <= segments; ++seg)
            {
                const float u = static_cast<float>(seg) / static_cast<float>(segments);
                const float angle = u * glm::two_pi<float>();
                const glm::vec3 radial(std::cos(angle), 0.0f, std::sin(angle));
                geometry.Vertices.push_back({ radial * r + glm::vec3(0.0f, y, 0.0f), glm::normalize(radial + glm::vec3(0.0f, normalY, 0.0f)), { u, v } });
            }
            return first;
        }

        // A flat disc at height y facing up (+Y) or down.
        void AddDisc(PrimitiveGeometry& geometry, float y, float r, bool facingUp, uint32_t segments)
        {
            const uint32_t center = static_cast<uint32_t>(geometry.Vertices.size());
            const glm::vec3 normal(0.0f, facingUp ? 1.0f : -1.0f, 0.0f);
            geometry.Vertices.push_back({ { 0.0f, y, 0.0f }, normal, { 0.5f, 0.5f } });
            for (uint32_t seg = 0; seg <= segments; ++seg)
            {
                const float angle = static_cast<float>(seg) / static_cast<float>(segments) * glm::two_pi<float>();
                const float x = std::cos(angle), z = std::sin(angle);
                geometry.Vertices.push_back({ { x * r, y, z * r }, normal, { 0.5f + x * 0.5f, 0.5f + (facingUp ? z : -z) * 0.5f } });
            }
            // (center, rim(s), rim(s + 1)) is clockwise seen from +Y, so it faces down.
            for (uint32_t seg = 0; seg < segments; ++seg)
            {
                const uint32_t rim = center + 1 + seg;
                if (facingUp)
                    geometry.Indices.insert(geometry.Indices.end(), { center, rim + 1, rim });
                else
                    geometry.Indices.insert(geometry.Indices.end(), { center, rim, rim + 1 });
            }
        }

        PrimitiveGeometry MakeCube()
        {
            PrimitiveGeometry geometry;
            const float h = Half;
            AddQuad(geometry, { -h, -h,  h }, {  h, -h,  h }, {  h,  h,  h }, { -h,  h,  h }, {  0.0f,  0.0f,  1.0f });
            AddQuad(geometry, {  h, -h, -h }, { -h, -h, -h }, { -h,  h, -h }, {  h,  h, -h }, {  0.0f,  0.0f, -1.0f });
            AddQuad(geometry, {  h, -h,  h }, {  h, -h, -h }, {  h,  h, -h }, {  h,  h,  h }, {  1.0f,  0.0f,  0.0f });
            AddQuad(geometry, { -h, -h, -h }, { -h, -h,  h }, { -h,  h,  h }, { -h,  h, -h }, { -1.0f,  0.0f,  0.0f });
            AddQuad(geometry, { -h,  h,  h }, {  h,  h,  h }, {  h,  h, -h }, { -h,  h, -h }, {  0.0f,  1.0f,  0.0f });
            AddQuad(geometry, { -h, -h, -h }, {  h, -h, -h }, {  h, -h,  h }, { -h, -h,  h }, {  0.0f, -1.0f,  0.0f });
            return geometry;
        }

        PrimitiveGeometry MakeSphere(uint32_t segments)
        {
            PrimitiveGeometry geometry;
            AddSphereBand(geometry, glm::vec3(0.0f), Half, -glm::half_pi<float>(), glm::half_pi<float>(), segments,
                          std::max(segments / 2u, 2u), 1.0f, 0.0f);
            return geometry;
        }

        PrimitiveGeometry MakeCylinder(uint32_t segments)
        {
            PrimitiveGeometry geometry;
            const uint32_t bottom = AddRow(geometry, -Half, Half, 0.0f, 1.0f, segments);
            const uint32_t top = AddRow(geometry, Half, Half, 0.0f, 0.0f, segments);
            ConnectRows(geometry, bottom, top, segments, false, false);
            AddDisc(geometry, Half, Half, true, segments);
            AddDisc(geometry, -Half, Half, false, segments);
            return geometry;
        }

        PrimitiveGeometry MakeCone(uint32_t segments)
        {
            PrimitiveGeometry geometry;
            // The side normal tilts up by radius / height (0.5 / 1) so the shading follows the slope. The apex is one vertex per
            // segment, at the segment's middle angle, so each side triangle gets a sensible normal there.
            const uint32_t base = AddRow(geometry, -Half, Half, 0.5f, 1.0f, segments);
            const uint32_t apex = static_cast<uint32_t>(geometry.Vertices.size());
            for (uint32_t seg = 0; seg < segments; ++seg)
            {
                const float u = (static_cast<float>(seg) + 0.5f) / static_cast<float>(segments);
                const float angle = u * glm::two_pi<float>();
                const glm::vec3 normal = glm::normalize(glm::vec3(std::cos(angle), 0.5f, std::sin(angle)));
                geometry.Vertices.push_back({ { 0.0f, Half, 0.0f }, normal, { u, 0.0f } });
            }
            for (uint32_t seg = 0; seg < segments; ++seg)
                geometry.Indices.insert(geometry.Indices.end(), { base + seg, apex + seg, base + seg + 1 });
            AddDisc(geometry, -Half, Half, false, segments);
            return geometry;
        }

        PrimitiveGeometry MakePyramid()
        {
            PrimitiveGeometry geometry;
            const float h = Half;
            const glm::vec3 apex(0.0f, h, 0.0f);
            const glm::vec3 frontLeft(-h, -h, h), frontRight(h, -h, h), backRight(h, -h, -h), backLeft(-h, -h, -h);
            AddTriangle(geometry, frontLeft, frontRight, apex);
            AddTriangle(geometry, frontRight, backRight, apex);
            AddTriangle(geometry, backRight, backLeft, apex);
            AddTriangle(geometry, backLeft, frontLeft, apex);
            AddQuad(geometry, backLeft, backRight, frontRight, frontLeft, { 0.0f, -1.0f, 0.0f });
            return geometry;
        }

        PrimitiveGeometry MakePlane()
        {
            // Facing +Y, one-sided like UE's and Godot's planes (the material is marked double sided).
            PrimitiveGeometry geometry;
            AddQuad(geometry, { -Half, 0.0f, Half }, { Half, 0.0f, Half }, { Half, 0.0f, -Half }, { -Half, 0.0f, -Half }, { 0.0f, 1.0f, 0.0f });
            return geometry;
        }

        PrimitiveGeometry MakeCapsule(uint32_t segments)
        {
            // Radius 0.5, 2 m tall: two hemispheres around a 1 m shaft. v runs over the whole height.
            PrimitiveGeometry geometry;
            const float radius = 0.5f;
            const float halfShaft = 0.5f;
            const uint32_t rings = std::max(segments / 4u, 2u);
            AddSphereBand(geometry, { 0.0f, -halfShaft, 0.0f }, radius, -glm::half_pi<float>(), 0.0f, segments, rings, 1.0f, 0.75f);
            const uint32_t lower = AddRow(geometry, -halfShaft, radius, 0.0f, 0.75f, segments);
            const uint32_t upper = AddRow(geometry, halfShaft, radius, 0.0f, 0.25f, segments);
            ConnectRows(geometry, lower, upper, segments, false, false);
            AddSphereBand(geometry, { 0.0f, halfShaft, 0.0f }, radius, 0.0f, glm::half_pi<float>(), segments, rings, 0.25f, 0.0f);
            return geometry;
        }

        template <typename T>
        void AppendBytes(std::vector<uint8_t>& bytes, const T& value)
        {
            const auto* raw = reinterpret_cast<const uint8_t*>(&value);
            bytes.insert(bytes.end(), raw, raw + sizeof(T));
        }

        std::string FloatText(float value)
        {
            std::ostringstream out;
            out.precision(9);
            out << value;
            return out.str();
        }
    }

    const char* PrimitiveShapes::GetName(PrimitiveShape shape)
    {
        switch (shape)
        {
        case PrimitiveShape::Cube: return "Cube";
        case PrimitiveShape::Sphere: return "Sphere";
        case PrimitiveShape::Cylinder: return "Cylinder";
        case PrimitiveShape::Cone: return "Cone";
        case PrimitiveShape::Pyramid: return "Pyramid";
        case PrimitiveShape::Plane: return "Plane";
        case PrimitiveShape::Capsule: return "Capsule";
        default: return "Shape";
        }
    }

    PrimitiveGeometry PrimitiveShapes::Generate(PrimitiveShape shape, uint32_t segments)
    {
        segments = std::clamp(segments, 3u, 256u);
        switch (shape)
        {
        case PrimitiveShape::Cube: return MakeCube();
        case PrimitiveShape::Sphere: return MakeSphere(segments);
        case PrimitiveShape::Cylinder: return MakeCylinder(segments);
        case PrimitiveShape::Cone: return MakeCone(segments);
        case PrimitiveShape::Pyramid: return MakePyramid();
        case PrimitiveShape::Plane: return MakePlane();
        case PrimitiveShape::Capsule: return MakeCapsule(segments);
        default: return {};
        }
    }

    std::filesystem::path PrimitiveShapes::WriteGltf(PrimitiveShape shape, const std::filesystem::path& directory, uint32_t segments)
    {
        const PrimitiveGeometry geometry = Generate(shape, segments);
        if (geometry.Vertices.empty() || geometry.Indices.empty())
            return {};

        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
            return {};

        // One buffer: the indices (uint32), then positions, normals and texture coordinates, each tightly packed.
        std::vector<uint8_t> bytes;
        for (uint32_t index : geometry.Indices)
            AppendBytes(bytes, index);
        const size_t indexBytes = bytes.size();

        glm::vec3 boundsMin(std::numeric_limits<float>::max());
        glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
        for (const PrimitiveVertex& vertex : geometry.Vertices)
        {
            AppendBytes(bytes, vertex.Position);
            boundsMin = glm::min(boundsMin, vertex.Position);
            boundsMax = glm::max(boundsMax, vertex.Position);
        }
        for (const PrimitiveVertex& vertex : geometry.Vertices)
            AppendBytes(bytes, vertex.Normal);
        for (const PrimitiveVertex& vertex : geometry.Vertices)
            AppendBytes(bytes, vertex.TexCoord);

        const size_t vertexCount = geometry.Vertices.size();
        const size_t vec3Bytes = vertexCount * sizeof(glm::vec3);
        const size_t positionOffset = indexBytes;
        const size_t normalOffset = positionOffset + vec3Bytes;
        const size_t texCoordOffset = normalOffset + vec3Bytes;

        const std::string name = GetName(shape);
        const std::filesystem::path binPath = directory / (name + ".bin");
        {
            std::ofstream bin(binPath, std::ios::binary | std::ios::trunc);
            bin.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!bin)
                return {};
        }

        std::ostringstream json;
        json << "{\n"
             << "  \"asset\": { \"version\": \"2.0\", \"generator\": \"Nox PrimitiveShapes\" },\n"
             << "  \"scene\": 0,\n"
             << "  \"scenes\": [ { \"nodes\": [ 0 ] } ],\n"
             << "  \"nodes\": [ { \"name\": \"" << name << "\", \"mesh\": 0 } ],\n"
             << "  \"meshes\": [ { \"name\": \"" << name << "\", \"primitives\": [ { \"attributes\": { \"POSITION\": 1, \"NORMAL\": 2, \"TEXCOORD_0\": 3 }, \"indices\": 0, \"material\": 0 } ] } ],\n"
             << "  \"materials\": [ { \"name\": \"" << name << "_Material\", \"pbrMetallicRoughness\": { \"baseColorFactor\": [0.8, 0.8, 0.8, 1.0], \"metallicFactor\": 0.0, \"roughnessFactor\": 0.6 }"
             << (shape == PrimitiveShape::Plane ? ", \"doubleSided\": true" : "") << " } ],\n"
             << "  \"accessors\": [\n"
             << "    { \"bufferView\": 0, \"componentType\": 5125, \"count\": " << geometry.Indices.size() << ", \"type\": \"SCALAR\" },\n"
             << "    { \"bufferView\": 1, \"componentType\": 5126, \"count\": " << vertexCount << ", \"type\": \"VEC3\", \"min\": ["
             << FloatText(boundsMin.x) << ", " << FloatText(boundsMin.y) << ", " << FloatText(boundsMin.z) << "], \"max\": ["
             << FloatText(boundsMax.x) << ", " << FloatText(boundsMax.y) << ", " << FloatText(boundsMax.z) << "] },\n"
             << "    { \"bufferView\": 2, \"componentType\": 5126, \"count\": " << vertexCount << ", \"type\": \"VEC3\" },\n"
             << "    { \"bufferView\": 3, \"componentType\": 5126, \"count\": " << vertexCount << ", \"type\": \"VEC2\" }\n"
             << "  ],\n"
             << "  \"bufferViews\": [\n"
             << "    { \"buffer\": 0, \"byteOffset\": 0, \"byteLength\": " << indexBytes << ", \"target\": 34963 },\n"
             << "    { \"buffer\": 0, \"byteOffset\": " << positionOffset << ", \"byteLength\": " << vec3Bytes << ", \"target\": 34962 },\n"
             << "    { \"buffer\": 0, \"byteOffset\": " << normalOffset << ", \"byteLength\": " << vec3Bytes << ", \"target\": 34962 },\n"
             << "    { \"buffer\": 0, \"byteOffset\": " << texCoordOffset << ", \"byteLength\": " << vertexCount * sizeof(glm::vec2) << ", \"target\": 34962 }\n"
             << "  ],\n"
             << "  \"buffers\": [ { \"uri\": \"" << name << ".bin\", \"byteLength\": " << bytes.size() << " } ]\n"
             << "}\n";

        const std::filesystem::path gltfPath = directory / (name + ".gltf");
        std::ofstream gltf(gltfPath, std::ios::trunc);
        gltf << json.str();
        if (!gltf)
            return {};
        return gltfPath;
    }
}
