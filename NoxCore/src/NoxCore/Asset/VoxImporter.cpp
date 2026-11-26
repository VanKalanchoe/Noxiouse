#include "VoxImporter.h"

#include "MaterialSerializer.h"
#include "TextureImporter.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Voxel/VoxMesher.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>

namespace Nox
{
    namespace
    {
        constexpr uint32_t PaletteSize = 256;

        float SrgbToLinear(float c)
        {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        float LinearToSrgb(float c)
        {
            c = std::clamp(c, 0.0f, 1.0f);
            return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        }

        uint8_t ToByte(float value)
        {
            return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }

        std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
                return {};
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }

        // A 256 x 1 texture, one mip, uncompressed, written only when its bytes changed (an unchanged palette writes nothing, so the file
        // watcher does not reload it).
        bool WritePaletteTexture(const std::filesystem::path& path, const std::vector<uint8_t>& texels, nri::ImageFormat format)
        {
            TextureData data;
            data.Width = PaletteSize;
            data.Height = 1;
            data.MipLevels = 1;
            data.MipOffsets = { 0 };
            data.Format = format;
            data.Data.Allocate(texels.size());
            std::copy(texels.begin(), texels.end(), data.Data.Data);

            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            std::filesystem::path temporary = path;
            temporary += ".tmp";
            const bool saved = TextureImporter::SaveExactTexture(data, temporary);
            data.Data.Release();
            if (!saved)
            {
                std::filesystem::remove(temporary, error);
                return false;
            }
            const std::vector<uint8_t> bytes = ReadFileBytes(temporary);
            std::filesystem::remove(temporary, error);
            if (std::filesystem::exists(path, error) && ReadFileBytes(path) == bytes)
                return true;
            // Written over in place (not renamed over): the file watcher reports the file itself, so the editor reloads it (a rename
            // was reported as the .tmp only).
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            if (!stream)
                return false;
            stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            return stream.good();
        }

        Vox::Pivot ToVoxPivot(VoxelPivot pivot)
        {
            switch (pivot)
            {
            case VoxelPivot::Center: return Vox::Pivot::Center;
            case VoxelPivot::FileOrigin: return Vox::Pivot::FileOrigin;
            default: return Vox::Pivot::BottomCenter;
            }
        }

