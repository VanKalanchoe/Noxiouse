#include "EditorAssetManager.h"

#include <entt/entt.hpp>

#include "AssetManager.h"

#include "AssetImporter.h"

#include <fstream>
#include <yaml-cpp/yaml.h>

#include "NoxCore/Core/Log.h"

namespace Nox
{
    static std::map<std::filesystem::path, AssetType> s_AssetExtensionMap = 
    {
        { ".nox", AssetType::Scene },
        { ".png", AssetType::Texture2D },
        { ".jpg", AssetType::Texture2D },
        { ".jpeg", AssetType::Texture2D },
        { ".ktx2", AssetType::Texture2D },
        { ".gltf", AssetType::MeshSource },
        { ".glb", AssetType::MeshSource },
        { ".nmesh", AssetType::Mesh },
        { ".nsmesh", AssetType::StaticMesh }
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
        NOX_CORE_INFO("Auto-Reimporting asset from source: {}", metadata.SourceFilePath.string());

        // 1. Delete the old cooked cache (.nsmesh/.nmesh) so the Importer is forced to re-cook the GLTF
        auto cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;
        if (std::filesystem::exists(cookedPath))
        {
            std::filesystem::remove(cookedPath);
        }

        // 2. Re-run the importer on the GLTF
        Ref<Asset> reimportedAsset = AssetImporter::ImportAsset(handle, metadata);
        
        // 3. Overwrite the loaded asset. (If your ECS holds a Ref<Asset> to this, 
        // it will automatically update in the viewport!)
        if (reimportedAsset)
        {
            reimportedAsset->Handle = handle;
            m_LoadedAssets[handle] = reimportedAsset;
        }
    }

    void EditorAssetManager::OnAssetModifiedOnDisk(const std::filesystem::path& absolutePath)
    {
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
        
        Ref<Asset> asset = AssetImporter::ImportAsset(handle, metadata);
        if (asset)
        {
            asset->Handle = handle;
            m_LoadedAssets[handle] = asset;
            m_AssetRegistry[handle] = metadata;
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
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
            out << YAML::EndMap; // Root
        }
        
        std::ofstream fout(path);
        fout << out.c_str();
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
        }

        return true;
    }
}
