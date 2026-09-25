#pragma once

#include "AssetLoader.h"
#include "TextureStreamer.h"
#include "AssetManagerBase.h"
#include "AssetMetadata.h"

#include <chrono>
#include <map>
#include <set>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "NoxCore/Tasks/JobSystem.h"
#include "MeshImporter.h"
#include "NoxCore/Utils/NOXWatcher.h"
#include "NoxCore/Utils/Utils.h"

namespace Nox
{
    using AssetRegistry = std::map<AssetHandle, AssetMetadata>;
    
    class EditorAssetManager : public AssetManagerBase
    {
    public:
        virtual Ref<Asset> GetAsset(AssetHandle handle) override;
        virtual AssetState RequestAsset(AssetHandle handle) override;
        virtual Asset* FindLoadedAsset(AssetHandle handle) const override;
        
        virtual bool IsAssetHandleValid(AssetHandle handle) const override;
        virtual bool IsAssetLoaded(AssetHandle handle) const override;
        virtual AssetType GetAssetType(AssetHandle handle) const override;

        static AssetType GetAssetTypeFromExtension(const std::filesystem::path& extension);
        void Init();
        // Once per frame, main thread: reimports edited sources and publishes the background loads that finished.
        void Update();
        void ReimportAsset(AssetHandle handle, bool force = false);
        
        void ImportAsset(const std::filesystem::path& sourcePath, const std::filesystem::path& destPath, AssetType targetType = AssetType::None, const MeshImportSettings& meshSettings = {});
        // Imports a glTF like Unreal: the file's content decides the assets -- skinned meshes become a skeletal mesh
        // (destPath with .nmesh, plus skeleton and clips), the rest a static mesh (.nsmesh); a file with both gets both.
        // False when the settings leave nothing to import. Cooking and loading happen in the background.
        bool ImportModel(const std::filesystem::path& sourcePath, const std::filesystem::path& destPath, const MeshImportSettings& settings);
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
        // True once when the background loads a sweep ran during have all finished: what they loaded may be unused
        // already (e.g. Bistro deleted while it streamed in), so the caller sweeps again.
        bool ConsumeLoadsSettledAfterSweep();

        // For the editor's status bar.
        size_t GetLoadingCount() const { return m_Loader.GetLoadCount(); }
        // Model imports cooking in the background (Do Not Combine): how many, and the meshes cooked of those known so far.
        struct ImportProgress
        {
            size_t Imports = 0;
            uint32_t Done = 0;
            uint32_t Total = 0;
        };
        ImportProgress GetImportProgress() const;
        size_t GetStreamingCount() const { return m_Streamer.GetStreamingCount(); }
        TextureStreamer& GetTextureStreamer() { return m_Streamer; }
        uint64_t GetPendingUploadBytes() const { return m_Loader.GetPendingUploadBytes() + m_Streamer.GetPendingUploadBytes(); }
        
        void SerializeAssetRegistry();
        bool DeserializeAssetRegistry();
        // Registers cooked files (.nanim/.nskel/.nmat/...) that aren't in the registry yet. Scans only
        // relativeDirectory (relative to the asset directory) when given, the whole asset tree otherwise.
        void ScanAndRegisterNewAssets(const std::filesystem::path& relativeDirectory = {});
        
    private:
        void OnAssetModifiedOnDisk(const std::filesystem::path& absolutePath);
        void ImportMeshTextures(const Ref<Asset>& meshAsset);
        void ImportMeshMaterials(const Ref<Asset>& meshAsset, const AssetMetadata& meshMetadata);
        void PublishLoadedAssets();
        // Registers the per-mesh static assets of finished ImportModel split cooks (Do Not Combine).
        void PublishSplitImports();
        // Adds a registry entry without loading anything (the caller writes the registry file).
        AssetHandle RegisterAsset(const AssetMetadata& metadata);
        static AssetMetadata TextureMetadata(const std::filesystem::path& sourcePath, const TextureSpecification& spec, const std::filesystem::path& destPath);
        // The skeleton and clips cooked from the same glTF, for model instances to animate with.
        void LinkImportedAssets(const Ref<Asset>& meshAsset, const AssetMetadata& meshMetadata);
    private:
        Utils::NOXWatcher m_AssetWatcher;
        
        // Filled by the watcher thread, drained by Update() on the main thread.
        std::vector<std::filesystem::path> m_PendingModifiedPaths;
        std::mutex m_ReimportMutex;

        // Do Not Combine imports cooking every static mesh of a file into its own asset in the background.
        struct PendingSplitImport
        {
            TaskFuture<std::vector<MeshImporter::SplitMesh>> Job;
            AssetMetadata MaterialBase; // only its path (the .nmat files are named after it) and source are used
            std::shared_ptr<std::atomic<uint32_t>> Total; // meshes to cook (0 while the glTF is still being parsed)
            std::shared_ptr<std::atomic<uint32_t>> Done;
        };
        std::vector<PendingSplitImport> m_PendingSplitImports;
        
        AssetRegistry m_AssetRegistry;
        // Normalized file / source path -> handle, for every registry entry, kept in step with the registry. A model load
        // used to walk the whole registry (thousands of entries, two path normalizations each) to find its textures and
        // materials; with hundreds of per-mesh assets that was most of the time streaming took.
        std::unordered_map<std::string, AssetHandle> m_HandleByPath;
        // Registrations made while models load (textures, materials) are written to disk once a second, not once per load:
        // hundreds of per-mesh loads each rewriting the whole registry file was most of a big import's time.
        bool m_RegistryDirty = false;
        std::chrono::steady_clock::time_point m_RegistryWritten = std::chrono::steady_clock::now();
        void IndexPath(AssetHandle handle, const AssetMetadata& metadata);
        AssetHandle FindHandleByPath(const std::filesystem::path& path, AssetType type) const;
        AssetMap m_LoadedAssets;

        // Background loads (RequestAsset) until they are published into m_LoadedAssets.
        AssetLoader m_Loader;
        // Streamed textures moving between mip residencies (§5.12).
        TextureStreamer m_Streamer;
        // Loads that failed are not retried on every request (entities keep requesting what they miss); a reimport
        // clears the entry.
        std::unordered_set<AssetHandle> m_FailedAssets;
        bool m_SweepDuringLoads = false;

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
