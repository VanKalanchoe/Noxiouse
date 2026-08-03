#pragma once

#include "AssetManagerBase.h"

#include "NoxCore/Project/Project.h"

namespace Nox
{
    class AssetManager
    {
    public:
        template<typename T>
        static Ref<T> GetAsset(AssetHandle handle)
        {
            // Get the base asset
            Ref<Asset> asset = Project::GetActive()->GetAssetManager()->GetAsset(handle);

            // Cast it to the derived type using Ref's constructor
            return Ref<T>(asset); // uses Ref<T>::Ref(const Ref<U>&)
        }

        static bool IsAssetHandleValid(AssetHandle handle)
        {
            return Project::GetActive()->GetAssetManager()->IsAssetHandleValid(handle);
        }
        
        static bool IsAssetLoaded(AssetHandle handle)
        {
            return Project::GetActive()->GetAssetManager()->IsAssetLoaded(handle);
        }
        
        static AssetType GetAssetType(AssetHandle handle)
        {
            return Project::GetActive()->GetAssetManager()->GetAssetType(handle);
        }
    };
}
