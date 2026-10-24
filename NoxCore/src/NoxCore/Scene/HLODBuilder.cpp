#include "HLODBuilder.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>

#define BCDEC_IMPLEMENTATION
#include <bcdec.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "Components.h"
#include "Entity.h"
#include "HLODBake.h"
#include "ModelInstance.h"
#include "PrefabInstance.h"
#include "Scene.h"
#include "SceneSerializer.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/MaterialSerializer.h"
#include "NoxCore/Asset/MeshImporter.h"
#include "NoxCore/Asset/TextureImporter.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/VirtualFileSystem.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Renderer.h"

namespace Nox
{
    namespace
    {
        // The bake samples textures at about its own texel size: a mip no larger than this is plenty for it.
        constexpr uint32_t BakeTextureSize = 512;
        // Frames a cell's model and prefab instances may take to spawn (their assets load) before it is built with what spawned.
        constexpr uint32_t SpawnFrameLimit = 1800;

        // Vulkan format numbers a KTX2 source keeps (TextureData::DirectFormat).
        enum class BlockFormat { None, RGBA8, BC1, BC2, BC3, BC4, BC5, BC6H, BC7 };

        BlockFormat FormatOf(const TextureData& texture)
        {
            if (texture.DirectFormat != UINT32_MAX)
            {
                switch (texture.DirectFormat)
                {
                case 37: case 43: return BlockFormat::RGBA8;                   // R8G8B8A8 UNORM / SRGB
                case 131: case 132: case 133: case 134: return BlockFormat::BC1;
                case 135: case 136: return BlockFormat::BC2;
                case 137: case 138: return BlockFormat::BC3;
                case 139: case 140: return BlockFormat::BC4;
                case 141: case 142: return BlockFormat::BC5;
                case 143: case 144: return BlockFormat::BC6H;
                case 145: case 146: return BlockFormat::BC7;
                default: return BlockFormat::None;
                }
            }
            switch (texture.Format)
            {
            case NRI::ImageFormat::RGBA8: case NRI::ImageFormat::SRGBA8: return BlockFormat::RGBA8;
            case NRI::ImageFormat::BC1_UNorm: case NRI::ImageFormat::BC1_UNorm_SRGB: return BlockFormat::BC1;
            case NRI::ImageFormat::BC2_UNorm: case NRI::ImageFormat::BC2_UNorm_SRGB: return BlockFormat::BC2;
            case NRI::ImageFormat::BC3_UNorm: case NRI::ImageFormat::BC3_UNorm_SRGB: return BlockFormat::BC3;
            case NRI::ImageFormat::BC4_UNorm: case NRI::ImageFormat::BC4_SNorm: return BlockFormat::BC4;
            case NRI::ImageFormat::BC5_UNorm: case NRI::ImageFormat::BC5_SNorm: return BlockFormat::BC5;
            case NRI::ImageFormat::BC6H_UF16: case NRI::ImageFormat::BC6H_SF16: return BlockFormat::BC6H;
            case NRI::ImageFormat::BC7_UNorm: case NRI::ImageFormat::BC7_UNorm_SRGB: return BlockFormat::BC7;
            default: return BlockFormat::None;
            }
        }

