#include "EditorAssetManager.h"

#include <entt/entt.hpp>

#include "AssetManager.h"

#include <algorithm>
#include "AssetImporter.h"
#include "NoxCore/Renderer/Mesh.h"
#include "NoxCore/Renderer/Renderer.h"
#include "Material.h"
#include "MaterialSerializer.h"

#include <fstream>
#include <cstring>
#include <cctype>
#include <yaml-cpp/yaml.h>

#include "NoxCore/Core/Log.h"

namespace Nox
{
    static std::string_view ImageFormatToString(NRI::ImageFormat format)
    {
        switch (format)
        {
        case NRI::ImageFormat::RGBA8:              return "RGBA8";
        case NRI::ImageFormat::SRGBA8:             return "SRGBA8";
        case NRI::ImageFormat::RGB8:               return "RGB8";
        case NRI::ImageFormat::SRGB8:              return "SRGB8";
        case NRI::ImageFormat::R16G16_SFLOAT:      return "R16G16_SFLOAT";
        case NRI::ImageFormat::R16G16B16A16_SFLOAT:return "R16G16B16A16_SFLOAT";
        case NRI::ImageFormat::BC1_UNorm:          return "BC1_UNorm";
        case NRI::ImageFormat::BC1_UNorm_SRGB:     return "BC1_UNorm_SRGB";
        case NRI::ImageFormat::BC2_UNorm:          return "BC2_UNorm";
        case NRI::ImageFormat::BC2_UNorm_SRGB:     return "BC2_UNorm_SRGB";
        case NRI::ImageFormat::BC3_UNorm:          return "BC3_UNorm";
        case NRI::ImageFormat::BC3_UNorm_SRGB:     return "BC3_UNorm_SRGB";
        case NRI::ImageFormat::BC4_UNorm:          return "BC4_UNorm";
        case NRI::ImageFormat::BC4_SNorm:          return "BC4_SNorm";
        case NRI::ImageFormat::BC5_UNorm:          return "BC5_UNorm";
        case NRI::ImageFormat::BC5_SNorm:          return "BC5_SNorm";
        case NRI::ImageFormat::BC6H_UF16:          return "BC6H_UF16";
        case NRI::ImageFormat::BC6H_SF16:          return "BC6H_SF16";
        case NRI::ImageFormat::BC7_UNorm:          return "BC7_UNorm";
        case NRI::ImageFormat::BC7_UNorm_SRGB:     return "BC7_UNorm_SRGB";
        default:                                   return "RGBA8";
        }
    }

    static NRI::ImageFormat ImageFormatFromString(std::string_view str)
    {
        if (str == "SRGBA8")             return NRI::ImageFormat::SRGBA8;
        if (str == "RGBA8")              return NRI::ImageFormat::RGBA8;
        if (str == "SRGB8")              return NRI::ImageFormat::SRGB8;
        if (str == "RGB8")               return NRI::ImageFormat::RGB8;
        if (str == "R16G16_SFLOAT")      return NRI::ImageFormat::R16G16_SFLOAT;
        if (str == "R16G16B16A16_SFLOAT")return NRI::ImageFormat::R16G16B16A16_SFLOAT;
        if (str == "BC1_UNorm")          return NRI::ImageFormat::BC1_UNorm;
        if (str == "BC1_UNorm_SRGB")     return NRI::ImageFormat::BC1_UNorm_SRGB;
        if (str == "BC2_UNorm")          return NRI::ImageFormat::BC2_UNorm;
        if (str == "BC2_UNorm_SRGB")     return NRI::ImageFormat::BC2_UNorm_SRGB;
        if (str == "BC3_UNorm")          return NRI::ImageFormat::BC3_UNorm;
        if (str == "BC3_UNorm_SRGB")     return NRI::ImageFormat::BC3_UNorm_SRGB;
        if (str == "BC4_UNorm")          return NRI::ImageFormat::BC4_UNorm;
        if (str == "BC4_SNorm")          return NRI::ImageFormat::BC4_SNorm;
        if (str == "BC5_UNorm")          return NRI::ImageFormat::BC5_UNorm;
        if (str == "BC5_SNorm")          return NRI::ImageFormat::BC5_SNorm;
        if (str == "BC6H_UF16")          return NRI::ImageFormat::BC6H_UF16;
        if (str == "BC6H_SF16")          return NRI::ImageFormat::BC6H_SF16;
        if (str == "BC7_UNorm")          return NRI::ImageFormat::BC7_UNorm;
        if (str == "BC7_UNorm_SRGB")     return NRI::ImageFormat::BC7_UNorm_SRGB;
        return NRI::ImageFormat::RGBA8;
    }

