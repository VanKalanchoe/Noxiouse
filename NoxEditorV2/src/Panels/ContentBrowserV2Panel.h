#pragma once

#include <filesystem>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Asset/AssetMetadata.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/Core/UUID.h"
#include <NoxUI/Core/UITypes.h>
#include <NoxUI/Widgets/UIMenu.h>

namespace NoxUI
{
        class UIBreadcrumb;
        class UIButton;
        class UIDrawList;
        class UIDropdown;
        class UIElement;
        class UILabel;
        class UISearchBar;
        class UIStackBox;
        class UITreeView;
    }

namespace Nox
{

    class ContentTileView;
    class ContentPathBar;

    // How an asset type looks in the editor: its name ("Static Mesh"), its colour (the line under a tile's picture, as in
    // Unreal) and its icon (drawn with vectors: crisp at any tile size, no image files).
    struct AssetTypeVisual
    {
        const char* Label = "File";
        NoxUI::UIColor Color{ 0.62f, 0.66f, 0.74f, 1.0f };
    };
    AssetTypeVisual GetAssetTypeVisual(AssetType type);
    // The type's icon centred at `center`, `size` pixels across.
    void DrawAssetTypeIcon(NoxUI::UIDrawList& drawList, AssetType type, const NoxUI::UIVec2& center, float size, const NoxUI::UIColor& color);
    void DrawFolderIcon(NoxUI::UIDrawList& drawList, const NoxUI::UIVec2& center, float size, const NoxUI::UIColor& color);

    // One file or folder the browser shows.
    struct ContentEntry
    {
        std::filesystem::path Path;   // absolute
        std::string Name;             // the file's name without its extension (a folder's name)
        std::string Extension;        // ".nmat"
        AssetHandle Handle = 0;       // 0: a folder, or a file the registry does not know (a C# script)
        AssetType Type = AssetType::None;
        bool IsDirectory = false;
        bool IsScript = false;        // a .cs file: opened in the code editor
    };

    // The Content Browser (Unreal's): the project's Assets folder as a folder tree on the left and the open folder's files as
    // tiles on the right -- a picture of the asset (a rendered mesh or material, the texture itself, else the type's icon), a
    // line in the type's colour, the name, the type at the bottom right. Search finds assets in the open folder and below.
    //
    //  - Click selects (Ctrl / Shift for more, Ctrl+A all); double-click opens (a folder, a scene, a material ...); drag tiles
    //    into the viewport to place them, onto a folder tile to move them; drag entities from the hierarchy onto the tiles to
    //    make a prefab. Right-click: Open, Rename (F2), Duplicate, Delete (Del), Find References, Reimport, Show in Explorer,
    //    Copy Path; on empty space: New Folder and the asset types + Add makes.
    //  - Ctrl + mouse wheel resizes the tiles; Back / Forward walk the folders visited; Backspace goes up.
    class ContentBrowserV2Panel
    {
    public:
        ContentBrowserV2Panel();
        ~ContentBrowserV2Panel();

        Ref<NoxUI::UIElement> BuildUI();
        // Once a frame: a folder or files changed outside the editor show up, a model import finishes, thumbnails come in.
        void Update(float dt);
        // Everything read again (the project changed, files were added by a tool).
        void Refresh();
        // The textures its tiles show right now (the editor's unused-asset sweep keeps them loaded).
        void CollectAssetReferences(std::unordered_set<AssetHandle>& handles) const;

        // A file dropped on the editor window from the system or picked with Import: copied into the open folder and imported
        // (a model opens the import dialog first).
        void ImportFile(const std::filesystem::path& path);

        // The open folder, relative to the asset directory ("" for the root).
        std::filesystem::path CurrentFolder() const;
        // An asset just made elsewhere (a prefab from the hierarchy): shown, selected, its name ready to be typed.
        void ShowNewAsset(AssetHandle handle);

        void SetOpenAssetCallback(std::function<void(AssetHandle)> callback) { m_OpenAsset = std::move(callback); }
        // Entities dragged from the hierarchy onto the browser: the prefab made in `folder` (relative), its handle or 0.
        void SetCreatePrefabCallback(std::function<AssetHandle(uint64_t dragId, const std::filesystem::path& folder)> callback) { m_CreatePrefab = std::move(callback); }
        // A prefab's right-click Create Variant: the variant made next to it in `folder` (relative), its handle or 0.
        void SetCreateVariantCallback(std::function<AssetHandle(AssetHandle base, const std::filesystem::path& folder)> callback) { m_CreateVariant = std::move(callback); }
        // Replace References: what uses `from` uses `to` instead (the files, the loaded assets and the open scene; the editor does it).
        void SetReplaceReferencesCallback(std::function<void(AssetHandle from, AssetHandle to)> callback) { m_ReplaceReferences = std::move(callback); }
        // The assets the open scenes use (unsaved changes included): Delete warns about them too.
        void SetSceneReferencesCallback(std::function<void(std::unordered_set<AssetHandle>&)> callback) { m_SceneReferences = std::move(callback); }
        // Saved, imported, failed: shown as a toast by the editor.
        void SetNotifyCallback(std::function<void(NoxUI::UINotification)> callback) { m_Notify = std::move(callback); }
        // An element put at the right end of the toolbar (the editor's "Dock in Layout" while the browser is in the drawer).
        void SetToolbarExtra(Ref<NoxUI::UIElement> element) { m_ToolbarExtra = std::move(element); }
        // What the browser is busy with in the background (the status bar shows it), empty when nothing.
        std::string GetBusyText() const;

