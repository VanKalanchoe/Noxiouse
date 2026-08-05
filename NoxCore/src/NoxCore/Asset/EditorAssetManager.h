#pragma once

#include "AssetManagerBase.h"
#include "AssetMetadata.h"

#include <map>
#include <set>
#include <mutex>

#include "NoxCore/Utils/NOXWatcher.h"

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

        const AssetMetadata GetMetadata(AssetHandle handle) const;
        const std::filesystem::path GetFilePath(AssetHandle handle) const;

        const AssetRegistry& GetAssetRegistry() const { return m_AssetRegistry; }

        void Shutdown();
        
        void SerializeAssetRegistry();
        bool DeserializeAssetRegistry();
        void ScanAndRegisterNewAssets();
        
    private:
        void OnAssetModifiedOnDisk(const std::filesystem::path& absolutePath);
    private:
        Utils::NOXWatcher m_AssetWatcher;
        
        std::set<AssetHandle> m_PendingReimports;
        std::mutex m_ReimportMutex;
        
        AssetRegistry m_AssetRegistry;
        AssetMap m_LoadedAssets;

        // todo: memory-only assets
    };
}
