#include "SceneHierarchyPanel.h"

#include <imgui.h>
#include <imgui_internal.h>
#include "misc/cpp/imgui_stdlib.h"
#include <glm/gtc/type_ptr.hpp>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Asset/MaterialSerializer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Animation/Animator.h"
#include "NoxCore/Project/Project.h"

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

    void SceneHierarchyPanel::OnImGuiRender()
{
    ImGui::Begin("Scene Hierarchy");
    left = ImGui::GetWindowSize();
    leftFocused = ImGui::IsWindowFocused();
    leftHovered = ImGui::IsWindowHovered();

    if (m_Context)
    {
        m_Context->m_Registry.view<TagComponent>().each([&](auto entityID, TagComponent&)
        {
            Entity entity(entityID, m_Context.get());

            bool isRoot = true;
            if (entity.HasComponent<RelationshipComponent>())
                if (entity.GetComponent<RelationshipComponent>().Parent != 0)
                    isRoot = false;

            if (isRoot)
                DrawEntityNode(entity);
        });

        // 1. Deselect entity when left-clicking blank space
        if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered())
        {
            ClearSelection();
        }

        // 2. Window-level Drag and Drop Target for root entity unparenting
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_HIERARCHY_ENTITY"))
            {
                UUID droppedEntityID = *(UUID*)payload->Data;
                Entity droppedEntity = m_Context->GetEntityByUUID(droppedEntityID);
                if (droppedEntity)
                {
                    droppedEntity.SetParent({}); // Make root
                }
            }
            ImGui::EndDragDropTarget();
        }

        // 3. Right-click context menu on blank space
        if (ImGui::BeginPopupContextWindow(0, ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
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

    void SceneHierarchyPanel::DrawEntityNode(Entity entity)
    {
        auto& tag = entity.GetComponent<TagComponent>().Tag;
        
        bool isSelected = IsSelected(entity);

        ImGuiTreeNodeFlags flags = (isSelected  ? ImGuiTreeNodeFlags_Selected : 0) |
            ImGuiTreeNodeFlags_OpenOnArrow;
        flags |= ImGuiTreeNodeFlags_SpanAvailWidth;

        bool hasChildren = false;
        if (entity.HasComponent<RelationshipComponent>())
            if (!entity.GetComponent<RelationshipComponent>().Children.empty())
                hasChildren = true;

        if (!hasChildren)
            flags |= ImGuiTreeNodeFlags_Leaf;

        bool opened = ImGui::TreeNodeEx((void*)(uint64_t)(uint32_t)entity, flags, tag.c_str());
        
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
                SetSelectedEntity(entity);
            }
        }

        // --- 1. DRAG SOURCE: Pick up this entity to drag it ---
        if (ImGui::BeginDragDropSource())
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
                        m_Context->DestroyEntity(e);
                }
            }
            else
            {
                if (m_SelectionAnchor == entity)
                    m_SelectionAnchor = {};

                m_Context->DestroyEntity(entity);

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
            strcpy_s(buffer, sizeof(buffer), tag.c_str());
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
            
            DisplayAddComponentEntry<AnimatorComponent>("Animator");

            DisplayAddComponentEntry<CameraComponent>("Camera");
            DisplayAddComponentEntry<ScriptComponent>("Script");
            DisplayAddComponentEntry<SpriteRendererComponent>("Sprite Renderer");
            DisplayAddComponentEntry<CircleRendererComponent>("Circle Renderer");
            DisplayAddComponentEntry<RigidBody2DComponent>("Rigidbody 2D");
            DisplayAddComponentEntry<BoxCollider2DComponent>("Box Collider 2D");
            DisplayAddComponentEntry<CircleCollider2DComponent>("Circle Collider 2D");
            DisplayAddComponentEntry<TextComponent>("Text Component");

            ImGui::EndPopup();
        }

        ImGui::PopItemWidth();

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
                    m_Context->m_Registry.emplace_or_replace<DirtyTransformComponent>(entity);

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

                                m_Context->m_Registry.emplace_or_replace<DirtyTransformComponent>(otherEntity);
                            }
                        }
                    }
                }
            });

        DrawComponent<MeshComponent>("Mesh", entity, [](auto& component)
        {
            std::string label = "None";
            bool isMeshValid = false;

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
                }

                ImGui::DragScalar("Submesh Index", ImGuiDataType_U32, &component.SubmeshIndex, 0.1f, nullptr, nullptr, "%u");
                ImGui::DragScalar("Submesh Count", ImGuiDataType_U32, &component.SubmeshCount, 0.1f, nullptr, nullptr, "%u");
            }

            ImGui::SameLine();
            ImGui::Text("Mesh Asset");
        });

