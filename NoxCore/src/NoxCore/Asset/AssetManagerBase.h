#pragma once

#include "Asset.h"
#include "AssetMetadata.h"

#include <filesystem>
#include <map>

namespace Nox
{
    using AssetMap = std::map<AssetHandle, Ref<Asset>>;
    
    class AssetManagerBase
    {
    public:
        // Loads the asset on the calling (main) thread if it is not loaded yet. For tools and paths that need it now.
        virtual Ref<Asset> GetAsset(AssetHandle handle) = 0;
        // Main thread. Starts loading the asset in the background unless it is loaded or loading already, and returns
        // where it is. Frame code requests and uses the asset once it is Ready.
        virtual AssetState RequestAsset(AssetHandle handle) = 0;
        // Never imports: the asset if it is loaded, null otherwise. For frame graph tasks: no reference-count writes,
        // and the pointer stays valid while they run because only the main thread loads/unloads assets and it waits
        // for them. GetAsset stays main-thread only.
        virtual Asset* FindLoadedAsset(AssetHandle handle) const = 0;
        
        virtual bool IsAssetHandleValid(AssetHandle handle) const = 0;
        virtual bool IsAssetLoaded(AssetHandle handle) const = 0;
        virtual AssetType GetAssetType(AssetHandle handle) const = 0;

        // Read-only registry lookups (docs/Runtime_And_Packaging_Plan_2026-10.md, R2). Engine code asks the active asset
        // manager through these and never casts it to the editor's, so the same code runs on a runtime asset manager.
        // The registered metadata of an asset (default-constructed for an unknown handle).
        virtual AssetMetadata GetMetadata(AssetHandle handle) const = 0;
        // The registered asset of this type at a path relative to the asset directory (its file or its source), 0 if none.
        virtual AssetHandle FindHandleByPath(const std::filesystem::path& path, AssetType type) const = 0;
        // How many assets are registered (grows with every registration: callers use it to notice new assets).
        virtual size_t GetAssetCount() const = 0;
    };
}