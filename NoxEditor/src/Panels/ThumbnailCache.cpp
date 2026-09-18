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

        // Everything is requested, never loaded here: a folder opens at once with file icons and each thumbnail
        // appears once its texture has loaded in the background (§5.11).
        AssetHandle textureHandle = 0;
        if (metadata.Type == AssetType::Texture2D)
        {
            // AssetManager resolves source textures to their cooked .ntex asset.
            textureHandle = handle;
        }
        else if (metadata.Type == AssetType::Material)
        {
            // Until the offscreen material preview pass exists, use the material's base-color asset as its thumbnail.
            textureHandle = GetMaterialTexture(handle);
        }
        if (textureHandle == 0 || AssetManager::RequestAsset(textureHandle) != AssetState::Ready)
            return {};

        Texture2D* loaded = AssetManager::FindLoadedAsset<Texture2D>(textureHandle);
        if (!loaded)
            return {};

        std::error_code error;
        const std::filesystem::file_time_type lastWriteTime =
            std::filesystem::last_write_time(m_Project->GetAssetAbsolutePath(metadata.FilePath), error);

        auto& cachedImage = m_CachedImages[handle];
        cachedImage.Timestamp = error ? 0 : std::chrono::duration_cast<std::chrono::seconds>(lastWriteTime.time_since_epoch()).count();
        cachedImage.Image = Ref<Texture2D>(loaded);
        return cachedImage.Image;
    }

    AssetHandle ThumbnailCache::GetMaterialTexture(AssetHandle material)
    {
        if (auto found = m_MaterialTextures.find(material); found != m_MaterialTextures.end())
            return found->second;
        if (AssetManager::RequestAsset(material) != AssetState::Ready)
            return 0;

        // Resolved once per material (a registry scan).
        AssetHandle textureHandle = 0;
        const Material* loaded = AssetManager::FindLoadedAsset<Material>(material);
        if (loaded && !loaded->GetData().BaseColorTexturePath.empty())
        {
            std::error_code error;
            std::filesystem::path texturePath = loaded->GetData().BaseColorTexturePath;
            if (texturePath.is_absolute())
                texturePath = std::filesystem::relative(texturePath, m_Project->GetAssetDirectory(), error);

            for (const auto& [candidate, textureMetadata] : m_Project->GetEditorAssetManager()->GetAssetRegistry())
            {
                if (textureMetadata.Type == AssetType::Texture2D &&
                    (textureMetadata.SourceFilePath == texturePath || textureMetadata.FilePath == texturePath))
                {
                    textureHandle = candidate;
                    break;
                }
            }
        }
        m_MaterialTextures.emplace(material, textureHandle);
        return textureHandle;
    }
}
