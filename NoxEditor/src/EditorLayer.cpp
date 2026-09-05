#include "EditorLayer.h"

#include <iostream>

#include <imgui.h>
#include <imgui_internal.h>// For Docking
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>  // for pointer to matrix or vector

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/MeshSerializer.h"
#include "NoxCore/Asset/SceneImporter.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Input.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/ImGui/ImGuiLayer.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Utils/Utils.h"

namespace Nox
{
    EditorLayer::EditorLayer() : Layer("EditorLayer")
    {
        NOX_INFO("EditorLayer Start");

        auto& app = Application::Get();
        m_Renderer = app.GetRenderer();
        m_Renderer2D = m_Renderer->getRenderer2D();

        m_Font = Font::GetDefault();

        m_IconPlay = TextureImporter::LoadTexture2D("assets/Icons/PlayButton.ktx2", {.generateMips = false});
        m_IconStop = TextureImporter::LoadTexture2D("assets/Icons/StopButton.ktx2", {.generateMips = false});
        m_IconPause = TextureImporter::LoadTexture2D("assets/Icons/PauseButton.ktx2", {.generateMips = false});
        m_IconSimulate = TextureImporter::LoadTexture2D("assets/Icons/SimulateButton.ktx2", {.generateMips = false});
        m_IconStep = TextureImporter::LoadTexture2D("assets/Icons/StepButton.ktx2", {.generateMips = false});

        m_EditorScene = CreateRef<Scene>();
        m_ActiveScene = m_EditorScene;

        auto commandLineArgs = Application::Get().GetSpecification().CommandLineArgs;
        if (commandLineArgs.Count > 1)
        {
            std::cout << "Loading Project: " << commandLineArgs[1] << std::endl;
            auto projectFilePath = commandLineArgs[1];
            OpenProject(projectFilePath);
        }
        else
        {
            // TODO: promp the user to select a directory
            //NewProject();

            // If no project is opened, close nox
            // note: this is while we dont have a new project path
            if (!OpenProject())
            {
                SDL_Event event;
                event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&event);
            }
        }

        m_EditorCamera = EditorCamera(30.0f, 1.778f, 0.1f, 1000.0f);

