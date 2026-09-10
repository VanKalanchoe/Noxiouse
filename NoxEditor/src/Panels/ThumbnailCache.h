#pragma once
#include "NoxCore/Project/Project.h"
#include "NRI/Texture.h"
#include "NoxCore/Asset/AssetMetadata.h"

namespace Nox
{
    struct ThumbnailImage
    {
        uint64_t Timestamp;
        Ref<Texture2D> Image;
    };
    
    class ThumbnailCache : public RefCounted
    {
    public:
        ThumbnailCache(Ref<Project> project);

        Ref<Texture2D> GetOrCreateThumbnail(AssetHandle handle, const AssetMetadata& metadata);
    private:
        Ref<Project> m_Project;

        std::map<AssetHandle, ThumbnailImage> m_CachedImages;
		
        // TEMP (replace with Nox::Serialization)
        std::filesystem::path m_ThumbnailCachePath;
    };   
}
