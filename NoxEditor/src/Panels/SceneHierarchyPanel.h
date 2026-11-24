#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "NoxCore/Asset/PrimitiveShapes.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Scene.h"
#include <NoxUI/Core/UITypes.h>
#include "EditorSelection.h"

namespace NoxUI
{
        class UIElement;
        class UILabel;
        class UISearchBar;
        class UITreeView;
        struct UITreeNode;
    }

namespace Nox
{

    class SceneHierarchyPanel
    {
    public:
        SceneHierarchyPanel();
        explicit SceneHierarchyPanel(const Ref<Scene>& context);
        ~SceneHierarchyPanel();

        const Ref<NoxUI::UIElement>& BuildUI();
        const Ref<NoxUI::UIElement>& GetRoot() const { return m_Root; }

        void SetContext(const Ref<Scene>& context);
        void Update();
        void MarkDirty() { m_Dirty = true; }

        // The editor's selection this panel shows and changes (null: a selection of its own). The selection is not the panel's:
        // the viewport, the inspector and the shortcuts change it too, and the rows follow.
        void SetSelection(EditorSelection* selection) { m_Selection = selection; }
        bool IsSelected(Entity entity) const;
        void SetSelectedEntity(Entity entity);
        void ToggleSelectedEntity(Entity entity);
        void SelectRangeTo(Entity targetEntity);
        void ClearSelection();

        void FocusSearch();
        void DeleteSelection();
        void DuplicateSelection();

        void SetFocusEntityCallback(std::function<void(Entity)> callback) { m_FocusEntity = std::move(callback); }
        void SetCreateShapeCallback(std::function<void(PrimitiveShape, const std::string&)> callback) { m_CreateShape = std::move(callback); }
        void SetCreatePrefabCallback(std::function<void(UUID)> callback) { m_CreatePrefab = std::move(callback); }
        void SetSceneMutatedCallback(std::function<void()> callback) { m_OnSceneMutated = std::move(callback); }

    private:
        enum class NodeKind : uint8_t { Entity, Folder };
        struct NodeMapping
        {
            NodeKind Kind = NodeKind::Entity;
            UUID EntityId = 0;
            std::string FolderPath;
        };

        void RebuildTree();
        uint64_t ComputeSceneSignature() const;
        std::set<std::string> CollectFolderPaths() const;
        std::vector<Entity> CollectFolderContents(const std::string& path) const;
        void SetFolderContentsActive(const std::string& path, bool active);
        void FileEntities(const std::vector<Entity>& entities, const std::string& folderPath);
        void ReparentEntities(const std::vector<Entity>& entities, Entity parent);
        bool MoveFolder(const std::string& sourcePath, const std::string& targetParentFolder);

        void ShowContextMenuForNode(uint64_t nodeId, const NoxUI::UIVec2& screenPos);
        void ShowBlankContextMenu(const NoxUI::UIVec2& screenPos);
        void ShowAddMenu(const NoxUI::UIVec2& screenPos);

        void SyncTreeSelection();
        void UpdateCountLabel();
        EditorSelection& Sel() { return m_Selection ? *m_Selection : m_OwnSelection; }
        const EditorSelection& Sel() const { return m_Selection ? *m_Selection : m_OwnSelection; }
        uint64_t NodeIdForEntity(UUID uuid) const;
        uint64_t NodeIdForFolder(const std::string& path);

        Ref<Scene> m_Context;
        EditorSelection* m_Selection = nullptr;
        EditorSelection m_OwnSelection;
        uint64_t m_SeenSelectionVersion = 0;
        Entity m_SelectionAnchor; // where Shift+click ranges start
        size_t m_TotalEntities = 0;
        size_t m_MatchedEntities = 0;
        bool m_Filtering = false;
        std::unordered_set<UUID> m_OpenRows;

        std::string m_SearchFilter;
        uint64_t m_LastSignature = 0;
        bool m_Dirty = true;

        std::unordered_map<uint64_t, NodeMapping> m_NodeMap;
        std::unordered_map<std::string, uint64_t> m_FolderIdCache;

        Ref<NoxUI::UIElement> m_Root;
        Ref<NoxUI::UISearchBar> m_SearchBar;
        Ref<NoxUI::UITreeView> m_TreeView;
        Ref<NoxUI::UILabel> m_CountLabel;

        std::function<void(Entity)> m_FocusEntity;
        std::function<void(PrimitiveShape, const std::string&)> m_CreateShape;
        std::function<void(UUID)> m_CreatePrefab;
        std::function<void()> m_OnSceneMutated;
    };
}
