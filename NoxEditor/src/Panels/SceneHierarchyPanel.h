#pragma once
#include <functional>
#include <set>
#include <string>

#include <imgui.h>

#include "NoxCore/Core/core.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Scene.h"

namespace Nox
{
    class SceneHierarchyPanel
    {
    public:
        SceneHierarchyPanel() = default;
        SceneHierarchyPanel(const Ref<Scene>& context);
        
        void SetContext(const Ref<Scene>& context);
        
        void OnImGuiRender();
        
        const std::vector<Entity>& GetSelectedEntities() const { return m_SelectionContexts; };
        Entity GetSelectedEntity() const { return m_SelectionContexts.empty() ? Entity{} : m_SelectionContexts.back(); }
        void SetSelectedEntity(Entity entity);
        
        bool IsSelected(Entity entity) const;
        void ToggleSelectedEntity(Entity entity);
        void ClearSelection();
        void SelectRange(Entity entity);
        // Called with an asset's handle when its reference field in the inspector is double-clicked (e.g. the
        // Animator's Graph); EditorLayer decides what opening means per AssetType.
        void SetOpenAssetCallback(std::function<void(AssetHandle)> callback) { m_OpenAsset = std::move(callback); }
        // Model assets dropped from the Content Browser onto a folder row (folder path) or blank space (""): EditorLayer places them.
        void SetPlaceAssetsCallback(std::function<void(const std::vector<AssetHandle>&, const std::string&)> callback) { m_PlaceAssets = std::move(callback); }

        ImVec2 left;
        bool leftFocused;
        bool leftHovered;
        bool leftPropFocused;
        bool leftPropHovered;
    private:
        template<typename T>
        void DisplayAddComponentEntry(const std::string& entryName);
        
        void DrawEntityNode(Entity entity);
        void DrawComponents(Entity entity);
        // Outliner folders (FolderComponent paths; Scene::GetFolders keeps empty ones): a folder only groups rows.
        void DrawFolderNode(const std::string& path, const std::set<std::string>& allFolders);
        void FileEntities(UUID dragged, const std::string& path);
        void AcceptAssetDrop(const std::string& folder);
    private:
        Ref<Scene> m_Context;
        std::vector<Entity> m_SelectionContexts;
        Entity m_SelectionAnchor;

        // Deletes requested while drawing. Destroying entities inside the registry view being
        // iterated invalidates it, so they're applied after the loop, resolved by UUID so an entity
        // already destroyed as a child of another pending delete is skipped.
        std::vector<UUID> m_PendingDestroy;
        std::string m_PendingFolderDelete;
        Entity m_PendingSingleSelect; // a plain click on a selected entity narrows the selection on release, so it can be dragged as a group
        std::string m_RenameFolderPath;
        char m_FolderNameBuffer[128] = {};
        bool m_OpenFolderRename = false;
        bool m_MaterialShowAll = false; // Material component: show every slot instead of just this entity's own submesh
        std::function<void(AssetHandle)> m_OpenAsset;
        std::function<void(const std::vector<AssetHandle>&, const std::string&)> m_PlaceAssets;
    };
}
