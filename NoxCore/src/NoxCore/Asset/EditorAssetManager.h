#pragma once

#include "AssetManagerBase.h"
#include "AssetMetadata.h"

#include <map>
#include <set>
#include <mutex>
#include <unordered_map>

#include "NoxCore/Utils/NOXWatcher.h"
#include "NoxCore/Utils/Utils.h"

namespace Nox
{
    using AssetRegistry = std::map<AssetHandle, AssetMetadata>;
    
    class EditorAssetManager : public AssetManagerBase
    {
    public:
        virtual Ref<Asset> GetAsset(AssetHandle handle) override;
        
        virtual bool IsAssetHandleValid(AssetHandle handle) const override;
        virtual bool IsAssetLoaded(AssetHandle handle) const override;
        virtual AssetType GetAssetType(AssetHandle handle) const override;

        static AssetType GetAssetTypeFromExtension(const std::filesystem::path& extension);
        void Init();
        void Update();
        void ReimportAsset(AssetHandle handle);
        
        void ImportAsset(const std::filesystem::path& sourcePath, const std::filesystem::path& destPath, AssetType targetType = AssetType::None);
        // New overload for textures with custom spec (also void)
        void ImportAsset(const std::filesystem::path& sourcePath, const TextureSpecification& spec, const std::filesystem::path& destPath = {});
        
        const AssetMetadata GetMetadata(AssetHandle handle) const;
        const std::filesystem::path GetFilePath(AssetHandle handle) const;

        const AssetRegistry& GetAssetRegistry() const { return m_AssetRegistry; }

        void Shutdown();
        
        void SerializeAssetRegistry();
        bool DeserializeAssetRegistry();
        void ScanAndRegisterNewAssets();
        
    private:
        void OnAssetModifiedOnDisk(const std::filesystem::path& absolutePath);
        void ImportMeshTextures(const Ref<Asset>& meshAsset);
        void ImportMeshMaterials(const Ref<Asset>& meshAsset, const AssetMetadata& meshMetadata);
    private:
        Utils::NOXWatcher m_AssetWatcher;
        
        std::set<AssetHandle> m_PendingReimports;
        std::mutex m_ReimportMutex;
        
        AssetRegistry m_AssetRegistry;
        AssetMap m_LoadedAssets;

        // Last content-hash seen for each asset's source file. Lets ReimportAsset tell a genuine
        // on-disk edit apart from a spurious file-watcher event caused by our own cooker writing
        // derived files (extracted textures, .nmesh/.hash/.nmat) into the same watched directory tree.
        std::unordered_map<AssetHandle, XXH128_hash_t> m_LastKnownSourceHash;

        // todo: memory-only assets
    };
}
