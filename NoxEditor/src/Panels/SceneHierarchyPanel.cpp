#include "SceneHierarchyPanel.h"

#include <cctype>
#include <imgui.h>
#include <imgui_internal.h>
#include "misc/cpp/imgui_stdlib.h"
#include <glm/gtc/type_ptr.hpp>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Asset/MaterialSerializer.h"
#include "NoxCore/Renderer/Mesh.h"
#include "NoxCore/Scene/ModelInstance.h"
#include "NoxCore/Scene/Prefab.h"
#include "NoxCore/Scene/PrefabInstance.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Animation/Animator.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scripting/ScriptEngine.h"
#include "NoxCore/Utils/Utils.h"

namespace Nox
{
    SceneHierarchyPanel::SceneHierarchyPanel(const Ref<Scene>& context)
    {
        SetContext(context);
    }

    void SceneHierarchyPanel::SetContext(const Ref<Scene>& context)
    {
        m_Context = context;
        ClearSelection(); // if youz want tabs dont null provide the scene
    }

    bool SceneHierarchyPanel::IsSelected(Entity entity) const
    {
        return std::find(m_SelectionContexts.begin(), m_SelectionContexts.end(), entity) != m_SelectionContexts.end();
    }

    void SceneHierarchyPanel::SetSelectedEntity(Entity entity)
    {
        m_SelectionContexts.clear();
        if (entity)
            m_SelectionContexts.push_back(entity);

        m_SelectionAnchor = entity; // Set the anchor for future Shift-clicks
    }

    void SceneHierarchyPanel::ToggleSelectedEntity(Entity entity)
    {
        auto it = std::find(m_SelectionContexts.begin(), m_SelectionContexts.end(), entity);
        if (it != m_SelectionContexts.end())
            m_SelectionContexts.erase(it);
        else
            m_SelectionContexts.push_back(entity);

        m_SelectionAnchor = entity;
    }

    void SceneHierarchyPanel::SelectRange(Entity targetEntity)
    {
        if (!m_SelectionAnchor || !m_Context)
        {
            SetSelectedEntity(targetEntity);
            return;
        }

        // 1. Traverse the scene in hierarchy display order (depth-first)
        std::vector<Entity> allEntities;

        auto collectHierarchy = [&](auto& self, Entity current) -> void
        {
            allEntities.push_back(current);
            if (current.HasComponent<RelationshipComponent>())
            {
                const auto& children = current.GetComponent<RelationshipComponent>().Children;
                for (UUID childID : children)
                {
                    Entity child = m_Context->GetEntityByUUID(childID);
                    if (child)
                        self(self, child);
                }
            }
        };

        m_Context->m_Registry.view<TagComponent>().each([&](auto entityID, TagComponent&)
        {
            Entity entity(entityID, m_Context.get());

            bool isRoot = true;
            if (entity.HasComponent<RelationshipComponent>())
            {
                if (entity.GetComponent<RelationshipComponent>().Parent != 0)
                    isRoot = false;
            }

            if (isRoot)
                collectHierarchy(collectHierarchy, entity);
        });

        // 2. Find indices of anchor and target entity
        auto itAnchor = std::find(allEntities.begin(), allEntities.end(), m_SelectionAnchor);
        auto itTarget = std::find(allEntities.begin(), allEntities.end(), targetEntity);

        if (itAnchor == allEntities.end() || itTarget == allEntities.end())
        {
            SetSelectedEntity(targetEntity);
            return;
        }

        size_t indexAnchor = std::distance(allEntities.begin(), itAnchor);
        size_t indexTarget = std::distance(allEntities.begin(), itTarget);

        size_t startIndex = std::min(indexAnchor, indexTarget);
        size_t endIndex = std::max(indexAnchor, indexTarget);

        // 3. Fill selection with all entities in between (inclusive)
        m_SelectionContexts.clear();
        for (size_t i = startIndex; i <= endIndex; ++i)
        {
            m_SelectionContexts.push_back(allEntities[i]);
        }
        // Note: Do not change m_SelectionAnchor so subsequent Shift-clicks range from the same origin
    }

    void SceneHierarchyPanel::ClearSelection()
    {
        m_SelectionContexts.clear();
        m_SelectionAnchor = {};
    }

    namespace
    {
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
    }

    void SceneHierarchyPanel::OnImGuiRender()
    {
        ImGui::Begin("Scene Hierarchy");
        left = ImGui::GetWindowSize();
        leftFocused = ImGui::IsWindowFocused();
        leftHovered = ImGui::IsWindowHovered();

        if (m_Context)
        {
            // Folders first (explicit ones plus those entities are filed under, with their parents), then the entities that are
            // at the top level: no parent and no folder.
            std::set<std::string> folders(m_Context->GetFolders().begin(), m_Context->GetFolders().end());
            for (auto handle : m_Context->m_Registry.view<FolderComponent>())
                folders.insert(m_Context->m_Registry.get<FolderComponent>(handle).Path);
            for (const std::string& path : std::set<std::string>(folders))
            {
                for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
                    folders.insert(path.substr(0, slash));
            }
            folders.erase(std::string());
            for (const std::string& path : folders)
            {
                if (path.find('/') == std::string::npos)
                    DrawFolderNode(path, folders);
            }

            m_Context->m_Registry.view<TagComponent>().each([&](auto entityID, TagComponent&)
            {
                Entity entity(entityID, m_Context.get());

                bool isRoot = true;
                if (entity.HasComponent<RelationshipComponent>())
                    if (entity.GetComponent<RelationshipComponent>().Parent != 0)
                        isRoot = false;
                if (entity.HasComponent<FolderComponent>() && !entity.GetComponent<FolderComponent>().Path.empty())
                    isRoot = false; // drawn inside its folder

                if (isRoot)
                    DrawEntityNode(entity);
            });

            if (!m_PendingFolderDelete.empty())
            {
                m_Context->RemoveFolder(m_PendingFolderDelete);
                m_PendingFolderDelete.clear();
            }

            if (m_OpenFolderRename)
            {
                ImGui::OpenPopup("RenameFolder");
                m_OpenFolderRename = false;
            }
            if (ImGui::BeginPopupModal("RenameFolder", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::InputText("Folder name", m_FolderNameBuffer, sizeof(m_FolderNameBuffer));
                if (ImGui::Button("OK"))
                {
                    const std::string leaf = m_FolderNameBuffer;
                    if (!leaf.empty() && leaf.find('/') == std::string::npos)
                    {
                        const size_t slash = m_RenameFolderPath.rfind('/');
                        const std::string parent = slash == std::string::npos ? std::string() : m_RenameFolderPath.substr(0, slash + 1);
                        m_Context->RenameFolder(m_RenameFolderPath, parent + leaf);
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            for (UUID entityID : m_PendingDestroy)
            {
                if (Entity entity = m_Context->GetEntityByUUID(entityID))
                    m_Context->DestroyEntity(entity);
            }
            m_PendingDestroy.clear();

            // 1. Deselect entity when left-clicking blank space

            // 2. Blank-space Drag and Drop Target (entity unparenting, assets dropped at the top level). BeginDragDropTarget
            // binds to the last item, so a filler item spans the free space below the list to make all of it a target.
            const ImVec2 free = ImGui::GetContentRegionAvail();
            ImGui::InvisibleButton("##hierarchy_blank", ImVec2(free.x, free.y > 24.0f ? free.y : 24.0f));
            if (ImGui::IsItemHovered() && ImGui::IsMouseDown(0))
                ClearSelection(); // left-click on blank space deselects
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_HIERARCHY_ENTITY"))
                {
                    UUID droppedEntityID = *(UUID*)payload->Data;
                    Entity droppedEntity = m_Context->GetEntityByUUID(droppedEntityID);
                    if (droppedEntity)
                    {
                        droppedEntity.SetParent({}); // Make root
                        if (droppedEntity.HasComponent<FolderComponent>())
                            droppedEntity.RemoveComponent<FolderComponent>(); // and back out of any folder
                    }
                }
                AcceptAssetDrop(std::string());
                ImGui::EndDragDropTarget();
            }

            // 3. Right-click context menu on blank space
            const bool blankContext = ImGui::BeginPopupContextItem("##hierarchy_blank_context", ImGuiPopupFlags_MouseButtonRight);
            if (blankContext)
            {
                if (ImGui::MenuItem("New Folder"))
                {
                    std::set<std::string> existing(m_Context->GetFolders().begin(), m_Context->GetFolders().end());
                    m_Context->AddFolder(UniqueFolderPath(existing, std::string(), "New Folder"));
                }
                if (ImGui::MenuItem("Create Empty Entity"))
                {
                    m_Context->CreateEntity("Empty Entity");
                }

                ImGui::EndPopup();
            }
        }
        ImGui::End();

        ImGui::Begin("Properties");
        leftPropFocused = ImGui::IsWindowFocused();
        leftPropHovered = ImGui::IsWindowHovered();
        if (Entity selectedEntity = GetSelectedEntity())
        {
            DrawComponents(selectedEntity);
        }

        ImGui::End();
    }


    // Files the dragged entity (and the whole selection when it is part of it) under `path`; "" = back to the top level. A
    // folder is only a label for the outliner, so nothing about the entities' transforms changes.
    void SceneHierarchyPanel::FileEntities(UUID dragged, const std::string& path)
    {
        std::vector<Entity> entities;
        Entity draggedEntity = m_Context->GetEntityByUUID(dragged);
        if (!draggedEntity)
            return;
        if (IsSelected(draggedEntity))
            entities = m_SelectionContexts;
        else
            entities.push_back(draggedEntity);

        for (Entity entity : entities)
        {
            if (!entity)
                continue;
            if (entity.HasComponent<RelationshipComponent>() && entity.GetComponent<RelationshipComponent>().Parent != 0)
                entity.SetParent({}); // a folder holds top-level entities; the parenting is what put it below another one
            if (path.empty())
            {
                if (entity.HasComponent<FolderComponent>())
                    entity.RemoveComponent<FolderComponent>();
            }
            else if (entity.HasComponent<FolderComponent>())
            {
                entity.GetComponent<FolderComponent>().Path = path;
            }
            else
            {
                entity.AddComponent<FolderComponent>(path);
            }
        }
    }

    // Inside a drag-drop target: Content Browser assets (one, or a multi-selection) dropped here are placed in `folder`.
    void SceneHierarchyPanel::AcceptAssetDrop(const std::string& folder)
    {
        std::vector<AssetHandle> handles;
        if (const ImGuiPayload* multiple = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEMS"))
        {
            const AssetHandle* data = static_cast<const AssetHandle*>(multiple->Data);
            handles.assign(data, data + multiple->DataSize / sizeof(AssetHandle));
        }
        else if (const ImGuiPayload* single = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
        {
            handles.push_back(*static_cast<const AssetHandle*>(single->Data));
        }
        if (!handles.empty() && m_PlaceAssets)
            m_PlaceAssets(handles, folder);
    }

    void SceneHierarchyPanel::DrawFolderNode(const std::string& path, const std::set<std::string>& allFolders)
    {
        ImGui::PushID(path.c_str());
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
        const std::string label = "[Folder]  " + FolderLeaf(path);
        const bool opened = ImGui::TreeNodeEx("##folder", flags, "%s", label.c_str());

        // Entities dragged onto the folder are filed in it.
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_HIERARCHY_ENTITY"))
                FileEntities(*static_cast<const UUID*>(payload->Data), path);
            AcceptAssetDrop(path);
            ImGui::EndDragDropTarget();
        }

        bool deleteFolder = false;
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem("New Folder"))
                m_Context->AddFolder(UniqueFolderPath(allFolders, path, "New Folder"));
            if (ImGui::MenuItem("Rename"))
            {
                m_RenameFolderPath = path;
                strncpy_s(m_FolderNameBuffer, FolderLeaf(path).c_str(), sizeof(m_FolderNameBuffer) - 1);
                m_OpenFolderRename = true;
            }
            if (ImGui::MenuItem("Select Contents"))
            {
                ClearSelection();
                for (auto handle : m_Context->m_Registry.view<FolderComponent>())
                {
                    Entity entity(handle, m_Context.get());
                    if (entity.GetComponent<FolderComponent>().Path == path)
                        ToggleSelectedEntity(entity);
                }
            }
            if (ImGui::MenuItem("Delete Folder"))
                deleteFolder = true;
            ImGui::EndPopup();
        }

        if (opened)
        {
            for (const std::string& other : allFolders)
            {
                if (other.size() > path.size() + 1 && other.rfind(path + "/", 0) == 0 && other.find('/', path.size() + 1) == std::string::npos)
                    DrawFolderNode(other, allFolders);
            }

            std::vector<Entity> inside;
            for (auto handle : m_Context->m_Registry.view<FolderComponent>())
            {
                Entity entity(handle, m_Context.get());
                const bool hasParent = entity.HasComponent<RelationshipComponent>() && entity.GetComponent<RelationshipComponent>().Parent != 0;
                if (!hasParent && entity.GetComponent<FolderComponent>().Path == path)
                    inside.push_back(entity);
            }
            for (Entity entity : inside)
                DrawEntityNode(entity);
            ImGui::TreePop();
        }
        ImGui::PopID();

        if (deleteFolder)
            m_PendingFolderDelete = path;
    }

    // An override's value for display: the numbers that are asset handles (a material, a mesh, a clip) become the asset's file name.
    static std::string PrettyOverrideValue(const std::string& text)
    {
        std::string result;
        for (size_t i = 0; i < text.size();)
        {
            if (!std::isdigit(static_cast<unsigned char>(text[i])))
            {
                result += text[i++];
                continue;
            }
            size_t end = i;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])))
                ++end;
            const std::string digits = text.substr(i, end - i);
            i = end;

