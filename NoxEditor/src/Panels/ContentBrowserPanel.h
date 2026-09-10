#pragma once
#include "ThumbnailCache.h"

namespace Nox
{
    class ContentBrowserPanel
    {
    public:
        ContentBrowserPanel(Ref<Project> project);
        
        void OnImGuiRender();
        void OnExternalFileDrop(const std::filesystem::path& path);
    private:
        void RefreshAssetTree();
        AssetHandle FindAssetHandle(const std::filesystem::path& relativePath) const;
        Ref<Texture2D> GetThumbnail(AssetHandle handle, const AssetMetadata& metadata);
        void SetImportDestination(const std::filesystem::path& path);
    private:
        Ref<Project> m_Project;
        Ref<ThumbnailCache> m_ThumbnailCache;
        
        std::filesystem::path m_BaseDirectory;
        std::filesystem::path m_CurrentDirectory;

        Ref<Texture2D> m_DirectoryIcon;
        Ref<Texture2D> m_FileIcon;

        struct BrowserEntry
        {
            std::filesystem::path Path;
            AssetMetadata Metadata;
            AssetHandle Handle = 0;
            bool IsDirectory = false;
        };
        std::vector<BrowserEntry> m_CurrentEntries;
        std::filesystem::path m_EntriesDirectory;

        char m_ImportDestPathBuffer[256] = {0};
        char m_ImportFileNameBuffer[128] = {0};
        std::filesystem::path m_PendingImportPath;
        std::filesystem::path m_PendingExternalSourcePath;
        std::filesystem::path m_PendingPackageDirectory;
        bool m_ShowImportModal = false;
        bool m_ImportAsStaticMesh = false;
        bool m_WindowHovered = false;
    };
};
