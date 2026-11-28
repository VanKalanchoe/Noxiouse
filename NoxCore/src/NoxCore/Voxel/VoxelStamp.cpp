#include "VoxelStamp.h"

#include <cmath>
#include <limits>

namespace Nox::Voxel
{
    namespace
    {
        float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
    }

    VoxelMaterial MaterialFromVox(const Vox::Scene& scene, uint8_t paletteIndex, float emissionScale)
    {
        const uint32_t rgba = scene.Palette[paletteIndex]; // 0xAABBGGRR
        const glm::vec3 colour(SrgbToLinear(static_cast<float>(rgba & 0xFF) / 255.0f), SrgbToLinear(static_cast<float>((rgba >> 8) & 0xFF) / 255.0f),
                               SrgbToLinear(static_cast<float>((rgba >> 16) & 0xFF) / 255.0f));
        const Vox::Material& source = scene.Materials[paletteIndex];
        VoxelMaterial material;
        material.BaseColor = glm::vec4(colour, source.IsTransparent() ? 1.0f - source.Transparency : 1.0f);
        material.Roughness = source.Roughness;
        material.Metallic = source.Metallic;
        if (source.Type == Vox::MaterialType::Emit && source.Emission > 0.0f)
        {
            // The import's formula (VoxImporter.h): _emit x 2^(_flux + 1) x the Emission Scale.
            material.Emission = colour;
            material.EmissionStrength = source.Emission * std::exp2(source.Flux + 1.0f) * emissionScale;
        }
        return material;
    }

    glm::ivec3 StampedCell(const Vox::Scene& scene, const Vox::Instance& instance, int32_t x, int32_t y, int32_t z)
    {
        const Vox::Model& model = scene.Models[instance.ModelIndex];
        // The voxel's centre: R (v + 0.5 - pivot) + t, the pivot the box's centre rounded down -- half-integers, exact in doubles.
        const std::array<double, 3> local = { x + 0.5 - model.SizeX / 2, y + 0.5 - model.SizeY / 2, z + 0.5 - model.SizeZ / 2 };
        const std::array<double, 3> rotated = instance.Rotate.Apply(local);
        const double cx = rotated[0] + instance.Translation[0];
        const double cy = rotated[1] + instance.Translation[1];
        const double cz = rotated[2] + instance.Translation[2];
        // Engine axes: x, z, -y.
        return { static_cast<int32_t>(std::floor(cx)), static_cast<int32_t>(std::floor(cz)), static_cast<int32_t>(std::floor(-cy)) };
    }

    StampResult StampVoxScene(VoxelWorld& world, VoxelMaterialTable& table, const Vox::Scene& scene, const StampOptions& options)
    {
        StampResult result;
        // Bounds first (the pivot needs them).
        glm::ivec3 low(std::numeric_limits<int32_t>::max());
        glm::ivec3 high(std::numeric_limits<int32_t>::min());
        bool any = false;
        for (const Vox::Instance& instance : scene.Instances)
        {
            const Vox::Model& model = scene.Models[instance.ModelIndex];
            for (int32_t z = 0; z < model.SizeZ; ++z)
                for (int32_t y = 0; y < model.SizeY; ++y)
                    for (int32_t x = 0; x < model.SizeX; ++x)
                        if (model.At(x, y, z) != 0)
                        {
                            const glm::ivec3 cell = StampedCell(scene, instance, x, y, z);
                            low = glm::min(low, cell);
                            high = glm::max(high, cell);
                            any = true;
                        }
        }
        if (!any)
            return result;
        glm::ivec3 offset = options.Origin;
        if (options.Pivot == StampPivot::BottomCenter)
            offset -= glm::ivec3(FloorDiv(low.x + high.x + 1, 2), low.y, FloorDiv(low.z + high.z + 1, 2));

        auto materialFor = [&](uint8_t index) -> MaterialId
        {
            if (result.Mapping[index] != Air)
                return result.Mapping[index];
            VoxelMaterial look = MaterialFromVox(scene, index, options.EmissionScale);
            look.Name = options.NamePrefix + " " + std::to_string(index);
            MaterialId id = Air;
            if (options.Materials == StampMaterials::Nearest)
                id = table.Nearest(look);
            else if (const MaterialId existing = table.Find(look.Name); existing != Air && table.Get(existing) == look)
                id = existing;
            if (id == Air)
                id = table.Add(look);
            return result.Mapping[index] = id;
        };

        result.Min = glm::ivec3(std::numeric_limits<int32_t>::max());
        result.Max = glm::ivec3(std::numeric_limits<int32_t>::min());
        for (const Vox::Instance& instance : scene.Instances)
        {
            const Vox::Model& model = scene.Models[instance.ModelIndex];
            for (int32_t z = 0; z < model.SizeZ; ++z)
                for (int32_t y = 0; y < model.SizeY; ++y)
                    for (int32_t x = 0; x < model.SizeX; ++x)
                    {
                        const uint8_t index = model.At(x, y, z);
                        if (index == 0)
                            continue;
                        const glm::ivec3 cell = StampedCell(scene, instance, x, y, z) + offset;
                        const glm::ivec3 chunk = ChunkOf(cell);
                        if (chunk.y < world.MinChunkY() || chunk.y > world.MaxChunkY())
                            continue;
                        world.SetVoxel(cell, materialFor(index));
                        result.Min = glm::min(result.Min, cell);
                        result.Max = glm::max(result.Max, cell);
                        ++result.Voxels;
                    }
        }
        if (result.Voxels == 0)
        {
            result.Min = glm::ivec3(0);
            result.Max = glm::ivec3(-1);
        }
        return result;
    }
}