            if (digits.size() >= 10 && digits.size() <= 20)
            {
                try
                {
                    const AssetHandle handle(std::stoull(digits));
                    if (AssetManager::IsAssetHandleValid(handle))
                    {
                        result += Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle).FilePath.filename().string();
                        continue;
                    }
                }
                catch (...)
                {
                }
            }
            result += digits;
        }
        return result;
    }

    void SceneHierarchyPanel::DrawEntityNode(Entity entity)
    {
        auto& tag = entity.GetComponent<TagComponent>().Tag;

        bool isSelected = IsSelected(entity);

        ImGuiTreeNodeFlags flags = (isSelected ? ImGuiTreeNodeFlags_Selected : 0) |
            ImGuiTreeNodeFlags_OpenOnArrow;
        flags |= ImGuiTreeNodeFlags_SpanAvailWidth;

        bool hasChildren = false;
        if (entity.HasComponent<RelationshipComponent>())
            if (!entity.GetComponent<RelationshipComponent>().Children.empty())
                hasChildren = true;

        if (!hasChildren)
            flags |= ImGuiTreeNodeFlags_Leaf;

        // Prefab instances are blue (Unity's convention); the entities spawned from the prefab are a dimmer blue.
        const bool prefabRoot = entity.HasComponent<PrefabInstanceComponent>();
        const bool prefabNode = entity.HasComponent<PrefabNodeComponent>();
        if (prefabRoot || prefabNode)
            ImGui::PushStyleColor(ImGuiCol_Text, prefabRoot ? ImVec4(0.45f, 0.7f, 1.0f, 1.0f) : ImVec4(0.45f, 0.7f, 1.0f, 0.6f));
        bool opened = ImGui::TreeNodeEx((void*)(uint64_t)(uint32_t)entity, flags, tag.c_str());
        if (prefabRoot || prefabNode)
            ImGui::PopStyleColor();

        // --- Multi-selection click handling ---
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            ImGuiIO& io = ImGui::GetIO();
            if (io.KeyShift) // Shift + Click: Select range between anchor and this entity
            {
                SelectRange(entity);
            }
            else if (io.KeyCtrl) // Ctrl + Click: Add or remove from multi-selection
            {
                ToggleSelectedEntity(entity);
            }
            else // Normal Click: Select single entity
            {
                // Pressing on something that is already selected must not drop the rest of the selection: that would make
                // dragging a multi-selection impossible. It narrows on release, if the mouse did not drag.
                if (IsSelected(entity) && m_SelectionContexts.size() > 1)
                    m_PendingSingleSelect = entity;
                else
                    SetSelectedEntity(entity);
            }
        }

        if (m_PendingSingleSelect == entity && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 16.0f)
                SetSelectedEntity(entity);
            m_PendingSingleSelect = {};
        }

        // --- 1. DRAG SOURCE: Pick up this entity to drag it (not the entities of a prefab instance: they belong to it) ---
        if (!prefabNode && ImGui::BeginDragDropSource())
        {
            UUID entityID = entity.GetUUID();
            ImGui::SetDragDropPayload("SCENE_HIERARCHY_ENTITY", &entityID, sizeof(UUID));
            ImGui::Text("%s", tag.c_str());
            ImGui::EndDragDropSource();
        }

        // --- 2. DROP TARGET: Drop another entity onto this one to make it a child ---
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_HIERARCHY_ENTITY"))
            {
                UUID droppedEntityID = *(UUID*)payload->Data;
                Entity droppedEntity = m_Context->GetEntityByUUID(droppedEntityID);

                if (droppedEntity && droppedEntity != entity)
                {
                    // Prevent circular parenting (cannot parent an entity to its own child/descendant)
                    bool isDescendant = false;
                    Entity currentCheck = entity;
                    while (currentCheck)
                    {
                        if (currentCheck == droppedEntity)
                        {
                            isDescendant = true;
                            break;
                        }
                        currentCheck = currentCheck.GetParent();
                    }

                    if (!isDescendant)
                    {
                        droppedEntity.SetParent(entity);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        bool entityDeleted = false;
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem("Delete Entity"))
            {
                entityDeleted = true;
            }

            ImGui::EndPopup();
        }

        if (opened)
        {
            if (hasChildren)
            {
                auto children = entity.GetComponent<RelationshipComponent>().Children;
                for (UUID childID : children)
                {
                    Entity childEntity = m_Context->GetEntityByUUID(childID);
                    if (childEntity)
                        DrawEntityNode(childEntity);
                }
            }
            /*//ImGuiTreeNodeFlags flags = ((m_SelectionContext == entity) ? ImGuiTreeNodeFlags_Selected : 0) | ImGuiTreeNodeFlags_OpenOnArrow;
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            bool opened = ImGui::TreeNodeEx((void*)9817239, flags, tag.c_str());
            if (opened)
                ImGui::TreePop();*/
            ImGui::TreePop();
        }
        // at the end
        if (entityDeleted)
        {
            // If the right-clicked entity is part of the selection, delete all selected entities
            if (IsSelected(entity))
            {
                auto toDelete = m_SelectionContexts;
                ClearSelection();

                for (auto e : toDelete)
                {
                    if (e)
                        m_PendingDestroy.push_back(e.GetUUID());
                }
            }
            else
            {
                if (m_SelectionAnchor == entity)
                    m_SelectionAnchor = {};

                m_PendingDestroy.push_back(entity.GetUUID());

                auto it = std::find(m_SelectionContexts.begin(), m_SelectionContexts.end(), entity);
                if (it != m_SelectionContexts.end())
                    m_SelectionContexts.erase(it);
            }
        }
    }

    //styling maybe in the future different clas
    static bool DrawVec3Control(const std::string& label, glm::vec3& values, float resetValue = 0.0f,
                                float columnWidth = 100.0f)
    {
        bool valueChanged = false;
        ImGuiIO& io = ImGui::GetIO();
        auto boldFont = io.Fonts->Fonts[1];

        ImGui::PushID(label.c_str());

        ImGui::Columns(2);
        ImGui::SetColumnWidth(0, columnWidth);
        ImGui::Text(label.c_str());
        ImGui::NextColumn();

        ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0, 0});

        float lineHeight = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
        ImVec2 buttonSize = {lineHeight + 3.0f, lineHeight};

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.8f, 0.1f, 0.15f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.9f, 0.2f, 0.2f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.8f, 0.1f, 0.15f, 1.0f});
        ImGui::PushFont(boldFont);
        if (ImGui::Button("X", buttonSize))
        {
            values.x = resetValue;
            valueChanged = true;
        }
        ImGui::PopStyleColor(3);
        ImGui::PopFont();

        ImGui::SameLine();
        if (ImGui::DragFloat("##X", &values.x, 0.1f, 0.0f, 0.0f, "%.2f"))
            valueChanged = true;
        // the last 3 paramater force it to be only show 2dec
        ImGui::PopItemWidth();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.2f, 0.7f, 0.2f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.3f, 0.8f, 0.3f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.2f, 0.7f, 0.3f, 1.0f});
        ImGui::PushFont(boldFont);
        if (ImGui::Button("Y", buttonSize))
        {
            values.y = resetValue;
            valueChanged = true;
        }
        ImGui::PopStyleColor(3);
        ImGui::PopFont();

        ImGui::SameLine();
        if (ImGui::DragFloat("##Y", &values.y, 0.1f, 0.0f, 0.0f, "%.2f"))
            valueChanged = true;
        // the last 3 paramater force it to be only show 2dec
        ImGui::PopItemWidth();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.1f, 0.25f, 0.8f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.2f, 0.35f, 0.9f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.1f, 0.25f, 0.8f, 1.0f});
        ImGui::PushFont(boldFont);
        if (ImGui::Button("Z", buttonSize))
        {
            values.z = resetValue;
            valueChanged = true;
        }
        ImGui::PopStyleColor(3);
        ImGui::PopFont();

        ImGui::SameLine();
        if (ImGui::DragFloat("##Z", &values.z, 0.1f, 0.0f, 0.0f, "%.2f"))
            valueChanged = true;
        // the last 3 paramater force it to be only show 2dec
        ImGui::PopItemWidth();
        ImGui::PopStyleVar();

        ImGui::Columns(1);

        ImGui::PopID();

        return valueChanged;
    }

    template <typename T, typename UIFunction>
    static void DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction)
    {
        const ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed |
            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_FramePadding;
        if (entity.HasComponent<T>())
        {
            auto& component = entity.GetComponent<T>();

            ImVec2 contentRegionAvailable = ImGui::GetContentRegionAvail();

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
            /*float lineHeight = GImGui->Font->FontSize + ImGui::GetStyle().FramePadding.y * 2.0f;*/ //wrong imgui version ? or older
            float lineHeight = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
            ImGui::Separator();
            bool open = ImGui::TreeNodeEx((void*)typeid(T).hash_code(), treeNodeFlags, name.c_str());
            ImGui::PopStyleVar();
            ImGui::SameLine(contentRegionAvailable.x - lineHeight * 0.5f);
            // Every component has a "+" button and a settings popup: without a scope of its own per component they all share
            // one ID ("+", "ComponentSettings") and ImGui reports conflicting IDs.
            ImGui::PushID(reinterpret_cast<void*>(typeid(T).hash_code()));
            if (ImGui::Button("+", ImVec2{lineHeight, lineHeight}))
            {
                ImGui::OpenPopup("ComponentSettings");
            }

            bool removeComponent = false;
            if (ImGui::BeginPopup("ComponentSettings"))
            {
                if (ImGui::MenuItem("Remove Component"))
                {
                    removeComponent = true;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            if (open)
            {
                uiFunction(component);
                ImGui::TreePop();
            }

            if (removeComponent)
            {
                entity.RemoveComponent<T>();
            }
        }
    }

    void SceneHierarchyPanel::DrawComponents(Entity entity)
    {
        if (!entity)
            return;

        if (entity.HasComponent<TagComponent>())
        {
            auto& tag = entity.GetComponent<TagComponent>().Tag;

            char buffer[256];
            memset(buffer, 0, sizeof(buffer));
            // Truncate rather than strcpy_s (which aborts the whole editor if tag.c_str() is >= 256
            // bytes) -- this runs every frame the entity is selected, so any tag that's too long for
            // any reason (bad scene file, future scripting API, etc.) should never be able to crash it.
            strncpy_s(buffer, sizeof(buffer), tag.c_str(), _TRUNCATE);
            if (ImGui::InputText("##Tag", buffer, sizeof(buffer)))
            {
                tag = std::string(buffer);
            }
        }

        ImGui::SameLine();
        ImGui::PushItemWidth(-1);

        if (ImGui::Button("Add Component"))
        {
            ImGui::OpenPopup("AddComponent");
        }

        if (ImGui::BeginPopup("AddComponent"))
        {
            DisplayAddComponentEntry<MeshComponent>("Mesh");
            DisplayAddComponentEntry<MaterialComponent>("Material");

            DisplayAddComponentEntry<DirectionalLightComponent>("Directional Light");
            DisplayAddComponentEntry<PointLightComponent>("Point Light");
            DisplayAddComponentEntry<SpotLightComponent>("Spot Light");
            DisplayAddComponentEntry<EnvironmentLightComponent>("Environment Light");

            DisplayAddComponentEntry<AnimatorComponent>("Animator");

            DisplayAddComponentEntry<CameraComponent>("Camera");
            DisplayAddComponentEntry<ScriptComponent>("Script");
            DisplayAddComponentEntry<SpriteRendererComponent>("Sprite Renderer");
            DisplayAddComponentEntry<CircleRendererComponent>("Circle Renderer");
            DisplayAddComponentEntry<RigidBody2DComponent>("Rigidbody 2D");
            DisplayAddComponentEntry<BoxCollider2DComponent>("Box Collider 2D");
            DisplayAddComponentEntry<CircleCollider2DComponent>("Circle Collider 2D");
            DisplayAddComponentEntry<RigidBody3DComponent>("Rigidbody 3D");
            DisplayAddComponentEntry<BoxCollider3DComponent>("Box Collider 3D");
            DisplayAddComponentEntry<SphereCollider3DComponent>("Sphere Collider 3D");
            DisplayAddComponentEntry<CapsuleCollider3DComponent>("Capsule Collider 3D");
            DisplayAddComponentEntry<CharacterController3DComponent>("Character Controller 3D");
            DisplayAddComponentEntry<TextComponent>("Text Component");

            ImGui::EndPopup();
        }

        ImGui::PopItemWidth();

        // A prefab instance root: where it comes from, and the way out (Unpack turns it into ordinary entities).
        if (entity.HasComponent<PrefabInstanceComponent>())
        {
            const AssetHandle prefabHandle = entity.GetComponent<PrefabInstanceComponent>().Prefab;
            std::string prefabName = "(missing asset)";
            if (AssetManager::IsAssetHandleValid(prefabHandle))
                prefabName = Project::GetActive()->GetEditorAssetManager()->GetMetadata(prefabHandle).FilePath.filename().string();
            ImGui::TextColored(ImVec4(0.45f, 0.7f, 1.0f, 1.0f), "Prefab: %s", prefabName.c_str());
            if (const Prefab* prefabAsset = AssetManager::FindLoadedAsset<Prefab>(prefabHandle);
                prefabAsset && prefabAsset->Base != 0 && AssetManager::IsAssetHandleValid(prefabAsset->Base))
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(variant of %s)", Project::GetActive()->GetEditorAssetManager()->GetMetadata(prefabAsset->Base).FilePath.filename().string().c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Open") && m_OpenAsset)
                m_OpenAsset(prefabHandle);
            ImGui::SameLine();
            if (ImGui::SmallButton("Unpack"))
                PrefabInstance::Unpack(*m_Context, entity);
            ImGui::TextDisabled("What you change here (fields, components, entities) is kept on this instance as an override; Apply writes it into the prefab.");

            // Overrides: everything that differs from the prefab (worked out from the spawned entities): fields, deleted entities,
            // removed and added components, entities attached below.
            const std::vector<PrefabInstance::OverrideEntry> overrides = PrefabInstance::ListOverrides(*m_Context, entity);
            const std::string overridesHeader = "Overrides (" + std::to_string(overrides.size()) + ")###PrefabOverrides";
            if (ImGui::CollapsingHeader(overridesHeader.c_str()))
            {
                if (overrides.empty())
                    ImGui::TextDisabled("Nothing differs from the prefab.");

                using Kind = PrefabInstance::OverrideEntry::EntryKind;
                auto componentName = [](std::string name)
                {
                    if (name.size() > 9 && name.compare(name.size() - 9, 9, "Component") == 0)
                        name.resize(name.size() - 9);
                    return name;
                };

                bool changedInstance = false;
                for (size_t i = 0; i < overrides.size() && !changedInstance; ++i)
                {
                    const PrefabInstance::OverrideEntry& entry = overrides[i];
                    const std::string targetName = entry.TargetName.empty() ? std::string("?") : entry.TargetName;

                    std::string label;
                    switch (entry.Kind)
                    {
                    case Kind::Property:
                        label = targetName + ": " + componentName(entry.Component) + "." + entry.Field + " = " + PrettyOverrideValue(entry.Value);
                        break;
                    case Kind::RemovedEntity:
                        label = "- Entity: " + targetName + " (deleted)";
                        break;
                    case Kind::RemovedComponent:
                        label = "- Component: " + targetName + " / " + componentName(entry.Component);
                        break;
                    case Kind::AddedComponent:
                        label = "+ Component: " + targetName + " / " + componentName(entry.Component);
                        break;
                    case Kind::AddedEntity:
                        label = "+ Entity: " + entry.Name + " (below " + targetName + ")";
                        break;
                    }

                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TextColored(ImVec4(0.45f, 0.7f, 1.0f, 1.0f), "%s", label.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Revert"))
                    {
                        PrefabInstance::RevertOverride(*m_Context, entity, &entry);
                        changedInstance = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Apply"))
                    {
                        PrefabInstance::ApplyOverride(*m_Context, entity, entry);
                        changedInstance = true;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Apply writes this change into the prefab: every instance takes it.");
                    ImGui::PopID();
                }
                if (!changedInstance && !overrides.empty() && ImGui::Button("Revert All"))
                {
                    PrefabInstance::RevertOverride(*m_Context, entity, nullptr);
                    changedInstance = true;
                }

                // The spawned entities are made again: what was selected below the instance is gone.
                if (changedInstance)
                    SetSelectedEntity(entity);
            }
        }

        if (entity.HasComponent<PrefabNodeComponent>())
        {
            Entity instance = m_Context->GetEntityByUUID(entity.GetComponent<PrefabNodeComponent>().Instance);
            ImGui::TextColored(ImVec4(0.45f, 0.7f, 1.0f, 0.8f), "Part of prefab instance '%s'.", instance ? instance.GetName().c_str() : "?");
            ImGui::TextDisabled("Changes here are kept as overrides on the instance (listed there); Apply writes them into the prefab.");
            if (instance && ImGui::SmallButton("Select Instance"))
                SetSelectedEntity(instance);
        }

        DrawComponent<TransformComponent>("Transform", entity, [this, entity](auto& component)
        {
            // Record old values before UI interaction to compute the delta
            glm::vec3 oldTranslation = component.Translation;
            glm::vec3 oldRotation = component.Rotation;
            glm::vec3 oldScale = component.Scale;

            bool posModified = DrawVec3Control("Position", component.Translation);

            bool rotModified = false;
            glm::vec3 rotation = glm::degrees(component.Rotation);
            if (DrawVec3Control("Rotation", rotation))
            {
                component.Rotation = glm::radians(rotation);
                rotModified = true;
            }

            bool scaleModified = DrawVec3Control("Scale", component.Scale, 1.0f);

            bool modified = posModified || rotModified || scaleModified;

            if (modified)
            {
                entity.MarkTransformDirty();

                // If multiple entities are selected, apply the exact same delta to all other selected entities!
                if (m_SelectionContexts.size() > 1)
                {
                    glm::vec3 deltaTranslation = component.Translation - oldTranslation;
                    glm::vec3 deltaRotation = component.Rotation - oldRotation;
                    glm::vec3 deltaScale = component.Scale - oldScale;

                    for (auto otherEntity : m_SelectionContexts)
                    {
                        if (!otherEntity || otherEntity == entity)
                            continue;

                        // If otherEntity's parent is also selected, skip it (its parent will move it)
                        if (otherEntity.HasComponent<RelationshipComponent>())
                        {
                            UUID parentUUID = otherEntity.GetComponent<RelationshipComponent>().Parent;
                            if (parentUUID != 0)
                            {
                                Entity parent = m_Context->GetEntityByUUID(parentUUID);
                                if (parent && IsSelected(parent))
                                    continue;
                            }
                        }

                        if (otherEntity.HasComponent<TransformComponent>())
                        {
                            auto& otherTc = otherEntity.GetComponent<TransformComponent>();

                            if (posModified)
                                otherTc.Translation += deltaTranslation;

                            if (rotModified)
                                otherTc.Rotation += deltaRotation;

                            if (scaleModified)
                                otherTc.Scale += deltaScale;

                            otherEntity.MarkTransformDirty();
                        }
                    }
                }
            }
        });

        DrawComponent<ModelInstanceComponent>("Model Instance", entity, [this, entity](auto& component)
        {
            // Removing this component unpacks the instance: its nodes stay and are saved as ordinary entities.
            std::string label = "None";
            if (AssetManager::IsAssetHandleValid(component.Model))
                label = Project::GetActive()->GetEditorAssetManager()->GetMetadata(component.Model).FilePath.filename().string();
            ImGui::Text("Model: %s", label.c_str());
            if (!component.Spawned)
                ImGui::TextDisabled("Loading...");

            if (!component.RemovedNodes.empty())
            {
                ImGui::Text("Removed nodes: %zu", component.RemovedNodes.size());
                if (ImGui::Button("Restore Removed Nodes"))
                    ModelInstance::Respawn(*m_Context, entity);
            }
        });

        DrawComponent<MeshComponent>("Mesh", entity, [entity](auto& component)
        {
            std::string label = "None";
            bool isMeshValid = false;
            bool changed = false;

            // 1. Resolve the current mesh name if one is assigned
            if (component.Mesh != 0)
            {
                if (AssetManager::IsAssetHandleValid(component.Mesh))
                {
                    // Get the type and allow Source files OR loaded meshes
                    AssetType type = AssetManager::GetAssetType(component.Mesh);
                    if (type == AssetType::MeshSource || type == AssetType::StaticMesh || type == AssetType::Mesh)
                    {
                        const AssetMetadata& metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(component.Mesh);
                        label = metadata.FilePath.filename().string();
                        isMeshValid = true;
                    }
                    else
                    {
                        label = "Invalid";
                    }
                }
                else
                {
                    label = "Invalid";
                }
            }

            // 2. Draw the button that acts as our Drag & Drop target
            ImVec2 buttonLabelSize = ImGui::CalcTextSize(label.c_str());
            buttonLabelSize.x += 20.0f;
            float buttonLabelWidth = glm::max<float>(100.0f, buttonLabelSize.x);

            ImGui::Button(label.c_str(), ImVec2(buttonLabelWidth, 0.0f));

            // 3. Accept the payload from the Content Browser
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                {
                    AssetHandle handle = *(AssetHandle*)payload->Data;

                    // Verify it's a mesh type
                    AssetType type = AssetManager::GetAssetType(handle);
                    if (type == AssetType::MeshSource || type == AssetType::StaticMesh || type == AssetType::Mesh)
                    {
                        component.Mesh = handle;
                        component.SubmeshIndex = 0;
                        changed = true;
                    }
                    else
                    {
                        NOX_CORE_WARN("Wrong Asset Type - Expected a Mesh");
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // 4. Draw a clear "X" button to remove the mesh
            if (isMeshValid)
            {
                ImGui::SameLine();
                ImVec2 xLabelSize = ImGui::CalcTextSize("X");
                float buttonSize = xLabelSize.y + ImGui::GetStyle().FramePadding.y * 2.0f;
                if (ImGui::Button("X", ImVec2(buttonSize, buttonSize)))
                {
                    component.Mesh = 0;
                    changed = true;
                }

                changed |= ImGui::DragScalar("Submesh Index", ImGuiDataType_U32, &component.SubmeshIndex, 0.1f, nullptr, nullptr, "%u");
                changed |= ImGui::DragScalar("Submesh Count", ImGuiDataType_U32, &component.SubmeshCount, 0.1f, nullptr, nullptr, "%u");
            }

            ImGui::SameLine();
            ImGui::Text("Mesh Asset");

            // The renderer's GPU scene re-registers the entity.
            if (changed)
                entity.PatchComponent<MeshComponent>();
        });

        DrawComponent<MaterialComponent>("Material", entity, [this, entity](auto& component)
        {
            // Per-slot overrides of the mesh's materials (UE's OverrideMaterials): a zero handle, or no entry, keeps the
            // mesh's own .nmat, and only real overrides are stored. The entries past the last override are dropped.
            auto setOverride = [&](size_t slot, AssetHandle material)
            {
                if (component.MaterialAssets.size() <= slot)
                    component.MaterialAssets.resize(slot + 1, AssetHandle(0));
                component.MaterialAssets[slot] = material;
                while (!component.MaterialAssets.empty() && component.MaterialAssets.back() == 0)
                    component.MaterialAssets.pop_back();
                entity.PatchComponent<MaterialComponent>();
            };

            // The slots are the mesh's (only this entity's submeshes unless all are shown); an entity without a loaded
            // mesh lists its own materials. (Local copy: `entity` is const in this lambda.)
            Entity e = entity;
            const std::vector<AssetHandle>* meshMaterials = nullptr;
            size_t firstSlot = 0;
            size_t slotEnd = component.MaterialAssets.size();
            if (e.HasComponent<MeshComponent>())
            {
                const MeshComponent& meshComponent = e.GetComponent<MeshComponent>();
                const AssetType meshType = AssetManager::GetAssetType(meshComponent.Mesh);
                if (meshType == AssetType::StaticMesh)
                {
                    if (const StaticMesh* mesh = AssetManager::FindLoadedAsset<StaticMesh>(meshComponent.Mesh))
                        meshMaterials = &mesh->GetMaterialAssets();
                }
                else if (meshType == AssetType::Mesh || meshType == AssetType::MeshSource)
                {
                    if (const Mesh* mesh = AssetManager::FindLoadedAsset<Mesh>(meshComponent.Mesh))
                        meshMaterials = &mesh->GetMaterialAssets();
                }

                if (meshMaterials)
                {
                    slotEnd = meshMaterials->size();
                    if (!m_MaterialShowAll)
                    {
                        firstSlot = std::min<size_t>(meshComponent.SubmeshIndex, slotEnd);
                        if (meshComponent.SubmeshCount != UINT32_MAX)
                            slotEnd = std::min<size_t>(firstSlot + std::max(meshComponent.SubmeshCount, 1u), slotEnd);
                    }
                }
            }

            if (!meshMaterials && component.MaterialAssets.empty())
            {
                ImGui::TextDisabled("Drop a material asset here");
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                    {
                        AssetHandle handle = *(const AssetHandle*)payload->Data;
                        if (AssetManager::IsAssetHandleValid(handle) &&
                            AssetManager::GetAssetType(handle) == AssetType::Material)
                        {
                            setOverride(0, handle);
                        }
                        else
                        {
                            NOX_CORE_WARN("Wrong Asset Type - Expected a Material (.nmat)");
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                return;
            }

            if (meshMaterials)
            {
                ImGui::Text("Material slots %zu-%zu of %zu", firstSlot, slotEnd > 0 ? slotEnd - 1 : 0, meshMaterials->size());
                if (ImGui::Button(m_MaterialShowAll ? "Show Only This Entity's Slots" : "Show All Slots"))
                    m_MaterialShowAll = !m_MaterialShowAll;
            }
            else
            {
                ImGui::Text("Material Assets (%zu)", component.MaterialAssets.size());
            }

            for (size_t i = firstSlot; i < slotEnd; ++i)
            {
                const AssetHandle overrideHandle = i < component.MaterialAssets.size() ? component.MaterialAssets[i] : AssetHandle(0);
                AssetHandle handle = overrideHandle;
                if (handle == 0 && meshMaterials && i < meshMaterials->size())
                    handle = (*meshMaterials)[i];
                const bool overridden = meshMaterials && overrideHandle != 0;

                std::string label = "None";
                if (handle != 0 && AssetManager::IsAssetHandleValid(handle) &&
                    AssetManager::GetAssetType(handle) == AssetType::Material)
                {
                    const auto& metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
                    label = metadata.FilePath.filename().string();
                }
                if (overridden)
                    label += " (override)";

                if (slotEnd - firstSlot == 1)
                    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                if (ImGui::TreeNode((void*)(uintptr_t)i, "%zu: %s", i, label.c_str()))
                {
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                        {
                            AssetHandle droppedHandle = *(const AssetHandle*)payload->Data;
                            if (AssetManager::IsAssetHandleValid(droppedHandle) &&
                                AssetManager::GetAssetType(droppedHandle) == AssetType::Material)
                            {
                                setOverride(i, droppedHandle);
                            }
                            else
                            {
                                NOX_CORE_WARN("Wrong Asset Type - Expected a Material (.nmat)");
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    if (overridden && ImGui::Button(("Use Mesh Material##" + std::to_string(i)).c_str()))
                        setOverride(i, 0);

                    if (handle != 0 && AssetManager::IsAssetHandleValid(handle) &&
                        AssetManager::GetAssetType(handle) == AssetType::Material)
                    {
                        Ref<Material> material = AssetManager::GetAsset<Material>(handle);
                        if (material)
                        {
                            MaterialData& data = material->GetData();
                            if (ImGui::Button(("Make Unique##" + std::to_string(i)).c_str()))
                            {
                                auto manager = Project::GetActive()->GetEditorAssetManager();
                                const auto& metadata = manager->GetMetadata(handle);
                                std::filesystem::path uniquePath = metadata.FilePath.parent_path() /
                                (metadata.FilePath.stem().string() + "_Instance_" +
                                    std::to_string(static_cast<uint64_t>(AssetHandle())) + ".nmat");
                                if (MaterialSerializer::Serialize(
                                    Project::GetActiveAssetDirectory() / uniquePath, data))
                                {
                                    manager->ImportAsset(uniquePath, uniquePath, AssetType::Material);
                                    for (const auto& [uniqueHandle, uniqueMetadata] : manager->GetAssetRegistry())
                                    {
                                        if (uniqueMetadata.Type == AssetType::Material &&
                                            uniqueMetadata.FilePath == uniquePath)
                                        {
                                            setOverride(i, uniqueHandle);
                                            break;
                                        }
                                    }
                                }
                            }

                            bool changed = false;
                            // The renderer shades with the factors of the material's workflow only (PackMaterial):
                            // specular-glossiness materials (e.g. Bistro) use Diffuse/Specular/Glossiness.
                            const bool specularGlossiness = data.Workflow == 1.0f;
                            if (specularGlossiness)
                            {
                                ImGui::TextDisabled("Workflow: Specular-Glossiness");
                                changed |= ImGui::ColorEdit4("Diffuse", glm::value_ptr(data.DiffuseFactor));
                                changed |= ImGui::ColorEdit3("Specular", glm::value_ptr(data.SpecularFactor));
                                changed |= ImGui::DragFloat("Glossiness", &data.SpecularFactor.a, 0.01f, 0.0f, 1.0f);
                            }
                            else
                            {
                                ImGui::TextDisabled("Workflow: Metallic-Roughness");
                                changed |= ImGui::ColorEdit4("Base Color", glm::value_ptr(data.BaseColorFactor));
                                changed |= ImGui::DragFloat("Metallic", &data.MetallicFactor, 0.01f, 0.0f, 1.0f);
                                changed |= ImGui::DragFloat("Roughness", &data.RoughnessFactor, 0.01f, 0.0f, 1.0f);
                            }
                            changed |= ImGui::ColorEdit3("Emissive", glm::value_ptr(data.EmissiveFactor));
                            changed |= ImGui::DragFloat("Emissive Strength", &data.emissiveStrength, 0.01f, 0.0f, 100.0f);
                            changed |= ImGui::DragFloat("Transmission", &data.TransmissionFactor, 0.01f, 0.0f, 1.0f);
                            changed |= ImGui::DragFloat("IOR", &data.IOR, 0.01f, 1.0f, 3.0f);
                            changed |= ImGui::DragFloat("Thickness", &data.Thickness, 0.01f, 0.0f, 10.0f);

                            auto drawTextureReference = [&](const char* labelName,
                                                            const char* id,
                                                            std::string& texturePath)
                            {
                                std::string label = texturePath.empty()
                                                        ? "None"
                                                        : std::filesystem::path(texturePath).filename().string();
                                ImGui::Text("%s", labelName);
                                ImGui::SameLine();
                                ImGui::Button((label + "##" + id).c_str(), ImVec2(150.0f, 0.0f));

                                if (ImGui::BeginDragDropTarget())
                                {
                                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                                    {
                                        AssetHandle textureHandle = *(const AssetHandle*)payload->Data;
                                        if (AssetManager::GetAssetType(textureHandle) == AssetType::Texture2D)
                                        {
                                            const auto& textureMetadata =
                                                Project::GetActive()->GetEditorAssetManager()->GetMetadata(textureHandle);
                                            texturePath = textureMetadata.SourceFilePath.empty()
                                                              ? textureMetadata.FilePath.generic_string()
                                                              : textureMetadata.SourceFilePath.generic_string();
                                            changed = true;
                                        }
                                        else
                                        {
                                            NOX_CORE_WARN("Wrong Asset Type - Expected a Texture (this slot takes a texture, not a material)");
                                        }
                                    }
                                    ImGui::EndDragDropTarget();
                                }
                            };

                            drawTextureReference(specularGlossiness ? "Diffuse Texture" : "Base Color Texture", "BaseColor", data.BaseColorTexturePath);
                            drawTextureReference(specularGlossiness ? "Specular Glossiness" : "Metallic Roughness", "MetallicRoughness", data.MetallicRoughnessTexturePath);
                            drawTextureReference("Normal Texture", "Normal", data.NormalTexturePath);
                            drawTextureReference("Occlusion Texture", "Occlusion", data.OcclusionTexturePath);
                            drawTextureReference("Emissive Texture", "Emissive", data.EmissiveTexturePath);
                            drawTextureReference("Transmission Texture", "Transmission", data.TransmissionTexturePath);

                            int alphaMode = static_cast<int>(data.Mode);
                            const char* alphaModes[] = {"Opaque", "Mask", "Blend"};
                            if (ImGui::Combo("Alpha Mode", &alphaMode, alphaModes, 3))
                            {
                                data.Mode = static_cast<AlphaMode>(alphaMode);
                                changed = true;
                            }
                            if (data.Mode == AlphaMode::Mask)
                                changed |= ImGui::DragFloat("Alpha Cutoff", &data.AlphaMaskCutoff, 0.005f, 0.0f, 1.0f);
                            changed |= ImGui::Checkbox("Double Sided", &data.DoubleSided);
                            changed |= ImGui::Checkbox("Unlit", &data.Unlit);

                            if (changed)
                            {
                                // Every instance using this material shades with the edit from this frame.
                                Renderer::MarkMaterialChanged(handle);
                                const auto& metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
                                MaterialSerializer::Serialize(
                                    Project::GetActiveAssetDirectory() / metadata.FilePath,
                                    data
                                );
                            }
                        }
                    }
                    ImGui::TreePop();
                }
            }
        });

        DrawComponent<DirectionalLightComponent>("Directional Light", entity, [](auto& component)
        {
            ImGui::ColorEdit3("Color", glm::value_ptr(component.Color));
            ImGui::DragFloat("Intensity", &component.Intensity, 0.1f, 0.0f, 100.0f);
            ImGui::DragFloat("Angular Diameter", &component.AngularDiameter, 0.05f, 0.0f, 20.0f, "%.2f deg");
            uint32_t minSamples = 1, maxSamples = 16;
            ImGui::DragScalar("Shadow Samples", ImGuiDataType_U32, &component.ShadowSamples, 0.1f, &minSamples, &maxSamples);
        });

        DrawComponent<PointLightComponent>("Point Light", entity, [](auto& component)
        {
            ImGui::ColorEdit3("Color", glm::value_ptr(component.Color));
            ImGui::DragFloat("Intensity", &component.Intensity, 0.5f, 0.0f, 1000.0f);
            ImGui::DragFloat("Range", &component.Range, 0.5f, 0.1f, 1000.0f);
            ImGui::DragFloat("Source Radius", &component.Radius, 0.01f, 0.0f, 10.0f, "%.2f m");
            uint32_t minSamples = 1, maxSamples = 16;
            ImGui::DragScalar("Shadow Samples", ImGuiDataType_U32, &component.ShadowSamples, 0.1f, &minSamples, &maxSamples);
        });

        DrawComponent<SpotLightComponent>("Spot Light", entity, [](auto& component)
        {
            ImGui::ColorEdit3("Color", glm::value_ptr(component.Color));
            ImGui::DragFloat("Intensity", &component.Intensity, 0.5f, 0.0f, 1000.0f);
            ImGui::DragFloat("Range", &component.Range, 0.5f, 0.1f, 1000.0f);
            ImGui::DragFloat("Inner Angle", &component.InnerAngle, 0.5f, 0.0f, component.OuterAngle);
            ImGui::DragFloat("Outer Angle", &component.OuterAngle, 0.5f, component.InnerAngle, 89.0f);
            ImGui::DragFloat("Source Radius", &component.Radius, 0.01f, 0.0f, 10.0f, "%.2f m");
            uint32_t minSamples = 1, maxSamples = 16;
            ImGui::DragScalar("Shadow Samples", ImGuiDataType_U32, &component.ShadowSamples, 0.1f, &minSamples, &maxSamples);
        });

        DrawComponent<EnvironmentLightComponent>("Environment Light", entity, [](auto& component)
        {
            ImGui::Checkbox("Enabled", &component.Enabled);
            ImGui::DragFloat3("Radiance Scale", glm::value_ptr(component.RadianceScale), 0.01f, 0.0f, 100.0f);
            ImGui::DragFloat("Rotation", &component.Rotation, 0.01f, -glm::radians(180.0f), glm::radians(180.0f), "%.3f rad");
            auto manager = Project::GetActive()->GetEditorAssetManager();
            const auto& registry = manager->GetAssetRegistry();
            std::string selectedEnvironment = component.TexturePath.empty() ? "No environment selected" : component.TexturePath;
            if (ImGui::BeginCombo("Environment", selectedEnvironment.c_str()))
            {
                if (ImGui::Selectable("None", component.TexturePath.empty()))
                    component.TexturePath.clear();
                for (const auto& [handle, metadata] : registry)
                {
                    if (metadata.Type != AssetType::Texture2D)
                        continue;
                    const auto source = metadata.SourceFilePath.empty() ? metadata.FilePath : metadata.SourceFilePath;
                    const auto extension = source.extension().string();
                    if (extension != ".hdr" && extension != ".dds")
                        continue;
                    const auto normalizedSource = source.lexically_normal();
                    if (normalizedSource.empty() || normalizedSource.begin()->generic_string() != "EnvironmentMaps")
                        continue;
                    const std::string path = source.generic_string();
                    if (ImGui::Selectable(path.c_str(), component.TexturePath == path))
                        component.TexturePath = path;
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("Import HDR / DDS Environment"))
            {
                static constexpr char filter[] = "Environment maps\0hdr;dds\0";
                const std::string selectedFile = Utility::OpenFile(filter);
                if (!selectedFile.empty())
                {
                    const std::filesystem::path sourcePath = selectedFile;
                    const std::filesystem::path relativePath = std::filesystem::path("EnvironmentMaps") / sourcePath.filename();
                    const std::filesystem::path destination = Project::GetActiveAssetDirectory() / relativePath;
                    std::error_code error;
                    std::filesystem::create_directories(destination.parent_path(), error);
                    std::filesystem::copy_file(sourcePath, destination,
                        std::filesystem::copy_options::overwrite_existing, error);
                    if (!error)
                    {
                        manager->ImportAsset(relativePath, relativePath, AssetType::Texture2D);
                        component.TexturePath = relativePath.generic_string();
                    }
                    else
                    {
                        NOX_CORE_ERROR("Environment import failed: {}", error.message());
                    }
                }
            }
        });

        DrawComponent<AnimatorComponent>("Animator", entity, [this](auto& component)
        {
            Ref<AnimationSequence> currentAnim = component.Animator.GetCurrentAnimation();

            // Auto-resolve animation if assigned on the component but not yet loaded into the Animator
            if (!currentAnim && component.Animation != 0)
            {
                Ref<AnimationSequence> anim = AssetManager::GetAsset<AnimationSequence>(component.Animation);
                if (anim)
                {
                    anim->Handle = component.Animation;
                    component.Animator.PlayAnimation(anim);
                    if (!component.Playing)
                        component.Animator.Pause();
                    currentAnim = component.Animator.GetCurrentAnimation();
                }
            }

            // --- Media Control Buttons ---
            bool isPlaying = component.Animator.IsPlaying();

            if (isPlaying)
            {
                if (ImGui::Button("Pause", ImVec2(80.0f, 0.0f)))
                {
                    component.Animator.Pause();
                    component.Playing = false;
                }
            }
            else
            {
                if (ImGui::Button("Play", ImVec2(80.0f, 0.0f)))
                {
                    // Reset to start if at the end and not looping
                    if (!component.Animator.IsLooping() && currentAnim)
                    {
                        if (component.Animator.GetCurrentAnimationTime() >= currentAnim->Duration)
                        {
                            component.Animator.SetCurrentTime(0.0f);
                        }
                    }
                    component.Animator.Resume();
                    component.Playing = true;
                }
            }

            ImGui::SameLine();
            if (ImGui::Button("Stop", ImVec2(80.0f, 0.0f)))
            {
                component.Animator.Stop();
                component.Playing = false;
            }

            ImGui::Spacing();

            // --- Looping & Playback Speed ---
            bool isLooping = component.Animator.IsLooping();
            if (ImGui::Checkbox("Looping", &isLooping))
            {
                component.Animator.SetLooping(isLooping);
            }

            float speed = component.Animator.GetPlaybackSpeed();
            if (ImGui::DragFloat("Playback Speed", &speed, 0.05f, 0.0f, 10.0f))
            {
                component.Animator.SetPlaybackSpeed(speed);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // --- Animation Selection Dropdown ---
            auto assetManager = Project::GetActive()->GetEditorAssetManager();
            const auto& registry = assetManager->GetAssetRegistry();

            std::string currentAnimName = "None (Select Animation)";
            if (currentAnim)
            {
                if (registry.contains(currentAnim->Handle))
                    currentAnimName = registry.at(currentAnim->Handle).FilePath.stem().string();
                else if (!currentAnim->Name.empty())
                    currentAnimName = currentAnim->Name;
                else
                    currentAnimName = "Selected Animation";
            }
            else if (component.Animation != 0 && registry.contains(component.Animation))
            {
                currentAnimName = registry.at(component.Animation).FilePath.stem().string();
            }

            if (ImGui::BeginCombo("Animation Clip", currentAnimName.c_str()))
            {
                bool isNoneSelected = (currentAnim == nullptr && component.Animation == 0);
                if (ImGui::Selectable("None", isNoneSelected))
                {
                    component.Animation = 0;
                    component.Playing = false;
                    component.Animator.Stop();
                    component.Animator.PlayAnimation(nullptr);
                }
                if (isNoneSelected)
                    ImGui::SetItemDefaultFocus();

                for (const auto& [handle, metadata] : registry)
                {
                    if (metadata.Type == AssetType::AnimationSequence)
                    {
                        std::string animName = metadata.FilePath.stem().string();
                        bool isSelected = (currentAnim && (currentAnim->Handle == handle || component.Animation == handle));

                        if (ImGui::Selectable(animName.c_str(), isSelected))
                        {
                            Ref<AnimationSequence> anim = AssetManager::GetAsset<AnimationSequence>(handle);
                            if (anim)
                            {
                                anim->Handle = handle;
                                component.Animation = handle;
                                component.Playing = true;
                                component.Animator.PlayAnimation(anim);
                            }
                        }

                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            // Drag and drop support from Content Browser onto the Animation Clip field
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                {
                    AssetHandle handle = *(AssetHandle*)payload->Data;
                    if (AssetManager::GetAssetType(handle) == AssetType::AnimationSequence)
                    {
                        Ref<AnimationSequence> anim = AssetManager::GetAsset<AnimationSequence>(handle);
                        if (anim)
                        {
                            anim->Handle = handle;
                            component.Animation = handle;
                            component.Playing = true;
                            component.Animator.PlayAnimation(anim);
                        }
                    }
                    else
                    {
                        NOX_CORE_WARN("Wrong Asset Type - Expected an AnimationSequence");
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // --- Timeline Slider ---
            float currentTime = component.Animator.GetCurrentAnimationTime();
            float maxDuration = currentAnim ? currentAnim->Duration : 100.0f;

            if (ImGui::SliderFloat("Time (Ticks)", &currentTime, 0.0f, maxDuration, "%.2f"))
            {
                component.Animator.SetCurrentTime(currentTime);
            }

            // Graph mode (docs/Animation_Graph_Architecture_Plan_2026.md): drives the same Skeleton/NodeEntities
            // above through a compiled .nanimgraph instead of the single clip. The node-canvas editor (Step 3/4
            // of that plan) is a separate window that edits the .nanimgraph asset itself, opened by double-
            // clicking the Graph field once that exists; this is the permanent parameter-tweaking counterpart.
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Animation Graph");

            std::string currentGraphName = "None";
            if (component.Graph != 0 && registry.contains(component.Graph))
                currentGraphName = registry.at(component.Graph).FilePath.stem().string();

            const bool graphComboOpen = ImGui::BeginCombo("Graph", currentGraphName.c_str());
            // Double-clicking the reference opens its editor window (checked right after BeginCombo, while the
            // combo is still the last item -- inside the popup body it no longer is).
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                component.Graph != 0 && m_OpenAsset)
                m_OpenAsset(component.Graph);
            if (graphComboOpen)
            {
                if (ImGui::Selectable("None", component.Graph == 0))
                    component.Graph = 0;

                for (const auto& [handle, metadata] : registry)
                {
                    if (metadata.Type != AssetType::AnimationGraph)
                        continue;
                    std::string name = metadata.FilePath.stem().string();
                    if (ImGui::Selectable(name.c_str(), component.Graph == handle))
                        component.Graph = handle;
                }
                ImGui::EndCombo();
            }

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                {
                    AssetHandle dropped = *(AssetHandle*)payload->Data;
                    if (AssetManager::GetAssetType(dropped) == AssetType::AnimationGraph)
                        component.Graph = dropped;
                    else
                        NOX_CORE_WARN("Wrong Asset Type - Expected an AnimationGraph");
                }
                ImGui::EndDragDropTarget();
            }

            if (component.Graph != 0)
            {
                ImGui::Text("Parameters");
                for (auto& [name, value] : component.GraphInstance.Parameters)
                {
                    if (float* f = std::get_if<float>(&value))
                        ImGui::DragFloat(name.c_str(), f, 0.01f); // not clamped: a parameter can be a speed, not just a 0-1 blend
                    else if (bool* b = std::get_if<bool>(&value))
                        ImGui::Checkbox(name.c_str(), b);
                    else if (int32_t* i = std::get_if<int32_t>(&value))
                        ImGui::DragInt(name.c_str(), i);
                }
            }
        });

        DrawComponent<CameraComponent>("Camera", entity, [](auto& component)
        {
            auto& camera = component.Camera;

            ImGui::Checkbox("Primary", &component.Primary);
            ImGui::Checkbox("Auto Exposure", &component.AutoExposure);
            ImGui::DragFloat("Exposure Compensation", &component.ExposureCompensation, 0.05f, -12.0f, 12.0f);
            if (component.AutoExposure)
            {
                ImGui::DragFloat("Auto Exposure Min EV", &component.AutoExposureMinEV, 0.1f, -16.0f, component.AutoExposureMaxEV);
                ImGui::DragFloat("Auto Exposure Max EV", &component.AutoExposureMaxEV, 0.1f, component.AutoExposureMinEV, 16.0f);
            }

            const char* projectionTypesStrings[] = {"Perspective", "Orthographic"};
            const char* currentProjectionTypeString = projectionTypesStrings[(int)camera.GetProjectionType()];
            if (ImGui::BeginCombo("Projection", currentProjectionTypeString))
            {
                for (int i = 0; i < 2; i++)
                {
                    bool isSelected = currentProjectionTypeString == projectionTypesStrings[i];
                    if (ImGui::Selectable(projectionTypesStrings[i], isSelected))
                    {
                        currentProjectionTypeString = projectionTypesStrings[i];
                        camera.SetProjectionType((SceneCamera::ProjectionType)i);
                    }

                    if (isSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }

                ImGui::EndCombo();
            }

            if (camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
            {
                float verticalFov = glm::degrees(camera.GetPerspectiveVerticalFOV());
                if (ImGui::DragFloat("Vertical FOV", &verticalFov))
                {
                    camera.SetPerspectiveVerticalFOV(glm::radians(verticalFov));
                }

                float orthoNear = camera.GetPerspectiveNearClip();
                if (ImGui::DragFloat("Near", &orthoNear))
                {
                    camera.SetPerspectiveNearClip(orthoNear);
                }

                float orthoFar = camera.GetPerspectiveFarClip();
                if (ImGui::DragFloat("Far", &orthoFar))
                {
                    camera.SetPerspectiveFarClip(orthoFar);
                }
            }

            if (camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic)
            {
                float orthoSize = camera.GetOrthographicSize();
                if (ImGui::DragFloat("Size", &orthoSize))
                {
                    camera.SetOrthographicSize(orthoSize);
                }

                float orthoNear = camera.GetOrthographicNearClip();
                if (ImGui::DragFloat("Near", &orthoNear))
                {
                    camera.SetOrthographicNearClip(orthoNear);
                }

                float orthoFar = camera.GetOrthographicFarClip();
                if (ImGui::DragFloat("Far", &orthoFar))
                {
                    camera.SetOrthographicFarClip(orthoFar);
                }
                ImGui::Checkbox("Fixed Aspect Ratio", &component.FixedAspectRatio);
            }
        });

        DrawComponent<ScriptComponent>("Script", entity, [this, entity](auto& component)
        {
            for (size_t index = 0; index < component.ClassNames.size(); ++index)
            {
                ImGui::PushID(static_cast<int>(index));
                ImGui::SetNextItemWidth(-32.0f);
                ImGui::InputText("##Class", &component.ClassNames[index]);
                ImGui::SameLine();
                const bool remove = ImGui::Button("-");

                const bool scriptClassExists =
                    ScriptEngine::EntityClassExists(component.ClassNames[index]);
                if (!component.ClassNames[index].empty() && !scriptClassExists)
                    ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.3f, 1.0f), "Class not found");

                if (remove)
                {
                    component.EntityReferences.erase(component.ClassNames[index]);
                    component.FieldOverrides.erase(component.ClassNames[index]);
                    component.ClassNames.erase(component.ClassNames.begin() + index);
                    ImGui::PopID();
                    break;
                }

                if (scriptClassExists)
                {
                    const std::string& className = component.ClassNames[index];
                    for (const ScriptFieldInfo& field : ScriptEngine::GetExposedFields(className))
                    {
                        const std::string& fieldName = field.Name;
                        if (field.Type != ScriptFieldType::Entity)
                        {
                            auto& overrides = component.FieldOverrides[className];
                            ScriptValue value = overrides.contains(fieldName) ? overrides[fieldName] : field.DefaultValue;
                            bool changed = false;
                            switch (field.Type)
                            {
                                case ScriptFieldType::Bool: changed = ImGui::Checkbox(fieldName.c_str(), &std::get<bool>(value)); break;
                                case ScriptFieldType::Int: changed = ImGui::DragScalar(fieldName.c_str(), ImGuiDataType_S32, &std::get<int32_t>(value), 1.0f); break;
                                case ScriptFieldType::UInt: changed = ImGui::DragScalar(fieldName.c_str(), ImGuiDataType_U32, &std::get<uint32_t>(value), 1.0f); break;
                                case ScriptFieldType::Long: changed = ImGui::DragScalar(fieldName.c_str(), ImGuiDataType_S64, &std::get<int64_t>(value), 1.0f); break;
                                case ScriptFieldType::ULong: changed = ImGui::DragScalar(fieldName.c_str(), ImGuiDataType_U64, &std::get<uint64_t>(value), 1.0f); break;
                                case ScriptFieldType::Float: changed = ImGui::DragFloat(fieldName.c_str(), &std::get<float>(value), 0.1f); break;
                                case ScriptFieldType::Double: changed = ImGui::DragScalar(fieldName.c_str(), ImGuiDataType_Double, &std::get<double>(value), 0.1f); break;
                                case ScriptFieldType::String: changed = ImGui::InputText(fieldName.c_str(), &std::get<std::string>(value)); break;
                                case ScriptFieldType::Vector3: changed = ImGui::DragFloat3(fieldName.c_str(), glm::value_ptr(std::get<glm::vec3>(value)), 0.1f); break;
                                case ScriptFieldType::Entity: break;
                            }
                            if (changed) overrides[fieldName] = std::move(value);
                            continue;
                        }

                        auto& reference = component.EntityReferences[className][fieldName];
                        Entity referencedEntity;
                        if (reference.IsModelNode())
                            referencedEntity = m_Context->GetEntityByUUID(
                                ModelInstance::NodeUUID(reference.ModelInstance, reference.ModelNodeIndex));
                        else if (reference.Entity != 0)
                            referencedEntity = m_Context->GetEntityByUUID(reference.Entity);

                        const bool invalidSelfReference = field.DisallowSelf && referencedEntity == entity;
                        const std::string preview = invalidSelfReference
                            ? "Self (not allowed)"
                            : (referencedEntity ? referencedEntity.GetName() : "None");
                        ImGui::TextUnformatted(fieldName.c_str());
                        ImGui::SameLine(110.0f);
                        ImGui::SetNextItemWidth(-55.0f);
                        if (ImGui::BeginCombo(("##" + fieldName).c_str(), preview.c_str()))
                        {
                            for (auto handle : m_Context->GetAllEntitiesWith<TagComponent>())
                            {
                                Entity candidate(handle, m_Context.get());
                                ImGui::PushID(static_cast<int>(static_cast<uint32_t>(handle)));
                                const bool rejectCandidate = field.DisallowSelf && candidate == entity;
                                if (rejectCandidate) ImGui::BeginDisabled();
                                if (ImGui::Selectable(candidate.GetName().c_str(), candidate == referencedEntity))
                                {
                                    reference = {};
                                    if (candidate.HasComponent<ModelNodeComponent>())
                                    {
                                        const auto& node = candidate.GetComponent<ModelNodeComponent>();
                                        reference.ModelInstance = node.Instance;
                                        reference.ModelNodeIndex = node.NodeIndex;
                                    }
                                    else
                                    {
                                        reference.Entity = candidate.GetUUID();
                                    }
                                }
                                if (rejectCandidate) ImGui::EndDisabled();
                                ImGui::PopID();
                            }
                            ImGui::EndCombo();
                        }

                        if (ImGui::BeginDragDropTarget())
                        {
                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_HIERARCHY_ENTITY"))
                            {
                                Entity candidate = m_Context->GetEntityByUUID(*static_cast<const UUID*>(payload->Data));
                                if (field.DisallowSelf && candidate == entity)
                                {
                                    ImGui::EndDragDropTarget();
                                    continue;
                                }
                                reference = {};
                                if (candidate && candidate.HasComponent<ModelNodeComponent>())
                                {
                                    const auto& node = candidate.GetComponent<ModelNodeComponent>();
                                    reference.ModelInstance = node.Instance;
                                    reference.ModelNodeIndex = node.NodeIndex;
                                }
                                else if (candidate)
                                {
                                    reference.Entity = candidate.GetUUID();
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }

                        if (invalidSelfReference)
                            ImGui::TextColored({0.9f, 0.2f, 0.3f, 1.0f}, "Target cannot reference this entity");

                        ImGui::SameLine();
                        if (ImGui::SmallButton(("X##" + fieldName).c_str()))
                            reference = {};
                    }
                }
                ImGui::PopID();
            }

            if (ImGui::Button("Add Script"))
                component.ClassNames.emplace_back();
            ImGui::TextDisabled("Example: Facerun.WASDMovement");

            // Fields
            /* Field metadata/inspection is tracked in the scripting roadmap.
            bool sceneRunning = scene->IsRunning();
            if (sceneRunning)
            {
                Ref<ScriptInstance> scriptInstance = ScriptEngine::GetEntityScriptInstance(entity.GetUUID());
                if (scriptInstance)
                {
                    const auto& fields = scriptInstance->GetScriptClass()->GetFields();
                    for (const auto& [name, field] : fields)
                    {
                        if (field.Type == ScriptFieldType::Float)
                        {
                            float data = scriptInstance->GetFieldValue<float>(name);
                            if (ImGui::DragFloat(name.c_str(), &data))
                            {
                                scriptInstance->SetFieldValue(name, data);
                            }
                        }
                    }
                }
            }
            else
            {
                if (scriptClassExists)
                {
                    Ref<ScriptClass> entityClass = ScriptEngine::GetEntityClass(component.ClassName);
                    const auto& fields = entityClass->GetFields();

                    auto& entityFields = ScriptEngine::GetScriptFieldMap(entity);
                    for (const auto& [name, field] : fields)
                    {
                        // Field has been set in editor
                        if (entityFields.find(name) != entityFields.end())
                        {
                            ScriptFieldInstance& scriptField = entityFields.at(name);

                            // Display control to set it maybe
                            if (field.Type == ScriptFieldType::Float)
                            {
                                float data = scriptField.GetValue<float>();
                                if (ImGui::DragFloat(name.c_str(), &data))
                                    scriptField.SetValue(data);
                            }
                        }
                        else
                        {
                            // Display control to set it maybe
                            if (field.Type == ScriptFieldType::Float)
                            {
                                float data = 0.0f;
                                if (ImGui::DragFloat(name.c_str(), &data))
                                {
                                    ScriptFieldInstance& fieldInstance = entityFields[name];
                                    fieldInstance.Field = field;
                                    fieldInstance.SetValue(data);
                                }
                            }
                        }
                    }
                }
            } */
        });

        DrawComponent<SpriteRendererComponent>("Sprite Renderer", entity, [](auto& component)
        {
            ImGui::ColorEdit4("Color", glm::value_ptr(component.Color));

            std::string label = "None";
            bool isTextureValid = false;
            if (component.Texture != 0)
            {
                if (AssetManager::IsAssetHandleValid(component.Texture) && AssetManager::GetAssetType(component.Texture) == AssetType::Texture2D)
                {
                    const AssetMetadata& metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(component.Texture);
                    label = metadata.FilePath.filename().string();
                    isTextureValid = true;
                }
                else
                {
                    label = "Invalid";
                }
            }
            ImVec2 buttonLabelSize = ImGui::CalcTextSize(label.c_str());
            buttonLabelSize.x += 20.0f;
            float buttonLabelWidth = glm::max<float>(100.0f, buttonLabelSize.x);

            ImGui::Button(label.c_str(), ImVec2(buttonLabelWidth, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                {
                    AssetHandle handle = *(AssetHandle*)payload->Data;
                    if (AssetManager::GetAssetType(handle) == AssetType::Texture2D)
                    {
                        component.Texture = handle;
                    }
                    else
                    {
                        NOX_CORE_WARN("Wrong Asset Type");
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (isTextureValid)
            {
                ImGui::SameLine();
                ImVec2 xLabelSize = ImGui::CalcTextSize("X");
                float buttonSize = xLabelSize.y + ImGui::GetStyle().FramePadding.y * 2.0f;
                if (ImGui::Button("X", ImVec2(buttonSize, buttonSize)))
                {
                    component.Texture = 0;
                }
            }

            ImGui::SameLine();
            ImGui::Text("Texture");

            ImGui::DragFloat("Tiling Factor", &component.TilingFactor, 0.1f, 0.0f, 100.0f);
        });

        DrawComponent<CircleRendererComponent>("Circle Renderer", entity, [](auto& component)
        {
            ImGui::ColorEdit4("Color", glm::value_ptr(component.Color));
            ImGui::DragFloat("Thickness", &component.Thickness, 0.025f, 0.0f, 1.0f);
            ImGui::DragFloat("Fade", &component.Fade, 0.00025f, 0.0f, 1.0f);
        });

        DrawComponent<RigidBody2DComponent>("Rigidbody 2D", entity, [](auto& component)
        {
            const char* bodyTypeStrings[] = {"Static", "Dynamic", "Kinematic"};
            const char* currentBodyTypeString = bodyTypeStrings[(int)component.Type];
            if (ImGui::BeginCombo("Body Type", currentBodyTypeString))
            {
                for (int i = 0; i < 2; i++)
                {
                    bool isSelected = currentBodyTypeString == bodyTypeStrings[i];
                    if (ImGui::Selectable(bodyTypeStrings[i], isSelected))
                    {
                        currentBodyTypeString = bodyTypeStrings[i];
                        component.Type = (RigidBody2DComponent::BodyType)i;
                    }

                    if (isSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }

                ImGui::EndCombo();
            }

            ImGui::Checkbox("Fixed Rotation", &component.FixedRotation);
        });

        DrawComponent<BoxCollider2DComponent>("Box Collider 2D", entity, [](auto& component)
        {
            ImGui::DragFloat2("Offset", glm::value_ptr(component.Offset));
            ImGui::DragFloat2("Size", glm::value_ptr(component.Size));
            ImGui::DragFloat("Density", &component.Density, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Friction", &component.Friction, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Restitution", &component.Restitution, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("RestitutionThreshold", &component.RestitutionThreshold, 0.01f, 0.0f);
        });

        DrawComponent<CircleCollider2DComponent>("Circle Collider 2D", entity, [](auto& component)
        {
            ImGui::DragFloat2("Offset", glm::value_ptr(component.Offset));
            ImGui::DragFloat("Radius", &component.Radius);
            ImGui::DragFloat("Density", &component.Density, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Friction", &component.Friction, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Restitution", &component.Restitution, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("RestitutionThreshold", &component.RestitutionThreshold, 0.01f, 0.0f);
        });

        DrawComponent<RigidBody3DComponent>("Rigidbody 3D", entity, [](auto& component)
        {
            const char* bodyTypeStrings[] = { "Static", "Dynamic", "Kinematic" };
            const char* currentBodyTypeString = bodyTypeStrings[(int)component.Type];
            if (ImGui::BeginCombo("Body Type", currentBodyTypeString))
            {
                for (int i = 0; i < 3; i++)
                {
                    bool isSelected = currentBodyTypeString == bodyTypeStrings[i];
                    if (ImGui::Selectable(bodyTypeStrings[i], isSelected))
                    {
                        currentBodyTypeString = bodyTypeStrings[i];
                        component.Type = (RigidBody3DComponent::BodyType)i;
                    }
                    if (isSelected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            const char* motionQualityStrings[] = { "Discrete", "LinearCast" };
            const char* currentQualityString = motionQualityStrings[(int)component.Quality];
            if (ImGui::BeginCombo("Collision Detection", currentQualityString))
            {
                for (int i = 0; i < 2; i++)
                {
                    bool isSelected = currentQualityString == motionQualityStrings[i];
                    if (ImGui::Selectable(motionQualityStrings[i], isSelected))
                    {
                        currentQualityString = motionQualityStrings[i];
                        component.Quality = (RigidBody3DComponent::MotionQuality)i;
                    }
                    if (isSelected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::DragFloat("Mass", &component.Mass, 0.1f, 0.001f, 10000.0f);
            ImGui::DragFloat("Linear Damping", &component.LinearDamping, 0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Angular Damping", &component.AngularDamping, 0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Gravity Factor", &component.GravityFactor, 0.05f, -10.0f, 10.0f);
            ImGui::Checkbox("Allow Sleeping", &component.AllowSleeping);
            ImGui::Checkbox("Is Sensor", &component.IsSensor);
        });

        DrawComponent<BoxCollider3DComponent>("Box Collider 3D", entity, [](auto& component)
        {
            ImGui::DragFloat3("Half Extents", glm::value_ptr(component.HalfExtents), 0.05f, 0.001f, 1000.0f);
            ImGui::DragFloat3("Offset", glm::value_ptr(component.Offset), 0.05f);
            ImGui::DragFloat("Friction", &component.Friction, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Restitution", &component.Restitution, 0.01f, 0.0f, 1.0f);
        });

        DrawComponent<SphereCollider3DComponent>("Sphere Collider 3D", entity, [](auto& component)
        {
            ImGui::DragFloat("Radius", &component.Radius, 0.05f, 0.001f, 1000.0f);
            ImGui::DragFloat3("Offset", glm::value_ptr(component.Offset), 0.05f);
            ImGui::DragFloat("Friction", &component.Friction, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Restitution", &component.Restitution, 0.01f, 0.0f, 1.0f);
        });

        DrawComponent<CapsuleCollider3DComponent>("Capsule Collider 3D", entity, [](auto& component)
        {
            ImGui::DragFloat("Half Height", &component.HalfHeight, 0.05f, 0.001f, 1000.0f);
            ImGui::DragFloat("Radius", &component.Radius, 0.05f, 0.001f, 1000.0f);
            ImGui::DragFloat3("Offset", glm::value_ptr(component.Offset), 0.05f);
            ImGui::DragFloat("Friction", &component.Friction, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Restitution", &component.Restitution, 0.01f, 0.0f, 1.0f);
        });

        DrawComponent<CharacterController3DComponent>("Character Controller 3D", entity, [](auto& component)
        {
            ImGui::DragFloat("Radius", &component.Radius, 0.01f, 0.01f, 10.0f);
            ImGui::DragFloat("Height", &component.Height, 0.05f, 0.0f, 20.0f);
            ImGui::DragFloat("Step Height", &component.StepHeight, 0.01f, 0.0f, 2.0f);
            ImGui::DragFloat("Max Slope", &component.MaxSlopeDegrees, 0.5f, 0.0f, 89.0f, "%.1f deg");
            ImGui::DragFloat("Gravity Scale", &component.GravityScale, 0.05f, 0.0f, 10.0f);
            ImGui::DragFloat("Air Control", &component.AirControl, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Max Acceleration", &component.MaxAcceleration, 0.1f, 0.1f, 200.0f, "%.1f m/s^2");
            ImGui::DragFloat("Braking Deceleration", &component.BrakingDeceleration, 0.1f, 0.1f, 200.0f, "%.1f m/s^2");
            ImGui::Separator();
            ImGui::BeginDisabled();
            ImGui::Checkbox("Grounded", &component.IsGrounded);
            ImGui::DragFloat3("Velocity", glm::value_ptr(component.Velocity));
            ImGui::EndDisabled();
        });

        DrawComponent<TextComponent>("Text Renderer", entity, [](auto& component)
        {
            ImGui::InputTextMultiline("Text String", &component.TextString);
            ImGui::ColorEdit4("Color", glm::value_ptr(component.Color));
            ImGui::DragFloat("Kerning", &component.Kerning, 0.025f);
            ImGui::DragFloat("Line Spacing", &component.LineSpacing, 0.025f);
        });
    }

    template <typename T>
    void SceneHierarchyPanel::DisplayAddComponentEntry(const std::string& entryName)
    {
        Entity entity = GetSelectedEntity();
        if (entity && !entity.HasComponent<T>())
        {
            if (ImGui::MenuItem(entryName.c_str()))
            {
                entity.AddComponent<T>();
                ImGui::CloseCurrentPopup();
            }
        }
    }
}
