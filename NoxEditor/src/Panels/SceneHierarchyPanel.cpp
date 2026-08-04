#include "SceneHierarchyPanel.h"

#include <imgui.h>
#include <imgui_internal.h>
#include "misc/cpp/imgui_stdlib.h"
#include <glm/gtc/type_ptr.hpp>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Animation/Animator.h"

namespace Nox
{
    SceneHierarchyPanel::SceneHierarchyPanel(const Ref<Scene>& context)
    {
        SetContext(context);
    }

    void SceneHierarchyPanel::SetContext(const Ref<Scene>& context)
    {
        m_Context = context;
        m_SelectionContext = {}; // if youz want tabs dont null provide the scene
    }

    void SceneHierarchyPanel::OnImGuiRender()
    {
        ImGui::Begin("Scene Hierarchy");
        left = ImGui::GetWindowSize(); // temporrary in.h
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

            // --- 1. FILL REMAINING SPACE WITH AN INVISIBLE BUTTON ---
            ImVec2 availSize = ImGui::GetContentRegionAvail();
            if (availSize.x < 1.0f) availSize.x = 1.0f;
            if (availSize.y < 1.0f) availSize.y = 1.0f;

            ImGui::InvisibleButton("##SceneHierarchyBackground", availSize);

            if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered())
            {
                m_SelectionContext = {};
            }

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_HIERARCHY_ENTITY"))
                {
                    UUID droppedEntityID = *(UUID*)payload->Data;
                    Entity droppedEntity = m_Context->GetEntityByUUID(droppedEntityID);
                    if (droppedEntity)
                    {
                        droppedEntity.SetParent({}); // Pass empty entity to make it root
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Right-click on blank space
            if (ImGui::BeginPopupContextWindow(0, 1 | ImGuiPopupFlags_NoOpenOverItems))
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
        if (m_SelectionContext)
        {
            DrawComponents(m_SelectionContext);
        }

        ImGui::End();
    }

    void SceneHierarchyPanel::SetSelectedEntity(Entity entity)
    {
        m_SelectionContext = entity;
    }

    void SceneHierarchyPanel::DrawEntityNode(Entity entity)
    {
        auto& tag = entity.GetComponent<TagComponent>().Tag;

        ImGuiTreeNodeFlags flags = ((m_SelectionContext == entity) ? ImGuiTreeNodeFlags_Selected : 0) |
            ImGuiTreeNodeFlags_OpenOnArrow;
        flags |= ImGuiTreeNodeFlags_SpanAvailWidth;

        bool hasChildren = false;
        if (entity.HasComponent<RelationshipComponent>())
            if (!entity.GetComponent<RelationshipComponent>().Children.empty())
                hasChildren = true;

        if (!hasChildren)
            flags |= ImGuiTreeNodeFlags_Leaf;

        bool opened = ImGui::TreeNodeEx((void*)(uint64_t)(uint32_t)entity, flags, tag.c_str());
        if (ImGui::IsItemClicked())
        {
            m_SelectionContext = entity;
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
            m_Context->DestroyEntity(entity);
            if (m_SelectionContext == entity)
            {
                m_SelectionContext = {};
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
            bool modified = false;
            modified |= DrawVec3Control("Position", component.Translation);

            glm::vec3 rotation = glm::degrees(component.Rotation);
            if (DrawVec3Control("Rotation", rotation))
            {
                component.Rotation = glm::radians(rotation);
                modified = true;
            }
            modified |= DrawVec3Control("Scale", component.Scale, 1.0f);

            if (modified)
                m_Context->m_Registry.emplace_or_replace<DirtyTransformComponent>(entity);
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
            }

            ImGui::SameLine();
            ImGui::Text("Mesh Asset");
        });

        DrawComponent<MaterialComponent>("Material", entity, [](auto& component)
        {
            ImGui::ColorEdit4("Albedo Color", glm::value_ptr(component.AlbedoColor));

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Material Slots (Submesh Textures)");
            ImGui::Spacing();

            if (component.AlbedoMaps.empty())
            {
                ImGui::TextDisabled("No material slots assigned.");
                if (ImGui::Button("+ Add Slot"))
                {
                    component.AlbedoMaps.push_back(0);
                }
            }
            else
            {
                for (size_t i = 0; i < component.AlbedoMaps.size(); i++)
                {
                    // PUSH ID to prevent button ID collisions across multiple vector slots
                    ImGui::PushID(static_cast<int>(i));

                    AssetHandle& texHandle = component.AlbedoMaps[i];
                    std::string label = "None";
                    bool isTextureValid = false;

                    if (texHandle != 0)
                    {
                        if (AssetManager::IsAssetHandleValid(texHandle) && AssetManager::GetAssetType(texHandle) == AssetType::Texture2D)
                        {
                            const AssetMetadata& metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(texHandle);
                            label = metadata.FilePath.filename().string();
                            isTextureValid = true;
                        }
                        else
                        {
                            label = "Invalid";
                        }
                    }

                    ImGui::Text("Slot [%zu]", i);
                    ImGui::SameLine();

                    ImVec2 buttonLabelSize = ImGui::CalcTextSize(label.c_str());
                    buttonLabelSize.x += 20.0f;
                    float buttonLabelWidth = glm::max<float>(100.0f, buttonLabelSize.x);

                    ImGui::Button(label.c_str(), ImVec2(buttonLabelWidth, 0.0f));

                    // Drag & Drop payload target for Texture2D
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                        {
                            AssetHandle handle = *(AssetHandle*)payload->Data;
                            if (AssetManager::GetAssetType(handle) == AssetType::Texture2D)
                            {
                                texHandle = handle;
                            }
                            else
                            {
                                NOX_CORE_WARN("Wrong Asset Type - Expected a Texture2D");
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    // Clear Texture Button
                    if (isTextureValid)
                    {
                        ImGui::SameLine();
                        ImVec2 xLabelSize = ImGui::CalcTextSize("X");
                        float buttonSize = xLabelSize.y + ImGui::GetStyle().FramePadding.y * 2.0f;
                        if (ImGui::Button("X", ImVec2(buttonSize, buttonSize)))
                        {
                            texHandle = 0;
                        }
                    }

                    ImGui::PopID();
                }

                ImGui::Spacing();
                if (ImGui::Button("+ Add Slot"))
                {
                    component.AlbedoMaps.push_back(0);
                }
                ImGui::SameLine();
                if (ImGui::Button("- Remove Slot") && !component.AlbedoMaps.empty())
                {
                    component.AlbedoMaps.pop_back();
                }
            }
        });

DrawComponent<AnimatorComponent>("Animator", entity, [](auto& component)
{
    Ref<AnimationSequence> currentAnim = component.Animator.GetCurrentAnimation();

    // --- Media Control Buttons ---
    bool isPlaying = component.Animator.IsPlaying();
    
    if (isPlaying)
    {
        if (ImGui::Button("Pause", ImVec2(80.0f, 0.0f)))
        {
            component.Animator.Pause();
        }
    }
    else
    {
        if (ImGui::Button("Play", ImVec2(80.0f, 0.0f)))
        {
            // If looping is off and the animation is at the end, reset to start before playing
            if (!component.Animator.IsLooping() && currentAnim)
            {
                float currentTime = component.Animator.GetCurrentAnimationTime();
                if (currentTime >= currentAnim->Duration)
                {
                    component.Animator.SetCurrentTime(0.0f);
                }
            }
            component.Animator.Resume();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(80.0f, 0.0f)))
    {
        component.Animator.Stop();
    }

    ImGui::Spacing();

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

    // Current animation sequence details
    std::string currentAnimName = "None";

    if (currentAnim)
    {
        currentAnimName = currentAnim->Name.empty() ? "Animation Sequence" : currentAnim->Name;
    }

    ImGui::Text("Animation: %s", currentAnimName.c_str());

    // Timeline control
    float currentTime = component.Animator.GetCurrentAnimationTime();
    float maxDuration = currentAnim ? currentAnim->Duration : 100.0f;

    if (ImGui::SliderFloat("Time (Ticks)", &currentTime, 0.0f, maxDuration, "%.2f"))
    {
        component.Animator.SetCurrentTime(currentTime);
    }

    // Drag-and-drop target for .nanim assets
    ImGui::Spacing();
    ImGui::Button("Drop Animation Here", ImVec2(-1.0f, 0.0f));
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
                    component.Animator.PlayAnimation(anim);
                }
            }
            else
            {
                NOX_CORE_WARN("Wrong Asset Type - Expected Animation Sequence (.nanim)");
            }
        }
        ImGui::EndDragDropTarget();
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
        if (m_SelectionContext && !m_SelectionContext.HasComponent<T>())
        {
            if (ImGui::MenuItem(entryName.c_str()))
            {
                m_SelectionContext.AddComponent<T>();
                ImGui::CloseCurrentPopup();
            }
        }
    }
}
