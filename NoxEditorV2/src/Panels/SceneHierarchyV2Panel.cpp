#include "SceneHierarchyV2Panel.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Widgets/UIMenu.h"
#include "NoxCore/UI/Widgets/UISearchBar.h"
#include "NoxCore/UI/Widgets/UITreeView.h"

namespace Nox
{
    namespace
    {
        std::string ToLower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::string FolderLeaf(const std::string& path)
        {
            const size_t slash = path.rfind('/');
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        std::string UniqueFolderPath(const std::set<std::string>& existing, const std::string& parent, const std::string& base)
        {
            const std::string prefix = parent.empty() ? std::string() : parent + "/";
            std::string candidate = prefix + base;
            for (int suffix = 2; existing.contains(candidate); ++suffix)
                candidate = prefix + base + " " + std::to_string(suffix);
            return candidate;
        }

        uint64_t Mix64(uint64_t h, uint64_t v)
        {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            return h;
        }

        uint64_t HashString(std::string_view s)
        {
            uint64_t h = 0xcbf29ce484222325ull;
            for (unsigned char c : s)
            {
                h ^= c;
                h *= 0x100000001b3ull;
            }
            return h;
        }

        UI::UIColor EntityDotColor(Entity entity)
        {
            if (entity.HasComponent<PrefabInstanceComponent>() || entity.HasComponent<PrefabNodeComponent>())
                return { 0.45f, 0.70f, 1.0f, 1.0f };
            if (entity.HasComponent<CameraComponent>())
                return { 0.36f, 0.80f, 0.96f, 1.0f };
            if (entity.HasComponent<DirectionalLightComponent>() || entity.HasComponent<PointLightComponent>() ||
                entity.HasComponent<SpotLightComponent>() || entity.HasComponent<RectLightComponent>() ||
                entity.HasComponent<DiscLightComponent>() || entity.HasComponent<EnvironmentLightComponent>())
                return { 0.95f, 0.80f, 0.32f, 1.0f };
            if (entity.HasComponent<AudioSourceComponent>() || entity.HasComponent<AudioListenerComponent>())
                return { 0.42f, 0.86f, 0.62f, 1.0f };
            if (entity.HasComponent<MeshComponent>() || entity.HasComponent<ModelInstanceComponent>())
                return { 0.36f, 0.58f, 0.95f, 1.0f };
            return { 0.56f, 0.62f, 0.72f, 1.0f };
        }
    }

    SceneHierarchyV2Panel::SceneHierarchyV2Panel() = default;

    SceneHierarchyV2Panel::SceneHierarchyV2Panel(const Ref<Scene>& context)
    {
        SetContext(context);
    }

    SceneHierarchyV2Panel::~SceneHierarchyV2Panel() = default;

