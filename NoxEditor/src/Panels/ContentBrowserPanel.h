#pragma once
#include <functional>
#include <unordered_set>

#include "NoxCore/Core/UUID.h"
#include "ThumbnailCache.h"

struct ImGuiPayload;

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
        // Called when entities are dragged from the hierarchy onto the browser: creates the prefab in the given folder (relative
        // to the asset directory) and returns its handle, 0 on failure. The new asset then goes into rename mode.
        void SetCreatePrefabCallback(std::function<AssetHandle(UUID, const std::filesystem::path&)> callback) { m_CreatePrefab = std::move(callback); }
        // Called for the right-click "Create Variant" of a prefab: creates the variant in the folder (relative to the asset
        // directory), returns its handle (0 on failure); it then goes into rename mode.
        void SetCreateVariantCallback(std::function<AssetHandle(AssetHandle, const std::filesystem::path&)> callback) { m_CreateVariant = std::move(callback); }
    private:
        void RefreshAssetTree();
        AssetHandle FindAssetHandle(const std::filesystem::path& relativePath) const;
        Ref<Texture2D> GetThumbnail(AssetHandle handle, const AssetMetadata& metadata);
        void SetImportDestination(const std::filesystem::path& path);
        // Windows-style rename: the asset's name becomes a text field with the whole name selected.
        void BeginRename(AssetHandle handle);
        void RequestPrefabFromDrop(const ImGuiPayload* payload, const std::filesystem::path& folder);
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
        std::function<AssetHandle(UUID, const std::filesystem::path&)> m_CreatePrefab;
        std::function<AssetHandle(AssetHandle, const std::filesystem::path&)> m_CreateVariant;
        bool m_HasVariantRequest = false;
        AssetHandle m_VariantBase = 0;
        // Handled at the start of the next frame (the entry list must not change while it is being drawn).
        bool m_HasPrefabDrop = false;
        UUID m_PrefabDropEntity = 0;
        std::filesystem::path m_PrefabDropFolder;
        AssetHandle m_RenameHandle = 0;
        char m_RenameBuffer[128] = {0};
        bool m_RenameFocus = false;
        bool m_RenameCommit = false;
        std::string m_RenameCommitText;
    };
};