    static std::filesystem::path GeneratedAssetPath(
        const std::filesystem::path& sourcePath,
        const char* folder,
        const char* extension)
    {
        auto parent = sourcePath.parent_path();
        std::string parentFolder = parent.filename().string();
        std::string expectedFolder = folder;
        std::transform(parentFolder.begin(), parentFolder.end(), parentFolder.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        std::transform(expectedFolder.begin(), expectedFolder.end(), expectedFolder.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });

        if (parentFolder == expectedFolder)
            return parent / (sourcePath.stem().string() + extension);
        return parent / folder / (sourcePath.stem().string() + extension);
    }

    static std::string SanitizeAssetName(std::string name)
    {
        constexpr const char* invalid = "<>:\"/\\|?*";
        for (char& character : name)
        {
            if (std::strchr(invalid, character) != nullptr)
                character = '_';
        }
        return name.empty() ? "Material" : name;
    }
    
    static std::map<std::filesystem::path, AssetType> s_AssetExtensionMap = 
    {
        { ".nox", AssetType::Scene },
        { ".png", AssetType::Texture2D },
        { ".jpg", AssetType::Texture2D },
        { ".jpeg", AssetType::Texture2D },
        { ".ktx2", AssetType::Texture2D },
        { ".ntex", AssetType::Texture2D },
        { ".gltf", AssetType::MeshSource },
        { ".glb", AssetType::MeshSource },
        { ".nmesh", AssetType::Mesh },
        { ".nsmesh", AssetType::StaticMesh },
        { ".nmat", AssetType::Material },
        
        { ".nskel",   AssetType::Skeleton },
        { ".nanim",   AssetType::AnimationSequence },
        { ".nskmesh", AssetType::SkeletalMesh }
    };

    AssetType EditorAssetManager::GetAssetTypeFromExtension(const std::filesystem::path& extension)
    {
        if (s_AssetExtensionMap.find(extension) == s_AssetExtensionMap.end())
        {
            NOX_CORE_WARN("Could not find AssetType for {}", extension.string());
            return AssetType::None;
        }

        return s_AssetExtensionMap.at(extension);
    }
    
    /*YAML::Emitter& operator<<(YAML::Emitter& out, const std::string_view& v)
    {
        out << std::string(v.data(), v.size());
        return out;
    }*/
    
    bool EditorAssetManager::IsAssetHandleValid(AssetHandle handle) const
    {
        return handle != 0 && m_AssetRegistry.find(handle) != m_AssetRegistry.end();
    }

    bool EditorAssetManager::IsAssetLoaded(AssetHandle handle) const
    {
        return m_LoadedAssets.find(handle) != m_LoadedAssets.end();
    }

    AssetType EditorAssetManager::GetAssetType(AssetHandle handle) const
    {
        if (!IsAssetHandleValid(handle))
            return AssetType::None;

        return m_AssetRegistry.at(handle).Type;
    }

    void EditorAssetManager::Init()
    {
        m_AssetWatcher.watch(Project::GetActiveAssetDirectory(), [this](const std::filesystem::path& path) 
        {
            OnAssetModifiedOnDisk(path);
        });
    }

    void EditorAssetManager::Update()
    {
        std::set<AssetHandle> toReimport;
        
        // Quickly copy and clear the queue safely
        {
            std::lock_guard<std::mutex> lock(m_ReimportMutex);
            toReimport = m_PendingReimports;
            m_PendingReimports.clear();
        }

        // Now we are on the MAIN THREAD, we can safely invoke the importer and Vulkan code
        for (AssetHandle handle : toReimport)
        {
            ReimportAsset(handle);
        }
    }
    
