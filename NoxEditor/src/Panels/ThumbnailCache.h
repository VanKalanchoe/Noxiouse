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

        // Null until the thumbnail's texture has loaded (requested in the background; the panel shows the file icon).
        Ref<Texture2D> GetOrCreateThumbnail(AssetHandle handle, const AssetMetadata& metadata);
    private:
        // The texture a material's thumbnail shows (its base color), 0 for none or until the material has loaded.
        AssetHandle GetMaterialTexture(AssetHandle material);
    private:
        Ref<Project> m_Project;

        std::map<AssetHandle, ThumbnailImage> m_CachedImages;
        std::map<AssetHandle, AssetHandle> m_MaterialTextures;
		
        // TEMP (replace with Nox::Serialization)
        std::filesystem::path m_ThumbnailCachePath;
    };   
}
