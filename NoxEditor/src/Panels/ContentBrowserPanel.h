#pragma once
#include <functional>
#include <unordered_set>

#include "ThumbnailCache.h"

namespace Nox
{
    class ContentBrowserPanel
    {
    public:
        ContentBrowserPanel(Ref<Project> project);
        
        void OnImGuiRender();
        void OnExternalFileDrop(const std::filesystem::path& path);
        // Called with an asset's handle when it's double-clicked (or just created); EditorLayer decides what
        // opening means per AssetType, so this panel doesn't need to know about any editor window.
        void SetOpenAssetCallback(std::function<void(AssetHandle)> callback) { m_OpenAsset = std::move(callback); }
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
            std::string Id;   // ImGui id of its button, built once per refresh
            std::string Name; // file name shown under it
        };
        std::vector<BrowserEntry> m_CurrentEntries;
        // Selected assets of the folder (click, Ctrl+click, Shift+click, Ctrl+A): dragging one of them drags all.
        std::unordered_set<AssetHandle> m_Selected;
        size_t m_LastClickedEntry = 0;
        std::filesystem::path m_EntriesDirectory;

        char m_ImportDestPathBuffer[256] = {0};
        char m_ImportFileNameBuffer[128] = {0};
        std::filesystem::path m_PendingImportPath;
        std::filesystem::path m_PendingExternalSourcePath;
        std::filesystem::path m_PendingPackageDirectory;
        bool m_ShowImportModal = false;
        MeshImportSettings m_ImportSettings; // the import dialog's Static / Skeletal / Animations options
        bool m_WindowHovered = false;
        bool m_ShowCreateScriptModal = false;
        char m_NewScriptName[128] = "NewBehaviour";
        bool m_ShowCreateGraphModal = false;
        char m_NewGraphName[128] = "NewAnimationGraph";
        std::function<void(AssetHandle)> m_OpenAsset;
    };
};