    const Ref<UI::UIElement>& SceneHierarchyV2Panel::BuildUI()
    {
        auto addBtn = UI::Button("+").Tooltip("Create entity or folder");
        addBtn->FixedSize = { 30.0f, 28.0f };
        addBtn->WidthMode = UI::UISizeMode::Fixed;
        addBtn->HeightMode = UI::UISizeMode::Fixed;
        addBtn->VAlign = UI::UIAlignment::Center;
        UI::UIButton* addBtnPtr = addBtn.Get().get();
        addBtn->OnClick = [this, addBtnPtr]()
        {
            const UI::UIRect& bounds = addBtnPtr->GetBounds();
            ShowAddMenu({ bounds.Min.x, bounds.Max.y + 2.0f });
        };

        m_SearchBar = UI::MakeUI<UI::UISearchBar>("Search entities & folders...");
        m_SearchBar->OnSearchChanged = [this](const std::string& q)
        {
            m_SearchFilter = q;
            m_Dirty = true;
        };

        auto topBar = UI::HStack(6.0f, { m_SearchBar, addBtn }).FillWidth();
        topBar->HeightMode = UI::UISizeMode::Auto;

        m_TreeView = UI::MakeUI<UI::UITreeView>();
        m_TreeView->WidthMode = UI::UISizeMode::Fill;
        m_TreeView->HeightMode = UI::UISizeMode::Fill;
        m_TreeView->RowHeight = 25.0f;
        m_TreeView->IndentWidth = 16.0f;
        m_TreeView->EmptyText = "No matching entities or folders.";

        m_TreeView->OnRowClick = [this](uint64_t id, bool ctrl, bool shift)
        {
            auto it = m_NodeMap.find(id);
            if (it == m_NodeMap.end())
                return;

            if (it->second.Kind == NodeKind::Folder)
            {
                if (!ctrl && !shift)
                {
                    ClearSelection();
                    m_TreeView->SetSelectedIds({ id }, id);
                }
                return;
            }

            Entity entity = m_Context ? m_Context->GetEntityByUUID(it->second.EntityId) : Entity{};
            if (!entity)
                return;

            if (shift)
                SelectRangeTo(entity);
            else if (ctrl)
                ToggleSelectedEntity(entity);
            else
                SetSelectedEntity(entity);
        };

        m_TreeView->OnClearSelection = [this]() { ClearSelection(); };

        m_TreeView->OnDoubleClick = [this](uint64_t id)
        {
            auto it = m_NodeMap.find(id);
            if (it == m_NodeMap.end() || !m_Context)
                return;

            if (it->second.Kind == NodeKind::Folder)
            {
                const std::string& path = it->second.FolderPath;
                m_Context->SetFolderCollapsed(path, !m_Context->IsFolderCollapsed(path));
                m_Dirty = true;
                return;
            }

            if (Entity entity = m_Context->GetEntityByUUID(it->second.EntityId); entity && m_FocusEntity)
                m_FocusEntity(entity);
        };

        m_TreeView->OnToggleExpand = [this](uint64_t id, bool expanded)
        {
            auto it = m_NodeMap.find(id);
            if (it == m_NodeMap.end() || !m_Context)
                return;
            if (it->second.Kind == NodeKind::Folder)
                m_Context->SetFolderCollapsed(it->second.FolderPath, !expanded);
            else if (expanded)
                m_OpenRows.insert(it->second.EntityId);
            else
                m_OpenRows.erase(it->second.EntityId);
        };

        m_TreeView->OnToggleVisibility = [this](uint64_t id, bool makeVisible)
        {
            auto it = m_NodeMap.find(id);
            if (it == m_NodeMap.end() || !m_Context)
                return;

            if (it->second.Kind == NodeKind::Folder)
            {
                SetFolderContentsActive(it->second.FolderPath, makeVisible);
            }
            else if (Entity entity = m_Context->GetEntityByUUID(it->second.EntityId))
            {
                if (m_Context->IsEntityLocked(entity))
                    return;
                if (!makeVisible && !entity.HasComponent<InactiveComponent>())
                    entity.AddComponent<InactiveComponent>();
                else if (makeVisible && entity.HasComponent<InactiveComponent>())
                    entity.RemoveComponent<InactiveComponent>();
            }
            m_Dirty = true;
            if (m_OnSceneMutated)
                m_OnSceneMutated();
        };

        m_TreeView->OnContextMenu = [this](uint64_t id, const UI::UIVec2& screenPos)
        {
            if (id == 0)
                ShowBlankContextMenu(screenPos);
            else
                ShowContextMenuForNode(id, screenPos);
        };

        m_TreeView->OnRename = [this](uint64_t id, const std::string& newName)
        {
            auto it = m_NodeMap.find(id);
            if (it == m_NodeMap.end() || !m_Context || newName.empty())
                return;

            if (it->second.Kind == NodeKind::Folder)
            {
                if (newName.find('/') != std::string::npos)
                    return;
                const std::string& oldPath = it->second.FolderPath;
                const size_t slash = oldPath.rfind('/');
                const std::string parent = (slash == std::string::npos) ? std::string() : oldPath.substr(0, slash + 1);
                m_Context->RenameFolder(oldPath, parent + newName);
            }
            else if (Entity entity = m_Context->GetEntityByUUID(it->second.EntityId))
            {
                if (!m_Context->IsEntityLocked(entity))
                    entity.GetComponent<TagComponent>().Tag = newName;
            }
            m_Dirty = true;
            if (m_OnSceneMutated)
                m_OnSceneMutated();
        };

        m_TreeView->OnBuildDragPayload = [this](uint64_t draggedId) -> std::optional<UI::UIDragPayload>
        {
            auto it = m_NodeMap.find(draggedId);
            if (it == m_NodeMap.end() || !m_Context)
                return std::nullopt;

            if (it->second.Kind == NodeKind::Folder)
            {
                return UI::UIDragPayload{ "SCENE_HIERARCHY_FOLDER", FolderLeaf(it->second.FolderPath), draggedId };
            }

            Entity entity = m_Context->GetEntityByUUID(it->second.EntityId);
            if (!entity || entity.HasComponent<PrefabNodeComponent>())
                return std::nullopt;

            const size_t count = IsSelected(entity) ? m_Selection.size() : 1;
            const std::string text = count > 1
                ? std::to_string(count) + " entities"
                : entity.GetComponent<TagComponent>().Tag;
            return UI::UIDragPayload{ "SCENE_HIERARCHY_ENTITY", text, draggedId };
        };

        m_TreeView->OnDropNode = [this](uint64_t draggedId, uint64_t targetId)
        {
            if (!m_Context)
                return;
            auto dragIt = m_NodeMap.find(draggedId);
            if (dragIt == m_NodeMap.end())
                return;

            if (dragIt->second.Kind == NodeKind::Folder)
            {
                std::string targetParent;
                if (targetId != 0)
                {
                    auto tgtIt = m_NodeMap.find(targetId);
                    if (tgtIt == m_NodeMap.end() || tgtIt->second.Kind != NodeKind::Folder)
                        return;
                    targetParent = tgtIt->second.FolderPath;
                }
                if (MoveFolder(dragIt->second.FolderPath, targetParent))
                {
                    m_Dirty = true;
                    if (m_OnSceneMutated)
                        m_OnSceneMutated();
                }
                return;
            }

            Entity draggedEntity = m_Context->GetEntityByUUID(dragIt->second.EntityId);
            if (!draggedEntity)
                return;

            std::vector<Entity> moved;
            if (IsSelected(draggedEntity))
                moved = m_Selection;
            else
                moved.push_back(draggedEntity);

            if (targetId == 0)
            {
                ReparentEntities(moved, Entity{});
                FileEntities(moved, std::string());
            }
            else
            {
                auto tgtIt = m_NodeMap.find(targetId);
                if (tgtIt == m_NodeMap.end())
                    return;

                if (tgtIt->second.Kind == NodeKind::Folder)
                {
                    FileEntities(moved, tgtIt->second.FolderPath);
                }
                else if (Entity targetEntity = m_Context->GetEntityByUUID(tgtIt->second.EntityId))
                {
                    ReparentEntities(moved, targetEntity);
                    m_OpenRows.insert(targetEntity.GetUUID());
                }
            }

            m_Dirty = true;
            if (m_OnSceneMutated)
                m_OnSceneMutated();
        };

        m_TreeView->OnDeleteRequested = [this]() { DeleteSelection(); };
        m_TreeView->OnFocusRequested = [this]()
        {
            if (m_FocusEntity && GetSelectedEntity())
                m_FocusEntity(GetSelectedEntity());
        };

        m_CountLabel = UI::Label("0 entities");
        m_CountLabel->UseColorOverride = true;
        m_CountLabel->ColorOverride = { 0.58f, 0.62f, 0.69f, 1.0f };

        auto footer = UI::HStack(8.0f, { m_CountLabel }).FillWidth();
        footer->HeightMode = UI::UISizeMode::Auto;

        m_Root = UI::VStack(8.0f, { topBar, m_TreeView, footer })
            .Padding(8.0f)
            .Fill();

        RebuildTree();
        return m_Root;
    }

