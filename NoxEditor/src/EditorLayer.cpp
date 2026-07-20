#include "EditorLayer.h"

#include <iostream>

#include <imgui.h>
#include <imgui_internal.h>// For Docking
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>  // for pointer to matrix or vector

#include "NoxCore/Math/Math.h"
#include "NoxCore/Asset/AssetManager.h"
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

        m_Font = Font::GetDefault();

        m_IconPlay = TextureImporter::LoadTexture2D("Resources/Icons/PlayButton.ktx2", {.generateMips = false});
        m_IconStop = TextureImporter::LoadTexture2D("Resources/Icons/StopButton.ktx2", {.generateMips = false});
        m_IconPause = TextureImporter::LoadTexture2D("Resources/Icons/PauseButton.ktx2", {.generateMips = false});
        m_IconSimulate = TextureImporter::LoadTexture2D("Resources/Icons/SimulateButton.ktx2", {.generateMips = false});
        m_IconStep = TextureImporter::LoadTexture2D("Resources/Icons/StepButton.ktx2", {.generateMips = false});

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

            // If no project is opened, close vank
            // note: this is while we dont have a new project path
            if (!OpenProject())
            {
                SDL_Event event;
                event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&event);
            }
        }

        m_EditorCamera = EditorCamera(30.0f, 1.778f, 0.1f, 1000.0f);
    }

    EditorLayer::~EditorLayer()
    {
        NOX_CORE_INFO("EditorLayer Shutdown");

        m_Font->ReleaseDefault(); // Since Editor Layer since static dies After renderer not needed for components
    }

    void EditorLayer::OnEvent(Event& event)
    {
        //std::println("{}", event.ToString());

        if (m_SceneState == SceneState::Edit)
            m_EditorCamera.OnEvent(event);

        EventDispatcher dispatcher(event);
        dispatcher.Dispatch<KeyPressedEvent>(Nox_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
        /*dispatcher.Dispatch<KeyPressedEvent>([this](KeyPressedEvent& e) { return OnKeyPressed(e); });*/
        dispatcher.Dispatch<MouseButtonPressedEvent>(Nox_BIND_EVENT_FN(EditorLayer::OnMouseButtonPressed));
        /*dispatcher.Dispatch<MouseButtonPressedEvent>([this](MouseButtonPressedEvent& e) { return OnMouseButtonPressed(e); });*/
    }

    void EditorLayer::OnUpdate(Timestep ts)
    {
        m_ActiveScene->OnViewportResize(m_ViewportSize.x, m_ViewportSize.y);
        
         // zero sized framebuffer is invalid
        if (m_ViewportSize.x > 0.0f && m_ViewportSize.y > 0.0f)
        {
            // Verify if the viewport has a new size and resize the RenderTarget accordingly.
            NRI::Extent2D viewportSize = m_Renderer->getViewPortSize();
            if ( m_ViewportSize.x != viewportSize.width || m_ViewportSize.y != viewportSize.height)
            {
                m_Renderer->onViewportSizeChange({m_ViewportSize.x, m_ViewportSize.y});
                m_EditorCamera.SetViewportSize(m_ViewportSize.x, m_ViewportSize.y);
            }
        }
        
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

                /*m_ActiveScene->OnUpdateSimulation(ts, m_EditorCamera);*/
                break;
            }
        case SceneState::Play:
            {
                m_ActiveScene->OnUpdateRuntime(ts);
                break;
            }
        }
        
        /*// Mouse Selection
        auto [mx, my] = ImGui::GetMousePos();
        mx -= m_ViewportBounds[0].x;
        my -= m_ViewportBounds[0].y;
        glm::vec2 viewportSize = m_ViewportBounds[1] - m_ViewportBounds[0];
        //my = viewportSize.y - my;
        int mouseX = (int)mx;
        int mouseY = (int)my;

        if (mouseX >= 0 && mouseY >= 0 && mouseX < (int)viewportSize.x && mouseY < (int)viewportSize.y)
        {
            /*ScopeTimer timer("MousePicking");#1#
            // Retrieve the pixel data (ID) from the calculated index
            // reading only 1 pixel right now but if multi select maybe i need full viewport ? 
            int pixelData = Renderer::ReadPixel(mouseX, mouseY);

            m_HoveredEntity = pixelData == -1 ? Entity() : Entity((entt::entity)pixelData, m_ActiveScene.get());
            
            Renderer::setPickRequest(mouseX, mouseY, true);
        }*/
        
        OnOverlayRender();
    }

    void EditorLayer::OnRender()
    {
    }

    void EditorLayer::OnImGuiRender()
    {
        auto& app = Application::Get();

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

            ImGui::EndMainMenuBar();
        }

        /* END Docking */

        // We define "viewport" with no padding an retrieve the rendering area
        // Using the dock "Viewport", this sets the window to cover the entire central viewport
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        if (ImGui::Begin("Viewport"))
        {
            if (m_ViewportHovered)
            {
                Application::Get().setBlockEvents(false);
            }
            else
            {
                Application::Get().setBlockEvents(true);
            }

            auto viewportMinRegion = ImGui::GetWindowContentRegionMin();
            auto viewportMaxRegion = ImGui::GetWindowContentRegionMax();
            auto viewportOffset = ImGui::GetWindowPos();
            m_ViewportBounds[0] = {viewportMinRegion.x + viewportOffset.x, viewportMinRegion.y + viewportOffset.y};
            m_ViewportBounds[1] = {viewportMaxRegion.x + viewportOffset.x, viewportMaxRegion.y + viewportOffset.y};

            m_ViewportFocused = ImGui::IsWindowFocused();
            m_ViewportHovered = ImGui::IsWindowHovered();

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
                // Adding overlay text on the upper left corner
                ImGui::SetCursorPos(ImVec2(0, 0));
                ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            }

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                {
                    AssetHandle handle = *(const AssetHandle*)payload->Data;
                    OpenScene(handle);
                }
                ImGui::EndDragDropTarget();
            }

            // Gizmos

            Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();

            if (selectedEntity && m_GizmoType != -1)
            {
                ImGuizmo::SetOrthographic(false);
                ImGuizmo::SetDrawlist();

                ImGuizmo::SetRect(m_ViewportBounds[0].x, m_ViewportBounds[0].y,
                                  m_ViewportBounds[1].x - m_ViewportBounds[0].x,
                                  m_ViewportBounds[1].y - m_ViewportBounds[0].y);

                // Camera
                // Runtime camera from entity
                /*auto cameraEntity = m_ActiveScene->GetPrimaryCameraEntity();
                const auto& camera = cameraEntity.GetComponent<CameraComponent>().Camera;
                const glm::mat4& cameraProjection = camera.GetProjection();
                glm::mat4 cameraView = glm::inverse(cameraEntity.GetComponent<TransformComponent>().GetTransform());*/

                // Editor camera
                const glm::mat4& cameraProjection = m_EditorCamera.GetGizmoProjection();

                glm::mat4 cameraView = m_EditorCamera.GetGizmoView();

                // Entity transform
                auto& tc = selectedEntity.GetComponent<TransformComponent>();
                glm::mat4 transform = tc.GetTransform();

                // Snapping
                bool snap = Input::IsKeyPressed(SDL_SCANCODE_LCTRL);
                float snapValue = 0.5f; // Snap to 0.5m for translation/scale
                // Snap to 45 degrees for rotation
                if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
                {
                    snapValue = 45.0f;
                }

                float snapValues[3] = {snapValue, snapValue, snapValue};

                ImGuizmo::Manipulate(glm::value_ptr(cameraView), glm::value_ptr(cameraProjection),
                                     static_cast<ImGuizmo::OPERATION>(m_GizmoType), ImGuizmo::LOCAL,
                                     glm::value_ptr(transform), nullptr, snap ? snapValues : nullptr);

                if (ImGuizmo::IsUsing())
                {
                    //translation is position for me maybe change
                    glm::vec3 translation, rotation, scale;
                    Math::DecomposeTransform(transform, translation, rotation, scale);

                    glm::vec3 deltaRotation = rotation - tc.Rotation;
                    tc.Translation = translation;
                    tc.Rotation += deltaRotation;
                    tc.Scale = scale;
                }
            }

            ImGui::End();
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
                    /*NewScene();*/
                }
                break;
            }
        case SDL_SCANCODE_O:
            {
                if (control)
                {
                    /*OpenProject();*/
                }
                break;
            }
        case SDL_SCANCODE_S:
            {
                if (control)
                {
                    /*if (shift)
                        SaveSceneAs();
                    else
                        SaveScene();*/
                }
                break;
            }

        // Scene Commands
        case SDL_SCANCODE_D:
            {
                if (control)
                {
                    /*OnDuplicateEntity();*/
                }
                break;
            }


        /*// Gizmos
        case SDL_SCANCODE_Q:
            m_GizmoType = -1;
            break;
        case SDL_SCANCODE_W:
            m_GizmoType = ImGuizmo::OPERATION::TRANSLATE;
            break;
        case SDL_SCANCODE_E:
            m_GizmoType = ImGuizmo::OPERATION::ROTATE;
            break;
        case SDL_SCANCODE_R:
            if (control)
            {
                /*ScriptEngine::ReloadAssembly();#1#
            }
            else
            {
                m_GizmoType = ImGuizmo::OPERATION::SCALE;
            }
            break;*/
        case SDL_SCANCODE_DELETE:
            {
                if (Application::Get().GetLayer<ImGuiLayer>()->GetActiveWidgetID() == 0)
                {
                    Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
                    if (selectedEntity)
                    {
                        m_SceneHierarchyPanel.SetSelectedEntity({});
                        /*m_ActiveScene->DestroyEntity(selectedEntity);*/
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
            /*if (m_ViewportHovered && !ImGuizmo::IsOver() && !Input::IsKeyPressed(SDL_SCANCODE_LALT))
            {
                m_SceneHierarchyPanel.SetSelectedEntity(m_HoveredEntity);
            }*/
        }

        return false;
    }

    void EditorLayer::OnOverlayRender()
    {
        /*if (m_SceneState == SceneState::Play)
        {
            Entity camera = m_ActiveScene->GetPrimaryCameraEntity();
            if (!camera)
                return;

            Renderer::BeginScene(camera.GetComponent<CameraComponent>().Camera, camera.GetComponent<TransformComponent>().GetTransform());
        }
        else
        {
            Renderer::BeginScene(m_EditorCamera);
        }

        if (m_ShowPhysicsColliders)
        {
            // Box Colliders
            {
                auto view = m_ActiveScene->GetAllEntitiesWith<TransformComponent, BoxCollider2DComponent>();
                for (auto entity : view)
                {
                    auto [tc, bc2d] = view.get<TransformComponent, BoxCollider2DComponent>(entity);

                    glm::vec3 translation = tc.Translation + glm::vec3(bc2d.Offset, 0.001f);
                    glm::vec3 scale = tc.Scale * glm::vec3(bc2d.Size * 2.0f, 1.0f);

                    // box2d needs first translation then offset otherwise it offsets the bounding box from center instead of creating from center around
                    glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation)
                        * glm::rotate(glm::mat4(1.0f), tc.Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f))
                        * glm::translate(glm::mat4(1.0f), glm::vec3(bc2d.Offset, 0.001f))
                        * glm::scale(glm::mat4(1.0f), scale * glm::vec3(bc2d.Size * 2.0f, 1.0f));

                    Renderer::DrawRect(transform, glm::vec4(0, 1, 0, 1));
                }
            }
            // Circle Colliders
            {
                auto view = m_ActiveScene->GetAllEntitiesWith<TransformComponent, CircleCollider2DComponent>();
                for (auto entity : view)
                {
                    auto [tc, cc2d] = view.get<TransformComponent, CircleCollider2DComponent>(entity);

                    glm::vec3 translation = tc.Translation + glm::vec3(cc2d.Offset, 0.001f);
                    glm::vec3 scale = tc.Scale * glm::vec3(cc2d.Radius * 2.0f);

                    glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation)
                        * glm::scale(glm::mat4(1.0f), scale);

                    Renderer::DrawCircle(transform, glm::vec4(0, 1, 0, 1), 0.01f);
                }
            }
        }

        // Draw selected entity outline
        if (Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity())
        {
            const TransformComponent& transform = selectedEntity.GetComponent<TransformComponent>();
            Renderer::DrawRect(transform.GetTransform(), glm::vec4(1.0f, 0.5f, 0.0f, 1.0f));
        }
        Renderer::EndScene();*/
    }

    void EditorLayer::NewProject()
    {
        Project::New();
    }

    bool EditorLayer::OpenProject()
    {
        std::string filepath = Utility::OpenFile("Nox Project *.nproj\0nproj\0");

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
        std::string filepath = Utility::SaveFile("Vank Scene *.vank\0vank\0");
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