    private:
        friend class ContentTileView;
        friend class ContentPathBar;

        void NavigateTo(const std::filesystem::path& absoluteFolder, bool recordHistory = true);
        void GoBack();
        void GoForward();
        void GoUp();
        void RebuildEntries();
        void RebuildFolderTree();
        void SyncChrome();         // breadcrumb, back / forward, footer
        void Notify(NoxUI::UINotificationSeverity severity, std::string title, std::string message = {});

        // Actions (the tiles' menus and keys call these).
        void Activate(const ContentEntry& entry);
        void BeginRename(AssetHandle handle);
        void CommitRename(AssetHandle handle, const std::string& newName);
        void Duplicate(AssetHandle handle);
        void RequestDelete(std::vector<AssetHandle> handles);
        void ShowReferences(AssetHandle handle);
        // Who uses assets is looked up on a worker thread (it reads the project's files); the dialog opens when it is known.
        enum class ReferenceCheckMode : uint8_t { Delete, Show, Replace };
        void StartReferenceCheck(ReferenceCheckMode mode, std::vector<AssetHandle> handles);
        void FinishReferenceCheck();
        void ShowDeleteDialog(NoxUI::UIContext& context, const std::vector<AssetHandle>& handles,
                              const std::vector<std::vector<std::filesystem::path>>& users, const std::unordered_set<AssetHandle>& inScene);
        // Replace References: the asset to use instead (same type, searchable), what will be rewritten and what cannot be.
        void ShowReplaceDialog(NoxUI::UIContext& context, AssetHandle from, const std::vector<std::filesystem::path>& users, bool sceneUses);
        void CreateVariant(AssetHandle base);
        void MoveAssets(const std::vector<AssetHandle>& handles, const std::filesystem::path& absoluteFolder);
        void CreatePrefabFromDrop(uint64_t dragId, const std::filesystem::path& absoluteFolder);
        void NewFolder();
        void CreateMaterial();
        void CreateGraph(int kind); // 0 animation, 1 audio, 2 material
        void CreateMaterialShader();
        void CreateTimeline();
        void CreateScript();
        void OpenModelImportDialog(const std::filesystem::path& relativeSource, const std::filesystem::path& externalSource);
        void ShowInExplorer(const std::filesystem::path& absolutePath);
        void PickAndImport();
        // A new file of the open folder: registered, listed, selected and put into rename mode.
        void AdoptNewFile(const std::filesystem::path& absolutePath);
        std::vector<NoxUI::UIMenuItem> BuildAssetMenu(const ContentEntry& entry);
        std::vector<NoxUI::UIMenuItem> BuildFolderSpaceMenu();
        std::vector<NoxUI::UIMenuItem> BuildAddMenu();
        bool PassesFilter(AssetType type, bool isScript) const;

        std::filesystem::path m_BaseDirectory;    // the project's Assets folder
        std::filesystem::path m_CurrentDirectory; // absolute
        std::vector<std::filesystem::path> m_Back;
        std::vector<std::filesystem::path> m_Forward;

        std::vector<ContentEntry> m_Entries;      // what the tiles show (folder content, or search results)
        std::string m_Search;
        int m_TypeFilter = 0;
        std::filesystem::file_time_type m_FolderStamp{};
        size_t m_RegistrySize = 0;
        float m_RescanTimer = 0.0f;
        bool m_EntriesDirty = true;
        bool m_TreeDirty = true;
        std::unordered_map<uint64_t, std::filesystem::path> m_TreeFolders; // tree node id -> absolute folder
        bool m_ImportsWereRunning = false;
        // What the tiles list now: a refresh of the same listing keeps its scroll position and selection.
        std::filesystem::path m_ListedDirectory;
        std::string m_ListedSearch;
        int m_ListedFilter = -1;
        size_t m_TreeSignature = 0; // the folder tree is rebuilt only when its folders changed

        Ref<NoxUI::UIElement> m_Root;
        Ref<NoxUI::UITreeView> m_FolderTree;
        Ref<ContentTileView> m_Tiles;
        Ref<ContentPathBar> m_PathBar;
        Ref<NoxUI::UIElement> m_ToolbarExtra;
        Ref<NoxUI::UISearchBar> m_SearchBar;
        Ref<NoxUI::UIDropdown> m_FilterDropdown;
        Ref<NoxUI::UIButton> m_BackButton;
        Ref<NoxUI::UIButton> m_ForwardButton;
        Ref<NoxUI::UILabel> m_Footer;

        std::function<void(AssetHandle)> m_OpenAsset;
        std::function<AssetHandle(uint64_t, const std::filesystem::path&)> m_CreatePrefab;
        std::function<void(std::unordered_set<AssetHandle>&)> m_SceneReferences;
        std::function<AssetHandle(AssetHandle, const std::filesystem::path&)> m_CreateVariant;
        std::function<void(AssetHandle, AssetHandle)> m_ReplaceReferences;
        std::function<void(NoxUI::UINotification)> m_Notify;
        AssetHandle m_PendingShow = 0;

        struct ReferenceCheck
        {
            ReferenceCheckMode Mode = ReferenceCheckMode::Delete;
            std::vector<AssetHandle> Handles;
            std::unordered_set<AssetHandle> InScene;          // what the open scenes use (taken on the main thread)
            std::future<std::vector<std::vector<std::filesystem::path>>> Users; // per handle
            NoxUI::UIContext* Context = nullptr;                 // the window it was asked in (the dialog opens there)
            std::string Name;                                 // the first asset's file name
        };
        std::unique_ptr<ReferenceCheck> m_ReferenceCheck;
    };
}