        // Any thread. The cooked texture as RGBA8 (BC5: x, y in red and green), at a mip no larger than maxSize.
        bool DecodeCookedTexture(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata, uint32_t maxSize, HLOD::Texture& out,
                                 bool& outTwoChannel, std::string& outError)
        {
            std::optional<CookedTextureHeader> header = TextureImporter::ReadCookedTextureHeader(assetDirectory, metadata);
            if (!header && TextureImporter::CookTexture(assetDirectory, metadata))
                header = TextureImporter::ReadCookedTextureHeader(assetDirectory, metadata);
            if (!header)
            {
                outError = "not cooked";
                return false;
            }
            const TextureData& texture = header->Texture;
            const BlockFormat format = FormatOf(texture);
            if (format == BlockFormat::None)
            {
                outError = "a format the bake does not read";
                return false;
            }
            uint32_t mip = 0;
            const uint32_t mipCount = std::max(texture.MipLevels, 1u);
            while (mip + 1 < mipCount && texture.MipOffsets.size() == mipCount && std::max(texture.Width >> mip, 1u) > maxSize)
                ++mip;
            const uint32_t width = std::max(texture.Width >> mip, 1u), height = std::max(texture.Height >> mip, 1u);
            const uint64_t begin = texture.MipOffsets.empty() ? 0 : texture.MipOffsets[mip];
            const uint64_t end = mip + 1 < texture.MipOffsets.size() ? texture.MipOffsets[mip + 1] : header->DataSize;
            std::vector<uint8_t> raw(end - begin);
            if (raw.empty() || !TextureImporter::ReadCookedTextureMips(*header, mip, 1, raw.data()))
            {
                outError = "unreadable";
                return false;
            }

            out.Width = width;
            out.Height = height;
            out.Texels.assign(size_t(width) * height * 4, 255);
            outTwoChannel = format == BlockFormat::BC5;
            if (format == BlockFormat::RGBA8)
            {
                if (raw.size() < out.Texels.size())
                {
                    outError = "truncated";
                    return false;
                }
                std::memcpy(out.Texels.data(), raw.data(), out.Texels.size());
                return true;
            }

            const size_t blockSize = format == BlockFormat::BC1 || format == BlockFormat::BC4 ? 8 : 16;
            const uint32_t blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
            if (raw.size() < size_t(blocksX) * blocksY * blockSize)
            {
                outError = "truncated";
                return false;
            }
            uint8_t block[4 * 4 * 4];
            uint8_t channels[4 * 4 * 2];
            float hdr[4 * 4 * 3];
            for (uint32_t by = 0; by < blocksY; ++by)
            {
                for (uint32_t bx = 0; bx < blocksX; ++bx)
                {
                    const uint8_t* source = raw.data() + (size_t(by) * blocksX + bx) * blockSize;
                    switch (format)
                    {
                    case BlockFormat::BC1: bcdec_bc1(source, block, 16); break;
                    case BlockFormat::BC2: bcdec_bc2(source, block, 16); break;
                    case BlockFormat::BC3: bcdec_bc3(source, block, 16); break;
                    case BlockFormat::BC7: bcdec_bc7(source, block, 16); break;
                    case BlockFormat::BC4:
                        bcdec_bc4(source, channels, 4);
                        for (int i = 0; i < 16; ++i)
                        {
                            block[i * 4 + 0] = block[i * 4 + 1] = block[i * 4 + 2] = channels[i];
                            block[i * 4 + 3] = 255;
                        }
                        break;
                    case BlockFormat::BC5:
                        bcdec_bc5(source, channels, 8);
                        for (int i = 0; i < 16; ++i)
                        {
                            block[i * 4 + 0] = channels[i * 2 + 0];
                            block[i * 4 + 1] = channels[i * 2 + 1];
                            block[i * 4 + 2] = 255;
                            block[i * 4 + 3] = 255;
                        }
                        break;
                    case BlockFormat::BC6H:
                        // HDR (an emissive map): clamped to 0..1, the factor carries the rest.
                        bcdec_bc6h_float(source, hdr, 4 * 3, texture.Format == NRI::ImageFormat::BC6H_SF16 ? 1 : 0);
                        for (int i = 0; i < 16; ++i)
                        {
                            for (int k = 0; k < 3; ++k)
                                block[i * 4 + k] = static_cast<uint8_t>(std::clamp(hdr[i * 3 + k], 0.0f, 1.0f) * 255.0f + 0.5f);
                            block[i * 4 + 3] = 255;
                        }
                        break;
                    default:
                        break;
                    }
                    for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y)
                        for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x)
                            std::memcpy(&out.Texels[(size_t(by * 4 + y) * width + bx * 4 + x) * 4], &block[(y * 4 + x) * 4], 4);
                }
            }
            return true;
        }

        glm::mat4 ComposedWorldMatrix(Scene& scene, Entity entity)
        {
            glm::mat4 world = entity.GetComponent<TransformComponent>().GetTransform();
            UUID parentID = entity.HasComponent<RelationshipComponent>() ? entity.GetComponent<RelationshipComponent>().Parent : UUID(0);
            for (int depth = 0; parentID != 0 && depth < 1024; ++depth)
            {
                Entity parent = scene.GetEntityByUUID(parentID);
                if (!parent)
                    break;
                world = parent.GetComponent<TransformComponent>().GetTransform() * world;
                parentID = parent.HasComponent<RelationshipComponent>() ? parent.GetComponent<RelationshipComponent>().Parent : UUID(0);
            }
            return world;
        }

        bool UnderAnimator(Scene& scene, Entity entity)
        {
            for (int depth = 0; entity && depth < 1024; ++depth)
            {
                if (entity.HasComponent<AnimatorComponent>())
                    return true;
                const UUID parent = entity.HasComponent<RelationshipComponent>() ? entity.GetComponent<RelationshipComponent>().Parent : UUID(0);
                entity = parent != 0 ? scene.GetEntityByUUID(parent) : Entity();
            }
            return false;
        }

        // A glTF 2.0 file (JSON, one .bin, PNG images) of the HLOD, positions in world units around its origin.
        bool WriteHLODGltf(const std::filesystem::path& gltfPath, const HLOD::Result& result, std::string& outError)
        {
            const std::filesystem::path directory = gltfPath.parent_path();
            const std::string stem = gltfPath.stem().string();
            std::error_code error;
            std::filesystem::create_directories(directory, error);

            // Images.
            struct Image { std::string Name; const std::vector<uint8_t>* Texels; };
            std::vector<Image> images = { { stem + "_BaseColor.png", &result.BaseColor }, { stem + "_Normal.png", &result.Normal },
                                          { stem + "_MetallicRoughness.png", &result.MetallicRoughness } };
            if (!result.Emissive.empty())
                images.push_back({ stem + "_Emissive.png", &result.Emissive });
            for (const Image& image : images)
            {
                const int size = static_cast<int>(result.TextureSize);
                if (!stbi_write_png((directory / image.Name).string().c_str(), size, size, 4, image.Texels->data(), size * 4))
                {
                    outError = "could not write " + image.Name;
                    return false;
                }
            }

            // Buffer: indices, positions, normals, tangents, UVs.
            std::vector<uint8_t> bin;
            auto append = [&](const void* data, size_t bytes)
            {
                const size_t offset = bin.size();
                bin.resize(offset + bytes);
                std::memcpy(bin.data() + offset, data, bytes);
                while (bin.size() % 4 != 0)
                    bin.push_back(0);
                return offset;
            };
            const size_t vertexCount = result.Positions.size();
            const size_t indicesOffset = append(result.Indices.data(), result.Indices.size() * sizeof(uint32_t));
            const size_t positionsOffset = append(result.Positions.data(), vertexCount * sizeof(glm::vec3));
            const size_t normalsOffset = append(result.Normals.data(), vertexCount * sizeof(glm::vec3));
            const size_t tangentsOffset = append(result.Tangents.data(), vertexCount * sizeof(glm::vec4));
            const size_t uvsOffset = append(result.UVs.data(), vertexCount * sizeof(glm::vec2));
            {
                std::ofstream file(directory / (stem + ".bin"), std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
                if (!file)
                {
                    outError = "could not write " + stem + ".bin";
                    return false;
                }
            }

            glm::vec3 low(FLT_MAX), high(-FLT_MAX);
            for (const glm::vec3& position : result.Positions)
            {
                low = glm::min(low, position);
                high = glm::max(high, position);
            }
            std::ostringstream json;
            json.precision(9);
            json << "{\n  \"asset\": { \"version\": \"2.0\", \"generator\": \"Nox HLOD builder\" },\n";
            json << "  \"scene\": 0,\n  \"scenes\": [ { \"nodes\": [ 0 ] } ],\n";
            json << "  \"nodes\": [ { \"name\": \"" << stem << "\", \"mesh\": 0 } ],\n";
            json << "  \"meshes\": [ { \"name\": \"" << stem << "\", \"primitives\": [ { \"attributes\": { \"POSITION\": 1, \"NORMAL\": 2, \"TANGENT\": 3, "
                    "\"TEXCOORD_0\": 4 }, \"indices\": 0, \"material\": 0 } ] } ],\n";
            json << "  \"materials\": [ { \"name\": \"" << stem << "\", \"pbrMetallicRoughness\": { \"baseColorTexture\": { \"index\": 0 }, "
                    "\"metallicRoughnessTexture\": { \"index\": 2 }, \"metallicFactor\": 1.0, \"roughnessFactor\": 1.0 }, \"normalTexture\": { \"index\": 1 }";
            if (!result.Emissive.empty())
                json << ", \"emissiveTexture\": { \"index\": 3 }, \"emissiveFactor\": [ 1.0, 1.0, 1.0 ], \"extensions\": { \"KHR_materials_emissive_strength\": "
                        "{ \"emissiveStrength\": " << result.EmissiveScale << " } }";
            if (result.Masked)
                json << ", \"alphaMode\": \"MASK\", \"alphaCutoff\": 0.5";
            json << " } ],\n";
            if (!result.Emissive.empty())
                json << "  \"extensionsUsed\": [ \"KHR_materials_emissive_strength\" ],\n";
            json << "  \"textures\": [ ";
            for (size_t index = 0; index < images.size(); ++index)
                json << (index ? ", " : "") << "{ \"source\": " << index << ", \"sampler\": 0 }";
            json << " ],\n  \"samplers\": [ { \"magFilter\": 9729, \"minFilter\": 9987, \"wrapS\": 33071, \"wrapT\": 33071 } ],\n";
            json << "  \"images\": [ ";
            for (size_t index = 0; index < images.size(); ++index)
                json << (index ? ", " : "") << "{ \"uri\": \"" << images[index].Name << "\" }";
            json << " ],\n";
            json << "  \"buffers\": [ { \"uri\": \"" << stem << ".bin\", \"byteLength\": " << bin.size() << " } ],\n";
            json << "  \"bufferViews\": [ "
                 << "{ \"buffer\": 0, \"byteOffset\": " << indicesOffset << ", \"byteLength\": " << result.Indices.size() * 4 << ", \"target\": 34963 }, "
                 << "{ \"buffer\": 0, \"byteOffset\": " << positionsOffset << ", \"byteLength\": " << vertexCount * 12 << ", \"target\": 34962 }, "
                 << "{ \"buffer\": 0, \"byteOffset\": " << normalsOffset << ", \"byteLength\": " << vertexCount * 12 << ", \"target\": 34962 }, "
                 << "{ \"buffer\": 0, \"byteOffset\": " << tangentsOffset << ", \"byteLength\": " << vertexCount * 16 << ", \"target\": 34962 }, "
                 << "{ \"buffer\": 0, \"byteOffset\": " << uvsOffset << ", \"byteLength\": " << vertexCount * 8 << ", \"target\": 34962 } ],\n";
            json << "  \"accessors\": [ "
                 << "{ \"bufferView\": 0, \"componentType\": 5125, \"count\": " << result.Indices.size() << ", \"type\": \"SCALAR\" }, "
                 << "{ \"bufferView\": 1, \"componentType\": 5126, \"count\": " << vertexCount << ", \"type\": \"VEC3\", \"min\": [ " << low.x << ", "
                 << low.y << ", " << low.z << " ], \"max\": [ " << high.x << ", " << high.y << ", " << high.z << " ] }, "
                 << "{ \"bufferView\": 2, \"componentType\": 5126, \"count\": " << vertexCount << ", \"type\": \"VEC3\" }, "
                 << "{ \"bufferView\": 3, \"componentType\": 5126, \"count\": " << vertexCount << ", \"type\": \"VEC4\" }, "
                 << "{ \"bufferView\": 4, \"componentType\": 5126, \"count\": " << vertexCount << ", \"type\": \"VEC2\" } ]\n}\n";

            std::ofstream file(gltfPath, std::ios::binary | std::ios::trunc);
            file << json.str();
            if (!file)
            {
                outError = "could not write " + gltfPath.filename().string();
                return false;
            }
            return true;
        }
    }

    // One cell's build: its scene, what it draws, and the jobs reading and baking it.
    struct HLODBuilder::CellWork
    {
        enum class Phase { Spawn, ReadMeshes, Bake };
        Phase Stage = Phase::Spawn;
        WorldCellCoord Cell;
        Ref<Scene> CellScene;
        uint32_t SpawnFrames = 0;

        struct Instance
        {
            AssetHandle Mesh = 0;
            glm::mat4 World{ 1.0f };
            uint32_t FirstSubmesh = 0;
            uint32_t SubmeshCount = UINT32_MAX;
            std::vector<AssetHandle> MaterialOverrides;
        };
        std::vector<Instance> Instances;
        std::map<AssetHandle, AssetMetadata> Meshes;                                  // the meshes to read
        std::shared_ptr<std::map<AssetHandle, std::optional<CookedMesh>>> Cooked;    // read on a worker
        TaskFuture<void> Read;

        // Resolved on the main thread: per instance submesh its material, per material its textures (metadata to read on a worker).
        struct MaterialSource
        {
            MaterialData Data;
            AssetHandle BaseColor = 0, MetallicRoughness = 0, Normal = 0, Emissive = 0;
        };
        std::vector<MaterialSource> Materials;
        struct Part
        {
            AssetHandle Mesh = 0;
            uint32_t Submesh = 0;
            glm::mat4 World{ 1.0f };
            uint32_t Material = 0;
        };
        std::vector<Part> Parts;
        std::map<AssetHandle, AssetMetadata> Textures;

        std::shared_ptr<HLOD::Result> Result;
        std::shared_ptr<std::string> Error;
        TaskFuture<void> Bake;
        std::filesystem::path GltfPath; // relative to the asset directory
    };

    HLODBuilder::HLODBuilder(Ref<Scene> world) : m_World(std::move(world))
    {
        const WorldPartition& partition = m_World->GetWorldPartition();
        if (!partition.Settings.Enabled || partition.CellDirectory.empty())
        {
            NOX_CORE_ERROR("Build HLODs: the scene is not a saved world (convert it and save it first)");
            m_Failed = true;
            m_Done = true;
            return;
        }
        for (const auto& [cell, info] : partition.Cells)
        {
            if (cell.Grid == WorldGrid::WorldGridIndex)
                m_Cells.push_back(cell);
        }
        // The HLODs shown now are rebuilt: out of the editor until the build is done.
        m_ShowHLODsBefore = m_World->GetShowHLODs();
        m_World->SetShowHLODs(false);
        m_World->UnloadWorldCells(m_World->GetLoadedWorldCells(true, WorldGrid::HLODGridIndex));
        NOX_CORE_INFO("Build HLODs: {} cells", m_Cells.size());
    }

    HLODBuilder::~HLODBuilder()
    {
        if (m_Work)
        {
            m_Work->Read.Wait();
            m_Work->Bake.Wait();
        }
        if (m_World)
            m_World->SetShowHLODs(m_ShowHLODsBefore);
    }

    void HLODBuilder::CollectAssetReferences(std::unordered_set<AssetHandle>& outHandles) const
    {
        if (m_Work && m_Work->CellScene)
            m_Work->CellScene->CollectAssetReferences(outHandles);
    }

    float HLODBuilder::GetProgress() const
    {
        return m_Cells.empty() ? 1.0f : static_cast<float>(m_Next) / static_cast<float>(m_Cells.size());
    }

    bool HLODBuilder::Step()
    {
        if (m_Done)
            return true;
        if (!m_Work)
        {
            if (m_Next >= m_Cells.size())
            {
                WriteHLODCells();
                m_World->SetShowHLODs(m_ShowHLODsBefore);
                m_Done = true;
                return true;
            }
            StartCell();
        }
        if (StepCell())
        {
            m_Work.reset();
            ++m_Next;
        }
        return false;
    }

    void HLODBuilder::StartCell()
    {
        m_Work = std::make_unique<CellWork>();
        m_Work->Cell = m_Cells[m_Next];
        m_Stage = "Reading " + WorldGrid::CellName(m_Work->Cell);
        // The cell as saved, in a scene of its own.
        m_Work->CellScene = CreateRef<Scene>();
        std::string text;
        const std::filesystem::path file = Project::GetActiveAssetDirectory() / m_World->GetWorldPartition().CellFile(m_Work->Cell);
        if (!VFS::ReadText(file, text) || !SceneSerializer(m_Work->CellScene).DeserializeText(text))
            NOX_CORE_WARN("Build HLODs: cell file '{}' could not be read", file.generic_string());
    }

    bool HLODBuilder::StepCell()
    {
        CellWork& work = *m_Work;
        Scene& scene = *work.CellScene;
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        const std::string cellName = WorldGrid::CellName(work.Cell);

        switch (work.Stage)
        {
        case CellWork::Phase::Spawn:
        {
            // Model and prefab instances spawn once their assets are loaded (nested ones the frames after).
            m_Stage = "Spawning " + cellName;
            ModelInstance::SpawnPending(scene);
            PrefabInstance::SpawnPending(scene);
            bool pending = false;
            for (auto handle : scene.GetAllEntitiesWith<ModelInstanceComponent>())
                pending |= !scene.GetRegistry().get<ModelInstanceComponent>(handle).Spawned;
            for (auto handle : scene.GetAllEntitiesWith<PrefabInstanceComponent>())
                pending |= !scene.GetRegistry().get<PrefabInstanceComponent>(handle).Spawned;
            if (pending && ++work.SpawnFrames < SpawnFrameLimit)
                return false;
            if (pending)
                NOX_CORE_WARN("Build HLODs: {}: instances still loading, built with what has spawned", cellName);

            // What it draws: static meshes (not under an animator: skinned or animated parts are left out, as Unreal's HLODs are static).
            for (auto handle : scene.GetAllEntitiesWith<MeshComponent>())
            {
                Entity entity(handle, &scene);
                const MeshComponent& mesh = entity.GetComponent<MeshComponent>();
                const AssetType type = AssetManager::GetAssetType(mesh.Mesh);
                if ((type != AssetType::StaticMesh && type != AssetType::Mesh) || UnderAnimator(scene, entity))
                    continue;
                CellWork::Instance instance;
                instance.Mesh = mesh.Mesh;
                instance.World = ComposedWorldMatrix(scene, entity);
                instance.FirstSubmesh = mesh.SubmeshIndex;
                instance.SubmeshCount = mesh.SubmeshCount;
                if (entity.HasComponent<MaterialComponent>())
                    instance.MaterialOverrides = entity.GetComponent<MaterialComponent>().MaterialAssets;
                work.Instances.push_back(std::move(instance));
                if (!work.Meshes.contains(mesh.Mesh))
                    work.Meshes.emplace(mesh.Mesh, Project::GetActive()->GetEditorAssetManager()->GetMetadata(mesh.Mesh));
            }
            if (work.Instances.empty())
                return true; // nothing to stand in for

            // The cooked meshes, on a worker.
            work.Cooked = std::make_shared<std::map<AssetHandle, std::optional<CookedMesh>>>();
            work.Read = JobSystem::Get().Async("HLOD Read Meshes", JobPriority::Normal,
                [cooked = work.Cooked, meshes = work.Meshes, assetDirectory](const CancellationToken&)
                {
                    for (const auto& [handle, metadata] : meshes)
                    {
                        std::optional<CookedMesh> mesh = MeshImporter::ReadCookedMesh(assetDirectory, metadata);
                        if (!mesh && MeshImporter::CookMesh(assetDirectory, metadata))
                            mesh = MeshImporter::ReadCookedMesh(assetDirectory, metadata);
                        (*cooked)[handle] = std::move(mesh);
                    }
                });
            work.Stage = CellWork::Phase::ReadMeshes;
            m_Stage = "Reading meshes of " + cellName;
            return false;
        }

        case CellWork::Phase::ReadMeshes:
        {
            if (!work.Read.IsReady())
                return false;
            // Each part's material: the entity's override, else the model's .nmat, else the material cooked into the mesh; and the
            // textures it names (the renderer's lookup).
            auto& assets = *Project::GetActive()->GetEditorAssetManager();
            std::map<std::pair<AssetHandle, const MaterialData*>, uint32_t> materialIndex;
            for (const CellWork::Instance& instance : work.Instances)
            {
                const auto cooked = work.Cooked->find(instance.Mesh);
                if (cooked == work.Cooked->end() || !cooked->second)
                    continue;
                const CookedMesh& mesh = *cooked->second;
                const AssetMetadata& meshMetadata = work.Meshes.at(instance.Mesh);
                const size_t total = mesh.Submeshes.size();
                const size_t first = std::min<size_t>(instance.FirstSubmesh, total);
                const size_t count = instance.SubmeshCount == UINT32_MAX ? total : std::max(instance.SubmeshCount, 1u);
                for (size_t submesh = first; submesh < std::min(first + count, total); ++submesh)
                {
                    AssetHandle materialHandle = submesh < instance.MaterialOverrides.size() ? instance.MaterialOverrides[submesh] : AssetHandle(0);
                    const MaterialData* embedded = submesh < mesh.Materials.size() ? &mesh.Materials[submesh] : nullptr;
                    if (materialHandle == 0 && embedded)
                    {
                        const std::filesystem::path base = meshMetadata.MeshSettings.MaterialBasePath.empty() ? meshMetadata.FilePath
                                                                                                             : meshMetadata.MeshSettings.MaterialBasePath;
                        materialHandle = assets.FindHandleByPath(MeshImporter::MaterialAssetPath(base, *embedded, submesh), AssetType::Material);
                    }
                    const auto key = std::make_pair(materialHandle, materialHandle != 0 ? nullptr : embedded);
                    auto [entry, added] = materialIndex.try_emplace(key, static_cast<uint32_t>(work.Materials.size()));
                    if (added)
                    {
                        CellWork::MaterialSource source;
                        if (materialHandle == 0 || !MaterialSerializer::Deserialize(assetDirectory / assets.GetMetadata(materialHandle).FilePath, source.Data))
                        {
                            if (embedded)
                                source.Data = *embedded;
                        }
                        auto texture = [&](const std::string& path) -> AssetHandle
                        {
                            const AssetHandle handle = path.empty() ? AssetHandle(0) : FindTextureAsset(path);
                            if (handle != 0 && !work.Textures.contains(handle))
                                work.Textures.emplace(handle, assets.GetMetadata(handle));
                            return handle;
                        };
                        source.BaseColor = texture(source.Data.BaseColorTexturePath);
                        source.MetallicRoughness = source.Data.Workflow == 1.0f ? AssetHandle(0) : texture(source.Data.MetallicRoughnessTexturePath);
                        source.Normal = texture(source.Data.NormalTexturePath);
                        source.Emissive = texture(source.Data.EmissiveTexturePath);
                        work.Materials.push_back(std::move(source));
                    }
                    work.Parts.push_back({ instance.Mesh, static_cast<uint32_t>(submesh), instance.World, entry->second });
                }
            }
            if (work.Parts.empty())
                return true;

            // The textures, the sources and the bake, on a worker.
            const WorldPartitionSettings& settings = m_World->GetWorldPartition().Settings;
            HLOD::Settings bakeSettings;
            bakeSettings.Error = WorldUnits::FromMeters(settings.HLODError);
            bakeSettings.MaxTriangles = settings.HLODMaxTriangles;
            bakeSettings.TexelsPerUnit = settings.HLODTexelsPerMeter / WorldUnits::FromMeters(1.0f);
            bakeSettings.ParallelFor = [](uint32_t count, const std::function<void(uint32_t)>& body)
            {
                JobSystem::Get().ParallelFor("HLOD Bake", count, 4, [&](uint32_t, uint32_t begin, uint32_t end)
                {
                    for (uint32_t index = begin; index < end; ++index)
                        body(index);
                });
            };
            work.GltfPath = m_World->GetWorldPartition().CellDirectory / "HLOD" / (cellName + ".gltf");
            work.Result = std::make_shared<HLOD::Result>();
            work.Error = std::make_shared<std::string>();
            work.Bake = JobSystem::Get().Async("HLOD Bake", JobPriority::Normal,
                [cooked = work.Cooked, parts = work.Parts, materialSources = work.Materials, textures = work.Textures, bakeSettings, assetDirectory,
                 gltf = assetDirectory / work.GltfPath, result = work.Result, error = work.Error, cellName](const CancellationToken&)
                {
                    // Textures, decoded once each.
                    std::map<AssetHandle, std::pair<std::shared_ptr<HLOD::Texture>, bool>> decoded;
                    for (const auto& [handle, metadata] : textures)
                    {
                        auto texture = std::make_shared<HLOD::Texture>();
                        bool twoChannel = false;
                        std::string why;
                        if (DecodeCookedTexture(assetDirectory, metadata, BakeTextureSize, *texture, twoChannel, why))
                            decoded[handle] = { texture, twoChannel };
                        else
                            NOX_CORE_WARN("Build HLODs: {}: texture '{}' left out ({})", cellName, metadata.FilePath.generic_string(), why);
                    }
                    auto find = [&](AssetHandle handle) -> std::pair<std::shared_ptr<HLOD::Texture>, bool>
                    {
                        auto found = decoded.find(handle);
                        return found != decoded.end() ? found->second : std::pair<std::shared_ptr<HLOD::Texture>, bool>{};
                    };
                    std::vector<HLOD::Material> materials;
                    materials.reserve(materialSources.size());
                    for (const auto& source : materialSources)
                    {
                        const MaterialData& data = source.Data;
                        HLOD::Material material;
                        material.SpecularGlossiness = data.Workflow == 1.0f;
                        material.BaseColorFactor = material.SpecularGlossiness ? data.DiffuseFactor : data.BaseColorFactor;
                        material.Glossiness = data.SpecularFactor.a;
                        material.MetallicFactor = data.MetallicFactor;
                        material.RoughnessFactor = data.RoughnessFactor;
                        material.EmissiveFactor = data.EmissiveFactor * data.emissiveStrength;
                        material.AlphaMask = data.Mode == AlphaMode::Mask;
                        material.AlphaCutoff = data.AlphaMaskCutoff;
                        material.Translucent = data.Mode == AlphaMode::Blend || data.TransmissionFactor > 0.0f;
                        material.Transmission = std::clamp(data.TransmissionFactor, 0.0f, 1.0f);
                        material.BaseColor = find(source.BaseColor).first;
                        material.MetallicRoughness = find(source.MetallicRoughness).first;
                        const auto normal = find(source.Normal);
                        material.Normal = normal.first;
                        material.NormalTwoChannel = normal.second;
                        material.Emissive = find(source.Emissive).first;
                        materials.push_back(std::move(material));
                    }
                    // The parts' original triangles (the clusters of LOD level 0, as the ray tracing BLAS takes them).
                    std::vector<HLOD::SourceMesh> meshes;
                    meshes.reserve(parts.size());
                    for (const auto& part : parts)
                    {
                        const auto found = cooked->find(part.Mesh);
                        if (found == cooked->end() || !found->second || part.Submesh >= found->second->Submeshes.size())
                            continue;
                        const MeshData& data = found->second->Submeshes[part.Submesh];
                        HLOD::SourceMesh mesh;
                        mesh.Transform = part.World;
                        mesh.Material = part.Material;
                        mesh.Positions.reserve(data.Vertices.size());
                        for (const shaderio::Vertex& vertex : data.Vertices)
                        {
                            mesh.Positions.push_back(vertex.pos);
                            mesh.Normals.push_back(vertex.normal);
                            mesh.Tangents.push_back(vertex.tangent);
                            mesh.UVs.push_back(vertex.uv0);
                        }
                        for (size_t index = 0; index < data.Draws.size() && index < data.Bounds.size(); ++index)
                        {
                            if (data.Bounds[index].lodLevel != 0)
                                continue;
                            const shaderio::MeshletDraw& draw = data.Draws[index];
                            for (uint32_t triangle = 0; triangle < draw.triangleCount; ++triangle)
                            {
                                const uint32_t base = draw.triangleOffset + triangle * 3;
                                for (uint32_t corner = 0; corner < 3; ++corner)
                                    mesh.Indices.push_back(data.MeshletVertices[draw.vertexOffset + data.MeshletTriangles[base + corner]]);
                            }
                        }
                        meshes.push_back(std::move(mesh));
                    }
                    *result = HLOD::Build(meshes, materials, bakeSettings);
                    if (!result->Ok)
                    {
                        *error = result->Error;
                        return;
                    }
                    if (!WriteHLODGltf(gltf, *result, *error))
                        result->Ok = false;
                });
            work.Stage = CellWork::Phase::Bake;
            m_Stage = "Baking " + cellName;
            return false;
        }

        case CellWork::Phase::Bake:
        {
            if (!work.Bake.IsReady())
                return false;
            if (!work.Result->Ok)
            {
                NOX_CORE_WARN("Build HLODs: {}: no HLOD ({})", cellName, *work.Error);
                return true;
            }
            // Imported like any model (cooked to .nsmesh / .nmat / .ntex): new, or the existing asset cooked again.
            auto& assets = *Project::GetActive()->GetEditorAssetManager();
            std::filesystem::path meshPath = work.GltfPath;
            meshPath.replace_extension(".nsmesh");
            AssetHandle mesh = assets.FindHandleByPath(meshPath, AssetType::StaticMesh);
            if (mesh != 0 && AssetManager::FindLoadedAsset<StaticMesh>(mesh))
                assets.ReimportAsset(mesh, true); // loaded: cooked again and reloaded now
            else if (mesh != 0)
            {
                // Not loaded: its cook goes, so its next load cooks the new file (the .gltf's text can be the same, the .bin is not).
                std::error_code error;
                const std::filesystem::path cooked = assetDirectory / assets.GetMetadata(mesh).FilePath;
                std::filesystem::remove(cooked, error);
                std::filesystem::remove(cooked.string() + ".hash", error);
            }
            else
            {
                MeshImportSettings importSettings;
                importSettings.ImportSkeletalMeshes = false;
                importSettings.ImportAnimations = false;
                importSettings.StaticCombine = MeshCombineMode::CombineAll;
                importSettings.ImportScale = 1.0f; // written in world units
                if (assets.ImportModel(work.GltfPath, meshPath, importSettings))
                    mesh = assets.FindHandleByPath(meshPath, AssetType::StaticMesh);
            }
            if (mesh == 0)
            {
                NOX_CORE_WARN("Build HLODs: {}: '{}' could not be imported", cellName, work.GltfPath.generic_string());
                return true;
            }
            const HLOD::Result& result = *work.Result;
            m_Entries.push_back({ work.Cell, mesh, result.Origin, result.BoundsMin, result.BoundsMax });
            NOX_CORE_INFO("Build HLODs: {}: {} source triangles -> {}, {}x{} texels ({:.0f} % baked)", cellName, result.SourceTriangles,
                          result.Indices.size() / 3, result.TextureSize, result.TextureSize,
                          result.SurfaceTexels ? 100.0 * result.BakedTexels / result.SurfaceTexels : 0.0);
            return true;
        }
        }
        return true;
    }

    void HLODBuilder::WriteHLODCells()
    {
        m_Stage = "Writing the HLOD grid";
        WorldPartition& world = m_World->GetWorldPartition();
        const std::filesystem::path directory = Project::GetActiveAssetDirectory() / world.CellDirectory;

        // The HLOD grid's old cells go (files and index), the new ones are written.
        std::error_code error;
        for (auto it = world.Cells.begin(); it != world.Cells.end();)
        {
            if (it->first.Grid != WorldGrid::HLODGridIndex)
            {
                ++it;
                continue;
            }
            std::filesystem::remove(directory / (WorldGrid::CellName(it->first) + WorldGrid::CellExtension), error);
            it = world.Cells.erase(it);
        }

        const float hlodCellSize = WorldUnits::FromMeters(world.Settings.HLODCellSize);
        Ref<Scene> hlodScene = CreateRef<Scene>();
        std::map<WorldCellCoord, std::vector<Entity>> groups;
        for (const Entry& entry : m_Entries)
        {
            Entity entity = hlodScene->CreateEntity("HLOD " + WorldGrid::CellName(entry.Source));
            entity.GetComponent<TransformComponent>().Translation = entry.Origin;
            entity.AddComponent<MeshComponent>(entry.Mesh).SubmeshCount = UINT32_MAX;
            entity.AddComponent<HLODComponent>().Source = entry.Source;
            groups[WorldGrid::CellFor(entry.BoundsMin, entry.BoundsMax, hlodCellSize, WorldGrid::HLODGridIndex)].push_back(entity);
        }
        for (const auto& [cell, entities] : groups)
        {
            uint32_t count = 0;
            const std::string text = SceneSerializer::WorldCellText(*hlodScene, entities, WorldGrid::CellName(cell), &count);
            const std::filesystem::path file = directory / (WorldGrid::CellName(cell) + WorldGrid::CellExtension);
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << text;
            if (!out)
            {
                NOX_CORE_ERROR("Build HLODs: could not write '{}'", file.generic_string());
                m_Failed = true;
                continue;
            }
            WorldCellInfo& info = world.Cells[cell];
            info.Entities = count;
            info.BoundsMin = glm::vec3(FLT_MAX);
            info.BoundsMax = glm::vec3(-FLT_MAX);
            for (const Entry& entry : m_Entries)
            {
                if (WorldGrid::CellFor(entry.BoundsMin, entry.BoundsMax, hlodCellSize, WorldGrid::HLODGridIndex) == cell)
                {
                    info.BoundsMin = glm::min(info.BoundsMin, entry.BoundsMin);
                    info.BoundsMax = glm::max(info.BoundsMax, entry.BoundsMax);
                }
            }
        }
        NOX_CORE_INFO("Build HLODs: {} HLODs in {} HLOD cells", m_Entries.size(), groups.size());
        m_Stage = "Done";
    }
}
