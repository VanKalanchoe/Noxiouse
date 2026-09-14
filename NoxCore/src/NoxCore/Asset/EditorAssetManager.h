#pragma once

#include "AssetManagerBase.h"
#include "AssetMetadata.h"

#include <map>
#include <set>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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

        // Mark-and-sweep: everything reachable from referencedAssets (the handles live scenes use)
        // stays loaded, including dependencies (mesh -> materials -> textures). Every other loaded
        // mesh/material/texture/animation that nothing else holds a Ref to is unloaded, its GPU
        // resources released once in-flight frames are done. Returns the number unloaded.
        size_t UnloadUnusedAssets(const std::unordered_set<AssetHandle>& referencedAssets);
        
        void SerializeAssetRegistry();
        bool DeserializeAssetRegistry();
        // Registers cooked files (.nanim/.nskel/.nmat/...) that aren't in the registry yet. Scans only
        // relativeDirectory (relative to the asset directory) when given, the whole asset tree otherwise.
        void ScanAndRegisterNewAssets(const std::filesystem::path& relativeDirectory = {});
        
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

        // Material texture path -> texture handle, for UnloadUnusedAssets. Resolving a path needs
        // std::filesystem::relative (disk access per call); Bistro alone has ~1500 such paths, so
        // re-resolving them on every sweep froze the editor. Handles are stable, so hits are cached.
        std::unordered_map<std::string, AssetHandle> m_TexturePathCache;
        // Paths with no texture asset yet; only valid while the registry hasn't changed size.
        std::unordered_set<std::string> m_TexturePathMisses;
        size_t m_TexturePathMissesRegistrySize = 0;

        // todo: memory-only assets
    };
}
