#pragma once

#include "Asset.h"

#include <map>

namespace Nox
{
    using AssetMap = std::map<AssetHandle, Ref<Asset>>;
    
    class AssetManagerBase
    {
    public:
        virtual Ref<Asset> GetAsset(AssetHandle handle) = 0;
        // Never imports: the asset if it is loaded, null otherwise. For frame graph tasks: no reference-count writes,
        // and the pointer stays valid while they run because only the main thread loads/unloads assets and it waits
        // for them. GetAsset stays main-thread only.
        virtual Asset* FindLoadedAsset(AssetHandle handle) const = 0;
        
        virtual bool IsAssetHandleValid(AssetHandle handle) const = 0;
        virtual bool IsAssetLoaded(AssetHandle handle) const = 0;
        virtual AssetType GetAssetType(AssetHandle handle) const = 0;
    };
}