        Project::GetActive()->GetEditorAssetManager()->Init();
    }

    EditorLayer::~EditorLayer()
    {
        NOX_CORE_INFO("EditorLayer Shutdown");

        m_Font->ReleaseDefault(); // Since Editor Layer since static dies After renderer not needed for components

        //idk what hapens if i have clientproject is this a good place here 
        if (Project::GetActive() && Project::GetActive()->GetEditorAssetManager())
        {
            std::static_pointer_cast<EditorAssetManager>(Project::GetActive()->GetEditorAssetManager())->Shutdown();
        }
    }

    void EditorLayer::OnEvent(Event& event)
    {
        //std::println("{}", event.ToString());

        if (m_SceneState == SceneState::Edit && m_ViewportHovered)
            m_EditorCamera.OnEvent(event);

        EventDispatcher dispatcher(event);
        dispatcher.Dispatch<KeyPressedEvent>(Nox_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
        dispatcher.Dispatch<MouseButtonPressedEvent>(Nox_BIND_EVENT_FN(EditorLayer::OnMouseButtonPressed));
    }

    void EditorLayer::OnUpdate(Timestep ts)
    {
        m_ActiveScene->OnViewportResize(m_ViewportSize.x, m_ViewportSize.y);

        // zero sized framebuffer is invalid
        if (m_ViewportSize.x > 0.0f && m_ViewportSize.y > 0.0f)
        {
            // Verify if the viewport has a new size and resize the RenderTarget accordingly.
            NRI::Extent2D viewportSize = m_Renderer->getViewPortSize();
            if (m_ViewportSize.x != viewportSize.width || m_ViewportSize.y != viewportSize.height)
            {
                m_Renderer->onViewportSizeChange({m_ViewportSize.x, m_ViewportSize.y});
                m_EditorCamera.SetViewportSize(m_ViewportSize.x, m_ViewportSize.y);
            }
        }

        m_ActiveScene->SetRenderer(m_Renderer);
        m_ActiveScene->SetRenderer2D(m_Renderer->getRenderer2D());

        switch (m_SceneState)
        {
        case SceneState::Edit:
            {
                if (m_ViewportFocused)
                {
                    /*m_CameraController.OnUpdate(ts);*/
                }

                m_EditorCamera.OnUpdate(ts);

                m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
                break;
            }
        case SceneState::Simulate:
            {
                m_EditorCamera.OnUpdate(ts);

                m_ActiveScene->OnUpdateSimulation(ts, m_EditorCamera);
                break;
            }
        case SceneState::Play:
            {
                m_ActiveScene->OnUpdateRuntime(ts);
                break;
            }
        }

        // Mouse Selection
        auto [mx, my] = ImGui::GetMousePos();
        mx -= m_ViewportBounds[0].x;
        my -= m_ViewportBounds[0].y;

        glm::vec2 viewportSize = m_ViewportBounds[1] - m_ViewportBounds[0];
        //my = viewportSize.y - my;
        int mouseX = (int)mx;
        int mouseY = (int)my;

        if (mouseX >= 0 && mouseY >= 0 && mouseX < (int)viewportSize.x && mouseY < (int)viewportSize.y)
        {
            /*ScopeTimer timer("MousePicking");*/
            // Retrieve the pixel data (ID) from the calculated index
            // reading only 1 pixel right now but if multi select maybe i need full viewport ? 
            /*int pixelData = Renderer::ReadPixel(mouseX, mouseY);

            m_HoveredEntity = pixelData == -1 ? Entity() : Entity((entt::entity)pixelData, m_ActiveScene.get());
            */
            int32_t pixelData = m_Renderer->getPickedEntityID();

            m_HoveredEntity = pixelData == -1 ? Entity() : Entity((entt::entity)pixelData, m_ActiveScene.get());

            // If not dragging a box, keep requesting 1x1 pixel under cursor for hover detection
            if (!m_IsBoxSelecting)
            {
                m_Renderer->setPickRequest(mouseX, mouseY, true);
            }
            
            // When actively dragging the box, request the full rectangle from the GPU entity texture
            if (m_IsBoxSelecting)
            {
                glm::vec2 boxMin = {
                    std::min(m_BoxSelectStart.x, m_BoxSelectEnd.x) - m_ViewportBounds[0].x,
                    std::min(m_BoxSelectStart.y, m_BoxSelectEnd.y) - m_ViewportBounds[0].y
                };
                glm::vec2 boxMax = {
                    std::max(m_BoxSelectStart.x, m_BoxSelectEnd.x) - m_ViewportBounds[0].x,
                    std::max(m_BoxSelectStart.y, m_BoxSelectEnd.y) - m_ViewportBounds[0].y
                };

                int startX = std::max(0, (int)boxMin.x);
                int startY = std::max(0, (int)boxMin.y);
                int width = std::max(1, (int)(boxMax.x - boxMin.x));
                int height = std::max(1, (int)(boxMax.y - boxMin.y));

                m_Renderer->setBoxPickRequest(startX, startY, (uint32_t)width, (uint32_t)height);
            }
        }

        OnOverlayRender();

        Project::GetActive()->GetEditorAssetManager()->Update();
    }

    void EditorLayer::OnRender()
    {
    }

    void EditorLayer::OnImGuiRender()
    {
        /*--
        * IMGUI Docking
        * Create a dockspace and dock the viewport and settings window.
        * The central node is named "Viewport", which can be used later with Begin("Viewport")
        * to render the final image.
        -*/

        const ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingInCentralNode;

        // 1. Grab the style and save the default minimum size
        ImGuiStyle& style = ImGui::GetStyle();
        float minWinSizeX = style.WindowMinSize.x;
        float minWinSizeY = style.WindowMinSize.y;

        // 2. Enforce the new minimum size globally for the DockSpace
        style.WindowMinSize.x = 370.0f;

        // 3. Submit the DockSpace (It will inherit the 370x350 constraint)
        ImGuiID dockID = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), dockFlags);

        // 4. Restore the original minimum size for standard floating windows
        style.WindowMinSize.x = minWinSizeX;
        style.WindowMinSize.y = minWinSizeY;

        // Docking layout, must be done only if it doesn't exist
        if (!ImGui::DockBuilderGetNode(dockID)->IsSplitNode() && !ImGui::FindWindowByName("Viewport"))
        {
            ImGui::DockBuilderDockWindow("Viewport", dockID); // Dock "Viewport" to  central node
            ImGui::DockBuilderGetCentralNode(dockID)->LocalFlags |= ImGuiDockNodeFlags_NoTabBar; // Remove "Tab" from the central node
            ImGuiID leftID = ImGui::DockBuilderSplitNode(dockID, ImGuiDir_Left, 0.2f, nullptr, &dockID); // Split the central node
            ImGui::DockBuilderDockWindow("Settings", leftID); // Dock "Settings" to the left node
        }

        // [optional] Show the menu bar
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open Project...", "Ctrl+O"))
                {
                    OpenProject();
                }

                ImGui::Separator();

                if (ImGui::MenuItem("New Scene", "Ctrl+N"))
                {
                    NewScene();
                }

                if (ImGui::MenuItem("Save Scene", "Ctrl+S"))
                {
                    SaveScene();
                }

                if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S"))
                {
                    SaveSceneAs();
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Exit"))
                    Application::Shutdown();

                ImGui::EndMenu();
            }


            if (ImGui::BeginMenu("Script"))
            {
                if (ImGui::MenuItem("Reload assembly", "Ctrl+R"))
                {
                    /*ScriptEngine::ReloadAssembly();*/ // otherwise it thinkgs its exectuing endmenu if you dont use {}
                }

                ImGui::EndMenu();
            }

            bool currentVSync = m_Renderer->getVSync();
            if (ImGui::MenuItem("vSync", "", &currentVSync))
                m_Renderer->setVSync(currentVSync); // Recreate the swapchain with the new vSync setting

            // Adding overlay text on the upper left corner
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

            ImGui::EndMainMenuBar();
        }

        /* END Docking */

        // We define "viewport" with no padding an retrieve the rendering area
        // Using the dock "Viewport", this sets the window to cover the entire central viewport
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        if (ImGui::Begin("Viewport"))
        {
            auto viewportMinRegion = ImGui::GetWindowContentRegionMin();
            auto viewportMaxRegion = ImGui::GetWindowContentRegionMax();
            auto viewportOffset = ImGui::GetWindowPos();
            m_ViewportBounds[0] = {viewportMinRegion.x + viewportOffset.x, viewportMinRegion.y + viewportOffset.y};
            m_ViewportBounds[1] = {viewportMaxRegion.x + viewportOffset.x, viewportMaxRegion.y + viewportOffset.y};

            m_ViewportFocused = ImGui::IsWindowFocused();
            m_ViewportHovered = ImGui::IsWindowHovered();

            Application::Get().GetImGuiLayer()->BlockEvents(!m_ViewportHovered);

            ImVec2 viewportPanelSize = ImGui::GetContentRegionAvail();
            m_ViewportSize = {viewportPanelSize.x, viewportPanelSize.y};

            auto* texture = m_Renderer->GetSceneResource();
            if (texture)
            {
                auto textureID = texture->getImTextureID();
                if (textureID)
                {
                    // !!! This is where the RenderTarget image is displayed !!!
                    ImGui::Image(textureID, viewportPanelSize);
                }
            }
               ImVec2 mousePos = ImGui::GetMousePos();

            // Only initiate box select when dragging on viewport and not using gizmos/alt-orbit
            if (m_ViewportHovered && !ImGuizmo::IsUsing() && !Input::IsKeyPressed(SDL_SCANCODE_LALT))
            {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    m_BoxSelectStart = { mousePos.x, mousePos.y };
                    m_BoxSelectEnd = m_BoxSelectStart;
                }

                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f) && !m_IsBoxSelecting)
                {
                    if (!ImGuizmo::IsOver())
                    {
                        m_IsBoxSelecting = true;
                    }
                }
            }

            // Draw visual marquee box using ImGui DrawList while dragging
            if (m_IsBoxSelecting)
            {
                m_BoxSelectEnd = { mousePos.x, mousePos.y };

                ImDrawList* drawList = ImGui::GetWindowDrawList();

                ImVec2 pMin = ImVec2(std::min(m_BoxSelectStart.x, m_BoxSelectEnd.x), std::min(m_BoxSelectStart.y, m_BoxSelectEnd.y));
                ImVec2 pMax = ImVec2(std::max(m_BoxSelectStart.x, m_BoxSelectEnd.x), std::max(m_BoxSelectStart.y, m_BoxSelectEnd.y));

                // Semi-transparent box fill
                drawList->AddRectFilled(pMin, pMax, IM_COL32(230, 140, 30, 40));
                // Solid border outline
                drawList->AddRect(pMin, pMax, IM_COL32(255, 160, 40, 220), 0.0f, 0, 1.5f);
            }

            // Commit selection when mouse button is released
            if (m_IsBoxSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                m_IsBoxSelecting = false;

                bool shift = Input::IsKeyPressed(SDL_SCANCODE_LSHIFT) || Input::IsKeyPressed(SDL_SCANCODE_RSHIFT);
                bool ctrl = Input::IsKeyPressed(SDL_SCANCODE_LCTRL) || Input::IsKeyPressed(SDL_SCANCODE_RCTRL);

                // If neither Shift nor Ctrl is held, clear previous selection
                if (!shift && !ctrl)
                {
                    m_SceneHierarchyPanel.ClearSelection();
                }

                // Read all unique entity IDs directly from the GPU entity resolve texture
                std::vector<int32_t> pickedIDs = m_Renderer->getPickedEntityIDs();

                for (int32_t id : pickedIDs)
                {
                    if (id >= 0)
                    {
                        Entity entity{ static_cast<entt::entity>(id), m_ActiveScene.get() };
                        if (entity)
                        {
                            if (ctrl)
                            {
                                m_SceneHierarchyPanel.ToggleSelectedEntity(entity);
                            }
                            else if (!m_SceneHierarchyPanel.IsSelected(entity))
                            {
                                auto& list = const_cast<std::vector<Entity>&>(m_SceneHierarchyPanel.GetSelectedEntities());
                                list.push_back(entity);
                            }
                        }
                    }
                }
            }

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                {
                    AssetHandle handle = *(const AssetHandle*)payload->Data;
                    auto type = AssetManager::GetAssetType(handle);
                    if (type == AssetType::Scene)
                        OpenScene(handle);
                    else if (type == AssetType::Mesh || type == AssetType::StaticMesh || type == AssetType::MeshSource)
                    {
                        // Get name from metada
                        const AssetMetadata& metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
                        std::string entityName = metadata.FilePath.filename().stem().string();
                        if (entityName.empty())
                            entityName = "Mesh Entity";

                        auto getOrImportTextureHandle =
                            [&](const std::string& texturePath, bool sRGB = true) -> AssetHandle
                        {
                            if (texturePath.empty())
                                return 0;

                            std::filesystem::path pathObj(texturePath);

                            // The importer should already have converted embedded
                            // data:image/... URIs into real files.
                            if (texturePath.starts_with("data:"))
                            {
                                NOX_CORE_ERROR(
                                    "[Scene Drop] Texture is still an embedded data URI: {}",
                                    texturePath.substr(0, 64)
                                );
                                return 0;
                            }

                            std::filesystem::path relPath;

                            if (pathObj.is_absolute())
                            {
                                std::error_code ec;

                                relPath = std::filesystem::relative(
                                    pathObj,
                                    Project::GetActiveAssetDirectory(),
                                    ec
                                );

                                if (ec)
                                {
                                    NOX_CORE_ERROR(
                                        "[Scene Drop] Failed to make texture path relative: {}",
                                        pathObj.string()
                                    );
                                    return 0;
                                }
                            }
                            else
                            {
                                relPath = pathObj;
                            }

                            auto assetManager =
                                Project::GetActive()->GetEditorAssetManager();

                            // Already imported?
                            for (const auto& [texHandle, meta] :
                                 assetManager->GetAssetRegistry())
                            {
                                if (meta.FilePath == relPath ||
                                    meta.SourceFilePath == relPath)
                                {
                                    return texHandle;
                                }
                            }

                            // Make sure the actual file exists before importing.
                            std::filesystem::path fullPath =
                                Project::GetActiveAssetDirectory() / relPath;

                            if (!std::filesystem::exists(fullPath))
                            {
                                NOX_CORE_ERROR(
                                    "[Scene Drop] Texture file does not exist: {}",
                                    fullPath.string()
                                );
                                return 0;
                            }
                                
                            TextureSpecification spec;
                            spec.format = sRGB ? NRI::ImageFormat::SRGBA8 : NRI::ImageFormat::RGBA8;
                                
                            assetManager->ImportAsset(relPath, spec, {});

                            // Find newly imported asset.
                            for (const auto& [texHandle, meta] :
                                 assetManager->GetAssetRegistry())
                            {
                                if (meta.FilePath == relPath ||
                                    meta.SourceFilePath == relPath)
                                {
                                    return texHandle;
                                }
                            }

                            NOX_CORE_ERROR(
                                "[Scene Drop] Failed to import texture: {}",
                                relPath.string()
                            );

                            return 0;
                        };

                        // Check if a cooked .nskel file exists on disk for this mesh
                        std::filesystem::path relSkelPath = metadata.FilePath;
                        relSkelPath.replace_extension(".nskel");

                        std::filesystem::path fullSkelPath = Project::GetActiveAssetDirectory() / relSkelPath;
                        bool hasSkeleton = std::filesystem::exists(fullSkelPath);

                        // Helper to find or import the Skeleton AssetHandle
                        auto getOrImportSkeletonHandle = [&](const std::filesystem::path& relPath) -> AssetHandle
                        {
                            auto assetManager = Project::GetActive()->GetEditorAssetManager();

                            for (const auto& [skelHandle, meta] : assetManager->GetAssetRegistry())
                            {
                                if (meta.FilePath == relPath || meta.SourceFilePath == relPath)
                                    return skelHandle;
                            }

                            assetManager->ImportAsset(relPath, {}, {});

                            for (const auto& [skelHandle, meta] : assetManager->GetAssetRegistry())
                            {
                                if (meta.FilePath == relPath || meta.SourceFilePath == relPath)
                                    return skelHandle;
                            }

                            return 0;
                        };

                        // Helper to attach AnimatorComponent and load its skeleton
                        auto tryAttachAnimator = [&](Entity entity)
                        {
                            if (!hasSkeleton) return;

                            auto& animatorComp = entity.AddComponent<AnimatorComponent>();
                            animatorComp.Skeleton = getOrImportSkeletonHandle(relSkelPath);
                        };
                        
                        // Helper lambda to spawn lights from glTF KHR_lights_punctual
                            auto spawnGltfLights = [&](Entity parent, const std::vector<LightNodeData>& lights)
                            {
                                for (const auto& l : lights)
                                {
                                    Entity lightEntity = m_ActiveScene->CreateEntity(l.Name);
                                    if (parent)
                                        lightEntity.SetParent(parent);

                                    auto& tc = lightEntity.GetComponent<TransformComponent>();
                                    tc.Translation = l.Translation;
                                    tc.Rotation = glm::eulerAngles(l.Rotation);
                                    tc.Scale = l.Scale;

                                    if (l.Type == GltfLightType::Directional)
                                    {
                                        auto& dlc = lightEntity.AddComponent<DirectionalLightComponent>();
                                        dlc.Color = l.Color;
                                        dlc.Intensity = l.Intensity;
                                    }
                                    else if (l.Type == GltfLightType::Point)
                                    {
                                        auto& plc = lightEntity.AddComponent<PointLightComponent>();
                                        plc.Color = l.Color;
                                        plc.Intensity = l.Intensity;
                                        plc.Range = l.Range;
                                    }
                                    else if (l.Type == GltfLightType::Spot)
                                    {
                                        auto& slc = lightEntity.AddComponent<SpotLightComponent>();
                                        slc.Color = l.Color;
                                        slc.Intensity = l.Intensity;
                                        slc.Range = l.Range;
                                        slc.InnerAngle = l.InnerConeAngle;
                                        slc.OuterAngle = l.OuterConeAngle;
                                    }
                                }
                            };

                        // Check if it's a dynamic mesh asset with multiple submeshes
                        if (type == AssetType::Mesh || type == AssetType::MeshSource)
                        {
                            Ref<Mesh> meshAsset = AssetManager::GetAsset<Mesh>(handle);
                            if (meshAsset && meshAsset->GetSubMeshCount() > 1)
                            {
                                // 1. Create a Parent Root Entity for the whole file
                                Entity parentEntity = m_ActiveScene->CreateEntity(entityName);

                                // 2. Create a Child Entity for each submesh
                                for (size_t i = 0; i < meshAsset->GetSubMeshCount(); i++)
                                {
                                    std::string subMeshName = meshAsset->GetSubmeshName(i);
                                    NOX_CORE_INFO("[Scene Drop] Submesh {} name retrieved: '{}'", i, subMeshName);
                                    if (subMeshName.empty())
                                        subMeshName = entityName + "_sub_" + std::to_string(i);

                                    Entity childEntity = m_ActiveScene->CreateEntity(subMeshName);
                                    childEntity.SetParent(parentEntity); // Link via Scene Graph!

                                    auto& meshComp = childEntity.AddComponent<MeshComponent>();
                                    meshComp.Mesh = handle;
                                    meshComp.SubmeshIndex = static_cast<uint32_t>(i);

                                    auto& matComp = childEntity.AddComponent<MaterialComponent>();
                                    const auto& matData = meshAsset->GetMaterial(i);

                                    // Base
                                    matComp.BaseColorFactors = {matData.BaseColorFactor};
                                    matComp.BaseColorMaps = {getOrImportTextureHandle(matData.BaseColorTexturePath, true)};
                                    matComp.BaseColorTextureSets = {matData.BaseColorTextureSet};
                                    
                                    // PBR
                                    matComp.MetallicFactors = {matData.MetallicFactor};
                                    matComp.RoughnessFactors = {matData.RoughnessFactor};
                                    matComp.MetallicRoughnessMaps = {getOrImportTextureHandle(matData.MetallicRoughnessTexturePath, false)};
                                    matComp.PhysicalDescriptorTextureSets = {matData.PhysicalDescriptorTextureSet};
                                    
                                    matComp.NormalMaps = {getOrImportTextureHandle(matData.NormalTexturePath, false)};
                                    matComp.NormalTextureSets = {matData.NormalTextureSet};
                                    
                                    matComp.OcclusionMaps = {getOrImportTextureHandle(matData.OcclusionTexturePath, false)};
                                    matComp.OcclusionTextureSets = {matData.OcclusionTextureSet};
                                    
                                    // Emission
                                    matComp.EmissiveFactors = {matData.EmissiveFactor};
                                    matComp.EmissiveMaps = {getOrImportTextureHandle(matData.EmissiveTexturePath, true)};
                                    matComp.EmissiveTextureSets = {matData.EmissiveTextureSet};
                                    matComp.EmissiveStrengths = {matData.emissiveStrength};
                                    
                                    // Transmission
                                    matComp.TransmissionFactors = {matData.TransmissionFactor};
                                    matComp.TransmissionMaps = {getOrImportTextureHandle(matData.TransmissionTexturePath, false)};
                                    matComp.TransmissionTextureSets = {matData.TransmissionTextureSet};
                                    
                                    matComp.Modes = {matData.Mode};
                                    matComp.AlphaMaskCutoffs = {matData.AlphaMaskCutoff};
                                    matComp.DoubleSidedFlags = {matData.DoubleSided};
                                    matComp.UnlitFlags = { matData.Unlit };

                                    // Auto-attach AnimatorComponent if skeleton exists
                                    tryAttachAnimator(childEntity);
                                }
                                
                                if (meshAsset)
                                    spawnGltfLights(parentEntity, meshAsset->GetLights());

                                m_SceneHierarchyPanel.SetSelectedEntity(parentEntity);
                            }
                            else
                            {
                                // Single submesh dynamic mesh
                                Entity newEntity = m_ActiveScene->CreateEntity(entityName);
                                auto& meshComp = newEntity.AddComponent<MeshComponent>();
                                meshComp.Mesh = handle;
                                meshComp.SubmeshIndex = 0;

                                auto& matComp = newEntity.AddComponent<MaterialComponent>();
                                if (meshAsset && !meshAsset->GetMaterials().empty())
                                {
                                    const auto& matData = meshAsset->GetMaterial(0);
                                    
                                    // Base
                                    matComp.BaseColorFactors = {matData.BaseColorFactor};
                                    matComp.BaseColorMaps = {getOrImportTextureHandle(matData.BaseColorTexturePath, true)};
                                    matComp.BaseColorTextureSets = {matData.BaseColorTextureSet};
                                    
                                    // PBR
                                    matComp.MetallicFactors = {matData.MetallicFactor};
                                    matComp.RoughnessFactors = {matData.RoughnessFactor};
                                    matComp.MetallicRoughnessMaps = {getOrImportTextureHandle(matData.MetallicRoughnessTexturePath, false)};
                                    matComp.PhysicalDescriptorTextureSets = {matData.PhysicalDescriptorTextureSet};
                                    
                                    matComp.NormalMaps = {getOrImportTextureHandle(matData.NormalTexturePath, false)};
                                    matComp.NormalTextureSets = {matData.NormalTextureSet};
                                    
                                    matComp.OcclusionMaps = {getOrImportTextureHandle(matData.OcclusionTexturePath, false)};
                                    matComp.OcclusionTextureSets = {matData.OcclusionTextureSet};
                                    
                                    // Emission
                                    matComp.EmissiveFactors = {matData.EmissiveFactor};
                                    matComp.EmissiveMaps = {getOrImportTextureHandle(matData.EmissiveTexturePath, true)};
                                    matComp.EmissiveTextureSets = {matData.EmissiveTextureSet};
                                    matComp.EmissiveStrengths = {matData.emissiveStrength};
                                    
                                    // Transmission
                                    matComp.TransmissionFactors = {matData.TransmissionFactor};
                                    matComp.TransmissionMaps = {getOrImportTextureHandle(matData.TransmissionTexturePath, false)};
                                    matComp.TransmissionTextureSets = {matData.TransmissionTextureSet};
                                    
                                    matComp.Modes = {matData.Mode};
                                    matComp.AlphaMaskCutoffs = {matData.AlphaMaskCutoff};
                                    matComp.DoubleSidedFlags = {matData.DoubleSided};
                                    matComp.UnlitFlags = { matData.Unlit };
                                }
                                else
                                {
                                    matComp.BaseColorFactors = {glm::vec4(1.0f)};
                                    matComp.MetallicFactors = {1.0f};
                                    matComp.RoughnessFactors = {1.0f};
                                    matComp.EmissiveFactors = {glm::vec3(0.0f)};
                                    matComp.TransmissionFactors = {0.0f};
                                }

                                tryAttachAnimator(newEntity);
                                
                                if (meshAsset)
                                    spawnGltfLights(newEntity, meshAsset->GetLights());

                                m_SceneHierarchyPanel.SetSelectedEntity(newEntity);
                            }
                        }
                        else // StaticMesh (.nsmesh) - always single flattened mesh
                        {
                            Ref<StaticMesh> staticMeshAsset = AssetManager::GetAsset<StaticMesh>(handle);
                            Entity newEntity = m_ActiveScene->CreateEntity(entityName);
                            auto& meshComp = newEntity.AddComponent<MeshComponent>();
                            meshComp.Mesh = handle;
                            meshComp.SubmeshIndex = 0;

                            auto& matComp = newEntity.AddComponent<MaterialComponent>();

                            if (staticMeshAsset)
                            {
                                size_t subMeshCount = staticMeshAsset->GetSubMeshCount();
                                matComp.BaseColorFactors.resize(subMeshCount);
                                matComp.BaseColorMaps.resize(subMeshCount, 0);
                                matComp.BaseColorTextureSets.resize(subMeshCount, 0);
                                
                                matComp.MetallicFactors.resize(subMeshCount, 1.0f);
                                matComp.RoughnessFactors.resize(subMeshCount, 1.0f);
                                matComp.MetallicRoughnessMaps.resize(subMeshCount, 0);
                                matComp.PhysicalDescriptorTextureSets.resize(subMeshCount, 0);
                                
                                matComp.NormalMaps.resize(subMeshCount, 0);
                                matComp.NormalTextureSets.resize(subMeshCount, 0);
                                
                                matComp.OcclusionMaps.resize(subMeshCount, 0);
                                matComp.OcclusionTextureSets.resize(subMeshCount, 0);
                                
                                matComp.EmissiveFactors.resize(subMeshCount, glm::vec3(0.0f));
                                matComp.EmissiveMaps.resize(subMeshCount, 0);
                                matComp.EmissiveTextureSets.resize(subMeshCount, 0);
                                matComp.EmissiveStrengths.resize(subMeshCount, 1.0f);
                                
                                matComp.TransmissionFactors.resize(subMeshCount, 0.0f);
                                matComp.TransmissionMaps.resize(subMeshCount, 0);
                                matComp.TransmissionTextureSets.resize(subMeshCount, 0);
                                
                                matComp.Modes.resize(subMeshCount, AlphaMode::Opaque);
                                matComp.AlphaMaskCutoffs.resize(subMeshCount, 0.5f);
                                matComp.DoubleSidedFlags.resize(subMeshCount, false);
                                matComp.UnlitFlags.resize(subMeshCount, false);

                                // Resolve paths to AssetHandles for each slot
                                for (size_t i = 0; i < staticMeshAsset->GetSubMeshCount(); ++i)
                                {
                                    const auto& matData = staticMeshAsset->GetMaterial(i);

                                    matComp.BaseColorFactors[i] = matData.BaseColorFactor;
                                    matComp.BaseColorMaps[i] = getOrImportTextureHandle(matData.BaseColorTexturePath, true);
                                    matComp.BaseColorTextureSets[i] = matData.BaseColorTextureSet;
                                    
                                    matComp.MetallicFactors[i] = matData.MetallicFactor;
                                    matComp.RoughnessFactors[i] = matData.RoughnessFactor;
                                    matComp.MetallicRoughnessMaps[i] = getOrImportTextureHandle(matData.MetallicRoughnessTexturePath, false);
                                    matComp.PhysicalDescriptorTextureSets[i] = matData.PhysicalDescriptorTextureSet;
                                    
                                    matComp.NormalMaps[i] = getOrImportTextureHandle(matData.NormalTexturePath, false);
                                    matComp.NormalTextureSets[i] = matData.NormalTextureSet;
                                    
                                    matComp.OcclusionMaps[i] = getOrImportTextureHandle(matData.OcclusionTexturePath, false);
                                    matComp.OcclusionTextureSets[i] = matData.OcclusionTextureSet;
                                    
                                    matComp.EmissiveFactors[i] = matData.EmissiveFactor;
                                    matComp.EmissiveMaps[i] = getOrImportTextureHandle(matData.EmissiveTexturePath, true);
                                    matComp.EmissiveTextureSets[i] = matData.EmissiveTextureSet;
                                    matComp.EmissiveStrengths[i] = matData.emissiveStrength;
                                    
                                    matComp.TransmissionFactors[i] = matData.TransmissionFactor;
                                    matComp.TransmissionMaps[i] = getOrImportTextureHandle(matData.TransmissionTexturePath, false);
                                    matComp.TransmissionTextureSets[i] = matData.TransmissionTextureSet;
                                    
                                    matComp.Modes[i] = matData.Mode;
                                    matComp.AlphaMaskCutoffs[i] = matData.AlphaMaskCutoff;
                                    matComp.DoubleSidedFlags[i] = matData.DoubleSided;
                                    matComp.UnlitFlags[i] = matData.Unlit;
                                }
                            }
                            
                            if (staticMeshAsset)
                                spawnGltfLights(newEntity, staticMeshAsset->GetLights());

                            m_SceneHierarchyPanel.SetSelectedEntity(newEntity);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Gizmos
            //maybe be a callback you subscribe to instead
            Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
            if (selectedEntity && m_GizmoType != -1)
            {
                ImGuizmo::SetOrthographic(false); // maybe needed later for setortho camera
                ImGuizmo::SetDrawlist();

                ImGuizmo::SetRect(m_ViewportBounds[0].x, m_ViewportBounds[0].y, m_ViewportBounds[1].x - m_ViewportBounds[0].x, m_ViewportBounds[1].y - m_ViewportBounds[0].y);

                // Camera

                // Runtime camera from entity
                // auto cameraEntity = m_ActiveScene->GetPrimaryCameraEntity();
                // const auto& camera = cameraEntity.GetComponent<CameraComponent>().Camera;
                // const glm::mat4& cameraProjection = camera.GetProjection();
                // glm::mat4 cameraView = glm::inverse(cameraEntity.GetComponent<TransformComponent>().GetTransform());

                // Editor camera
                const glm::mat4& cameraProjection = m_EditorCamera.GetGizmoProjection();
                glm::mat4 cameraView = m_EditorCamera.GetGizmoView();

                // Grab the WORLD transform for ImGuizmo
                glm::mat4 worldTransform = selectedEntity.GetComponent<WorldTransformComponent>().WorldMatrix;

                // Snapping
                bool snap = Input::IsKeyPressed(SDL_SCANCODE_LCTRL);
                float snapValue = 0.5f; // Snap to 0.5m for translation/scale
                // Snap to 45 degrees for rotation
                if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
                {
                    snapValue = 45.0f;
                }

                float snapValues[3] = {snapValue, snapValue, snapValue};

                glm::mat4 deltaMatrix(1.0f);
                
                ImGuizmo::Manipulate(glm::value_ptr(cameraView), glm::value_ptr(cameraProjection),
                                     static_cast<ImGuizmo::OPERATION>(m_GizmoType), ImGuizmo::LOCAL,
                                     glm::value_ptr(worldTransform), glm::value_ptr(deltaMatrix), snap ? snapValues : nullptr);

                if (ImGuizmo::IsUsing())
                {
                    const auto& selectedEntities = m_SceneHierarchyPanel.GetSelectedEntities();
                    if (selectedEntities.size() > 1)
                    {
                        for (auto entity : selectedEntities)
                        {
                            if (!entity) continue;

                            // If this entity's parent is ALSO selected, skip it (the parent moving will already move it)
                            if (entity.HasComponent<RelationshipComponent>())
                            {
                                UUID parentUUID = entity.GetComponent<RelationshipComponent>().Parent;
                                if (parentUUID != 0)
                                {
                                    Entity parent = m_ActiveScene->GetEntityByUUID(parentUUID);
                                    if (parent && m_SceneHierarchyPanel.IsSelected(parent))
                                        continue;
                                }
                            }

                            glm::mat4 currentWorld = entity.GetComponent<WorldTransformComponent>().WorldMatrix;
                            entity.SetWorldTransform(deltaMatrix * currentWorld);
                        }
                    }
                    else
                    {
                        // One single line does all the math, finds the parent, and marks it dirty!
                        selectedEntity.SetWorldTransform(worldTransform);
                    }
                }
            }

            ImGui::End(); // End viewport
            ImGui::PopStyleVar();
        }

        // Extra ImGui windows can be added in OnImGuiRender() layer, like the demo window.
        // ImGui::ShowDemoWindow();

        m_SceneHierarchyPanel.OnImGuiRender();
        m_ContentBrowserPanel->OnImGuiRender();

        // "Right" Window
        ImGui::Begin("Stats");

        std::string name = "None";
        if (m_HoveredEntity && m_HoveredEntity.HasComponent<TagComponent>())
        {
            name = m_HoveredEntity.GetComponent<TagComponent>().Tag;
        }
        ImGui::Text("Hovered Entity: %s", name.c_str());

        ImGui::Spacing();

        ImGui::Text("ImGui ActiveID: %u", Application::Get().GetLayer<ImGuiLayer>()->GetActiveWidgetID());
        ImGui::End(); // End "right" Window

        ImGui::Begin("Settings");

        ImGui::Checkbox("Show physics collider", &m_ShowPhysicsColliders);
        ImGui::Image(m_Font->GetAtlasTexture()->getImTextureID(), {512, 512}, ImVec2(0, 1), ImVec2(1, 0));

        ImGui::End(); // End Settings


        UI_ToolBar();
    }

    void EditorLayer::UI_ToolBar()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 2});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2{0, 0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2{0, 50});
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
        auto& colors = ImGui::GetStyle().Colors; //imguilayer styling ganz unten
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{
                                  colors[ImGuiCol_ButtonHovered].x, colors[ImGuiCol_ButtonHovered].y,
                                  colors[ImGuiCol_ButtonHovered].z, 0.5f
                              });
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{
                                  colors[ImGuiCol_ButtonActive].x, colors[ImGuiCol_ButtonActive].y,
                                  colors[ImGuiCol_ButtonActive].z, 0.5f
                              });

        ImGui::Begin("##toolbar", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImGui::SetWindowSize(ImVec2(ImGui::GetWindowWidth(), 40.0f));

        float size = ImGui::GetWindowHeight() - 4.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowContentRegionMax().x * 0.5f) - (size * 0.5f));

        bool hasPlayButton = m_SceneState == SceneState::Edit || m_SceneState == SceneState::Play;
        bool hasSimulateButton = m_SceneState == SceneState::Edit || m_SceneState == SceneState::Simulate;
        bool hasPauseButton = m_SceneState != SceneState::Edit;

        if (hasPlayButton)
        {
            {
                Ref<Texture2D> icon = (m_SceneState == SceneState::Edit || m_SceneState == SceneState::Simulate) ? m_IconPlay : m_IconStop;
                if (ImGui::ImageButton("##icon", icon->getImTextureID(), ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1),
                                       ImVec4(0, 0, 0, 0)))
                {
                    if (hasSimulateButton)
                    {
                        OnScenePlay();
                    }
                    else if (m_SceneState == SceneState::Play)
                    {
                        OnSceneStop();
                    }
                }
            }
        }
        if (hasSimulateButton)
        {
            if (hasPlayButton)
                ImGui::SameLine();
            {
                Ref<Texture2D> icon = (m_SceneState == SceneState::Edit || m_SceneState == SceneState::Play) ? m_IconSimulate : m_IconStop;
                if (ImGui::ImageButton("##icon2", icon->getImTextureID(), ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1),
                                       ImVec4(0, 0, 0, 0)))
                {
                    if (m_SceneState == SceneState::Edit || m_SceneState == SceneState::Play)
                    {
                        OnSceneSimulate();
                    }
                    else if (m_SceneState == SceneState::Simulate)
                    {
                        OnSceneStop();
                    }
                }
            }
        }
        if (hasPauseButton)
        {
            bool isPaused = m_ActiveScene->IsPaused();
            ImGui::SameLine();
            {
                Ref<Texture2D> icon = m_IconPause;
                if (ImGui::ImageButton("##icon3", icon->getImTextureID(), ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1),
                                       ImVec4(0, 0, 0, 0)))
                {
                    m_ActiveScene->SetPaused(!isPaused);
                }
            }

            // Step button
            if (isPaused)
            {
                ImGui::SameLine();
                {
                    Ref<Texture2D> icon = m_IconStep;
                    if (ImGui::ImageButton("##icon4", icon->getImTextureID(), ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1),
                                           ImVec4(0, 0, 0, 0)))
                    {
                        m_ActiveScene->Step(); // make this tweakableinside imgui instead of hardcoding 1
                    }
                }
            }
        }
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(3);
        ImGui::End();
    }

    bool EditorLayer::OnKeyPressed(KeyPressedEvent& e)
    {
        // 1. Abort if the user is typing in an ImGui text field
        if (ImGui::GetIO().WantTextInput)
            return false;

        // Shortcuts
        if (e.IsRepeat())
        {
            return false;
        }


        bool control = (Input::IsKeyPressed(SDL_SCANCODE_LCTRL) || Input::IsKeyPressed(SDL_SCANCODE_RCTRL));
        bool shift = (Input::IsKeyPressed(SDL_SCANCODE_LSHIFT) || Input::IsKeyPressed(SDL_SCANCODE_RSHIFT));

        switch (e.GetKeyCode())
        {
        case SDL_SCANCODE_N:
            {
                if (control)
                {
                    NewScene();
                }
                break;
            }
        case SDL_SCANCODE_O:
            {
                if (control)
                {
                    OpenProject();
                }
                break;
            }
        case SDL_SCANCODE_S:
            {
                if (control)
                {
                    if (shift)
                        SaveSceneAs();
                    else
                        SaveScene();
                }
                break;
            }

        // Scene Commands
        case SDL_SCANCODE_D:
            {
                if (control)
                {
                    OnDuplicateEntity();
                }
                break;
            }

        // Gizmos
        case SDL_SCANCODE_Q:
            {
                if (m_ViewportHovered)
                    m_GizmoType = -1;
                break;
            }
        case SDL_SCANCODE_W:
            {
                if (m_ViewportHovered)
                    m_GizmoType = ImGuizmo::OPERATION::TRANSLATE;
                break;
            }
        case SDL_SCANCODE_E:
            {
                if (m_ViewportHovered)
                    m_GizmoType = ImGuizmo::OPERATION::ROTATE;
                break;
            }
        case SDL_SCANCODE_R:
            if (control)
            {
                /*ScriptEngine::ReloadAssembly();*/
            }
            else
            {
                m_GizmoType = ImGuizmo::OPERATION::SCALE;
            }
            break;
        case SDL_SCANCODE_DELETE:
            {
                if (Application::Get().GetLayer<ImGuiLayer>()->GetActiveWidgetID() == 0)
                {
                    Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
                    if (selectedEntity)
                    {
                        m_SceneHierarchyPanel.SetSelectedEntity({});
                        m_ActiveScene->DestroyEntity(selectedEntity);
                    }
                }
                break;
            }
        default:
            break;
        }

        return false;
    }

    bool EditorLayer::IsButtonHovered() const
    {
        return true; //maybe useful for imgui ImGui::IsItemHovered()
    }

    bool EditorLayer::OnMouseButtonPressed(MouseButtonPressedEvent& event)
    {
        if (event.GetMouseButton() == SDL_BUTTON_LEFT)
        {
            if (m_ViewportHovered && !ImGuizmo::IsOver() && !Input::IsKeyPressed(SDL_SCANCODE_LALT))
            {
                bool shift = Input::IsKeyPressed(SDL_SCANCODE_LSHIFT) || Input::IsKeyPressed(SDL_SCANCODE_RSHIFT);
                bool control = Input::IsKeyPressed(SDL_SCANCODE_LCTRL) || Input::IsKeyPressed(SDL_SCANCODE_RCTRL);

                if (m_HoveredEntity)
                {
                    if (shift)
                        m_SceneHierarchyPanel.SelectRange(m_HoveredEntity);
                    else if (control)
                        m_SceneHierarchyPanel.ToggleSelectedEntity(m_HoveredEntity);
                    else
                        m_SceneHierarchyPanel.SetSelectedEntity(m_HoveredEntity);
                }
                else
                {
                    // Clicking empty space: clear selection unless Shift/Ctrl is held
                    if (!shift && !control)
                    {
                        m_SceneHierarchyPanel.ClearSelection();
                    }
                }
            }
        }

        return false;
    }

    void EditorLayer::OnOverlayRender()
    {
        if (m_SceneState == SceneState::Play)
        {
            Entity camera = m_ActiveScene->GetPrimaryCameraEntity();
            if (!camera)
                return;

            m_Renderer->BeginScene(camera.GetComponent<CameraComponent>().Camera, camera.GetComponent<TransformComponent>().GetTransform());
        }
        else
        {
            m_Renderer->BeginScene(m_EditorCamera);
        }

        if (m_ShowPhysicsColliders)
        {
            // Box Colliders
            {
                auto view = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, BoxCollider2DComponent>();
                for (auto entity : view)
                {
                    auto [wtc, bc2d] = view.get<WorldTransformComponent, BoxCollider2DComponent>(entity);

                    /*glm::vec3 translation = tc.Translation + glm::vec3(bc2d.Offset, 0.001f);
                    glm::vec3 scale = tc.Scale * glm::vec3(bc2d.Size * 2.0f, 1.0f);

                    // box2d needs first translation then offset otherwise it offsets the bounding box from center instead of creating from center around
                    glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation)
                        * glm::rotate(glm::mat4(1.0f), tc.Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f))
                        * glm::translate(glm::mat4(1.0f), glm::vec3(bc2d.Offset, 0.001f))
                        * glm::scale(glm::mat4(1.0f), scale * glm::vec3(bc2d.Size * 2.0f, 1.0f));*/

                    glm::mat4 transform = wtc.WorldMatrix
                        * glm::translate(glm::mat4(1.0f), glm::vec3(bc2d.Offset, 0.001f))
                        * glm::scale(glm::mat4(1.0f), glm::vec3(bc2d.Size * 2.0f, 1.0f));

                    m_Renderer2D->DrawRect(transform, glm::vec4(0, 1, 0, 1));
                }
            }
            // Circle Colliders
            {
                auto view = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, CircleCollider2DComponent>();
                for (auto entity : view)
                {
                    auto [wtc, cc2d] = view.get<WorldTransformComponent, CircleCollider2DComponent>(entity);

                    /*glm::vec3 translation = tc.Translation + glm::vec3(cc2d.Offset, 0.001f);
                    glm::vec3 scale = tc.Scale * glm::vec3(cc2d.Radius * 2.0f);

                    glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation)
                        * glm::scale(glm::mat4(1.0f), scale);*/

                    glm::mat4 transform = wtc.WorldMatrix
                        * glm::translate(glm::mat4(1.0f), glm::vec3(cc2d.Offset, 0.001f))
                        * glm::scale(glm::mat4(1.0f), glm::vec3(cc2d.Radius * 2.0f));

                    m_Renderer2D->DrawCircle(transform, glm::vec4(0, 1, 0, 1), 0.01f);
                }
            }
        }

        // Draw selected entity outline 2D and 3D
        const auto& selectedEntities = m_SceneHierarchyPanel.GetSelectedEntities();
        if (!selectedEntities.empty())
        {
            std::vector<int32_t> selectedIDs;
            selectedIDs.reserve(selectedEntities.size());

            for (const Entity& entity : selectedEntities)
            {
                if (entity)
                {
                    selectedIDs.push_back(static_cast<int32_t>(static_cast<uint32_t>(entity)));
                }
            }

            m_Renderer->SetSelectedEntityID(selectedIDs);
        }
        else
        {
            m_Renderer->SetSelectedEntityID({});
        }
        
        /*if (Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity())
        {
            glm::mat4 transform = glm::mat4(1.0f);

            // Use the WorldMatrix so the outline respects parent transformations!
            if (selectedEntity.HasComponent<WorldTransformComponent>())
            {
                transform = selectedEntity.GetComponent<WorldTransformComponent>().WorldMatrix;
            }
            else if (selectedEntity.HasComponent<TransformComponent>())
            {
                // Fallback just in case an entity somehow doesn't have a WorldTransformComponent yet
                transform = selectedEntity.GetComponent<TransformComponent>().GetTransform();
            }
            m_Renderer2D->DrawRect(transform, glm::vec4(1.0f, 0.5f, 0.0f, 1.0f));
            /*const TransformComponent& transform = selectedEntity.GetComponent<TransformComponent>();
            m_Renderer2D->DrawRect(transform.GetTransform(), glm::vec4(1.0f, 0.5f, 0.0f, 1.0f));#1#
        }*/
        
        m_Renderer->EndScene();
    }

    void EditorLayer::NewProject()
    {
        Project::New();
    }

    bool EditorLayer::OpenProject()
    {
        std::string filepath = "E:/dev/noxiouse/Facerun/Facerun.nproj"/*Utility::OpenFile("Nox Project *.nproj\0nproj\0")*/;

        if (filepath.empty())
            return false;

        OpenProject(filepath);
        return true;
    }

    void EditorLayer::OpenProject(const std::filesystem::path& path)
    {
        if (Project::Load(path))
        {
            /*ScriptEngine::Init();*/

            AssetHandle startScene = Project::GetActive()->GetConfig().StartScene;
            if (startScene)
                OpenScene(startScene);

            m_ContentBrowserPanel = CreateScope<ContentBrowserPanel>(Project::GetActive());
        }
    }

    void EditorLayer::SaveProject()
    {
        //Project::SaveActive();
    }

    void EditorLayer::NewScene()
    {
        m_HoveredEntity = Entity();
        m_SceneHierarchyPanel.SetSelectedEntity(Entity());

        m_EditorScene = CreateRef<Scene>();
        m_ActiveScene = m_EditorScene;

        m_SceneHierarchyPanel.SetContext(m_ActiveScene);

        m_EditorScenePath = std::filesystem::path();
    }

    void EditorLayer::OpenScene()
    {
        /*std::string filepath = Utility::OpenFile("Nox Scene *.nox\0nox\0");
        NOX_CORE_ERROR("openscene {0}", filepath);
        if (!filepath.empty())
        {
            OpenScene(filepath);
        }*/
    }

    void EditorLayer::OpenScene(AssetHandle handle)
    {
        NOX_CORE_ASSERT(handle);

        if (m_SceneState != SceneState::Edit)
        {
            OnSceneStop();
        }

        Ref<Scene> readOnlyScene = AssetManager::GetAsset<Scene>(handle);
        Ref<Scene> newScene = Scene::Copy(readOnlyScene);

        m_EditorScene = newScene;
        m_SceneHierarchyPanel.SetContext(m_EditorScene);

        m_ActiveScene = m_EditorScene;
        m_EditorScenePath = Project::GetActive()->GetEditorAssetManager()->GetFilePath(handle);
    }

    void EditorLayer::SaveScene()
    {
        if (!m_EditorScenePath.empty())
        {
            SerializeScene(m_ActiveScene, m_EditorScenePath);
        }
        else
        {
            SaveSceneAs();
        }
    }

    void EditorLayer::SaveSceneAs()
    {
        std::string filepath = Utility::SaveFile("Nox Scene *.nox\0nox\0");
        if (!filepath.empty())
        {
            SerializeScene(m_ActiveScene, filepath);
            m_EditorScenePath = filepath;
        }
    }

    void EditorLayer::SerializeScene(Ref<Scene> scene, const std::filesystem::path& path)
    {
        SceneImporter::SaveScene(scene, path);
    }

    void EditorLayer::OnScenePlay()
    {
        if (m_SceneState == SceneState::Simulate)
            OnSceneStop();

        m_SceneState = SceneState::Play;

        m_ActiveScene = Scene::Copy(m_EditorScene);
        m_ActiveScene->OnRuntimeStart();

        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
    }

    void EditorLayer::OnSceneSimulate()
    {
        if (m_SceneState == SceneState::Play)
            OnSceneStop();

        m_SceneState = SceneState::Simulate;

        m_ActiveScene = Scene::Copy(m_EditorScene);
        m_ActiveScene->OnSimulationStart();

        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
    }

    void EditorLayer::OnSceneStop()
    {
        NOX_CORE_ASSERT("OnSceneStop failed no sceneState match", m_SceneState == SceneState::Play || m_SceneState == SceneState::Simulate);

        if (m_SceneState == SceneState::Play)
            m_ActiveScene->OnRuntimeStop();
        else if (m_SceneState == SceneState::Simulate)
            m_ActiveScene->OnSimulationStop();

        m_SceneState = SceneState::Edit;

        m_ActiveScene = m_EditorScene;

        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
    }

    void EditorLayer::OnScenePause()
    {
        if (m_SceneState == SceneState::Edit)
            return;

        m_ActiveScene->SetPaused(true);
    }

    void EditorLayer::OnDuplicateEntity()
    {
        if (m_SceneState != SceneState::Edit)
        {
            return;
        }

        Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
        if (selectedEntity)
        {
            Entity newEntity = m_EditorScene->DuplicateEntity(selectedEntity);
            m_SceneHierarchyPanel.SetSelectedEntity(newEntity);
        }
    }
}