    void EditorAssetManager::ReimportAsset(AssetHandle handle)
    {
        if (!IsAssetHandleValid(handle)) return;

        const AssetMetadata& metadata = GetMetadata(handle);
        auto sourcePath = Project::GetActiveAssetDirectory() / metadata.SourceFilePath;
        if (!std::filesystem::exists(sourcePath))
        {
            NOX_CORE_WARN("Skipping auto-reimport: source file no longer exists: {}", sourcePath.string());
            return;
        }

        // The file watcher can fire on this source file even though its content never actually
        // changed -- e.g. our own cooker rewrites extracted embedded textures unconditionally on
        // every recook, and that write is picked up by the same recursive watch. Verify against the
        // last known content hash before doing anything destructive; a spurious event is a no-op.
        XXH128_hash_t currentHash = Utility::calcul_hash_streaming(sourcePath.string());
        auto knownHashIt = m_LastKnownSourceHash.find(handle);
        if (knownHashIt != m_LastKnownSourceHash.end() && XXH128_isEqual(currentHash, knownHashIt->second))
        {
            NOX_CORE_INFO("Skipping auto-reimport, source content unchanged: {}", metadata.SourceFilePath.string());
            return;
        }
        m_LastKnownSourceHash[handle] = currentHash;

        NOX_CORE_INFO("Auto-Reimporting asset from source: {}", metadata.SourceFilePath.string());

        // 1. Delete the old cooked cache (.nsmesh/.nmesh) so the Importer is forced to re-cook the GLTF
        if (metadata.Type == AssetType::Mesh || metadata.Type == AssetType::StaticMesh ||
            metadata.Type == AssetType::MeshSource || metadata.Type == AssetType::SkeletalMesh)
        {
            auto cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;
            auto ext = cookedPath.extension();
            if ((ext == ".nsmesh" || ext == ".nmesh") && std::filesystem::exists(cookedPath))
            {
                std::filesystem::remove(cookedPath);
                if (std::filesystem::exists(cookedPath.string() + ".hash"))
                    std::filesystem::remove(cookedPath.string() + ".hash");
            }
        }
        else if (metadata.Type == AssetType::Texture2D)
        {
            auto cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;
            if (cookedPath.extension() == ".ntex" && std::filesystem::exists(cookedPath))
            {
                std::filesystem::remove(cookedPath);
                if (std::filesystem::exists(cookedPath.string() + ".hash"))
                    std::filesystem::remove(cookedPath.string() + ".hash");
            }
        }

        // 2. Re-run the importer on the GLTF
        Ref<Asset> reimportedAsset = AssetImporter::ImportAsset(handle, metadata);
        
        // 3. Overwrite the loaded asset. (If your ECS holds a Ref<Asset> to this, 
        // it will automatically update in the viewport!)
        if (reimportedAsset)
        {
            reimportedAsset->Handle = handle;
            m_LoadedAssets[handle] = reimportedAsset;

            if (metadata.Type == AssetType::Mesh ||
                metadata.Type == AssetType::StaticMesh ||
                metadata.Type == AssetType::MeshSource)
            {
                ImportMeshTextures(reimportedAsset);
                ImportMeshMaterials(reimportedAsset, metadata);
            }
        }

        // 4. Scan and register any new .nanim / .nskel files generated during cooking (they land
        // next to the model: <Model>/Meshes, <Model>/Materials, <Model>/Textures).
        ScanAndRegisterNewAssets(metadata.FilePath.parent_path().parent_path());
    }

    void EditorAssetManager::OnAssetModifiedOnDisk(const std::filesystem::path& absolutePath)
    {
        if (!std::filesystem::exists(absolutePath))
            return;
        
        if (absolutePath.extension() == ".nsmesh" || absolutePath.extension() == ".nmesh")
            return;
        
        // Convert to relative path to match our Asset Registry
        std::filesystem::path relativePath = std::filesystem::relative(absolutePath, Project::GetActiveAssetDirectory());
        
        AssetHandle handleToReimport = 0;

        // Search the registry to see if this modified file is a Source file for one of our assets
        for (const auto& [handle, metadata] : m_AssetRegistry)
        {
            if (metadata.SourceFilePath == relativePath)
            {
                handleToReimport = handle;
                break;
            }
        }

        // If we found it, safely queue it for the main thread
        if (handleToReimport != 0)
        {
            std::lock_guard<std::mutex> lock(m_ReimportMutex);
            m_PendingReimports.insert(handleToReimport);
        }
    }

    void EditorAssetManager::ImportAsset(const std::filesystem::path& sourcePath, const std::filesystem::path& destPath, AssetType targetType)
    {
        AssetHandle handle; // generate new handle
        AssetMetadata metadata;
        metadata.FilePath = destPath.empty() ? sourcePath : destPath;
        metadata.SourceFilePath = sourcePath;
        
        // If a target type was provided (e.g. from a UI menu), use it. 
        // Otherwise, fall back to whatever the file extension is.
        metadata.Type = (targetType != AssetType::None) ? targetType : GetAssetTypeFromExtension(sourcePath.extension());
        NOX_CORE_ASSERT(metadata.Type != AssetType::None, "could not determine asset type from extension");

        if (destPath.empty() && metadata.Type == AssetType::MeshSource)
            metadata.FilePath = GeneratedAssetPath(sourcePath, "Meshes", ".nmesh");

        if (metadata.Type == AssetType::Texture2D && destPath.empty() &&
            sourcePath.extension() != ".ntex" && sourcePath.extension() != ".ktx2")
        {
            metadata.FilePath = GeneratedAssetPath(sourcePath, "Textures", ".ntex");
        }
        
        Ref<Asset> asset = AssetImporter::ImportAsset(handle, metadata);
        if (asset)
        {
            asset->Handle = handle;
            m_LoadedAssets[handle] = asset;
            m_AssetRegistry[handle] = metadata;
            m_LastKnownSourceHash[handle] = Utility::calcul_hash_streaming((Project::GetActiveAssetDirectory() / metadata.SourceFilePath).string());

            if (metadata.Type == AssetType::Mesh ||
                metadata.Type == AssetType::StaticMesh ||
                metadata.Type == AssetType::MeshSource)
            {
                ImportMeshTextures(asset);
                ImportMeshMaterials(asset, metadata);
            }

            // Scan for extracted .nanim / .nskel files (generated next to the model)
            ScanAndRegisterNewAssets(metadata.FilePath.parent_path().parent_path());

            SerializeAssetRegistry();
        }
    }

