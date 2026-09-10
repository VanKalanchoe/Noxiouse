#include "ThumbnailCache.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Asset/TextureImporter.h"

namespace Nox
{
    ThumbnailCache::ThumbnailCache(Ref<Project> project)
        : m_Project(project)
    {
        // TODO(Yan): revisit this path (move to Cache dir)
        m_ThumbnailCachePath = m_Project->GetAssetDirectory() / "Thumbnail.cache";
    }

    Ref<Texture2D> ThumbnailCache::GetOrCreateThumbnail(AssetHandle handle, const AssetMetadata& metadata)
    {
        // Thumbnails are GPU assets kept alive for the lifetime of this cache.
        // Do not hit the filesystem again for every ImGui frame.
        auto existing = m_CachedImages.find(handle);
        if (existing != m_CachedImages.end())
            return existing->second.Image;

        const auto absolutePath = m_Project->GetAssetAbsolutePath(metadata.FilePath);
        std::error_code error;
        const std::filesystem::file_time_type lastWriteTime =
            std::filesystem::last_write_time(absolutePath, error);
        if (error)
            return {};

        uint64_t timestamp = std::chrono::duration_cast<std::chrono::seconds>(lastWriteTime.time_since_epoch()).count();

        Ref<Texture2D> texture;
        if (metadata.Type == AssetType::Texture2D)
        {
            // AssetManager resolves source textures to their cooked .ntex asset.
            texture = AssetManager::GetAsset<Texture2D>(handle);
        }
        else if (metadata.Type == AssetType::Material)
        {
            // Until the offscreen material preview pass exists, use the material's
            // base-color asset as its thumbnail. The panel remains asset-driven.
            Ref<Material> material = AssetManager::GetAsset<Material>(handle);
            if (material && !material->GetData().BaseColorTexturePath.empty())
            {
                std::filesystem::path texturePath = material->GetData().BaseColorTexturePath;
                if (texturePath.is_absolute())
                    texturePath = std::filesystem::relative(texturePath, m_Project->GetAssetDirectory(), error);

                for (const auto& [textureHandle, textureMetadata] :
                     m_Project->GetEditorAssetManager()->GetAssetRegistry())
                {
                    if (textureMetadata.Type != AssetType::Texture2D)
                        continue;
                    if (textureMetadata.SourceFilePath == texturePath || textureMetadata.FilePath == texturePath)
                    {
                        texture = AssetManager::GetAsset<Texture2D>(textureHandle);
                        break;
                    }
                }
            }
        }

        if (!texture)
            return {};

        auto& cachedImage = m_CachedImages[handle];
        cachedImage.Timestamp = timestamp;
        cachedImage.Image = texture;
        return cachedImage.Image;
    }
}