        // An axis-aligned face's tangent frame (no normal maps, but every cooked vertex carries one): a unit vector in the face's plane.
        glm::vec4 FaceTangent(const glm::vec3& normal)
        {
            if (std::abs(normal.y) > 0.5f)
                return glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            return glm::vec4(glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), normal)), 1.0f);
        }

        // One submesh from mesher output: each vertex's UVs on its palette entry's texel centre.
        void AppendSubmesh(const Vox::VoxGeometry& geometry, const std::string& name, const MaterialData& material,
                           std::vector<MeshData>& meshes, std::vector<MaterialData>& materials, std::vector<PendingGeometry>& pending)
        {
            MeshData& mesh = meshes.emplace_back();
            mesh.Name = name;
            mesh.Vertices.reserve(geometry.Positions.size());
            glm::vec3 low(std::numeric_limits<float>::max());
            glm::vec3 high(std::numeric_limits<float>::lowest());
            for (size_t i = 0; i < geometry.Positions.size(); ++i)
            {
                shaderio::Vertex vertex{};
                vertex.pos = geometry.Positions[i];
                vertex.normal = geometry.Normals[i];
                const glm::vec2 uv((static_cast<float>(geometry.ColorIndices[i]) + 0.5f) / static_cast<float>(PaletteSize), 0.5f);
                vertex.uv0 = uv;
                vertex.uv1 = uv;
                vertex.tangent = FaceTangent(vertex.normal);
                vertex.color = glm::vec4(1.0f);
                mesh.Vertices.push_back(vertex);
                low = glm::min(low, vertex.pos);
                high = glm::max(high, vertex.pos);
            }
            mesh.BoundsMin = low;
            mesh.BoundsMax = high;
            materials.push_back(material);
            PendingGeometry& triangles = pending.emplace_back();
            triangles.Indices = geometry.Indices;
            triangles.Skinned = false;
        }
    }

    bool VoxImporter::IsVoxFile(const std::filesystem::path& path)
    {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension == ".vox";
    }

    std::filesystem::path VoxImporter::PaletteTexturePath(const std::filesystem::path& materialBase, const char* kind)
    {
        // Beside the materials (MeshImporter::MaterialAssetPath: <model folder>/Materials), in the model's Textures folder.
        return materialBase.parent_path().parent_path() / "Textures" / (materialBase.stem().string() + "_Palette" + kind + ".ntex");
    }

    std::vector<MeshData> VoxImporter::ParseToMeshData(const std::filesystem::path& sourcePath, const std::filesystem::path& assetDirectory,
                                                       const std::filesystem::path& materialBase, const MeshImportSettings& settings,
                                                       std::vector<MaterialData>& outMaterials, std::vector<MeshNodeData>& outNodes,
                                                       std::vector<PendingGeometry>& outPending)
    {
        std::vector<MeshData> meshes;
        Vox::Scene scene;
        std::string error;
        if (!Vox::ReadVoxFile(sourcePath, scene, error))
        {
            NOX_CORE_ERROR("VoxImporter: {} cannot be read: {}", sourcePath.string(), error);
            return meshes;
        }
        const Vox::MeshedScene meshed = Vox::MeshScene(scene, Vox::MeshOptions{ ToVoxPivot(settings.VoxelOrigin), settings.VoxelMergeFaces });
        if (meshed.Nodes.empty())
        {
            NOX_CORE_WARN("VoxImporter: {} has no visible voxels", sourcePath.string());
            return meshes;
        }

        // The palette textures.
        std::vector<uint8_t> color(PaletteSize * 4, 0);
        std::vector<uint8_t> surface(PaletteSize * 4, 0);
        std::vector<uint8_t> emissive(PaletteSize * 4, 0);
        double iorSum = 0.0;
        uint32_t iorCount = 0;
        // Emission strength per entry (radiance, 1 = the colour itself): _emit x 2^(_flux + 1), the Blender MagicaVoxel add-on's mapping
        // of MagicaVoxel's Emission and Power (MagicaVoxel publishes none), times the import's Emission Scale. The texture holds each
        // entry relative to the brightest; the materials' Emissive Strength is the brightest.
        std::array<float, PaletteSize> strength{};
        float maxStrength = 0.0f;
        for (uint32_t index = 1; index < PaletteSize; ++index)
        {
            const Vox::Material& material = scene.Materials[index];
            if (material.Type != Vox::MaterialType::Emit)
                continue;
            strength[index] = std::max(material.Emission * std::exp2(material.Flux + 1.0f) * std::max(settings.VoxelEmissionScale, 0.0f), 0.0f);
            if (std::isfinite(strength[index]))
                maxStrength = std::max(maxStrength, strength[index]);
            else
                strength[index] = 0.0f;
        }
        std::vector<bool> used(PaletteSize, false);
        for (const Vox::ModelMesh& model : meshed.Models)
            for (const Vox::VoxGeometry* geometry : { &model.Opaque, &model.Transparent })
                for (uint8_t index : geometry->ColorIndices)
                    used[index] = true;
        for (uint32_t index = 0; index < PaletteSize; ++index)
        {
            const uint32_t rgba = scene.Palette[index];
            const Vox::Material& material = scene.Materials[index];
            const uint8_t r = static_cast<uint8_t>(rgba & 0xFF);
            const uint8_t g = static_cast<uint8_t>((rgba >> 8) & 0xFF);
            const uint8_t b = static_cast<uint8_t>((rgba >> 16) & 0xFF);
            uint8_t* c = &color[index * 4];
            c[0] = r;
            c[1] = g;
            c[2] = b;
            c[3] = 255;
            uint8_t* s = &surface[index * 4];
            s[0] = ToByte(material.IsTransparent() ? material.Transparency : 0.0f);
            s[1] = ToByte(material.Roughness);
            s[2] = ToByte(material.Metallic);
            s[3] = 255;
            if (strength[index] > 0.0f && maxStrength > 0.0f)
            {
                const float relative = strength[index] / maxStrength;
                uint8_t* e = &emissive[index * 4];
                e[0] = ToByte(LinearToSrgb(SrgbToLinear(r / 255.0f) * relative));
                e[1] = ToByte(LinearToSrgb(SrgbToLinear(g / 255.0f) * relative));
                e[2] = ToByte(LinearToSrgb(SrgbToLinear(b / 255.0f) * relative));
            }
            emissive[index * 4 + 3] = 255;
            if (used[index] && material.IsTransparent())
            {
                iorSum += material.IOR;
                ++iorCount;
            }
        }
        const std::filesystem::path colorPath = PaletteTexturePath(materialBase, "Color");
        const std::filesystem::path surfacePath = PaletteTexturePath(materialBase, "Surface");
        const std::filesystem::path emissivePath = PaletteTexturePath(materialBase, "Emissive");
        if (!WritePaletteTexture(assetDirectory / colorPath, color, NRI_FORMAT_SRGBA8) ||
            !WritePaletteTexture(assetDirectory / surfacePath, surface, NRI_FORMAT_RGBA8) ||
            !WritePaletteTexture(assetDirectory / emissivePath, emissive, NRI_FORMAT_SRGBA8))
        {
            NOX_CORE_ERROR("VoxImporter: could not write the palette textures of {} next to {}", sourcePath.string(), materialBase.generic_string());
            return meshes;
        }

        // The materials: everything from the textures (factors 1), the glass one with transmission (its R channel) and the mean IOR of
        // the file's transparent entries (one material, one IOR; editable).
        MaterialData palette;
        palette.Name = "Palette";
        palette.BaseColorTexturePath = colorPath.generic_string();
        palette.MetallicRoughnessTexturePath = surfacePath.generic_string();
        palette.MetallicFactor = 1.0f;
        palette.RoughnessFactor = 1.0f;
        palette.EmissiveTexturePath = emissivePath.generic_string();
        // No emissive entries: no emission at all (the mesh is then no light for the path tracer and the ray-traced GI to sample).
        palette.EmissiveFactor = glm::vec3(maxStrength > 0.0f ? 1.0f : 0.0f);
        palette.emissiveStrength = maxStrength > 0.0f ? maxStrength : 1.0f;
        MaterialData glass = palette;
        glass.Name = "PaletteGlass";
        glass.TransmissionFactor = 1.0f;
        glass.TransmissionTexturePath = surfacePath.generic_string();
        glass.IOR = iorCount ? static_cast<float>(iorSum / iorCount) : 1.5f;

        // The materials are written once (MeshImporter::WriteMaterials; edits survive), but their emission follows the palette: an existing
        // one gets this cook's Emissive and Emissive Strength (the rest stays as edited).
        for (const MaterialData& material : { palette, glass })
        {
            const std::filesystem::path path = assetDirectory / MeshImporter::MaterialAssetPath(materialBase, material, 0);
            std::error_code existsError;
            MaterialData existing;
            if (!std::filesystem::exists(path, existsError) || !MaterialSerializer::Deserialize(path, existing))
                continue;
            if (existing.EmissiveFactor == material.EmissiveFactor && existing.emissiveStrength == material.emissiveStrength)
                continue;
            existing.EmissiveFactor = material.EmissiveFactor;
            existing.emissiveStrength = material.emissiveStrength;
            if (!MaterialSerializer::Serialize(path, existing))
                NOX_CORE_WARN("VoxImporter: could not update the emission of {}", path.string());
        }

        // Submeshes per model (opaque, then glass), the root node at the pivot, a node per placed model under it.
        const std::string stem = sourcePath.stem().string();
        std::vector<uint32_t> firstSubmesh(meshed.Models.size(), UINT32_MAX);
        std::vector<uint32_t> submeshCount(meshed.Models.size(), 0);
        std::vector<bool> placed(meshed.Models.size(), false);
        for (const Vox::MeshedNode& node : meshed.Nodes)
            placed[node.ModelIndex] = true;
        uint32_t placedModels = 0;
        for (bool p : placed)
            placedModels += p;
        auto modelName = [&](size_t model) { return placedModels > 1 ? stem + "_" + std::to_string(model) : stem; };
        for (size_t model = 0; model < meshed.Models.size(); ++model)
        {
            if (!placed[model])
                continue; // an animation's other frames
            firstSubmesh[model] = static_cast<uint32_t>(meshes.size());
            const Vox::ModelMesh& mesh = meshed.Models[model];
            if (!mesh.Opaque.Empty())
                AppendSubmesh(mesh.Opaque, modelName(model), palette, meshes, outMaterials, outPending);
            if (!mesh.Transparent.Empty())
                AppendSubmesh(mesh.Transparent, modelName(model) + "_Glass", glass, meshes, outMaterials, outPending);
            submeshCount[model] = static_cast<uint32_t>(meshes.size()) - firstSubmesh[model];
        }

        MeshNodeData& root = outNodes.emplace_back();
        root.Name = stem;
        root.Parent = -1;
        root.Translation = meshed.RootTranslation;
        for (const Vox::MeshedNode& placement : meshed.Nodes)
        {
            if (submeshCount[placement.ModelIndex] == 0)
                continue;
            MeshNodeData& node = outNodes.emplace_back();
            node.Name = placement.Name.empty() ? modelName(placement.ModelIndex) : placement.Name;
            node.Parent = 0;
            node.Translation = placement.Translation;
            node.Rotation = placement.Rotation;
            node.Scale = placement.Scale;
            node.FirstSubmesh = firstSubmesh[placement.ModelIndex];
            node.SubmeshCount = submeshCount[placement.ModelIndex];
            node.MeshIndex = static_cast<int32_t>(placement.ModelIndex);
            node.MeshName = modelName(placement.ModelIndex);
        }

        size_t triangles = 0;
        for (const PendingGeometry& geometry : outPending)
            triangles += geometry.Indices.size() / 3;
        NOX_CORE_INFO("VoxImporter: {} -- {} models, {} placed, {} submeshes, {} triangles{}", sourcePath.filename().string(), scene.Models.size(),
                      outNodes.size() - 1, meshes.size(), triangles, iorCount ? " (with glass)" : "");
        return meshes;
    }

    MeshImporter::GltfContent VoxImporter::Inspect(const std::filesystem::path& sourcePath)
    {
        MeshImporter::GltfContent content;
        Vox::Scene scene;
        std::string error;
        if (!Vox::ReadVoxFile(sourcePath, scene, error))
        {
            NOX_CORE_ERROR("VoxImporter: {} cannot be read: {}", sourcePath.string(), error);
            return content;
        }
        const Vox::MeshedScene meshed = Vox::MeshScene(scene, Vox::MeshOptions{});
        std::vector<bool> placed(scene.Models.size(), false);
        for (const Vox::MeshedNode& node : meshed.Nodes)
            placed[node.ModelIndex] = true;
        content.HasStaticMeshes = !meshed.Nodes.empty();
        content.StaticMeshCount = static_cast<uint32_t>(std::count(placed.begin(), placed.end(), true));
        content.HasBounds = !meshed.Nodes.empty();
        content.BoundsMin = meshed.BoundsMin;
        content.BoundsMax = meshed.BoundsMax;
        return content;
    }
}