    void SceneHierarchyV2Panel::SetContext(const Ref<Scene>& context)
    {
        m_Context = context;
        ClearSelection();
        m_LastSignature = 0;
        m_Dirty = true;
    }

    void SceneHierarchyV2Panel::Update()
    {
        if (!m_Context || !m_TreeView)
            return;

        const uint64_t sig = ComputeSceneSignature();
        if (m_Dirty || sig != m_LastSignature)
        {
            m_LastSignature = sig;
            m_Dirty = false;
            RebuildTree();
        }
    }

    uint64_t SceneHierarchyV2Panel::ComputeSceneSignature() const
    {
        if (!m_Context)
            return 0;

        uint64_t h = 0x517cc1b727220a95ull;
        const auto& folders = m_Context->GetFolders();
        h = Mix64(h, folders.size());
        for (const std::string& f : folders)
        {
            h = Mix64(h, HashString(f));
            h = Mix64(h, m_Context->IsFolderCollapsed(f) ? 1ull : 0ull);
        }

        const auto& subLevels = m_Context->GetSubLevels();
        h = Mix64(h, subLevels.size());

        auto view = m_Context->GetRegistry().view<IDComponent, TagComponent>();
        h = Mix64(h, view.size_hint());
        for (auto handle : view)
        {
            Entity entity(handle, m_Context.get());
            h = Mix64(h, static_cast<uint64_t>(view.get<IDComponent>(handle).ID));
            h = Mix64(h, HashString(view.get<TagComponent>(handle).Tag));
            if (entity.HasComponent<RelationshipComponent>())
                h = Mix64(h, static_cast<uint64_t>(entity.GetComponent<RelationshipComponent>().Parent));
            if (entity.HasComponent<FolderComponent>())
                h = Mix64(h, HashString(entity.GetComponent<FolderComponent>().Path));
            if (entity.HasComponent<InactiveComponent>())
                h = Mix64(h, 0xA11CEu);
        }
        return h;
    }

    bool SceneHierarchyV2Panel::IsSelected(Entity entity) const
    {
        return std::find(m_Selection.begin(), m_Selection.end(), entity) != m_Selection.end();
    }

    void SceneHierarchyV2Panel::SetSelectedEntity(Entity entity)
    {
        m_Selection.clear();
        if (entity && m_Context && m_Context->IsEntityLocked(entity))
            entity = {};
        if (entity)
            m_Selection.push_back(entity);
        m_SelectionAnchor = entity;
        SyncTreeSelection();
    }

    void SceneHierarchyV2Panel::ToggleSelectedEntity(Entity entity)
    {
        if (entity && m_Context && m_Context->IsEntityLocked(entity))
            return;
        auto it = std::find(m_Selection.begin(), m_Selection.end(), entity);
        if (it != m_Selection.end())
            m_Selection.erase(it);
        else
            m_Selection.push_back(entity);
        m_SelectionAnchor = entity;
        SyncTreeSelection();
    }