    void EditorAssetManager::ImportMeshTextures(const Ref<Asset>& meshAsset)
    {
        const std::vector<MaterialData>* materials = nullptr;
        if (meshAsset->GetType() == AssetType::Mesh)
            materials = &static_cast<Mesh*>(meshAsset.get())->GetMaterials();
        else if (meshAsset->GetType() == AssetType::StaticMesh)
            materials = &static_cast<StaticMesh*>(meshAsset.get())->GetMaterials();

        if (!materials)
            return;

        // Bistro: 254 materials x 6 slots = 1524 paths, all usually already imported. Checking each
        // with std::filesystem::relative + a linear registry scan measured 2.1s per load. Index the
        // registry once, resolve paths lexically, and handle each distinct path only once.
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        std::unordered_set<std::string> registeredTexturePaths;
        for (const auto& [textureHandle, metadata] : m_AssetRegistry)
        {
            if (metadata.Type != AssetType::Texture2D)
                continue;
            registeredTexturePaths.insert(metadata.FilePath.lexically_normal().generic_string());
            if (!metadata.SourceFilePath.empty())
                registeredTexturePaths.insert(metadata.SourceFilePath.lexically_normal().generic_string());
        }
        std::unordered_set<std::string> processedPaths;

        auto importTexture = [&](const std::string& texturePath, bool sRGB)
        {
            if (texturePath.empty() || !processedPaths.insert(texturePath).second)
                return;

            std::filesystem::path sourcePath(texturePath);
            std::filesystem::path relativePath;
            if (sourcePath.is_absolute())
            {
                relativePath = sourcePath.lexically_normal().lexically_relative(assetDirectory.lexically_normal());
                if (relativePath.empty() || *relativePath.begin() == "..")
                {
                    std::error_code ec;
                    relativePath = std::filesystem::relative(sourcePath, assetDirectory, ec);
                    if (ec)
                        return;
                }
            }
            else
            {
                relativePath = sourcePath;
            }

            std::filesystem::path cookedPath = GeneratedAssetPath(relativePath, "Textures", ".ntex");

            if (registeredTexturePaths.contains(cookedPath.lexically_normal().generic_string()) ||
                registeredTexturePaths.contains(relativePath.lexically_normal().generic_string()))
            {
                return;
            }

            std::filesystem::path fullSourcePath =
                Project::GetActiveAssetDirectory() / relativePath;
            if (!std::filesystem::exists(fullSourcePath))
                return;

            TextureSpecification spec;
            spec.format = sRGB ? NRI::ImageFormat::SRGBA8 : NRI::ImageFormat::RGBA8;
            ImportAsset(relativePath, spec, {});

            // Keep the index current so another path spelling of the same file isn't imported twice.
            registeredTexturePaths.insert(cookedPath.lexically_normal().generic_string());
            registeredTexturePaths.insert(relativePath.lexically_normal().generic_string());
        };

        for (const MaterialData& material : *materials)
        {
            importTexture(material.BaseColorTexturePath, true);
            importTexture(material.MetallicRoughnessTexturePath, false);
            importTexture(material.NormalTexturePath, false);
            importTexture(material.OcclusionTexturePath, false);
            importTexture(material.EmissiveTexturePath, true);
            importTexture(material.TransmissionTexturePath, false);
        }
    }

