#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

#include "Asset.h"
#include "AssetMetadata.h"
#include "NoxCore/Utils/Utils.h"

namespace Nox
{
    // A background load that finished, for the asset manager to publish.
    struct LoadedAsset
    {
        AssetHandle Handle;
        Ref<Asset> Loaded;                       // null when the load failed
        bool NeedsImport = false;                // no current cooked data: import synchronously instead
        bool Cooked = false;                     // cooked by this load (new files were written next to it)
        std::optional<XXH128_hash_t> SourceHash; // of the source file, for telling real edits from watcher noise
        double Milliseconds = 0.0;               // from the request to Ready
    };

    // Background loads (§5.11): file reads on the IO workers, cooking (first import) and decoding on the CPU workers, GPU
    // uploads through the renderer's staging under a per-frame byte budget, and each resource published once its copies
    // have completed on the GPU. Owned and driven by the asset manager, main thread.
    class AssetLoader
    {
    public:
        AssetLoader();
        ~AssetLoader();

        AssetLoader(const AssetLoader&) = delete;
        AssetLoader& operator=(const AssetLoader&) = delete;

        // Textures and meshes (cooked first when needed), materials, skeletons and animations; everything else imports
        // synchronously.
        static bool IsStreamable(AssetType type);
        void Begin(AssetHandle handle, const AssetMetadata& metadata);
        bool IsLoading(AssetHandle handle) const { return m_Loading.contains(handle); }
        bool IsIdle() const { return m_Loads.empty(); }
        size_t GetLoadCount() const { return m_Loads.size(); }
        // Bytes the loads have yet to get onto the GPU (known once a load has read its header).
        uint64_t GetPendingUploadBytes() const;
        // Once per frame: advances every load as far as it can go, in request order, and hands back the finished ones.
        void Update(std::vector<LoadedAsset>& outFinished);
        // While the renderer still exists: waits for work in flight and drops every unfinished load with what it holds.
        void Shutdown();

        class Load;

    private:
        std::vector<std::unique_ptr<Load>> m_Loads; // in request order
        std::unordered_set<AssetHandle> m_Loading;
    };
}