    void SceneHierarchyV2Panel::SelectRangeTo(Entity targetEntity)
    {
        if (!m_SelectionAnchor || !m_Context || !m_TreeView)
        {
            SetSelectedEntity(targetEntity);
            return;
        }

        const uint64_t anchorId = NodeIdForEntity(m_SelectionAnchor.GetUUID());
        const uint64_t targetId = NodeIdForEntity(targetEntity.GetUUID());
        const std::vector<uint64_t> visible = m_TreeView->GetVisibleIdsInOrder();
        auto itA = std::find(visible.begin(), visible.end(), anchorId);
        auto itT = std::find(visible.begin(), visible.end(), targetId);
        if (itA == visible.end() || itT == visible.end())
        {
            SetSelectedEntity(targetEntity);
            return;
        }

        const size_t i0 = static_cast<size_t>(std::distance(visible.begin(), itA));
        const size_t i1 = static_cast<size_t>(std::distance(visible.begin(), itT));
        const size_t lo = std::min(i0, i1);
        const size_t hi = std::max(i0, i1);

        m_Selection.clear();
        for (size_t i = lo; i <= hi; ++i)
        {
            auto mapIt = m_NodeMap.find(visible[i]);
            if (mapIt == m_NodeMap.end() || mapIt->second.Kind != NodeKind::Entity)
                continue;
            Entity e = m_Context->GetEntityByUUID(mapIt->second.EntityId);
            if (e && !m_Context->IsEntityLocked(e))
                m_Selection.push_back(e);
        }
        SyncTreeSelection();
    }

    void SceneHierarchyV2Panel::ClearSelection()
    {
        m_Selection.clear();
        m_SelectionAnchor = {};
        SyncTreeSelection();
    }

    void SceneHierarchyV2Panel::FocusSearch()
    {
        if (m_SearchBar)
        {
            if (UI::UIContext* ctx = UI::UIContext::GetCurrent())
                ctx->SetFocus(m_SearchBar.get());
        }
    }

    void SceneHierarchyV2Panel::DeleteSelection()
    {
        if (!m_Context || m_Selection.empty())
            return;
        std::vector<UUID> toDelete;
        toDelete.reserve(m_Selection.size());
        for (Entity e : m_Selection)
        {
            if (e && !m_Context->IsEntityLocked(e))
                toDelete.push_back(e.GetUUID());
        }
        ClearSelection();
        for (UUID id : toDelete)
        {
            if (Entity entity = m_Context->GetEntityByUUID(id))
                m_Context->DestroyEntity(entity);
        }
        m_Dirty = true;
        if (m_OnSceneMutated)
            m_OnSceneMutated();
    }

    void SceneHierarchyV2Panel::DuplicateSelection()
    {
        if (!m_Context || m_Selection.empty())
            return;
        std::vector<Entity> copies;
        for (Entity e : m_Selection)
        {
            if (e && !m_Context->IsEntityLocked(e))
                copies.push_back(m_Context->DuplicateEntity(e));
        }
        if (!copies.empty())
        {
            m_Selection = copies;
            m_SelectionAnchor = copies.back();
            m_Dirty = true;
            if (m_OnSceneMutated)
                m_OnSceneMutated();
        }
    }

    std::set<std::string> SceneHierarchyV2Panel::CollectFolderPaths() const
    {
        std::set<std::string> folders;
        if (!m_Context)
            return folders;
        for (const std::string& f : m_Context->GetFolders())
            folders.insert(f);
        for (auto handle : m_Context->GetRegistry().view<FolderComponent>())
            folders.insert(m_Context->GetRegistry().get<FolderComponent>(handle).Path);
        for (const std::string& path : std::set<std::string>(folders))
        {
            for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
                folders.insert(path.substr(0, slash));
        }
        folders.erase(std::string());
        return folders;
    }

    std::vector<Entity> SceneHierarchyV2Panel::CollectFolderContents(const std::string& path) const
    {
        std::vector<Entity> contents;
        if (!m_Context)
            return contents;
        const std::string prefix = path + "/";
        for (auto handle : m_Context->GetRegistry().view<FolderComponent>())
        {
            Entity entity(handle, m_Context.get());
            const std::string& filed = entity.GetComponent<FolderComponent>().Path;
            const bool hasParent = entity.HasComponent<RelationshipComponent>() && entity.GetComponent<RelationshipComponent>().Parent != 0;
            if (!hasParent && (filed == path || filed.rfind(prefix, 0) == 0))
                contents.push_back(entity);
        }
        return contents;
    }

    void SceneHierarchyV2Panel::SetFolderContentsActive(const std::string& path, bool active)
    {
        if (!m_Context)
            return;
        for (Entity entity : CollectFolderContents(path))
        {
            if (m_Context->IsEntityLocked(entity))
                continue;
            if (!active && !entity.HasComponent<InactiveComponent>())
                entity.AddComponent<InactiveComponent>();
            else if (active && entity.HasComponent<InactiveComponent>())
                entity.RemoveComponent<InactiveComponent>();
        }
    }