    void EditorAssetManager::ImportMeshMaterials(const Ref<Asset>& meshAsset, const AssetMetadata& meshMetadata)
    {
        const std::vector<MaterialData>* materials = nullptr;
        std::vector<AssetHandle> materialAssets;

        if (meshAsset->GetType() == AssetType::Mesh)
        {
            auto* mesh = static_cast<Mesh*>(meshAsset.get());
            materials = &mesh->GetMaterials();
        }
        else if (meshAsset->GetType() == AssetType::StaticMesh)
        {
            auto* mesh = static_cast<StaticMesh*>(meshAsset.get());
            materials = &mesh->GetMaterials();
        }

        if (!materials)
            return;

        materialAssets.reserve(materials->size());

        // One registry pass instead of a linear scan per material (254 for Bistro).
        std::unordered_map<std::string, AssetHandle> materialByPath;
        for (const auto& [handle, metadata] : m_AssetRegistry)
        {
            if (metadata.Type == AssetType::Material)
                materialByPath.emplace(metadata.FilePath.lexically_normal().generic_string(), handle);
        }

        for (size_t index = 0; index < materials->size(); ++index)
        {
            const MaterialData& material = (*materials)[index];
            std::string materialName = material.Name.empty()
                ? "Material_" + std::to_string(index)
                : SanitizeAssetName(material.Name);

            std::filesystem::path materialPath = meshMetadata.FilePath.parent_path().parent_path() / "Materials" /
                (meshMetadata.FilePath.stem().string() + "_" + materialName + ".nmat");

            AssetHandle materialHandle = 0;
            if (auto found = materialByPath.find(materialPath.lexically_normal().generic_string()); found != materialByPath.end())
                materialHandle = found->second;

            const auto fullMaterialPath = Project::GetActiveAssetDirectory() / materialPath;
            if (materialHandle == 0 || !std::filesystem::exists(fullMaterialPath))
            {
                if (!MaterialSerializer::Serialize(fullMaterialPath, material))
                    continue;

                if (materialHandle != 0)
                {
                    ReimportAsset(materialHandle);
                }
                else
                {
                    ImportAsset(materialPath, materialPath, AssetType::Material);
                    for (const auto& [handle, metadata] : m_AssetRegistry)
                    {
                        if (metadata.Type == AssetType::Material && metadata.FilePath == materialPath)
                        {
                            materialHandle = handle;
                            break;
                        }
                    }
                }
            }

            if (materialHandle != 0)
                materialAssets.push_back(materialHandle);
        }

        if (meshAsset->GetType() == AssetType::Mesh)
            static_cast<Mesh*>(meshAsset.get())->SetMaterialAssets(std::move(materialAssets));
        else
            static_cast<StaticMesh*>(meshAsset.get())->SetMaterialAssets(std::move(materialAssets));
    }
    
    void EditorAssetManager::ImportAsset(const std::filesystem::path& sourcePath, const TextureSpecification& spec, const std::filesystem::path& destPath)
    {
        AssetHandle handle;
        AssetMetadata metadata;
        if (!destPath.empty())
        {
            metadata.FilePath = destPath;
        }
        else if (sourcePath.extension() == ".ntex" || sourcePath.extension() == ".ktx2")
        {
            metadata.FilePath = sourcePath;
        }
        else
        {
            metadata.FilePath = GeneratedAssetPath(sourcePath, "Textures", ".ntex");
        }
        metadata.SourceFilePath = sourcePath;
        metadata.Type = AssetType::Texture2D;
        metadata.TextureSpec = spec; // <-- Store spec in metadata

        Ref<Asset> asset = AssetImporter::ImportAsset(handle, metadata);
        if (asset)
        {
            asset->Handle = handle;
            m_LoadedAssets[handle] = asset;
            m_AssetRegistry[handle] = metadata;
            m_LastKnownSourceHash[handle] = Utility::calcul_hash_streaming((Project::GetActiveAssetDirectory() / metadata.SourceFilePath).string());

            // A texture import only produces this one .ntex, registered right above - no scan needed.
            SerializeAssetRegistry();
        }
    }

    const AssetMetadata EditorAssetManager::GetMetadata(AssetHandle handle) const
    {
        static AssetMetadata s_NullMetadata;
        auto it = m_AssetRegistry.find(handle);
        if (it == m_AssetRegistry.end())
            return s_NullMetadata;

        return it->second;
    }

    const std::filesystem::path EditorAssetManager::GetFilePath(AssetHandle handle) const
    {
        return GetMetadata(handle).FilePath;
    }

