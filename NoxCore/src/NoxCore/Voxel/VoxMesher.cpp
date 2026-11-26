#include "VoxMesher.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Nox::Vox
{
    namespace
    {
        // MagicaVoxel's pivot of a model inside its box: the box centre, rounded down (the scene graph's translations place it).
        glm::ivec3 BoxPivot(const Model& model)
        {
            return { model.SizeX / 2, model.SizeY / 2, model.SizeZ / 2 };
        }

        glm::vec3 LocalPivot(const Model& model, const ModelMesh& mesh, Pivot origin)
        {
            if (origin == Pivot::FileOrigin || !mesh.HasVoxels)
                return glm::vec3(BoxPivot(model));
            const glm::vec3 low(mesh.SolidMin);
            const glm::vec3 high = glm::vec3(mesh.SolidMax) + glm::vec3(1.0f);
            glm::vec3 pivot = (low + high) * 0.5f;
            if (origin == Pivot::BottomCenter)
                pivot.z = low.z; // MagicaVoxel is Z-up
            return pivot;
        }

        // The rotation as a matrix (MagicaVoxel axes): column c of row r.
        glm::mat3 ToMatrix(const Rotation& rotation)
        {
            glm::mat3 m(0.0f);
            for (int row = 0; row < 3; ++row)
                m[rotation.Column[row]][row] = static_cast<float>(rotation.Sign[row]); // glm: m[column][row]
            return m;
        }

        glm::vec3 Apply(const Rotation& rotation, const glm::vec3& v)
        {
            return { rotation.Sign[0] * v[rotation.Column[0]], rotation.Sign[1] * v[rotation.Column[1]], rotation.Sign[2] * v[rotation.Column[2]] };
        }
    }

    void MeshModel(const Model& model, const std::array<Material, 256>& materials, bool mergeFaces, const glm::vec3& pivot, ModelMesh& out)
    {
        out.Opaque = {};
        out.Transparent = {};
        out.HasVoxels = model.VoxelCount > 0;
        if (!out.HasVoxels)
            return;

        // Solid bounds.
        glm::ivec3 low(std::numeric_limits<int32_t>::max());
        glm::ivec3 high(std::numeric_limits<int32_t>::min());
        for (int32_t z = 0; z < model.SizeZ; ++z)
            for (int32_t y = 0; y < model.SizeY; ++y)
                for (int32_t x = 0; x < model.SizeX; ++x)
                {
                    if (model.At(x, y, z) == 0)
                        continue;
                    low = glm::min(low, glm::ivec3(x, y, z));
                    high = glm::max(high, glm::ivec3(x, y, z));
                }
        out.SolidMin = low;
        out.SolidMax = high;

        const int32_t size[3] = { model.SizeX, model.SizeY, model.SizeZ };
        auto transparent = [&](uint8_t index) { return index != 0 && materials[index].IsTransparent(); };

        std::vector<uint8_t> mask;
        for (int d = 0; d < 3; ++d)
        {
            const int u = (d + 1) % 3;
            const int v = (d + 2) % 3; // e_u x e_v = e_d
            const int32_t sizeU = size[u];
            const int32_t sizeV = size[v];
            mask.assign(static_cast<size_t>(sizeU) * sizeV, 0);

            for (int sign = -1; sign <= 1; sign += 2)
            {
                glm::vec3 normal(0.0f);
                normal[d] = static_cast<float>(sign);
                const glm::vec3 engineNormal = ToEngineAxes(normal);

                for (int32_t slice = 0; slice < size[d]; ++slice)
                {
                    // Faces of this slice towards `sign`: the palette entry that shows, 0 for none.
                    bool any = false;
                    for (int32_t b = 0; b < sizeV; ++b)
                        for (int32_t a = 0; a < sizeU; ++a)
                        {
                            int32_t cell[3];
                            cell[d] = slice;
                            cell[u] = a;
                            cell[v] = b;
                            const uint8_t self = model.At(cell[0], cell[1], cell[2]);
                            uint8_t shown = 0;
                            if (self != 0)
                            {
                                cell[d] += sign;
                                const uint8_t neighbour = model.At(cell[0], cell[1], cell[2]);
                                if (neighbour == 0 || (transparent(neighbour) && !transparent(self)))
                                    shown = self;
                            }
                            mask[static_cast<size_t>(a) + static_cast<size_t>(sizeU) * b] = shown;
                            any |= shown != 0;
                        }
                    if (!any)
                        continue;

                    const float plane = static_cast<float>(sign > 0 ? slice + 1 : slice);
                    for (int32_t b = 0; b < sizeV; ++b)
                    {
                        for (int32_t a = 0; a < sizeU;)
                        {
                            const uint8_t index = mask[static_cast<size_t>(a) + static_cast<size_t>(sizeU) * b];
                            if (index == 0)
                            {
                                ++a;
                                continue;
                            }
                            // The rectangle: as wide as the run along u, then as tall as every row of that width matches.
                            int32_t width = 1;
                            int32_t height = 1;
                            if (mergeFaces)
                            {
                                while (a + width < sizeU && mask[static_cast<size_t>(a + width) + static_cast<size_t>(sizeU) * b] == index)
                                    ++width;
                                bool grow = true;
                                while (grow && b + height < sizeV)
                                {
                                    for (int32_t k = 0; k < width; ++k)
                                    {
                                        if (mask[static_cast<size_t>(a + k) + static_cast<size_t>(sizeU) * (b + height)] != index)
                                        {
                                            grow = false;
                                            break;
                                        }
                                    }
                                    if (grow)
                                        ++height;
                                }
                            }
                            for (int32_t row = 0; row < height; ++row)
                                for (int32_t k = 0; k < width; ++k)
                                    mask[static_cast<size_t>(a + k) + static_cast<size_t>(sizeU) * (b + row)] = 0;

                            glm::vec3 base(0.0f);
                            base[d] = plane;
                            base[u] = static_cast<float>(a);
                            base[v] = static_cast<float>(b);
                            glm::vec3 alongU(0.0f);
                            alongU[u] = static_cast<float>(width);
                            glm::vec3 alongV(0.0f);
                            alongV[v] = static_cast<float>(height);
                            glm::vec3 corners[4] = { base, base + alongU, base + alongU + alongV, base + alongV };
                            if (sign < 0)
                                std::swap(corners[1], corners[3]); // counter-clockwise seen from -e_d

                            VoxGeometry& geometry = transparent(index) ? out.Transparent : out.Opaque;
                            const uint32_t first = static_cast<uint32_t>(geometry.Positions.size());
                            for (const glm::vec3& corner : corners)
                            {
                                geometry.Positions.push_back(ToEngineAxes(corner - pivot));
                                geometry.Normals.push_back(engineNormal);
                                geometry.ColorIndices.push_back(index);
                            }
                            for (uint32_t i : { 0u, 1u, 2u, 0u, 2u, 3u })
                                geometry.Indices.push_back(first + i);
                            a += width;
                        }
                    }
                }
            }
        }
    }

    MeshedScene MeshScene(const Scene& scene, const MeshOptions& options)
    {
        MeshedScene result;
        result.Models.resize(scene.Models.size());
        std::vector<glm::vec3> pivots(scene.Models.size(), glm::vec3(0.0f));
        for (size_t i = 0; i < scene.Models.size(); ++i)
        {
            const Model& model = scene.Models[i];
            ModelMesh& mesh = result.Models[i];
            // The bounds first (the pivot depends on them), then the faces relative to the pivot.
            MeshModel(model, scene.Materials, options.MergeFaces, glm::vec3(0.0f), mesh);
            pivots[i] = LocalPivot(model, mesh, options.Origin);
            if (pivots[i] != glm::vec3(0.0f))
            {
                const glm::vec3 offset = ToEngineAxes(pivots[i]);
                for (VoxGeometry* geometry : { &mesh.Opaque, &mesh.Transparent })
                    for (glm::vec3& position : geometry->Positions)
                        position -= offset;
            }
        }

        // MagicaVoxel places a model by world = R (p - boxPivot) + t (p in the model's cells). With the mesh relative to its own
        // pivot L: world = R q + R (L - boxPivot) + t.
        const glm::mat3 toEngine(glm::vec3(1, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0)); // columns: images of x, y, z
        glm::vec3 low(std::numeric_limits<float>::max());
        glm::vec3 high(std::numeric_limits<float>::lowest());
        for (const Instance& instance : scene.Instances)
        {
            const Model& model = scene.Models[instance.ModelIndex];
            const ModelMesh& mesh = result.Models[instance.ModelIndex];
            if (!mesh.HasVoxels)
                continue;
            const glm::vec3 boxPivot(BoxPivot(model));
            const glm::vec3 translation(static_cast<float>(instance.Translation[0]), static_cast<float>(instance.Translation[1]),
                                        static_cast<float>(instance.Translation[2]));
            const glm::vec3 placed = Apply(instance.Rotate, pivots[instance.ModelIndex] - boxPivot) + translation;

            MeshedNode& node = result.Nodes.emplace_back();
            node.Name = instance.Name;
            node.ModelIndex = instance.ModelIndex;
            node.Translation = ToEngineAxes(placed);
            glm::mat3 rotation = toEngine * ToMatrix(instance.Rotate) * glm::transpose(toEngine);
            if (glm::determinant(rotation) < 0.0f)
            {
                // A mirror: the rotation part as a quaternion, the reflection as a negative scale (rotation = R' diag(-1, 1, 1)).
                node.Scale = glm::vec3(-1.0f, 1.0f, 1.0f);
                rotation[0] = -rotation[0];
            }
            node.Rotation = glm::normalize(glm::quat_cast(rotation));

            // The placed solid box, for the scene's bounds (MagicaVoxel axes).
            for (int corner = 0; corner < 8; ++corner)
            {
                const glm::vec3 local((corner & 1) ? mesh.SolidMax.x + 1.0f : static_cast<float>(mesh.SolidMin.x),
                                      (corner & 2) ? mesh.SolidMax.y + 1.0f : static_cast<float>(mesh.SolidMin.y),
                                      (corner & 4) ? mesh.SolidMax.z + 1.0f : static_cast<float>(mesh.SolidMin.z));
                const glm::vec3 world = Apply(instance.Rotate, local - boxPivot) + translation;
                low = glm::min(low, world);
                high = glm::max(high, world);
            }
        }
        if (result.Nodes.empty())
            return result;

        glm::vec3 scenePivot(0.0f);
        if (options.Origin != Pivot::FileOrigin)
        {
            scenePivot = (low + high) * 0.5f;
            if (options.Origin == Pivot::BottomCenter)
                scenePivot.z = low.z;
        }
        result.RootTranslation = -ToEngineAxes(scenePivot);
        // Bounds in engine axes: x stays, MagicaVoxel z is y, MagicaVoxel y is -z (its min and max swap).
        const glm::vec3 a = ToEngineAxes(low - scenePivot);
        const glm::vec3 b = ToEngineAxes(high - scenePivot);
        result.BoundsMin = glm::min(a, b);
        result.BoundsMax = glm::max(a, b);
        return result;
    }
}
