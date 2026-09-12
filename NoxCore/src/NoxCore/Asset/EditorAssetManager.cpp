#include "EditorAssetManager.h"

#include <entt/entt.hpp>

#include "AssetManager.h"

#include <algorithm>
#include "AssetImporter.h"
#include "NoxCore/Renderer/Mesh.h"
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

        // 4. Scan and register any new .nanim / .nskel files generated during cooking
        ScanAndRegisterNewAssets();
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

            // Scan for extracted .nanim / .nskel files
            ScanAndRegisterNewAssets();

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

        auto importTexture = [&](const std::string& texturePath, bool sRGB)
        {
            if (texturePath.empty())
                return;

            std::filesystem::path sourcePath(texturePath);
            std::filesystem::path relativePath;
            if (sourcePath.is_absolute())
            {
                std::error_code ec;
                relativePath = std::filesystem::relative(
                    sourcePath,
                    Project::GetActiveAssetDirectory(),
                    ec
                );
                if (ec)
                    return;
            }
            else
            {
                relativePath = sourcePath;
            }

            std::filesystem::path cookedPath = GeneratedAssetPath(relativePath, "Textures", ".ntex");

            for (const auto& [textureHandle, metadata] : m_AssetRegistry)
            {
                if (metadata.Type == AssetType::Texture2D &&
                    (metadata.FilePath == cookedPath || metadata.SourceFilePath == relativePath))
                {
                    return;
                }
            }

            std::filesystem::path fullSourcePath =
                Project::GetActiveAssetDirectory() / relativePath;
            if (!std::filesystem::exists(fullSourcePath))
                return;

            TextureSpecification spec;
            spec.format = sRGB ? NRI::ImageFormat::SRGBA8 : NRI::ImageFormat::RGBA8;
            ImportAsset(relativePath, spec, {});
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

        for (size_t index = 0; index < materials->size(); ++index)
        {
            const MaterialData& material = (*materials)[index];
            std::string materialName = material.Name.empty()
                ? "Material_" + std::to_string(index)
                : SanitizeAssetName(material.Name);

            std::filesystem::path materialPath = meshMetadata.FilePath.parent_path().parent_path() / "Materials" /
                (meshMetadata.FilePath.stem().string() + "_" + materialName + ".nmat");

            AssetHandle materialHandle = 0;
            for (const auto& [handle, metadata] : m_AssetRegistry)
            {
                if (metadata.Type == AssetType::Material && metadata.FilePath == materialPath)
                {
                    materialHandle = handle;
                    break;
                }
            }

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

            ScanAndRegisterNewAssets();
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
            const AssetMetadata& metadata = GetMetadata(handle);
            asset = AssetImporter::ImportAsset(handle, metadata);
            if (!asset)
            {
                // import failed
                NOX_CORE_ASSERT("EditorAssetManager::GetAsset - asset import failed")
            }
            m_LoadedAssets[handle] = asset;

            if (asset && !metadata.SourceFilePath.empty())
            {
                auto sourcePath = Project::GetActiveAssetDirectory() / metadata.SourceFilePath;
                if (std::filesystem::exists(sourcePath))
                    m_LastKnownSourceHash[handle] = Utility::calcul_hash_streaming(sourcePath.string());
            }

            if (asset && (metadata.Type == AssetType::Mesh ||
                          metadata.Type == AssetType::StaticMesh ||
                          metadata.Type == AssetType::MeshSource))
            {
                ImportMeshTextures(asset);
                ImportMeshMaterials(asset, metadata);
                ScanAndRegisterNewAssets();
            }
        }
        // 3. return asset
        return asset;
    }

    void EditorAssetManager::Shutdown()
    {
        m_LoadedAssets.clear();
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

    void EditorAssetManager::ScanAndRegisterNewAssets()
    {
        auto assetDir = Project::GetActiveAssetDirectory();
        if (!std::filesystem::exists(assetDir)) return;

        bool registryChanged = false;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(assetDir))
        {
            if (!entry.is_regular_file()) continue;

            std::filesystem::path ext = entry.path().extension();
            if (ext == ".nox" || ext == ".nanim" || ext == ".nskel" || ext == ".nmat" ||
                ext == ".ntex" || ext == ".nmesh" || ext == ".nsmesh")
            {
                std::filesystem::path relativePath = std::filesystem::relative(entry.path(), assetDir);

                bool found = false;
                for (const auto& [handle, metadata] : m_AssetRegistry)
                {
                    if (metadata.FilePath == relativePath || metadata.SourceFilePath == relativePath)
                    {
                        found = true;
                        break;
                    }
                }

                if (!found)
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