    Ref<Asset> EditorAssetManager::GetAsset(AssetHandle handle)
    {
        // 1. check if handle is valid
        if (!IsAssetHandleValid(handle))
            return {};
        
        // 2. check if asset needs load (and if so, load)
        Ref<Asset> asset;
        if (IsAssetLoaded(handle))
        {
            asset = m_LoadedAssets.at(handle);
        }
        else
        {
            // load asset
            using Clock = std::chrono::steady_clock;
            auto elapsedMs = [](Clock::time_point from, Clock::time_point to)
            {
                return std::chrono::duration<double, std::milli>(to - from).count();
            };
            const Clock::time_point loadStart = Clock::now();

            const AssetMetadata& metadata = GetMetadata(handle);
            asset = AssetImporter::ImportAsset(handle, metadata);
            if (!asset)
            {
                // import failed
                NOX_CORE_ASSERT("EditorAssetManager::GetAsset - asset import failed")
            }
            m_LoadedAssets[handle] = asset;
            const Clock::time_point importDone = Clock::now();

            if (asset && !metadata.SourceFilePath.empty())
            {
                auto sourcePath = Project::GetActiveAssetDirectory() / metadata.SourceFilePath;
                if (std::filesystem::exists(sourcePath))
                    m_LastKnownSourceHash[handle] = Utility::calcul_hash_streaming(sourcePath.string());
            }
            const Clock::time_point hashDone = Clock::now();

            Clock::time_point texturesDone = hashDone, materialsDone = hashDone, scanDone = hashDone;
            if (asset && (metadata.Type == AssetType::Mesh ||
                          metadata.Type == AssetType::StaticMesh ||
                          metadata.Type == AssetType::MeshSource))
            {
                ImportMeshTextures(asset);
                texturesDone = Clock::now();
                ImportMeshMaterials(asset, metadata);
                materialsDone = Clock::now();
                ScanAndRegisterNewAssets(metadata.FilePath.parent_path().parent_path());
                scanDone = Clock::now();
            }

            // Main-thread asset loads stall the editor; log the slow ones with a phase breakdown.
            const double totalMs = elapsedMs(loadStart, scanDone);
            if (totalMs >= 5.0)
            {
                NOX_CORE_INFO("[AssetLoad] {} ({}) took {:.1f} ms: import {:.1f}, source hash {:.1f}, mesh textures {:.1f}, materials {:.1f}, scan {:.1f}",
                              metadata.FilePath.generic_string(), AssetTypeToString(metadata.Type), totalMs,
                              elapsedMs(loadStart, importDone), elapsedMs(importDone, hashDone),
                              elapsedMs(hashDone, texturesDone), elapsedMs(texturesDone, materialsDone),
                              elapsedMs(materialsDone, scanDone));
            }
        }
        // 3. return asset
        return asset;
    }

    void EditorAssetManager::Shutdown()
    {
        m_LoadedAssets.clear();
    }

