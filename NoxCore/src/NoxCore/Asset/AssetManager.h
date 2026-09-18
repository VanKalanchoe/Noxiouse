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
            Ref<Asset> asset = Project::GetActiveAssetManager().GetAsset(handle);

            // Cast it to the derived type using Ref's constructor
            return Ref<T>(asset); // uses Ref<T>::Ref(const Ref<U>&)
        }

        static AssetState RequestAsset(AssetHandle handle)
        {
            return Project::GetActiveAssetManager().RequestAsset(handle);
        }

        template<typename T>
        static T* FindLoadedAsset(AssetHandle handle)
        {
            return static_cast<T*>(Project::GetActiveAssetManager().FindLoadedAsset(handle));
        }

        static bool IsAssetHandleValid(AssetHandle handle)
        {
            return Project::GetActiveAssetManager().IsAssetHandleValid(handle);
        }
        
        static bool IsAssetLoaded(AssetHandle handle)
        {
            return Project::GetActiveAssetManager().IsAssetLoaded(handle);
        }
        
        static AssetType GetAssetType(AssetHandle handle)
        {
            return Project::GetActiveAssetManager().GetAssetType(handle);
        }
    };
}