    void SceneHierarchyV2Panel::FileEntities(const std::vector<Entity>& entities, const std::string& folderPath)
    {
        for (Entity entity : entities)
        {
            if (!entity || entity.HasComponent<PrefabNodeComponent>())
                continue;
            if (entity.HasComponent<RelationshipComponent>() && entity.GetComponent<RelationshipComponent>().Parent != 0)
                entity.SetParent({});
            if (folderPath.empty())
            {
                if (entity.HasComponent<FolderComponent>())
                    entity.RemoveComponent<FolderComponent>();
            }
            else if (entity.HasComponent<FolderComponent>())
            {
                entity.GetComponent<FolderComponent>().Path = folderPath;
            }
            else
            {
                entity.AddComponent<FolderComponent>(folderPath);
            }
        }
    }

    void SceneHierarchyV2Panel::ReparentEntities(const std::vector<Entity>& entities, Entity parent)
    {
        for (Entity entity : entities)
        {
            if (!entity || entity == parent || entity.HasComponent<PrefabNodeComponent>())
                continue;

            bool isDescendant = false;
            Entity check = parent;
            while (check)
            {
                if (check == entity)
                {
                    isDescendant = true;
                    break;
                }
                check = check.GetParent();
            }
            if (!isDescendant)
                entity.SetParent(parent);
        }
    }

    bool SceneHierarchyV2Panel::MoveFolder(const std::string& sourcePath, const std::string& targetParentFolder)
    {
        if (!m_Context || sourcePath.empty())
            return false;
        if (!targetParentFolder.empty() &&
            (targetParentFolder == sourcePath || targetParentFolder.rfind(sourcePath + "/", 0) == 0))
            return false;
        const std::string leaf = FolderLeaf(sourcePath);
        const std::string destPath = targetParentFolder.empty() ? leaf : (targetParentFolder + "/" + leaf);
        if (destPath == sourcePath)
            return false;
        m_Context->RenameFolder(sourcePath, destPath);
        return true;
    }

    uint64_t SceneHierarchyV2Panel::NodeIdForEntity(UUID uuid) const
    {
        const uint64_t id = static_cast<uint64_t>(uuid);
        return id == 0 ? 1 : id;
    }

    uint64_t SceneHierarchyV2Panel::NodeIdForFolder(const std::string& path)
    {
        if (auto it = m_FolderIdCache.find(path); it != m_FolderIdCache.end())
            return it->second;
        uint64_t id = HashString(path) | (1ull << 63);
        while (id == 0 || m_NodeMap.contains(id))
            ++id;
        m_FolderIdCache[path] = id;
        return id;
    }

    void SceneHierarchyV2Panel::SyncTreeSelection()
    {
        if (!m_TreeView)
            return;
        std::unordered_set<uint64_t> ids;
        ids.reserve(m_Selection.size());
        uint64_t primary = 0;
        for (Entity e : m_Selection)
        {
            if (e)
            {
                const uint64_t id = NodeIdForEntity(e.GetUUID());
                ids.insert(id);
                primary = id;
            }
        }
        m_TreeView->SetSelectedIds(std::move(ids), primary);
    }