    size_t EditorAssetManager::UnloadUnusedAssets(const std::unordered_set<AssetHandle>& referencedAssets)
    {
        // Materials reference textures by path, not handle. Cached hits skip the filesystem entirely;
        // the registry index is only built if some path isn't cached yet.
        std::unordered_map<std::string, AssetHandle> textureByPath;
        bool textureIndexBuilt = false;

        if (m_TexturePathMissesRegistrySize != m_AssetRegistry.size())
        {
            m_TexturePathMisses.clear();
            m_TexturePathMissesRegistrySize = m_AssetRegistry.size();
        }

        auto resolveTexture = [&](const std::string& texturePath) -> AssetHandle
        {
            if (texturePath.empty())
                return 0;

            if (auto cached = m_TexturePathCache.find(texturePath); cached != m_TexturePathCache.end())
                return cached->second;
            if (m_TexturePathMisses.contains(texturePath))
                return 0;

            if (!textureIndexBuilt)
            {
                for (const auto& [handle, metadata] : m_AssetRegistry)
                {
                    if (metadata.Type != AssetType::Texture2D)
                        continue;
                    if (!metadata.SourceFilePath.empty())
                        textureByPath[metadata.SourceFilePath.generic_string()] = handle;
                    textureByPath[metadata.FilePath.generic_string()] = handle;
                }
                textureIndexBuilt = true;
            }

            std::filesystem::path relativePath(texturePath);
            if (relativePath.is_absolute())
            {
                // Material paths are built from the project's asset directory string, so a lexical
                // relative path almost always works; std::filesystem::relative (disk access) is the fallback.
                std::filesystem::path lexical = relativePath.lexically_normal().lexically_relative(
                    Project::GetActiveAssetDirectory().lexically_normal());
                if (!lexical.empty() && *lexical.begin() != "..")
                {
                    relativePath = lexical;
                }
                else
                {
                    std::error_code ec;
                    relativePath = std::filesystem::relative(relativePath, Project::GetActiveAssetDirectory(), ec);
                    if (ec)
                    {
                        m_TexturePathMisses.insert(texturePath);
                        return 0;
                    }
                }
            }

            auto it = textureByPath.find(relativePath.generic_string());
            if (it == textureByPath.end())
                it = textureByPath.find(GeneratedAssetPath(relativePath, "Textures", ".ntex").generic_string());
            if (it == textureByPath.end())
            {
                m_TexturePathMisses.insert(texturePath);
                return 0;
            }

            m_TexturePathCache.emplace(texturePath, it->second);
            return it->second;
        };

        // --- Mark ---
        std::unordered_set<AssetHandle> marked;
        std::vector<AssetHandle> pending(referencedAssets.begin(), referencedAssets.end());

        auto markMaterialTextures = [&](const MaterialData& material)
        {
            for (const std::string* path : { &material.BaseColorTexturePath, &material.MetallicRoughnessTexturePath,
                                             &material.NormalTexturePath, &material.OcclusionTexturePath,
                                             &material.EmissiveTexturePath, &material.TransmissionTexturePath })
            {
                if (AssetHandle textureHandle = resolveTexture(*path))
                    pending.push_back(textureHandle);
            }
        };

        while (!pending.empty())
        {
            AssetHandle handle = pending.back();
            pending.pop_back();
            if (handle == 0 || !marked.insert(handle).second)
                continue;

            auto loaded = m_LoadedAssets.find(handle);
            if (loaded == m_LoadedAssets.end() || !loaded->second)
                continue;

            const Ref<Asset>& asset = loaded->second;
            switch (asset->GetType())
            {
                case AssetType::Mesh:
                {
                    const auto* mesh = static_cast<Mesh*>(asset.get());
                    pending.insert(pending.end(), mesh->GetMaterialAssets().begin(), mesh->GetMaterialAssets().end());
                    for (const MaterialData& material : mesh->GetMaterials())
                        markMaterialTextures(material);
                    break;
                }
                case AssetType::StaticMesh:
                {
                    const auto* mesh = static_cast<StaticMesh*>(asset.get());
                    pending.insert(pending.end(), mesh->GetMaterialAssets().begin(), mesh->GetMaterialAssets().end());
                    for (const MaterialData& material : mesh->GetMaterials())
                        markMaterialTextures(material);
                    break;
                }
                case AssetType::Material:
                    markMaterialTextures(static_cast<Material*>(asset.get())->GetData());
                    break;
                default:
                    break;
            }
        }

        // --- Sweep ---
        auto isUnloadable = [](AssetType type)
        {
            switch (type)
            {
                case AssetType::Mesh:
                case AssetType::StaticMesh:
                case AssetType::MeshSource:
                case AssetType::Material:
                case AssetType::Texture2D:
                case AssetType::Skeleton:
                case AssetType::AnimationSequence:
                    return true;
                default:
                    return false;
            }
        };

        size_t unloadedCount = 0;
        std::vector<uint32_t> unloadedTextureSlots;
        for (auto it = m_LoadedAssets.begin(); it != m_LoadedAssets.end();)
        {
            const Ref<Asset>& asset = it->second;

            // RefCount > 1: something outside the registry still holds this asset (renderer,
            // thumbnail cache, an open editor panel) - it's in use even if no scene references it.
            if (marked.contains(it->first) || !asset || !isUnloadable(asset->GetType()) || asset->GetRefCount() > 1)
            {
                ++it;
                continue;
            }

            if (asset->GetType() == AssetType::Texture2D)
            {
                if (auto* texture = dynamic_cast<Texture2D*>(asset.get()))
                    unloadedTextureSlots.push_back(texture->GetDescriptorIndexSlot());
            }
            Renderer::DeferAssetRelease(asset);
            it = m_LoadedAssets.erase(it);
            ++unloadedCount;
        }

        // Only evict cache entries for the unloaded textures' slots: clearing the whole cache made
        // every remaining textured draw (all of Bistro) re-resolve its paths on the next frame.
        Renderer::InvalidateTextureDescriptorSlots(unloadedTextureSlots);

        if (unloadedCount > 0)
            NOX_CORE_INFO("EditorAssetManager: unloaded {} unused asset(s)", unloadedCount);

        return unloadedCount;
    }

    void EditorAssetManager::SerializeAssetRegistry()
    {
        auto path = Project::GetActiveAssetRegistryPath();

        YAML::Emitter out;
        {
            out << YAML::BeginMap; // Root
            out << YAML::Key << "AssetRegistry" << YAML::Value;

            out << YAML::BeginSeq;
            for (const auto&[handle, metadata] : m_AssetRegistry)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Handle" << YAML::Value << handle;
                std::string filepathStr = metadata.FilePath.generic_string();
                out << YAML::Key << "FilePath" << YAML::Value << filepathStr;
                if (!metadata.SourceFilePath.empty())
                    out << YAML::Key << "SourceFilePath" << YAML::Value << metadata.SourceFilePath.generic_string();
                out << YAML::Key << "Type" << YAML::Value << AssetTypeToString(metadata.Type);
                
                // --- ADD THIS: Save TextureSpecification for Texture2D assets ---
                if (metadata.Type == AssetType::Texture2D)
                {
                    out << YAML::Key << "TextureSpec" << YAML::Value;
                    out << YAML::BeginMap;
                    out << YAML::Key << "Format" << YAML::Value << std::string(ImageFormatToString(metadata.TextureSpec.format));
                    out << YAML::Key << "GenerateMips" << YAML::Value << metadata.TextureSpec.generateMips;
                    out << YAML::Key << "Flip" << YAML::Value << metadata.TextureSpec.flip;
                    out << YAML::EndMap;
                }
                
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
            out << YAML::EndMap; // Root
        }
        
        std::ofstream fout(path);
        fout << out.c_str();
    }

