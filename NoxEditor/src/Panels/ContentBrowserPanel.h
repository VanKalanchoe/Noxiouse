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
        // The folder being shown (relative to the asset directory; empty for the root), and "this asset was just created": the browser
        // shows it, selects it and puts its name into rename mode (next frame).
        std::filesystem::path CurrentFolder() const
        {
            const std::filesystem::path relative = m_CurrentDirectory.lexically_relative(m_BaseDirectory);
            return relative == "." ? std::filesystem::path() : relative;
        }
        void ShowNewAsset(AssetHandle handle) { m_PendingShowAsset = handle; }
        // Called when entities are dragged from the hierarchy onto the browser: creates the prefab in the given folder (relative
        // to the asset directory) and returns its handle, 0 on failure. The new asset then goes into rename mode.
        void SetCreatePrefabCallback(std::function<AssetHandle(UUID, const std::filesystem::path&)> callback) { m_CreatePrefab = std::move(callback); }
        // Called for the right-click "Create Variant" of a prefab: creates the variant in the folder (relative to the asset
        // directory), returns its handle (0 on failure); it then goes into rename mode.
        void SetCreateVariantCallback(std::function<AssetHandle(AssetHandle, const std::filesystem::path&)> callback) { m_CreateVariant = std::move(callback); }
        // The assets the open scenes use right now (unsaved changes included), for the reference checks before deleting.
        void SetSceneReferencesCallback(std::function<void(std::unordered_set<AssetHandle>&)> callback) { m_SceneReferences = std::move(callback); }
    private:
        void RefreshAssetTree();
        AssetHandle FindAssetHandle(const std::filesystem::path& relativePath) const;
        Ref<Texture2D> GetThumbnail(AssetHandle handle, const AssetMetadata& metadata, bool& rendered);
        void SetImportDestination(const std::filesystem::path& path);
        // Windows-style rename: the asset's name becomes a text field with the whole name selected.
        void BeginRename(AssetHandle handle);
        void RequestPrefabFromDrop(const ImGuiPayload* payload, const std::filesystem::path& folder);
        // Assets dragged from the browser onto a folder (tile, tree, path): moved there next frame.
        void AcceptAssetMoveDrop(const std::filesystem::path& folder);
        // The right-click menu of an asset tile; delete / find references / unused assets dialogs and what they act on.
        void DrawAssetContextMenu(AssetHandle handle, const AssetMetadata& metadata);
        void RequestDelete(std::vector<AssetHandle> handles);
        void DrawAssetDialogs();
        void ProcessPendingAssetActions();

        // The modern layout (docs/Future.md item 2): a toolbar with the add/import buttons, a clickable path, search, a type filter
        // and the tile size; the folder tree on the left; the tiles of the folder (or the search results) on the right.
        void DrawToolbar();
        void DrawBreadcrumb();
        void RebuildFolderTree();
        struct FolderNode;
        void DrawFolderTree(const FolderNode& node);
        void DrawTiles();
        // m_VisibleEntries for the current folder, search text and type filter (rebuilt when one of them or the folder changes).
        void UpdateVisibleEntries();
        bool PassesTypeFilter(AssetType type) const;
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
        // What the tiles show: the folder's entries, or with a search every matching asset below the folder; filtered by type.
        std::vector<BrowserEntry> m_VisibleEntries;
        std::string m_VisibleKey; // the inputs m_VisibleEntries was built from
        uint64_t m_EntriesVersion = 0; // bumped by every RefreshAssetTree

        struct FolderNode
        {
            std::filesystem::path Path; // absolute
            std::string Name;
            std::vector<FolderNode> Children;
        };
        FolderNode m_FolderTree;
        bool m_FolderTreeDirty = true;
        double m_FolderTreeTime = 0.0; // ImGui time of the last rebuild
        std::filesystem::path m_TreeRevealedDirectory; // the folder the tree was last unfolded to

        char m_SearchBuffer[128] = {0};
        int m_TypeFilter = 0; // index into the toolbar's type list, 0 = all
        float m_ThumbnailSize = 96.0f;
        float m_Padding = 16.0f;
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
        // What the dialog shows about the file (read when it opens): its size in the file's own units, and the unit the user picked.
        void ReadImportFileInfo(const std::filesystem::path& absoluteSource);
        bool m_ImportHasBounds = false;
        glm::vec3 m_ImportBoundsMin{ 0.0f };
        glm::vec3 m_ImportBoundsMax{ 0.0f };
        int m_ImportSourceUnit = 0;         // WorldUnits::SourceUnit, or 5 = custom scale
        float m_ImportFitHeightCm = 0.0f; // "Fit to height"; 0 = not set yet, and it is remembered across imports once you do
        bool m_WindowHovered = false;
        bool m_ShowCreateScriptModal = false;
        char m_NewScriptName[128] = "NewBehaviour";
        bool m_ShowCreateGraphModal = false;
        char m_NewGraphName[128] = "NewAnimationGraph";
        std::function<void(AssetHandle)> m_OpenAsset;
        std::function<AssetHandle(UUID, const std::filesystem::path&)> m_CreatePrefab;
        std::function<AssetHandle(AssetHandle, const std::filesystem::path&)> m_CreateVariant;
        AssetHandle m_PendingShowAsset = 0;
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

        std::function<void(std::unordered_set<AssetHandle>&)> m_SceneReferences;
        // Asset management (Future.md item 2, B3): moves dropped on folders, deletes with their reference check, find references,
        // unused assets. The lists are filled when the dialog opens (they read the project's files).
        std::vector<AssetHandle> m_PendingMoveAssets;
        std::filesystem::path m_PendingMoveFolder;
        struct DeleteCandidate
        {
            AssetHandle Handle = 0;
            std::string Name;
            std::vector<std::string> UsedBy; // files, and "the open scene" for in-memory use
        };
        std::vector<DeleteCandidate> m_DeleteCandidates;
        bool m_OpenDeleteDialog = false;
        AssetHandle m_ReferencesAsset = 0;
        std::vector<std::string> m_References;
        bool m_OpenReferencesDialog = false;
        struct UnusedEntry
        {
            AssetHandle Handle = 0;
            std::string Path;
            bool Delete = true;
        };
        std::vector<UnusedEntry> m_UnusedAssets;
        bool m_OpenUnusedDialog = false;
    };
};