    void SceneHierarchyV2Panel::RebuildTree()
    {
        if (!m_TreeView)
            return;

        m_NodeMap.clear();
        m_FolderIdCache.clear();

        if (!m_Context)
        {
            m_TreeView->SetNodes({});
            if (m_CountLabel)
                m_CountLabel->Text = "No active scene";
            return;
        }

        // Remove stale dead handles from m_Selection cleanly
        std::erase_if(m_Selection, [](const Entity& e) { return !static_cast<bool>(e); });
        if (!m_SelectionAnchor)
            m_SelectionAnchor = {};

        const std::string needle = ToLower(m_SearchFilter);
        const bool filtering = !needle.empty();

        const bool hasSubLevels = !m_Context->GetSubLevels().empty();
        size_t totalEntities = 0;
        size_t matchedEntities = 0;

        auto buildEntityNode = [&](auto& self, Entity entity, bool& outMatches) -> UI::UITreeNode
        {
            ++totalEntities;
            const auto& tag = entity.GetComponent<TagComponent>().Tag;
            const UUID uuid = entity.GetUUID();
            const uint64_t nodeId = NodeIdForEntity(uuid);
            m_NodeMap[nodeId] = { NodeKind::Entity, uuid, {} };

            const bool prefabRoot = entity.HasComponent<PrefabInstanceComponent>();
            const bool prefabNode = entity.HasComponent<PrefabNodeComponent>();
            const bool locked = m_Context->IsEntityLocked(entity);
            const bool selfActive = !entity.HasComponent<InactiveComponent>();
            const bool activeInHierarchy = m_Context->IsActiveInHierarchy(entity);

            UI::UITreeNode node;
            node.Id = nodeId;
            node.Label = tag;
            node.IsFolder = false;
            node.HasVisibilityToggle = true;
            node.IsVisible = selfActive;
            node.IsVisibleInHierarchy = activeInHierarchy;
            node.IsDimmed = locked || !activeInHierarchy;
            node.Draggable = !prefabNode && !locked;
            node.TagColor = EntityDotColor(entity);
            if (prefabRoot || prefabNode)
                node.LabelColor = prefabRoot ? UI::UIColor{ 0.45f, 0.70f, 1.0f, 1.0f } : UI::UIColor{ 0.45f, 0.70f, 1.0f, 0.65f };

            if (hasSubLevels && !entity.GetParent())
            {
                if (const AssetHandle level = m_Context->GetEntityLevel(entity); level != 0 && AssetManager::IsAssetHandleValid(level))
                    node.Badge = AssetManager::GetMetadata(level).FilePath.stem().string() + (locked ? " (locked)" : "");
                else if (locked)
                    node.Badge = "locked";
            }
            else if (locked)
            {
                node.Badge = "locked";
            }

            bool selfMatches = !filtering || (ToLower(tag).find(needle) != std::string::npos);
            bool anyChildMatches = false;

            if (entity.HasComponent<RelationshipComponent>())
            {
                const auto& children = entity.GetComponent<RelationshipComponent>().Children;
                for (UUID childId : children)
                {
                    if (Entity child = m_Context->GetEntityByUUID(childId))
                    {
                        bool childMatch = false;
                        UI::UITreeNode childNode = self(self, child, childMatch);
                        if (!filtering || childMatch)
                        {
                            node.Children.push_back(std::move(childNode));
                            if (childMatch)
                                anyChildMatches = true;
                        }
                    }
                }
            }

            outMatches = selfMatches || anyChildMatches;
            if (selfMatches && filtering)
                ++matchedEntities;

            node.Expanded = filtering ? anyChildMatches : m_OpenRows.contains(uuid);
            return node;
        };

        const std::set<std::string> folders = CollectFolderPaths();

        auto buildFolderNode = [&](auto& self, const std::string& path, bool& outMatches) -> UI::UITreeNode
        {
            const uint64_t nodeId = NodeIdForFolder(path);
            m_NodeMap[nodeId] = { NodeKind::Folder, 0, path };

            const std::vector<Entity> contents = CollectFolderContents(path);
            bool anyVisible = false;
            for (Entity e : contents)
                anyVisible |= !e.HasComponent<InactiveComponent>();

            UI::UITreeNode node;
            node.Id = nodeId;
            node.Label = FolderLeaf(path);
            node.IsFolder = true;
            node.HasVisibilityToggle = true;
            node.IsVisible = anyVisible || contents.empty();
            node.IsVisibleInHierarchy = node.IsVisible;
            node.IsDimmed = contents.empty();
            node.Draggable = true;
            node.TagColor = { 0.90f, 0.72f, 0.30f, 1.0f };

            const bool selfMatches = !filtering || (ToLower(node.Label).find(needle) != std::string::npos);
            bool anyChildMatches = false;

            for (const std::string& other : folders)
            {
                if (other.size() > path.size() + 1 && other.rfind(path + "/", 0) == 0 && other.find('/', path.size() + 1) == std::string::npos)
                {
                    bool subMatch = false;
                    UI::UITreeNode subFolder = self(self, other, subMatch);
                    if (!filtering || subMatch)
                    {
                        node.Children.push_back(std::move(subFolder));
                        if (subMatch)
                            anyChildMatches = true;
                    }
                }
            }

            std::vector<Entity> inside;
            for (auto handle : m_Context->GetRegistry().view<FolderComponent>())
            {
                Entity entity(handle, m_Context.get());
                const bool hasParent = entity.HasComponent<RelationshipComponent>() && entity.GetComponent<RelationshipComponent>().Parent != 0;
                if (!hasParent && entity.GetComponent<FolderComponent>().Path == path)
                    inside.push_back(entity);
            }
            std::sort(inside.begin(), inside.end(), [](Entity a, Entity b)
            {
                return a.GetComponent<TagComponent>().Tag < b.GetComponent<TagComponent>().Tag;
            });

            for (Entity entity : inside)
            {
                bool childMatch = false;
                UI::UITreeNode childNode = buildEntityNode(buildEntityNode, entity, childMatch);
                if (!filtering || childMatch)
                {
                    node.Children.push_back(std::move(childNode));
                    if (childMatch)
                        anyChildMatches = true;
                }
            }

            outMatches = selfMatches || anyChildMatches;
            node.Expanded = filtering ? anyChildMatches : !m_Context->IsFolderCollapsed(path);
            return node;
        };

        std::vector<UI::UITreeNode> roots;
        for (const std::string& path : folders)
        {
            if (path.find('/') == std::string::npos)
            {
                bool folderMatch = false;
                UI::UITreeNode folderNode = buildFolderNode(buildFolderNode, path, folderMatch);
                if (!filtering || folderMatch)
                    roots.push_back(std::move(folderNode));
            }
        }

        m_Context->GetRegistry().view<TagComponent>().each([&](auto entityHandle, TagComponent&)
        {
            Entity entity(entityHandle, m_Context.get());
            if (entity.HasComponent<RelationshipComponent>() && entity.GetComponent<RelationshipComponent>().Parent != 0)
                return;
            if (entity.HasComponent<FolderComponent>() && !entity.GetComponent<FolderComponent>().Path.empty())
                return;
            bool match = false;
            UI::UITreeNode node = buildEntityNode(buildEntityNode, entity, match);
            if (!filtering || match)
                roots.push_back(std::move(node));
        });

        m_TreeView->SetNodes(std::move(roots));
        SyncTreeSelection();

        if (m_CountLabel)
        {
            char buf[96];
            if (filtering)
                std::snprintf(buf, sizeof(buf), "%zu / %zu matching", matchedEntities, totalEntities);
            else if (!m_Selection.empty())
                std::snprintf(buf, sizeof(buf), "%zu entities  |  %zu selected", totalEntities, m_Selection.size());
            else
                std::snprintf(buf, sizeof(buf), "%zu entities", totalEntities);
            m_CountLabel->Text = buf;
        }
    }

