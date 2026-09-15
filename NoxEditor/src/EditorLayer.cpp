#include "EditorLayer.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>

#include <imgui.h>
#include <imgui_internal.h>// For Docking
#include <ImGuizmo.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>  // for pointer to matrix or vector

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/MeshSerializer.h"
#include "NoxCore/Asset/SceneImporter.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Input.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/ImGui/ImGuiLayer.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Profiling/StatsOverlayLayer.h"
#include "NoxCore/Profiling/StatsReport.h"
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

        // These editor icon .ktx2 files carry a KTXorientation tag claiming bottom-up (Y=up)
        // storage even though their actual pixel data is top-down like everything else in this
        // project - flip=true cancels out TextureImporter's automatic orientation correction so
        // they render the same as before that correction was added.
        m_IconPlay = TextureImporter::LoadTexture2D("assets/Icons/PlayButton.ktx2", {.flip = true, .generateMips = false});
        m_IconStop = TextureImporter::LoadTexture2D("assets/Icons/StopButton.ktx2", {.flip = true, .generateMips = false});
        m_IconPause = TextureImporter::LoadTexture2D("assets/Icons/PauseButton.ktx2", {.flip = true, .generateMips = false});
        m_IconSimulate = TextureImporter::LoadTexture2D("assets/Icons/SimulateButton.ktx2", {.flip = true, .generateMips = false});
        m_IconStep = TextureImporter::LoadTexture2D("assets/Icons/StepButton.ktx2", {.flip = true, .generateMips = false});

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

        m_EditorCamera = EditorCamera(30.0f, 1.778f, 0.01f, 1000.0f);

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
        dispatcher.Dispatch<ExternalFileDropEvent>([this](ExternalFileDropEvent& drop)
        {
            if (m_ContentBrowserPanel)
                m_ContentBrowserPanel->OnExternalFileDrop(drop.GetPath());
            return false;
        });
        dispatcher.Dispatch<KeyPressedEvent>(Nox_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
        dispatcher.Dispatch<MouseButtonPressedEvent>(Nox_BIND_EVENT_FN(EditorLayer::OnMouseButtonPressed));
    }

    void EditorLayer::OnUpdate(Timestep ts)
    {
        // Deleting entities, switching scenes, or leaving Play can leave meshes/textures unreferenced.
        // Sweep at the start of the next frame, before anything re-requests them this frame.
        if (m_EditorScene && m_EditorScene->ConsumeAssetReferencesChanged())
            m_UnloadUnusedAssetsRequested = true;
        if (m_ActiveScene && m_ActiveScene != m_EditorScene && m_ActiveScene->ConsumeAssetReferencesChanged())
            m_UnloadUnusedAssetsRequested = true;
        if (m_UnloadUnusedAssetsRequested)
        {
            NOX_PROFILE_SCOPE("Unload Unused Assets");
            m_UnloadUnusedAssetsRequested = false;
            UnloadUnusedAssets();
        }

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

        {
            NOX_PROFILE_SCOPE("Scene Update");
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

        {
            NOX_PROFILE_SCOPE("Asset Manager Update");
            Project::GetActive()->GetEditorAssetManager()->Update();
        }
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

            // A cancelled drag has no delivery callback, so remove its temporary entity here.
            if (m_PlacementPreview.Active && !ImGui::IsDragDropActive())
            {
                if (m_SceneHierarchyPanel.GetSelectedEntity() == m_PlacementPreview.Root)
                    m_SceneHierarchyPanel.ClearSelection();
                if (m_PlacementPreview.Root)
                    m_ActiveScene->DestroyEntity(m_PlacementPreview.Root);
                m_PlacementPreview = {};
            }

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
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                        "CONTENT_BROWSER_ITEM", ImGuiDragDropFlags_AcceptBeforeDelivery))
                {
                    AssetHandle handle = *(const AssetHandle*)payload->Data;
                    auto type = AssetManager::GetAssetType(handle);
                    if (type == AssetType::Scene && payload->Delivery)
                        OpenScene(handle);
                    else if (type == AssetType::Mesh || type == AssetType::StaticMesh || type == AssetType::MeshSource)
                    {
                        if (!m_PlacementPreview.Active || m_PlacementPreview.Handle != handle)
                        {
                            if (m_PlacementPreview.Active &&
                                m_SceneHierarchyPanel.GetSelectedEntity() == m_PlacementPreview.Root)
                            {
                                m_SceneHierarchyPanel.ClearSelection();
                            }
                            if (m_PlacementPreview.Active && m_PlacementPreview.Root)
                                m_ActiveScene->DestroyEntity(m_PlacementPreview.Root);
                            m_PlacementPreview = {};

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

                            std::filesystem::path cookedRelPath = relPath;
                            cookedRelPath.replace_extension(".ntex");

                            // Prefer the cooked texture when the source was already imported.
                            for (const auto& [texHandle, meta] :
                                 assetManager->GetAssetRegistry())
                            {
                                if (meta.FilePath == cookedRelPath &&
                                    meta.Type == AssetType::Texture2D)
                                {
                                    return texHandle;
                                }
                            }

                            // Existing source records can still be used for formats that are
                            // not cooked by the texture importer.
                            for (const auto& [texHandle, meta] :
                                 assetManager->GetAssetRegistry())
                            {
                                if ((meta.FilePath == relPath ||
                                     meta.SourceFilePath == relPath) &&
                                    meta.Type == AssetType::Texture2D &&
                                    !std::filesystem::exists(
                                        Project::GetActiveAssetDirectory() / cookedRelPath))
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

                            // Find newly imported cooked asset.
                            for (const auto& [texHandle, meta] :
                                 assetManager->GetAssetRegistry())
                            {
                                if (meta.FilePath == cookedRelPath &&
                                    meta.Type == AssetType::Texture2D)
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

                        // A glTF animation does not imply skinning. Bistro, for example, has node
                        // animations (Vespa parts, a basket, a light) but no skins at all. Resolve this once
                        // and only attach an animator when the imported skeleton actually contains
                        // joints; otherwise every mesh node would run a full skeleton update.
                        AssetHandle resolvedSkeletonHandle = 0;
                        Ref<Skeleton> resolvedSkeleton;
                        bool skeletonResolved = false;

                        // Helper to attach AnimatorComponent and load its skeleton
                        auto tryAttachAnimator = [&](Entity entity)
                        {
                            if (!hasSkeleton) return;

                            if (!skeletonResolved)
                            {
                                resolvedSkeletonHandle = getOrImportSkeletonHandle(relSkelPath);
                                if (resolvedSkeletonHandle != 0)
                                    resolvedSkeleton = AssetManager::GetAsset<Skeleton>(resolvedSkeletonHandle);
                                skeletonResolved = true;
                            }

                            if (!resolvedSkeleton || resolvedSkeleton->Skins.empty())
                                return;

                            auto& animatorComp = entity.AddComponent<AnimatorComponent>();
                            animatorComp.Skeleton = resolvedSkeletonHandle;
                        };

                        // Every clip cooked from this glTF, sorted by file path so the default clip
                        // doesn't depend on the registry's unordered_map iteration order.
                        auto findImportAnimations = [&]() -> std::vector<AssetHandle>
                        {
                            const std::filesystem::path animationDirectory = metadata.FilePath.parent_path();
                            const std::string animationPrefix = metadata.FilePath.stem().string() + "_";
                            std::vector<std::pair<std::string, AssetHandle>> found;
                            for (const auto& [animationHandle, animationMetadata] :
                                 Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry())
                            {
                                if (animationMetadata.Type != AssetType::AnimationSequence ||
                                    animationMetadata.FilePath.parent_path() != animationDirectory)
                                    continue;

                                const std::string animationStem = animationMetadata.FilePath.stem().string();
                                if (animationStem.starts_with(animationPrefix))
                                    found.emplace_back(animationMetadata.FilePath.generic_string(), animationHandle);
                            }
                            std::sort(found.begin(), found.end());

                            std::vector<AssetHandle> handles;
                            for (const auto& [path, animationHandle] : found)
                                handles.push_back(animationHandle);
                            return handles;
                        };

                        auto addLightComponent = [&](Entity entity, const LightNodeData& l)
                        {
                            if (l.Type == GltfLightType::Directional)
                            {
                                auto& dlc = entity.AddComponent<DirectionalLightComponent>();
                                dlc.Color = l.Color;
                                dlc.Intensity = l.Intensity;
                            }
                            else if (l.Type == GltfLightType::Point)
                            {
                                auto& plc = entity.AddComponent<PointLightComponent>();
                                plc.Color = l.Color;
                                plc.Intensity = l.Intensity;
                                plc.Range = l.Range;
                            }
                            else if (l.Type == GltfLightType::Spot)
                            {
                                auto& slc = entity.AddComponent<SpotLightComponent>();
                                slc.Color = l.Color;
                                slc.Intensity = l.Intensity;
                                slc.Range = l.Range;
                                slc.InnerAngle = l.InnerConeAngle;
                                slc.OuterAngle = l.OuterConeAngle;
                            }
                        };
                        
                        // glTF cameras: the first imported camera becomes the scene's primary camera (used by Play) unless
                        // the scene already has one.
                        bool importedPrimaryCamera = static_cast<bool>(m_ActiveScene->GetPrimaryCameraEntity());
                        auto addCameraComponent = [&](Entity entity, const CameraNodeData& c)
                        {
                            auto& cameraComponent = entity.AddComponent<CameraComponent>();
                            if (c.Type == GltfCameraType::Orthographic)
                                cameraComponent.Camera.SetOrthographic(c.OrthographicSize, c.NearClip, c.FarClip);
                            else
                                cameraComponent.Camera.SetPerspective(c.VerticalFov, c.NearClip, c.FarClip);

                            // Scene cameras follow the viewport aspect; set it now so the projection is valid immediately.
                            if (m_ViewportSize.x > 0 && m_ViewportSize.y > 0)
                                cameraComponent.Camera.SetViewportSize(m_ViewportSize.x, m_ViewportSize.y);

                            cameraComponent.Primary = !importedPrimaryCamera;
                            importedPrimaryCamera = true;
                        };

                        auto spawnGltfCameras = [&](Entity parent, const std::vector<CameraNodeData>& cameras)
                        {
                            for (const auto& c : cameras)
                            {
                                Entity cameraEntity = m_ActiveScene->CreateEntity(c.Name);
                                if (parent)
                                    cameraEntity.SetParent(parent);

                                auto& tc = cameraEntity.GetComponent<TransformComponent>();
                                tc.Translation = c.Translation;
                                tc.Rotation = glm::eulerAngles(c.Rotation);
                                tc.Scale = c.Scale;

                                addCameraComponent(cameraEntity, c);
                            }
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

                                    addLightComponent(lightEntity, l);
                                }
                            };

                        auto createGltfNodeEntities = [&](const auto& meshAsset, AssetHandle meshHandle) -> Entity
                        {
                            const auto& nodes = meshAsset->GetNodes();
                            if (nodes.empty())
                                return {};

                            std::vector<Entity> createdNodes(nodes.size());
                            Entity firstRoot;
                            size_t rootCount = 0;

                            for (size_t i = 0; i < nodes.size(); i++)
                            {
                                if (nodes[i].Parent < 0)
                                    rootCount++;
                            }

                            Entity importRoot;
                            if (rootCount > 1)
                                importRoot = m_ActiveScene->CreateEntity(entityName);

                            for (size_t i = 0; i < nodes.size(); i++)
                            {
                                std::string nodeName = nodes[i].Name.empty() ? entityName + "_" + std::to_string(i) : nodes[i].Name;
                                createdNodes[i] = m_ActiveScene->CreateEntity(nodeName);
                                if (!firstRoot && nodes[i].Parent < 0)
                                    firstRoot = createdNodes[i];
                            }

                            for (size_t i = 0; i < nodes.size(); i++)
                            {
                                int32_t parentIndex = nodes[i].Parent;
                                if (parentIndex >= 0 && parentIndex < static_cast<int32_t>(createdNodes.size()))
                                    createdNodes[i].SetParent(createdNodes[parentIndex]);
                                else if (importRoot)
                                    createdNodes[i].SetParent(importRoot);
                            }

                            for (size_t i = 0; i < nodes.size(); i++)
                            {
                                auto& tc = createdNodes[i].GetComponent<TransformComponent>();
                                tc.Translation = nodes[i].Translation;
                                tc.Rotation = glm::eulerAngles(nodes[i].Rotation);
                                tc.Scale = nodes[i].Scale;
                                createdNodes[i].AddOrReplaceComponent<DirtyTransformComponent>();

                                if (nodes[i].SubmeshCount > 0)
                                {
                                    auto& meshComp = createdNodes[i].AddComponent<MeshComponent>();
                                    meshComp.Mesh = meshHandle;
                                    meshComp.SubmeshIndex = nodes[i].FirstSubmesh;
                                    meshComp.SubmeshCount = nodes[i].SubmeshCount;

                                    auto& matComp = createdNodes[i].AddComponent<MaterialComponent>();
                                    matComp.MaterialAssets = meshAsset->GetMaterialAssets();

                                    tryAttachAnimator(createdNodes[i]);
                                }
                            }

                            for (const auto& light : meshAsset->GetLights())
                            {
                                if (light.NodeIndex >= 0 && light.NodeIndex < static_cast<int32_t>(createdNodes.size()))
                                    addLightComponent(createdNodes[light.NodeIndex], light);
                                else
                                    spawnGltfLights(importRoot ? importRoot : firstRoot, { light });
                            }

                            for (const auto& camera : meshAsset->GetCameras())
                            {
                                if (camera.NodeIndex >= 0 && camera.NodeIndex < static_cast<int32_t>(createdNodes.size()))
                                    addCameraComponent(createdNodes[camera.NodeIndex], camera);
                                else
                                    spawnGltfCameras(importRoot ? importRoot : firstRoot, { camera });
                            }

                            // glTF node index -> entity, shared by the clip and by every skin.
                            std::vector<UUID> nodeTable(createdNodes.size(), UUID(0));
                            for (size_t i = 0; i < createdNodes.size(); ++i)
                            {
                                if (createdNodes[i])
                                    nodeTable[i] = createdNodes[i].GetUUID();
                            }

                            // Skinned meshes follow their joint ENTITIES (glTF skinning) instead of
                            // evaluating a private skeleton copy.
                            Entity firstSkinnedMesh;
                            for (Entity node : createdNodes)
                            {
                                if (node && node.HasComponent<AnimatorComponent>() &&
                                    node.GetComponent<AnimatorComponent>().Skeleton != 0)
                                {
                                    node.GetComponent<AnimatorComponent>().NodeEntities = nodeTable;
                                    if (!firstSkinnedMesh)
                                        firstSkinnedMesh = node;
                                }
                            }

                            // glTF animations are independent clips. Blender exports one per animated
                            // object (Bistro: each fan part has its own), and those all play at once;
                            // a character's clips (Fox: Survey/Walk/Run) all target the same joints and
                            // are alternatives. So: clips whose target nodes overlap form one group that
                            // shares a single animator (first clip plays, the rest are swappable), and
                            // every disjoint group gets its own animator so they run simultaneously.
                            if (type == AssetType::Mesh || type == AssetType::MeshSource)
                            {
                                struct ClipTargets
                                {
                                    AssetHandle Handle = 0;
                                    std::vector<int32_t> Nodes;
                                };
                                std::vector<ClipTargets> clips;
                                for (AssetHandle clipHandle : findImportAnimations())
                                {
                                    Ref<AnimationSequence> clip = AssetManager::GetAsset<AnimationSequence>(clipHandle);
                                    if (!clip)
                                        continue;
                                    ClipTargets targets{ clipHandle, {} };
                                    for (const auto& channel : clip->Channels)
                                    {
                                        if (channel.TargetNodeIndex >= 0 &&
                                            channel.TargetNodeIndex < static_cast<int32_t>(createdNodes.size()))
                                            targets.Nodes.push_back(channel.TargetNodeIndex);
                                    }
                                    std::sort(targets.Nodes.begin(), targets.Nodes.end());
                                    targets.Nodes.erase(std::unique(targets.Nodes.begin(), targets.Nodes.end()), targets.Nodes.end());
                                    if (!targets.Nodes.empty())
                                        clips.push_back(std::move(targets));
                                }

                                auto overlaps = [](const std::vector<int32_t>& a, const std::vector<int32_t>& b)
                                {
                                    size_t i = 0, j = 0;
                                    while (i < a.size() && j < b.size())
                                    {
                                        if (a[i] == b[j]) return true;
                                        if (a[i] < b[j]) ++i; else ++j;
                                    }
                                    return false;
                                };

                                // Union-find over clips by shared target nodes.
                                std::vector<size_t> group(clips.size());
                                for (size_t i = 0; i < clips.size(); ++i)
                                    group[i] = i;
                                std::function<size_t(size_t)> findGroup = [&](size_t i) -> size_t
                                {
                                    return group[i] == i ? i : (group[i] = findGroup(group[i]));
                                };
                                for (size_t i = 0; i < clips.size(); ++i)
                                    for (size_t j = i + 1; j < clips.size(); ++j)
                                        if (overlaps(clips[i].Nodes, clips[j].Nodes))
                                            group[findGroup(j)] = findGroup(i);

                                std::vector<int32_t> jointIndices;
                                if (resolvedSkeleton && !resolvedSkeleton->Skins.empty() && resolvedSkeleton->Skins[0])
                                {
                                    for (const Node* joint : resolvedSkeleton->Skins[0]->Joints)
                                        if (joint)
                                            jointIndices.push_back(joint->Index);
                                    std::sort(jointIndices.begin(), jointIndices.end());
                                }

                                std::vector<bool> groupAssigned(clips.size(), false);
                                for (size_t i = 0; i < clips.size(); ++i)
                                {
                                    size_t root = findGroup(i);
                                    if (groupAssigned[root])
                                        continue;
                                    groupAssigned[root] = true;

                                    // Clip i is the group's first (sorted) clip -> the default.
                                    // Joint clips belong to the skinned mesh (it plays them and skins from
                                    // them); anything else goes on the node it animates.
                                    Entity owner;
                                    if (firstSkinnedMesh && overlaps(clips[i].Nodes, jointIndices))
                                        owner = firstSkinnedMesh;
                                    else
                                        owner = createdNodes[clips[i].Nodes.front()];

                                    if (!owner)
                                        continue;

                                    if (owner.HasComponent<AnimatorComponent>() &&
                                        owner.GetComponent<AnimatorComponent>().Animation != 0)
                                    {
                                        NOX_WARN("Animation clip {} targets entity '{}' which already plays another clip; skipped.",
                                                 (uint64_t)clips[i].Handle, owner.GetName());
                                        continue;
                                    }

                                    auto& animator = owner.HasComponent<AnimatorComponent>()
                                        ? owner.GetComponent<AnimatorComponent>()
                                        : owner.AddComponent<AnimatorComponent>();
                                    animator.Animation = clips[i].Handle;
                                    animator.NodeEntities = nodeTable;
                                }
                            }

                            if (importRoot)
                                return importRoot;

                            return firstRoot ? firstRoot : createdNodes.front();
                        };

                        // Check if it's a dynamic mesh asset with multiple submeshes
                        if (type == AssetType::Mesh || type == AssetType::MeshSource)
                        {
                            Ref<Mesh> meshAsset = AssetManager::GetAsset<Mesh>(handle);
                            if (meshAsset && !meshAsset->GetNodes().empty())
                            {
                                Entity rootEntity = createGltfNodeEntities(meshAsset, handle);
                                m_SceneHierarchyPanel.SetSelectedEntity(rootEntity);
                                m_PlacementPreview.Root = rootEntity;
                            }
                            else
                            {
                                // Single submesh dynamic mesh
                                Entity newEntity = m_ActiveScene->CreateEntity(entityName);
                                auto& meshComp = newEntity.AddComponent<MeshComponent>();
                                meshComp.Mesh = handle;
                                meshComp.SubmeshIndex = 0;
                                meshComp.SubmeshCount = meshAsset ? static_cast<uint32_t>(meshAsset->GetSubMeshCount()) : 1;

                                auto& matComp = newEntity.AddComponent<MaterialComponent>();
                                if (meshAsset)
                                    matComp.MaterialAssets = meshAsset->GetMaterialAssets();

                                tryAttachAnimator(newEntity);
                                
                                if (meshAsset)
                                {
                                    spawnGltfLights(newEntity, meshAsset->GetLights());
                                    spawnGltfCameras(newEntity, meshAsset->GetCameras());
                                }

                                m_SceneHierarchyPanel.SetSelectedEntity(newEntity);
                                m_PlacementPreview.Root = newEntity;
                            }
                        }
                        else // StaticMesh (.nsmesh) - always single flattened mesh
                        {
                            Ref<StaticMesh> staticMeshAsset = AssetManager::GetAsset<StaticMesh>(handle);
                            if (staticMeshAsset && !staticMeshAsset->GetNodes().empty())
                            {
                                Entity rootEntity = createGltfNodeEntities(staticMeshAsset, handle);
                                m_SceneHierarchyPanel.SetSelectedEntity(rootEntity);
                                m_PlacementPreview.Root = rootEntity;
                            }
                            else
                            {
                            Entity newEntity = m_ActiveScene->CreateEntity(entityName);
                            auto& meshComp = newEntity.AddComponent<MeshComponent>();
                            meshComp.Mesh = handle;
                            meshComp.SubmeshIndex = 0;
                            meshComp.SubmeshCount = staticMeshAsset ? static_cast<uint32_t>(staticMeshAsset->GetSubMeshCount()) : 1;

                            auto& matComp = newEntity.AddComponent<MaterialComponent>();
                            if (staticMeshAsset)
                                matComp.MaterialAssets = staticMeshAsset->GetMaterialAssets();

                            if (staticMeshAsset)
                            {
                                spawnGltfLights(newEntity, staticMeshAsset->GetLights());
                                spawnGltfCameras(newEntity, staticMeshAsset->GetCameras());
                            }

                            m_SceneHierarchyPanel.SetSelectedEntity(newEntity);
                            m_PlacementPreview.Root = newEntity;
                            }
                        }

                            m_PlacementPreview.Handle = handle;
                            if (m_PlacementPreview.Root)
                            {
                                m_PlacementPreview.InitialTranslation =
                                    m_PlacementPreview.Root.GetComponent<TransformComponent>().Translation;
                            }
                            m_PlacementPreview.Active = static_cast<bool>(m_PlacementPreview.Root);
                        }

                        if (m_PlacementPreview.Active && m_PlacementPreview.Root)
                        {
                            ImVec2 imguiMouse = ImGui::GetMousePos();
                            glm::vec2 mouse = {
                                imguiMouse.x - m_ViewportBounds[0].x,
                                imguiMouse.y - m_ViewportBounds[0].y
                            };
                            glm::vec2 viewportSize = m_ViewportBounds[1] - m_ViewportBounds[0];

                            if (viewportSize.x > 0.0f && viewportSize.y > 0.0f)
                            {
                                glm::vec2 ndc = {
                                    (mouse.x / viewportSize.x) * 2.0f - 1.0f,
                                    1.0f - (mouse.y / viewportSize.y) * 2.0f
                                };

                                glm::mat4 inverseViewProjection = glm::inverse(
                                    m_EditorCamera.GetGizmoProjection() * m_EditorCamera.GetGizmoView());
                                glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndc, -1.0f, 1.0f);
                                glm::vec4 farPoint = inverseViewProjection * glm::vec4(ndc, 1.0f, 1.0f);
                                nearPoint /= nearPoint.w;
                                farPoint /= farPoint.w;

                                glm::vec3 rayOrigin = glm::vec3(nearPoint);
                                glm::vec3 rayDirection = glm::normalize(glm::vec3(farPoint - nearPoint));

                                // Keep the preview on a plane facing the editor camera. A horizontal
                                // ground plane becomes parallel to the ray in a level side view.
                                glm::vec3 planeNormal = m_EditorCamera.GetForwardDirection();
                                float rayPlaneDenominator = glm::dot(rayDirection, planeNormal);
                                if (std::abs(rayPlaneDenominator) > 0.0001f)
                                {
                                    float distance = glm::dot(
                                        m_PlacementPreview.InitialTranslation - rayOrigin,
                                        planeNormal
                                    ) / rayPlaneDenominator;
                                    if (distance >= 0.0f)
                                    {
                                        auto& transform = m_PlacementPreview.Root.GetComponent<TransformComponent>();
                                        transform.Translation = rayOrigin + rayDirection * distance;
                                        m_PlacementPreview.Root.AddOrReplaceComponent<DirtyTransformComponent>();
                                    }
                                }
                            }

                            if (payload->Delivery)
                            {
                                m_SceneHierarchyPanel.SetSelectedEntity(m_PlacementPreview.Root);
                                m_PlacementPreview = {};
                            }
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

#if NOX_PROFILE_STATS
        if (StatsOverlayLayer* statsLayer = Application::Get().GetLayer<StatsOverlayLayer>())
        {
            bool statsVisible = statsLayer->IsVisible();
            if (ImGui::Checkbox("Stats Overlay (F3)", &statsVisible))
                statsLayer->SetVisible(statsVisible);

            ImGui::SameLine();
            bool statsDetailed = statsLayer->IsDetailed();
            if (ImGui::Checkbox("Detailed", &statsDetailed))
                statsLayer->SetDetailed(statsDetailed);

            ImGui::SameLine();
            if (ImGui::Button("Reset Stats"))
                Profiler::Get().ResetStats();

            ImGui::SameLine();
            if (ImGui::Button("Save Report (Ctrl+F3)"))
                SaveStatsReport(*m_Renderer);

            ImGui::Separator();
        }
#endif
        
        static const char* debugModeNames[] = {
            "0: Full PBR Lit",
            "1: Base Color (Sascha 1:1)",
            "2: Normal Texture (Sascha 1:1)",
            "3: Occlusion (Sascha 1:1)",
            "4: Emissive (Sascha 1:1)",
            "5: Metallic (Sascha 1:1)",
            "6: Roughness (Sascha 1:1)",
            "7: Shading Normal (World)",
            "8: Direct Lights Only",
            "9: IBL Ambient Only",
            "10: World Position",
            "11: Entity ID",
            "12: Depth Buffer",
            "13: RT Shadow Mask",
            "14: RT Reflections",
            "15: Motion Vectors (Velocity Buffer)",
            "16: Indirect Diffuse GI Only (DDGI / ReSTIR GI)",
            "17: DDGI Probe Grid Spheres",
            "18: Path Tracer (1-SPP Raw)",
            "19: Path Tracer (Progressive Ground Truth)"
        };
        int currentMode = static_cast<int>(m_Renderer->getDebugMode());
        if (ImGui::Combo("PBR Debug View", &currentMode, debugModeNames, IM_ARRAYSIZE(debugModeNames)))
        {
            m_Renderer->setDebugMode(static_cast<uint32_t>(currentMode));
        }
        
        static const char* tonemapModeNames[] = {
            "0: None (Clamped Linear)",
            "1: ACES (Narkowicz)",
            "2: ACES (Hill)",
            "3: ACES (Hill + Exposure Boost)",
            "4: Khronos PBR Neutral"
        };
        int currentTonemap = static_cast<int>(m_Renderer->getTonemapMode());
        if (ImGui::Combo("Tonemapping Mode", &currentTonemap, tonemapModeNames, IM_ARRAYSIZE(tonemapModeNames)))
        {
            m_Renderer->setTonemapMode(static_cast<uint32_t>(currentTonemap));
        }
        
        float exposure = m_Renderer->getExposure();
        if (ImGui::SliderFloat("Exposure", &exposure, 0.0f, 5.0f, "%.2f"))
        {
            m_Renderer->setExposure(exposure);
        }

        float gamma = m_Renderer->getGamma();
        if (ImGui::SliderFloat("Gamma", &gamma, 0.5f, 3.5f, "%.2f"))
        {
            m_Renderer->setGamma(gamma);
        }

        // There was no way to change this without editing the hardcoded constructor value in
        // EditorLayer's OnAttach and recompiling - this is that missing control.
        float fov = m_EditorCamera.GetFOV();
        if (ImGui::SliderFloat("Camera FOV", &fov, 10.0f, 120.0f, "%.1f"))
        {
            m_EditorCamera.SetFOV(fov);
        }

        float iblAmbient = m_Renderer->getScaleIBLAmbient();
        if (ImGui::SliderFloat("IBL Ambient Scale", &iblAmbient, 0.0f, 5.0f, "%.2f"))
        {
            m_Renderer->setScaleIBLAmbient(iblAmbient);
        }
        
        bool jitter = m_Renderer->getCameraJitterEnabled();
        if (ImGui::Checkbox("Camera Subpixel Jitter (Halton 2,3)", &jitter))
        {
            m_Renderer->setCameraJitterEnabled(jitter);
        }
        if (jitter)
        {
            glm::vec2 j = m_Renderer->getCurrentJitter(); // or expose m_currentJitter
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Jitter Offset: (%.3f, %.3f) px", j.x, j.y);
        }

        ImGui::Separator();
        ImGui::Text("DLSS / Upscaling");

        if (!m_Renderer->isDLSSSupported())
        {
            ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.4f, 1.0f), "Not supported (Streamline/NGX unavailable on this GPU or driver)");
        }
        else
        {
            bool dlssEnabled = m_Renderer->isDLSSEnabled();
            if (ImGui::Checkbox("Enable DLSS", &dlssEnabled))
            {
                m_Renderer->setDLSSEnabled(dlssEnabled);
            }

            if (dlssEnabled)
            {
                static const char* upscaleModeNames[] = {
                    "Off",
                    "DLAA (no upscale, best quality)",
                    "Quality",
                    "Balanced",
                    "Performance",
                    "Ultra Performance"
                };
                int currentUpscaleMode = static_cast<int>(m_Renderer->getUpscaleMode());
                if (ImGui::Combo("Mode", &currentUpscaleMode, upscaleModeNames, IM_ARRAYSIZE(upscaleModeNames)))
                {
                    m_Renderer->setUpscaleMode(static_cast<NRI::UpscaleMode>(currentUpscaleMode));
                }
                
                if (m_Renderer->isDLSSRayReconstructionSupported())
                {
                    bool rrEnabled = m_Renderer->isDLSSRayReconstructionEnabled();
                    if (ImGui::Checkbox("Ray Reconstruction (DLSS 3.5 Denoising)", &rrEnabled))
                    {
                        m_Renderer->setDLSSRayReconstructionEnabled(rrEnabled);
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("AI Neural Reconstruction for reflections & indirect lighting (hybrid mode), or the whole\nimage (Path Tracing mode). Replaces downstream NRD denoisers -- automatically disables\nany active NRD REBLUR/RELAX. NRD SIGMA shadows remain compatible and independent.");
                    }
                    if (rrEnabled)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "(Active)");
                    }
                }

                NRI::Extent2D renderSize = m_Renderer->getRenderSize();
                NRI::Extent2D outputSize = m_Renderer->getViewPortSize();
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Rendering %ux%u -> %ux%u",
                    renderSize.width, renderSize.height, outputSize.width, outputSize.height);
            }
        }

        ImGui::Separator();
        ImGui::Text("Ray Tracing");

        bool ptEnabled = m_Renderer->isPathTracingEnabled();
        if (ImGui::Checkbox("Enable Path Tracing (Unified Light Transport)", &ptEnabled))
        {
            m_Renderer->setPathTracingEnabled(ptEnabled);
        }

        if (ptEnabled)
        {
            ImGui::Indent();

            bool restirPTEnabled = m_Renderer->getReSTIRPTEnabled();
            if (ImGui::Checkbox("ReSTIR PT (RTXDI Screen-Space Path Resampling) [v1]", &restirPTEnabled))
            {
                m_Renderer->setReSTIRPTEnabled(restirPTEnabled);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Replaces the plain path tracer's own accumulation loop with RTXDI's ReSTIR PT reservoir\nresampling (1 candidate combined via RIS, no temporal/spatial reuse yet -- that's next).\nExpect a noisier but unbiased single-frame result versus multi-frame progressive accumulation;\nthis is the same image every frame (no ground-truth convergence) until temporal reuse lands.");
            }

            if (restirPTEnabled)
            {
                ImGui::Indent();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "ReSTIR PT v1: None resampling mode (initial sample + RIS only)");

                bool ptTemporalEnabled = m_Renderer->getReSTIRPTTemporalEnabled();
                if (ImGui::Checkbox("Temporal Resampling (Experimental)", &ptTemporalEnabled))
                {
                    m_Renderer->setReSTIRPTTemporalEnabled(ptTemporalEnabled);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Reuses last frame's resampled path via hybrid-shift reconnection instead of drawing a fresh\ncandidate every frame -- known issue: currently produces a visible lighting-rotation artifact\neven with a static camera. Off by default until this is debugged; leave off for the clean v1 look.");
                }
                if (ptTemporalEnabled)
                {
                    ImGui::TextColored(ImVec4(0.9f, 0.65f, 0.2f, 1.0f), "Known bug: lighting rotates/swirls -- for testing only");
                }

                uint32_t& ptInitialSamples = m_Renderer->getReSTIRPTNumInitialSamples();
                int ptInitialSamplesInt = static_cast<int>(ptInitialSamples);
                if (ImGui::SliderInt("Initial Samples", &ptInitialSamplesInt, 1, 16))
                {
                    ptInitialSamples = static_cast<uint32_t>(ptInitialSamplesInt);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("How many full candidate paths are traced per pixel and RIS-combined into one reservoir\n(RTXPT's equivalent: \"Samples per pixel\"). Higher = less noise, higher cost -- scales roughly\nlinearly since each sample re-traces bounces from scratch (no reuse across samples yet).");
                }

                uint32_t& ptMaxBounces = m_Renderer->getReSTIRPTMaxBounceDepth();
                int ptMaxBouncesInt = static_cast<int>(ptMaxBounces);
                if (ImGui::SliderInt("Max Bounces", &ptMaxBouncesInt, 1, 16))
                {
                    ptMaxBounces = static_cast<uint32_t>(ptMaxBouncesInt);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Max indirect bounce depth traced per candidate path (RTXPT's equivalent: \"Max bounces\").\nDoes not include the primary surface's own direct lighting, which is always evaluated\nseparately regardless of this setting. Higher = more light transport (longer indirect\nlight chains, e.g. light bouncing around corners), higher cost.");
                }

                uint32_t& ptNeeSamples = m_Renderer->getReSTIRPTNumNeeSamples();
                int ptNeeSamplesInt = static_cast<int>(ptNeeSamples);
                if (ImGui::SliderInt("NEE Light Samples", &ptNeeSamplesInt, 0, 8))
                {
                    ptNeeSamples = static_cast<uint32_t>(ptNeeSamplesInt);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Next-event-estimation light samples drawn per bounce surface (and for the primary\nsurface's own direct lighting). Higher = less noise on direct lighting/shadows with many\nlights, higher cost. 0 disables NEE entirely (emissive-hit sampling only).");
                }

                ImGui::Unindent();
            }

            bool ptUsesRTXDI = m_Renderer->getPathTracerUsesRTXDI();
            if (ImGui::Checkbox("Use ReSTIR DI/GI With Path Tracer", &ptUsesRTXDI))
            {
                m_Renderer->setPathTracerUsesRTXDI(ptUsesRTXDI);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("RTXPT-style hybrid path tracing: run RTXDI ReSTIR DI/GI before the plain path tracer\nand let the path tracer consume those primary-surface lighting buffers. This is separate\nfrom ReSTIR PT path resampling.");
            }
            if (ptUsesRTXDI)
            {
                ImGui::Indent();

                static const char* ptGiModeNames[] = {
                    "Off (Path Tracer Only)",
                    "ReSTIR GI (Primary Diffuse Indirect)"
                };
                int currentPTGIMode = m_Renderer->getDiffuseGIMode() == 2 ? 1 : 0;
                if (ImGui::Combo("PT Diffuse GI", &currentPTGIMode, ptGiModeNames, IM_ARRAYSIZE(ptGiModeNames)))
                {
                    m_Renderer->setDiffuseGIMode(currentPTGIMode == 1 ? 2u : 0u);
                    m_Renderer->setDDGIEnabled(false);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("ReSTIR GI is consumed as primary diffuse indirect lighting. The path tracer still traces\nspecular/reflection paths, while diffuse continuation from the primary surface is replaced\nto avoid double counting.");
                }

                static const char* ptDirectLightingModeNames[] = {
                    "Path Tracer NEE",
                    "ReSTIR DI (Primary Direct)"
                };
                int currentPTDirectMode = static_cast<int>(m_Renderer->getDirectLightingMode());
                if (ImGui::Combo("PT Direct Lighting", &currentPTDirectMode, ptDirectLightingModeNames, IM_ARRAYSIZE(ptDirectLightingModeNames)))
                {
                    m_Renderer->setDirectLightingMode(static_cast<uint32_t>(currentPTDirectMode));
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("ReSTIR DI replaces the path tracer's primary-bounce direct light loop. Deeper bounce\nsurfaces still use the path tracer's own next-event estimation.");
                }

                ImGui::Unindent();
            }

            bool ptAccum = m_Renderer->isPathTracingAccumulation();
            if (ImGui::Checkbox("Progressive Ground Truth (Accumulate when static)", &ptAccum))
            {
                m_Renderer->setPathTracingAccumulation(ptAccum);
            }
            if (restirPTEnabled && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Not used by ReSTIR PT (it has no accumulation buffer yet) -- only affects the plain path tracer path.");
            }

            bool ptDlssRR = m_Renderer->isDLSSRayReconstructionEnabled();
            if (ptDlssRR)
            {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Denoiser: DLSS Ray Reconstruction (Active)");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("DLSS Ray Reconstruction is active and denoises the whole path-traced image.\nSelect NRD REBLUR or RELAX below to switch to cross-vendor non-AI denoising (auto-disables DLSS-RR).");
                }
            }

            static const char* ptDenoiserNames[] = {
                "Off (Raw 1-SPP / Progressive Accumulation)",
                "NRD REBLUR (Variance Guided)",
                "NRD RELAX (A-Trous Wavelet)"
            };
            int currentPTDenoiser = static_cast<int>(m_Renderer->getNRDPTDenoiser());
            if (ImGui::Combo("PT Denoiser", &currentPTDenoiser, ptDenoiserNames, IM_ARRAYSIZE(ptDenoiserNames)))
            {
                m_Renderer->setNRDPTDenoiser(static_cast<NRI::NRDDiffuseDenoiser>(currentPTDenoiser));
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Cross-vendor non-AI fallback for hardware/preference without DLSS Ray Reconstruction.\nEnabling REBLUR or RELAX automatically disables DLSS-RR and switches off progressive\naccumulation (the denoiser handles temporal stability instead, via motion vectors).");
            }
            ImGui::Unindent();
        }
        else
        {
            // Hybrid Ray Tracing options (only visible when not in full Path Tracing)
            bool rtEnabled = m_Renderer->getRayTracingEnabled();
            if (ImGui::Checkbox("Enable Hybrid Ray Tracing", &rtEnabled))
            {
                m_Renderer->setRayTracingEnabled(rtEnabled);
            }

            if (rtEnabled)
            {
                ImGui::Indent();
                bool rtShadows = m_Renderer->getRayTracingShadows();
                if (ImGui::Checkbox("Ray Tracing Shadows", &rtShadows))
                {
                    m_Renderer->setRayTracingShadows(rtShadows);
                }

                bool rtReflections = m_Renderer->getRayTracingReflections();
                if (ImGui::Checkbox("Ray Tracing Reflections", &rtReflections))
                {
                    m_Renderer->setRayTracingReflections(rtReflections);
                }

                // Diffuse Global Illumination (GI)
                static const char* giModeNames[] = {
                    "Off (IBL Ambient)",
                    "DDGI (Probe Volumes)",
                    "ReSTIR GI (Screen-Space Resampling via RTXDI)"
                };
                int currentGIMode = static_cast<int>(m_Renderer->getDiffuseGIMode());
                if (ImGui::Combo("Diffuse Global Illumination", &currentGIMode, giModeNames, IM_ARRAYSIZE(giModeNames)))
                {
                    m_Renderer->setDiffuseGIMode(static_cast<uint32_t>(currentGIMode));
                    if (currentGIMode == 1)
                    {
                        m_Renderer->setDDGIEnabled(true);
                    }
                    else
                    {
                        m_Renderer->setDDGIEnabled(false);
                    }

                    // "PBR Debug View" mode 16 ("Indirect Diffuse GI Only") is a second, independent
                    // switch that also keeps ReSTIR GI's full pipeline running (see runReSTIRGI's
                    // `m_debugMode == 16 && m_diffuseGIMode != 1` clause) even after this combo is
                    // switched away from ReSTIR GI -- leaving it stuck on 16 silently keeps paying
                    // for TLAS build + all three ReSTIR GI passes + NRD GI denoise every frame with
                    // no visual indication why. Clear it whenever GI mode no longer needs it.
                    if (currentGIMode != 2 && m_Renderer->getDebugMode() == 16)
                    {
                        m_Renderer->setDebugMode(0);
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Select indirect diffuse lighting technique:\n- Off: Constant or Cubemap IBL\n- DDGI: Irradiance probe volume grid\n- ReSTIR GI: RTXDI Spatiotemporal reservoir resampling (screen-space indirect diffuse)");
                }

                uint32_t activeDebugMode = m_Renderer->getDebugMode();

                // ReSTIR GI Settings (RTXDI)
                if (m_Renderer->getDiffuseGIMode() == 2 || (activeDebugMode == 16 && m_Renderer->getDiffuseGIMode() != 1))
                {
                    ImGui::Indent();
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "ReSTIR GI (RTXDI SDK Active)");

                    float& spatialRadius = m_Renderer->getReSTIRGISpatialRadius();
                    ImGui::SliderFloat("Spatial Radius (px)", &spatialRadius, 4.0f, 64.0f, "%.1f");

                    uint32_t& numSamples = m_Renderer->getReSTIRGINumSpatialSamples();
                    int samplesInt = static_cast<int>(numSamples);
                    if (ImGui::SliderInt("Spatial Samples", &samplesInt, 1, 8))
                    {
                        numSamples = static_cast<uint32_t>(samplesInt);
                    }

                    uint32_t& maxM = m_Renderer->getReSTIRGIMaxHistoryLength();
                    int maxMInt = static_cast<int>(maxM);
                    if (ImGui::SliderInt("Max History Length (M)", &maxMInt, 1, 32))
                    {
                        maxM = static_cast<uint32_t>(maxMInt);
                    }

                    float& normalThresh = m_Renderer->getReSTIRGINormalThreshold();
                    ImGui::SliderFloat("Normal Threshold", &normalThresh, 0.1f, 0.99f, "%.2f");

                    float& depthThresh = m_Renderer->getReSTIRGIDepthThreshold();
                    ImGui::SliderFloat("Depth Threshold", &depthThresh, 0.01f, 0.5f, "%.2f");

                    bool& boiling = m_Renderer->getReSTIRGIEnableBoilingFilter();
                    ImGui::Checkbox("Enable Boiling Filter", &boiling);
                    if (boiling)
                    {
                        float& strength = m_Renderer->getReSTIRGIBoilingFilterStrength();
                        ImGui::SliderFloat("Boiling Filter Strength", &strength, 0.0f, 1.0f, "%.2f");
                    }
                    ImGui::TextDisabled("(GI Denoiser moved to the Denoising section below)");
                    ImGui::Unindent();
                }

                // DDGI Settings
                if (m_Renderer->getDiffuseGIMode() == 1 || activeDebugMode == 17)
                {
                    ImGui::Indent();
                    uint32_t totalProbes = m_Renderer->getDDGIProbeCountTotal();
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Grid: %ux%ux%u (%u probes, %u rays/probe)",
                        m_Renderer->getDDGIProbeCountX(),
                        m_Renderer->getDDGIProbeCountY(),
                        m_Renderer->getDDGIProbeCountZ(),
                        totalProbes,
                        m_Renderer->getDDGIRaysPerProbe());

                    glm::vec3& origin = m_Renderer->getDDGIGridOrigin();
                    ImGui::DragFloat3("Grid Origin", glm::value_ptr(origin), 0.1f);

                    glm::vec3& spacing = m_Renderer->getDDGIGridSpacing();
                    ImGui::DragFloat3("Grid Spacing", glm::value_ptr(spacing), 0.05f, 0.2f, 10.0f);

                    float& hysteresis = m_Renderer->getDDGIHysteresis();
                    ImGui::SliderFloat("Temporal Hysteresis", &hysteresis, 0.80f, 0.995f, "%.3f");

                    float& normalBias = m_Renderer->getDDGINormalBias();
                    ImGui::SliderFloat("Normal Bias", &normalBias, 0.0f, 1.0f, "%.2f");

                    float& sphereRadius = m_Renderer->getDDGIDebugSphereRadius();
                    ImGui::SliderFloat("Debug Sphere Radius", &sphereRadius, 0.02f, 0.5f, "%.2f");

                    bool& xray = m_Renderer->getDDGIDebugXRay();
                    ImGui::Checkbox("Probe Spheres X-Ray (See Through Walls)", &xray);
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("Disables depth testing for probe spheres so all probes are visible in the scene without being occluded by walls or floors.");
                    }

                    if (ImGui::Button("Reset Grid to Sponza Defaults"))
                    {
                        m_Renderer->resetDDGIGridToDefaults();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reset DDGI History"))
                    {
                        m_Renderer->resetDDGIHistory();
                    }
                    ImGui::Unindent();
                }

                // Direct Lighting (ReSTIR DI)
                static const char* directLightingModeNames[] = {
                    "Brute-Force Analytic Loop (existing)",
                    "ReSTIR DI (Screen-Space Resampled, RTXDI)"
                };
                int currentDirectLightingMode = static_cast<int>(m_Renderer->getDirectLightingMode());
                if (ImGui::Combo("Direct Lighting", &currentDirectLightingMode, directLightingModeNames, IM_ARRAYSIZE(directLightingModeNames)))
                {
                    m_Renderer->setDirectLightingMode(static_cast<uint32_t>(currentDirectLightingMode));
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Brute-force loop only ray-traces a shadow for light index 0 -- every other point/spot light is unshadowed.\nReSTIR DI resamples down to one light per pixel and shadow-rays it, so every light gets a real shadow.\nSwitch \"PBR Debug View\" to \"8: Direct Lights Only\" to compare the two in isolation.");
                }

                if (m_Renderer->getDirectLightingMode() == 1)
                {
                    ImGui::Indent();
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "ReSTIR DI (RTXDI SDK Active) -- v1: uniform light sampling, no RIS/ReGIR yet");

                    uint32_t& numLocal = m_Renderer->getReSTIRDINumLocalLightSamples();
                    int numLocalInt = static_cast<int>(numLocal);
                    if (ImGui::SliderInt("Local Light Samples", &numLocalInt, 1, 16))
                        numLocal = static_cast<uint32_t>(numLocalInt);

                    uint32_t& numInfinite = m_Renderer->getReSTIRDINumInfiniteLightSamples();
                    int numInfiniteInt = static_cast<int>(numInfinite);
                    if (ImGui::SliderInt("Infinite (Directional) Light Samples", &numInfiniteInt, 1, 4))
                        numInfinite = static_cast<uint32_t>(numInfiniteInt);

                    float& diSpatialRadius = m_Renderer->getReSTIRDISpatialRadius();
                    ImGui::SliderFloat("Spatial Radius (px)##DI", &diSpatialRadius, 4.0f, 64.0f, "%.1f");

                    uint32_t& diNumSpatialSamples = m_Renderer->getReSTIRDINumSpatialSamples();
                    int diSpatialInt = static_cast<int>(diNumSpatialSamples);
                    if (ImGui::SliderInt("Spatial Samples##DI", &diSpatialInt, 1, 8))
                        diNumSpatialSamples = static_cast<uint32_t>(diSpatialInt);

                    uint32_t& diMaxM = m_Renderer->getReSTIRDIMaxHistoryLength();
                    int diMaxMInt = static_cast<int>(diMaxM);
                    if (ImGui::SliderInt("Max History Length (M)##DI", &diMaxMInt, 1, 32))
                        diMaxM = static_cast<uint32_t>(diMaxMInt);

                    float& diNormalThresh = m_Renderer->getReSTIRDINormalThreshold();
                    ImGui::SliderFloat("Normal Threshold##DI", &diNormalThresh, 0.1f, 0.99f, "%.2f");

                    float& diDepthThresh = m_Renderer->getReSTIRDIDepthThreshold();
                    ImGui::SliderFloat("Depth Threshold##DI", &diDepthThresh, 0.01f, 0.5f, "%.2f");

                    bool& regirEnabled = m_Renderer->getReGIREnabled();
                    ImGui::Checkbox("ReGIR (World-Space Light Grid)", &regirEnabled);
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("Pre-bakes power-weighted local-light candidates into a world-space grid so nearby\npixels draw from lights that actually matter at that location, instead of a uniform screen-wide RIS tile.\nOff falls back to the plain RIS tile for every pixel -- still power-weighted, just not spatially localized.");
                    }
                    if (regirEnabled)
                    {
                        ImGui::Indent();
                        float& regirCellSize = m_Renderer->getReGIRCellSize();
                        ImGui::SliderFloat("Cell Size (m)", &regirCellSize, 0.1f, 8.0f, "%.2f");
                        glm::vec3& regirCenter = m_Renderer->getReGIRGridCenter();
                        ImGui::DragFloat3("Grid Center", glm::value_ptr(regirCenter), 0.5f);
                        float& regirJitter = m_Renderer->getReGIRSamplingJitter();
                        ImGui::SliderFloat("Cell Jitter", &regirJitter, 0.0f, 1.0f, "%.2f");
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("How much each cell's sampled position wobbles every frame (as a fraction of cell size).\n0 = fully static cell assignment: perfectly stable per-pixel, but you may see a faint grid pattern at cell boundaries.\n1 = RTXPT's default: diffuses that boundary across frames, but with only a handful of lights and no heavy\ntemporal accumulation this reads as light edges constantly moving/flickering instead. Try lowering this\ntoward 0 if lights feel unstable -- raise it back up once you have many more lights in the scene.");
                        }
                        ImGui::Unindent();
                    }

                    ImGui::Unindent();
                }

                ImGui::Unindent();
            }
        }

        // =========================================================================
        // DENOISING (NRD / DLSS) -- its own top-level section, independent of whichever tracer mode
        // or GI technique is active above: a denoiser is a separate concern from what produced the
        // signal it's cleaning up, not a sub-setting of "Ray Tracing". NOTE: right now NRD is only
        // wired to Hybrid Ray Tracing's separate 1-SPP shadow/reflection/GI passes -- the Path Tracer
        // instead relies on DLSS Ray Reconstruction or progressive accumulation, so these controls
        // stay hidden while Path Tracing is active rather than implying they'd do something they
        // currently don't.
        // =========================================================================
        ImGui::Separator();
        ImGui::Text("Denoising");

        bool showAnyDenoiserControl = false;
        if (!m_Renderer->isPathTracingEnabled() && m_Renderer->getRayTracingEnabled())
        {
            if (m_Renderer->getRayTracingShadows())
            {
                showAnyDenoiserControl = true;
                bool nrdShadows = m_Renderer->getNRDShadowsEnabled();
                if (ImGui::Checkbox("NRD SIGMA Denoiser (Shadows)", &nrdShadows))
                {
                    m_Renderer->setNRDShadowsEnabled(nrdShadows);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Uses NVIDIA Real-Time Denoisers (SIGMA) for penumbra filtering & contact hardening.\nRuns upstream pre-lighting on the shadow mask; fully compatible with DLSS-SR and DLSS-RR.\nUncheck to view raw 1-SPP shadows.");
                }
            }

            if (m_Renderer->getRayTracingReflections())
            {
                showAnyDenoiserControl = true;
                bool dlssRR = m_Renderer->isDLSSRayReconstructionEnabled();
                if (dlssRR)
                {
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Reflections Denoiser: DLSS Ray Reconstruction (Active)");
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("DLSS Ray Reconstruction is active and handles reflections downstream.\nSelect NRD REBLUR or RELAX below to switch to cross-vendor non-AI denoising (auto-disables DLSS-RR).");
                    }
                }

                static const char* reflDenoiserNames[] = {
                    "Off (Raw 1-SPP)",
                    "NRD REBLUR (Variance Guided)",
                    "NRD RELAX (A-Trous Wavelet)"
                };
                int currentReflDenoiser = static_cast<int>(m_Renderer->getNRDReflectionDenoiser());
                if (ImGui::Combo("Reflection Denoiser", &currentReflDenoiser, reflDenoiserNames, IM_ARRAYSIZE(reflDenoiserNames)))
                {
                    m_Renderer->setNRDReflectionDenoiser(static_cast<NRI::NRDReflectionDenoiser>(currentReflDenoiser));
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Cross-vendor non-AI denoisers for specular reflections.\nEnabling NRD REBLUR or RELAX automatically disables DLSS Ray Reconstruction to prevent double-filtering.");
                }
            }

            if (m_Renderer->getDiffuseGIMode() == 2 || (m_Renderer->getDebugMode() == 16 && m_Renderer->getDiffuseGIMode() != 1))
            {
                showAnyDenoiserControl = true;
                static const char* giDenoiserNames[] = {
                    "Off (Raw 1-SPP)",
                    "NRD REBLUR Diffuse (Variance Guided)",
                    "NRD RELAX Diffuse (A-Trous Wavelet)"
                };
                int currentGIDenoiser = static_cast<int>(m_Renderer->getNRDGIDenoiser());
                if (ImGui::Combo("GI Denoiser", &currentGIDenoiser, giDenoiserNames, IM_ARRAYSIZE(giDenoiserNames)))
                {
                    m_Renderer->setNRDGIDenoiser(static_cast<NRI::NRDDiffuseDenoiser>(currentGIDenoiser));
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Denoises the raw 1-SPP ReSTIR GI diffuse output using NRD (same suite already denoising reflections/shadows).\nStabilizes the swimming/rotating artifacts inherent to raw ReSTIR GI under camera motion.");
                }
            }

            if (m_Renderer->getDirectLightingMode() == 1)
            {
                showAnyDenoiserControl = true;
                static const char* diDenoiserNames[] = {
                    "Off (Raw 1-SPP)",
                    "NRD REBLUR Diffuse (Variance Guided)",
                    "NRD RELAX Diffuse (A-Trous Wavelet)"
                };
                int currentDIDenoiser = static_cast<int>(m_Renderer->getNRDDIDenoiser());
                if (ImGui::Combo("DI Denoiser", &currentDIDenoiser, diDenoiserNames, IM_ARRAYSIZE(diDenoiserNames)))
                {
                    m_Renderer->setNRDDIDenoiser(static_cast<NRI::NRDDiffuseDenoiser>(currentDIDenoiser));
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Denoises the raw 1-SPP ReSTIR DI direct lighting output using NRD (its own separate history from the GI denoiser above).\nStabilizes the light-selection noise from RIS/ReGIR resampling under camera motion.");
                }
            }
        }

        if (!showAnyDenoiserControl)
        {
            ImGui::TextDisabled(m_Renderer->isPathTracingEnabled()
                ? "Path Tracer denoising (DLSS-RR / NRD REBLUR-RELAX / progressive accumulation) is configured under Ray Tracing above."
                : "Enable Ray Tracing Shadows, Reflections, or ReSTIR GI above to configure their denoisers.");
        }

        ImGui::Separator();
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

            m_Renderer->BeginScene(camera.GetComponent<CameraComponent>().Camera, camera.GetComponent<WorldTransformComponent>().WorldMatrix);
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
        m_UnloadUnusedAssetsRequested = true;
#if NOX_PROFILE_STATS
        Profiler::Get().ResetStats();
#endif
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
        m_UnloadUnusedAssetsRequested = true;
#if NOX_PROFILE_STATS
        Profiler::Get().ResetStats();
#endif
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
        m_UnloadUnusedAssetsRequested = true;
    }

    void EditorLayer::UnloadUnusedAssets()
    {
        std::unordered_set<AssetHandle> referencedAssets;
        if (m_EditorScene)
            m_EditorScene->CollectAssetReferences(referencedAssets);
        if (m_ActiveScene && m_ActiveScene != m_EditorScene)
            m_ActiveScene->CollectAssetReferences(referencedAssets);

        Project::GetActive()->GetEditorAssetManager()->UnloadUnusedAssets(referencedAssets);
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