DrawComponent<MaterialComponent>("Material", entity, [](auto& component)
{
    if (component.MaterialAssets.empty())
    {
        ImGui::TextDisabled("Drop a material asset here");
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
            {
                AssetHandle handle = *(const AssetHandle*)payload->Data;
                if (AssetManager::IsAssetHandleValid(handle) &&
                    AssetManager::GetAssetType(handle) == AssetType::Material)
                    component.MaterialAssets.push_back(handle);
            }
            ImGui::EndDragDropTarget();
        }
        return;
    }

    if (!component.MaterialAssets.empty())
    {
        ImGui::Text("Material Assets");
        for (size_t i = 0; i < component.MaterialAssets.size(); ++i)
        {
            AssetHandle handle = component.MaterialAssets[i];
            std::string label = "None";
            if (handle != 0 && AssetManager::IsAssetHandleValid(handle) &&
                AssetManager::GetAssetType(handle) == AssetType::Material)
            {
                const auto& metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
                label = metadata.FilePath.filename().string();
            }

            if (ImGui::TreeNode((void*)(uintptr_t)i, "%zu: %s", i, label.c_str()))
            {
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                    {
                        AssetHandle droppedHandle = *(const AssetHandle*)payload->Data;
                        if (AssetManager::IsAssetHandleValid(droppedHandle) &&
                            AssetManager::GetAssetType(droppedHandle) == AssetType::Material)
                            component.MaterialAssets[i] = droppedHandle;
                    }
                    ImGui::EndDragDropTarget();
                }

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
                                        component.MaterialAssets[i] = uniqueHandle;
                                        break;
                                    }
                                }
                            }
                        }

                        bool changed = false;
                        changed |= ImGui::ColorEdit4("Base Color", glm::value_ptr(data.BaseColorFactor));
                        changed |= ImGui::DragFloat("Metallic", &data.MetallicFactor, 0.01f, 0.0f, 1.0f);
                        changed |= ImGui::DragFloat("Roughness", &data.RoughnessFactor, 0.01f, 0.0f, 1.0f);
                        changed |= ImGui::ColorEdit3("Emissive", glm::value_ptr(data.EmissiveFactor));
                        changed |= ImGui::DragFloat("Emissive Strength", &data.emissiveStrength, 0.01f, 0.0f, 100.0f);
                        changed |= ImGui::DragFloat("Transmission", &data.TransmissionFactor, 0.01f, 0.0f, 1.0f);

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
                                }
                                ImGui::EndDragDropTarget();
                            }
                        };

                        drawTextureReference("Base Color Texture", "BaseColor", data.BaseColorTexturePath);
                        drawTextureReference("Metallic Roughness", "MetallicRoughness", data.MetallicRoughnessTexturePath);
                        drawTextureReference("Normal Texture", "Normal", data.NormalTexturePath);
                        drawTextureReference("Occlusion Texture", "Occlusion", data.OcclusionTexturePath);
                        drawTextureReference("Emissive Texture", "Emissive", data.EmissiveTexturePath);
                        drawTextureReference("Transmission Texture", "Transmission", data.TransmissionTexturePath);

                        int alphaMode = static_cast<int>(data.Mode);
                        const char* alphaModes[] = { "Opaque", "Mask", "Blend" };
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
        return;
    }

});
        
        DrawComponent<DirectionalLightComponent>("Directional Light", entity, [](auto& component)
            {
                ImGui::ColorEdit3("Color", glm::value_ptr(component.Color));
                ImGui::DragFloat("Intensity", &component.Intensity, 0.1f, 0.0f, 100.0f);
            });

        DrawComponent<PointLightComponent>("Point Light", entity, [](auto& component)
        {
            ImGui::ColorEdit3("Color", glm::value_ptr(component.Color));
            ImGui::DragFloat("Intensity", &component.Intensity, 0.5f, 0.0f, 1000.0f);
            ImGui::DragFloat("Range", &component.Range, 0.5f, 0.1f, 1000.0f);
        });

        DrawComponent<SpotLightComponent>("Spot Light", entity, [](auto& component)
        {
            ImGui::ColorEdit3("Color", glm::value_ptr(component.Color));
            ImGui::DragFloat("Intensity", &component.Intensity, 0.5f, 0.0f, 1000.0f);
            ImGui::DragFloat("Range", &component.Range, 0.5f, 0.1f, 1000.0f);
            ImGui::DragFloat("Inner Angle", &component.InnerAngle, 0.5f, 0.0f, component.OuterAngle);
            ImGui::DragFloat("Outer Angle", &component.OuterAngle, 0.5f, component.InnerAngle, 89.0f);
        });

        DrawComponent<AnimatorComponent>("Animator", entity, [](auto& component)
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
            });

        DrawComponent<CameraComponent>("Camera", entity, [](auto& component)
        {
            auto& camera = component.Camera;

            ImGui::Checkbox("Primary", &component.Primary);

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

        DrawComponent<ScriptComponent>("Script", entity, [entity, scene = m_Context](auto& component) mutable
        {
            /*bool scriptClassExists = ScriptEngine::EntityClassExists(component.ClassName);

            static char buffer[64];
            strcpy_s(buffer, sizeof(buffer), component.ClassName.c_str());

            UI::ScopedStyleColor textColor(ImGuiCol_Text, ImVec4(0.9f, 0.2f, 0.3f, 1.0f), !scriptClassExists);

            if (ImGui::InputText("Class", buffer, sizeof(buffer)))
            {
                component.ClassName = buffer;
                return;
            }

            // Fields
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
            }*/
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
