#include "ThumbnailCache.h"
#include "NoxCore/Asset/TextureImporter.h"

namespace Nox
{
    ThumbnailCache::ThumbnailCache(Ref<Project> project)
        : m_Project(project)
    {
        // TODO(Yan): revisit this path (move to Cache dir)
        m_ThumbnailCachePath = m_Project->GetAssetDirectory() / "Thumbnail.cache";
    }

    Ref<Texture2D> ThumbnailCache::GetOrCreateThumbnail(const std::filesystem::path& assetPath)
    {
        // 1. Read file timestamp
        // 2. Compare hashed timestamp with existing cached image (in memory first, then from cache file)
        // 3. If equal, return associated thumbnail, otherwise load asset from disk and generate thumbnail
        // 4. If generated new thumbnail, store in cache obviously

        auto absolutePath = m_Project->GetAssetAbsolutePath(assetPath);
        std::filesystem::file_time_type lastWriteTime = std::filesystem::last_write_time(absolutePath);
        uint64_t timestamp = std::chrono::duration_cast<std::chrono::seconds>(lastWriteTime.time_since_epoch()).count();

        if (m_CachedImages.find(assetPath) != m_CachedImages.end())
        {
            auto& cachedImage = m_CachedImages.at(assetPath);
            if (cachedImage.Timestamp == timestamp)
                return cachedImage.Image;
        }

        // TODO(Yan): PNGs, JPG/JPEG for now
        if (assetPath.extension() != ".png" && assetPath.extension() != ".jpg" && assetPath.extension() != ".jpeg")
            return {};

        Ref<Texture2D> texture = TextureImporter::LoadTexture2D(absolutePath, {.generateMips = false});
        if (!texture)
            return {};

        auto& cachedImage = m_CachedImages[assetPath];
        cachedImage.Timestamp = timestamp;
        cachedImage.Image = texture;
        return cachedImage.Image;
    }
}