    void EditorAssetManager::ScanAndRegisterNewAssets(const std::filesystem::path& relativeDirectory)
    {
        // This runs on the main thread whenever a mesh is loaded or imported. The old version walked
        // the entire asset tree, called std::filesystem::relative (disk access) per cooked file and
        // compared each against the whole registry - measured ~150ms+ per call on this project,
        // sometimes several calls per drag-in. Now: only the imported model's folder, a lexical
        // relative path (entries are already prefixed by the scan root), and a hash set lookup.
        const std::filesystem::path assetDir = Project::GetActiveAssetDirectory();
        const std::filesystem::path scanRoot = relativeDirectory.empty() ? assetDir : assetDir / relativeDirectory;

        std::error_code ec;
        if (!std::filesystem::is_directory(scanRoot, ec)) return;

        std::unordered_set<std::string> registeredPaths;
        registeredPaths.reserve(m_AssetRegistry.size() * 2);
        for (const auto& [handle, metadata] : m_AssetRegistry)
        {
            registeredPaths.insert(metadata.FilePath.lexically_normal().generic_string());
            if (!metadata.SourceFilePath.empty())
                registeredPaths.insert(metadata.SourceFilePath.lexically_normal().generic_string());
        }

        bool registryChanged = false;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(scanRoot, ec))
        {
            if (!entry.is_regular_file(ec)) continue;

            std::filesystem::path ext = entry.path().extension();
            if (ext == ".nox" || ext == ".nanim" || ext == ".nskel" || ext == ".nmat" ||
                ext == ".ntex" || ext == ".nmesh" || ext == ".nsmesh")
            {
                std::filesystem::path relativePath = entry.path().lexically_relative(assetDir).lexically_normal();

                // insert() returning false means it was already registered.
                if (registeredPaths.insert(relativePath.generic_string()).second)
                {
                    AssetHandle newHandle; // generates new random handle
                    AssetMetadata metadata;
                    metadata.FilePath = relativePath;
                    metadata.SourceFilePath = relativePath;
                    metadata.Type = GetAssetTypeFromExtension(ext);

                    m_AssetRegistry[newHandle] = metadata;
                    registryChanged = true;
                    NOX_CORE_INFO("[EditorAssetManager] Auto-registered newly discovered asset: {}", relativePath.string());
                }
            }
        }

        if (registryChanged)
        {
            SerializeAssetRegistry();
        }
    }

    bool EditorAssetManager::DeserializeAssetRegistry()
    {
        auto path = Project::GetActiveAssetRegistryPath();

        if (!std::filesystem::exists(path))
        {
            NOX_CORE_ERROR("Asset Registry file does not exist: {0}", path.string());
            return false;
        }
        
        YAML::Node data;
        try
        {
            data = YAML::LoadFile(path.string());
        }
        catch (YAML::ParserException e)
        {
            NOX_CORE_ERROR("Failed to load project file '{0}'\n    {1}", path.string(), e.what());
        }

        auto rootNode = data["AssetRegistry"];
        if (!rootNode)
            return false;

        for (const auto& node : rootNode)
        {
            AssetHandle handle = node["Handle"].as<uint64_t>();
            auto& metadata = m_AssetRegistry[handle];
            metadata.FilePath = node["FilePath"].as<std::string>();
            if (node["SourceFilePath"])
                metadata.SourceFilePath = node["SourceFilePath"].as<std::string>();
            metadata.Type = AssetTypeFromString(node["Type"].as<std::string>());
            
            // --- ADD THIS: Load TextureSpecification if present ---
            if (node["TextureSpec"])
            {
                auto specNode = node["TextureSpec"];
                if (specNode["Format"])
                    metadata.TextureSpec.format = ImageFormatFromString(specNode["Format"].as<std::string>());
                if (specNode["GenerateMips"])
                    metadata.TextureSpec.generateMips = specNode["GenerateMips"].as<bool>();
                if (specNode["Flip"])
                    metadata.TextureSpec.flip = specNode["Flip"].as<bool>();
            }
        }

        return true;
    }
}