    void SceneHierarchyV2Panel::ShowContextMenuForNode(uint64_t nodeId, const UI::UIVec2& screenPos)
    {
        UI::UIContext* ctx = UI::UIContext::GetCurrent();
        if (!ctx || !m_Context)
            return;

        auto it = m_NodeMap.find(nodeId);
        if (it == m_NodeMap.end())
            return;

        std::vector<UI::UIMenuItem> items;

        if (it->second.Kind == NodeKind::Folder)
        {
            const std::string path = it->second.FolderPath;
            const std::set<std::string> allFolders = CollectFolderPaths();

            items.push_back({"New Subfolder", "", [this, allFolders, path]()
            {
                m_Context->AddFolder(UniqueFolderPath(allFolders, path, "New Folder"));
                m_Context->SetFolderCollapsed(path, false);
                m_Dirty = true;
                if (m_OnSceneMutated)
                    m_OnSceneMutated();
            }});
            items.push_back({"Rename", "F2", [this, nodeId]()
            {
                if (m_TreeView)
                    m_TreeView->BeginRename(nodeId);
            }});
            items.push_back({"Select Contents", "", [this, path]()
            {
                ClearSelection();
                for (Entity e : CollectFolderContents(path))
                    if (!m_Context->IsEntityLocked(e))
                        m_Selection.push_back(e);
                if (!m_Selection.empty())
                    m_SelectionAnchor = m_Selection.back();
                SyncTreeSelection();
                m_Dirty = true;
            }});
            items.push_back({"Hide Contents", "", [this, path]()
            {
                SetFolderContentsActive(path, false);
                m_Dirty = true;
                if (m_OnSceneMutated)
                    m_OnSceneMutated();
            }});
            items.push_back({"Show Contents", "", [this, path]()
            {
                SetFolderContentsActive(path, true);
                m_Dirty = true;
                if (m_OnSceneMutated)
                    m_OnSceneMutated();
            }});

            if (m_CreateShape)
            {
                std::vector<UI::UIMenuItem> shapeItems;
                for (uint8_t idx = 0; idx < static_cast<uint8_t>(PrimitiveShape::Count); ++idx)
                {
                    const PrimitiveShape shape = static_cast<PrimitiveShape>(idx);
                    shapeItems.push_back({PrimitiveShapes::GetName(shape), "", [this, shape, path]() { m_CreateShape(shape, path); }});
                }
                items.push_back({"Shapes", "", {}, false, true, false, false, "", std::move(shapeItems)});
            }

            items.push_back({"", "", {}, true});
            items.push_back({"Delete Folder", "", [this, path]()
            {
                m_Context->RemoveFolder(path);
                m_Dirty = true;
                if (m_OnSceneMutated)
                    m_OnSceneMutated();
            }});

            UI::UIMenuPopup::ShowContextMenu(*ctx, screenPos, std::move(items));
            return;
        }

        Entity entity = m_Context->GetEntityByUUID(it->second.EntityId);
        if (!entity)
            return;
        if (!IsSelected(entity))
            SetSelectedEntity(entity);

        const bool locked = m_Context->IsEntityLocked(entity);

        if (m_FocusEntity)
        {
            items.push_back({"Focus", "F", [this, entity]() { m_FocusEntity(entity); }});
        }
        items.push_back({"Rename", "F2", [this, nodeId]()
        {
            if (m_TreeView)
                m_TreeView->BeginRename(nodeId);
        }, false, !locked});
        items.push_back({"Duplicate", "Ctrl+D", [this]() { DuplicateSelection(); }, false, !locked});
        items.push_back({"Create Child Entity", "", [this, entity, parentId = entity.GetUUID()]() mutable
        {
            Entity child = m_Context->CreateEntity("Empty Entity");
            child.SetParent(entity);
            m_OpenRows.insert(parentId);
            SetSelectedEntity(child);
            m_Dirty = true;
            if (m_OnSceneMutated)
                m_OnSceneMutated();
        }, false, !locked});

        if (m_CreatePrefab)
        {
            items.push_back({"Create Prefab", "", [this, uuid = entity.GetUUID()]() { m_CreatePrefab(uuid); }});
        }

        if (!m_Context->GetSubLevels().empty() && !m_Context->IsRunning())
        {
            const std::vector<Entity> moved = m_Selection;
            const AssetHandle from = m_Context->GetEntityLevel(entity);
            std::vector<UI::UIMenuItem> levelItems;
            levelItems.push_back({"Persistent Level", "", [this, moved]()
            {
                m_Context->MoveToLevel(moved, 0);
                m_Dirty = true;
                if (m_OnSceneMutated)
                    m_OnSceneMutated();
            }, false, true, from == 0});

            for (const Scene::SubLevel& subLevel : m_Context->GetSubLevels())
            {
                const std::string name = AssetManager::IsAssetHandleValid(subLevel.Level)
                    ? AssetManager::GetMetadata(subLevel.Level).FilePath.stem().string()
                    : std::string("(missing scene)");
                const bool enabled = subLevel.Loaded && subLevel.Visible && !subLevel.Locked;
                levelItems.push_back({name, "", [this, moved, level = subLevel.Level]()
                {
                    m_Context->MoveToLevel(moved, level);
                    m_Dirty = true;
                    if (m_OnSceneMutated)
                        m_OnSceneMutated();
                }, false, enabled, from == subLevel.Level});
            }
            items.push_back({"Move to Level", "", {}, false, true, false, false, "", std::move(levelItems)});
        }

        if (entity.GetParent())
        {
            items.push_back({"Unparent", "", [this]()
            {
                ReparentEntities(m_Selection, Entity{});
                m_Dirty = true;
                if (m_OnSceneMutated)
                    m_OnSceneMutated();
            }, false, !locked});
        }

        items.push_back({"", "", {}, true});
        items.push_back({"Delete", "Del", [this]() { DeleteSelection(); }, false, !locked});

        UI::UIMenuPopup::ShowContextMenu(*ctx, screenPos, std::move(items));
    }

    void SceneHierarchyV2Panel::ShowBlankContextMenu(const UI::UIVec2& screenPos)
    {
        ShowAddMenu(screenPos);
    }

    void SceneHierarchyV2Panel::ShowAddMenu(const UI::UIVec2& screenPos)
    {
        UI::UIContext* ctx = UI::UIContext::GetCurrent();
        if (!ctx || !m_Context)
            return;

        std::vector<UI::UIMenuItem> items;
        items.push_back({"New Folder", "", [this]()
        {
            const std::set<std::string> existing = CollectFolderPaths();
            m_Context->AddFolder(UniqueFolderPath(existing, std::string(), "New Folder"));
            m_Dirty = true;
            if (m_OnSceneMutated)
                m_OnSceneMutated();
        }});
        items.push_back({"Create Empty Entity", "", [this]()
        {
            Entity created = m_Context->CreateEntity("Empty Entity");
            SetSelectedEntity(created);
            m_Dirty = true;
            if (m_OnSceneMutated)
                m_OnSceneMutated();
        }});

        if (m_CreateShape)
        {
            std::vector<UI::UIMenuItem> shapeItems;
            for (uint8_t idx = 0; idx < static_cast<uint8_t>(PrimitiveShape::Count); ++idx)
            {
                const PrimitiveShape shape = static_cast<PrimitiveShape>(idx);
                shapeItems.push_back({PrimitiveShapes::GetName(shape), "", [this, shape]() { m_CreateShape(shape, std::string()); }});
            }
            items.push_back({"Shapes", "", {}, false, true, false, false, "", std::move(shapeItems)});
        }

        items.push_back({"", "", {}, true});
        items.push_back({"Expand All", "", [this]()
        {
            for (const std::string& path : CollectFolderPaths())
                m_Context->SetFolderCollapsed(path, false);
            for (auto handle : m_Context->GetRegistry().view<IDComponent, RelationshipComponent>())
            {
                if (!m_Context->GetRegistry().get<RelationshipComponent>(handle).Children.empty())
                    m_OpenRows.insert(m_Context->GetRegistry().get<IDComponent>(handle).ID);
            }
            m_Dirty = true;
        }});
        items.push_back({"Collapse All", "", [this]()
        {
            for (const std::string& path : CollectFolderPaths())
                m_Context->SetFolderCollapsed(path, true);
            m_OpenRows.clear();
            m_Dirty = true;
        }});

        UI::UIMenuPopup::ShowContextMenu(*ctx, screenPos, std::move(items));
    }
}
