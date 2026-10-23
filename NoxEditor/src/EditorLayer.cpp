#include "EditorLayer.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <unordered_set>
#include <functional>
#include <iostream>

#include <imgui.h>
#include <imgui_internal.h>// For Docking
#include <ImGuizmo.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_keyboard.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>  // for pointer to matrix or vector

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/MeshSerializer.h"
#include "NoxCore/Asset/SceneImporter.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Input.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Core/Window.h"
#include "NoxCore/Scene/EntityBounds.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/ImGui/ImGuiLayer.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Profiling/StatsOverlayLayer.h"
#include "NoxCore/Profiling/StatsReport.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Asset/PrefabImporter.h"
#include "NoxCore/Scene/PrefabInstance.h"
#include "NoxCore/Scene/SceneSerializer.h"
#include "NoxCore/Scripting/ScriptEngine.h"
#include "NoxCore/Utils/Utils.h"
#include "Panels/AssetPreviewRenderer.h"
#include "GamePackager.h"

namespace Nox
{

    namespace
    {
        // Unreal's snap steps: location in cm, rotation in degrees, scale as a factor.
        constexpr float kLocationSnapsCm[] = { 1.0f, 5.0f, 10.0f, 50.0f, 100.0f, 500.0f, 1000.0f, 5000.0f, 10000.0f };
        constexpr float kRotationSnaps[] = { 5.0f, 10.0f, 15.0f, 30.0f, 45.0f, 60.0f, 90.0f, 120.0f };
        constexpr float kScaleSnaps[] = { 0.1f, 0.25f, 0.5f, 1.0f };
    }

    namespace
    {
        constexpr const char* kViewModeNames[] = { "Perspective", "Top", "Bottom", "Front", "Back", "Left", "Right" };

        // Which viewports were open and their views, kept in imgui.ini next to the layout they dock into ("[NoxViewports][State]").
        // Static, not the layer's: ImGui writes the file once more when its context goes away, after the layer.
        struct ViewportIniState
        {
            bool Loaded = false;
            bool Open[4] = { true, false, false, false };
            int View[4] = { 0, 1, 3, 6 };
            // Each viewport's options as an extra view (ViewportSlot): show flags, grid, resolution, realtime.
            uint32_t Flags[4] = { Renderer::ExtraViewOutline, Renderer::ExtraViewOutline, Renderer::ExtraViewOutline, Renderer::ExtraViewOutline };
            int Grid[4] = { 1, 1, 1, 1 };
            int Resolution[4] = { 0, 0, 0, 0 };
            int Realtime[4] = { 1, 1, 1, 1 };
        };
        ViewportIniState s_ViewportIni;

        void RegisterViewportIniHandler()
        {
            ImGuiSettingsHandler handler;
            handler.TypeName = "NoxViewports";
            handler.TypeHash = ImHashStr("NoxViewports");
            handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char*) -> void* { return &s_ViewportIni; };
            handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
            {
                int index = 0, open = 0, view = 0, grid = 1, resolution = 0, realtime = 1;
                unsigned flags = Renderer::ExtraViewOutline;
                const int read = sscanf(line, "Viewport%d=%d,%d,%u,%d,%d,%d", &index, &open, &view, &flags, &grid, &resolution, &realtime);
                if (read >= 3 && index >= 0 && index < 4 && view >= 0 && view <= static_cast<int>(EditorViewMode::Right))
                {
                    s_ViewportIni.Open[index] = index == 0 || open != 0;
                    s_ViewportIni.View[index] = view;
                    if (read == 7) // the options (older files have the view only)
                    {
                        s_ViewportIni.Flags[index] = flags;
                        s_ViewportIni.Grid[index] = grid;
                        s_ViewportIni.Resolution[index] = std::clamp(resolution, 0, 2);
                        s_ViewportIni.Realtime[index] = realtime;
                    }
                    s_ViewportIni.Loaded = true;
                }
            };
            handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* self, ImGuiTextBuffer* buffer)
            {
                buffer->appendf("[%s][State]\n", self->TypeName);
                for (int index = 0; index < 4; ++index)
                    buffer->appendf("Viewport%d=%d,%d,%u,%d,%d,%d\n", index, s_ViewportIni.Open[index] ? 1 : 0, s_ViewportIni.View[index],
                                    s_ViewportIni.Flags[index], s_ViewportIni.Grid[index], s_ViewportIni.Resolution[index], s_ViewportIni.Realtime[index]);
                buffer->append("\n");
            };
            ImGui::AddSettingsHandler(&handler);
        }
    }

    EditorLayer::EditorLayer() : Layer("EditorLayer")
    {
        NOX_INFO("EditorLayer Start");

        auto& app = Application::Get();
        m_Renderer = app.GetRenderer();
        m_RenderGraphPanel = CreateScope<RenderGraphPanel>(m_Renderer);
        m_Renderer2D = m_Renderer->getRenderer2D();

        m_AssetOpeners[AssetType::AnimationGraph] = [this](AssetHandle handle) { OpenNodeGraphEditor(handle); };
        m_AssetOpeners[AssetType::Prefab] = [this](AssetHandle handle) { OpenPrefabMode(handle); };
        // A scene (from a Level Streaming Volume's list, say): opened for editing, as a double click in the Content Browser does.
        m_AssetOpeners[AssetType::Scene] = [this](AssetHandle handle) { OpenScene(handle); };
        m_AssetOpeners[AssetType::Material] = [this](AssetHandle handle) { OpenMaterialEditor(handle); };
        m_AssetOpeners[AssetType::AnimationSequence] = [this](AssetHandle handle)
        {
            for (auto& panel : m_AnimationClipPanels)
            {
                if (panel->GetClip() == handle)
                {
                    ImGui::SetWindowFocus(panel->GetTitle().c_str());
                    return;
                }
            }
            m_AnimationClipPanels.push_back(CreateScope<AnimationClipPanel>(handle));
        };
        // A skinned mesh opens the window of a clip made for it (cooked from the same model, else one in its folder), previewing on it.
        m_AssetOpeners[AssetType::Mesh] = [this](AssetHandle handle)
        {
            const auto& registry = Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry();
            const AssetMetadata& mesh = AssetManager::GetMetadata(handle);
            bool skinned = false;
            AssetHandle sameSource = 0, sameFolder = 0;
            for (const auto& [candidate, metadata] : registry)
            {
                if (metadata.Type == AssetType::Skeleton && !mesh.SourceFilePath.empty() && metadata.SourceFilePath == mesh.SourceFilePath)
                    skinned = true;
                if (metadata.Type != AssetType::AnimationSequence)
                    continue;
                if (!mesh.SourceFilePath.empty() && metadata.SourceFilePath == mesh.SourceFilePath && sameSource == 0)
                    sameSource = candidate;
                if (metadata.FilePath.parent_path() == mesh.FilePath.parent_path() && sameFolder == 0)
                    sameFolder = candidate;
            }
            const AssetHandle clip = sameSource != 0 ? sameSource : sameFolder;
            if (!skinned || clip == 0)
            {
                NOX_CORE_INFO("'{}': {}", mesh.FilePath.filename().string(), !skinned ? "not a skinned mesh, nothing to preview" : "no animation clip found for it");
                return;
            }
            for (auto& panel : m_AnimationClipPanels)
            {
                if (panel->GetClip() == clip)
                {
                    ImGui::SetWindowFocus(panel->GetTitle().c_str());
                    return;
                }
            }
            m_AnimationClipPanels.push_back(CreateScope<AnimationClipPanel>(clip, handle));
        };
        m_AssetOpeners[AssetType::Timeline] = [this](AssetHandle handle)
        {
            for (auto& panel : m_TimelinePanels)
            {
                if (panel->GetTimeline() == handle)
                {
                    ImGui::SetWindowFocus(panel->GetTitle().c_str());
                    return;
                }
            }
            m_TimelinePanels.push_back(CreateScope<TimelinePanel>(handle));
        };
        m_SceneHierarchyPanel.SetOpenAssetCallback([this](AssetHandle handle) { OpenAsset(handle); });
        m_SceneHierarchyPanel.SetFocusEntityCallback([this](Entity entity) { FocusOnEntity(entity); });
        m_SceneHierarchyPanel.SetPlaceAssetsCallback([this](const std::vector<AssetHandle>& handles, const std::string& folder)
        {
            // No viewport point for a drop on the hierarchy: at the point the editor camera orbits (in front of it).
            PlaceAssets(handles, m_EditorCamera.GetPosition() + m_EditorCamera.GetForwardDirection() * m_EditorCamera.GetDistance(), folder);
        });
        m_SceneHierarchyPanel.SetCreateShapeCallback([this](PrimitiveShape shape, const std::string& folder) { CreateShape(shape, folder); });

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
        else if (!OpenStartupProject())
        {
            // The user chose to quit without a project: the editor closes before its first frame (every frame needs one).
            SDL_Event event;
            event.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&event);
        }

        m_EditorCamera = EditorCamera(30.0f, 1.778f, 0.01f, 1000.0f);
        constexpr const char* kViewportTitles[kMaxViewports] = { "Viewport", "Viewport 2", "Viewport 3", "Viewport 4" };
        for (int index = 0; index < kMaxViewports; ++index)
            m_Viewports[index].Title = kViewportTitles[index];
        m_Viewports[0].Open = true;
        m_Viewports[0].CameraSet = true;
        // Before ImGui's first frame, which reads imgui.ini.
        RegisterViewportIniHandler();
        // Material thumbnails and the Material Editor's preview show on the Sphere shape.
        AssetPreviewRenderer::SetRenderer(m_Renderer);
        AssetPreviewRenderer::Get().SetSphereProvider([this]() { return GetShapeAsset(PrimitiveShape::Sphere); });

        if (!ScriptEngine::Initialize(SDL_GetBasePath(), true))
            NOX_CORE_ERROR("Failed to initialize the .NET scripting backend");
    }

    EditorLayer::~EditorLayer()
    {
        NOX_CORE_INFO("EditorLayer Shutdown");

        SaveEditorCameras();
        ScriptEngine::Shutdown();
        AssetPreviewRenderer::Get().Clear();
        AssetPreviewRenderer::SetRenderer(nullptr);

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
        if (!Project::GetActive())
            return; // closing: no project was opened

        // A click or a scroll in another viewport makes it the active one first, so its camera and the tools get the input. Not while
        // a button is held: a drag that began in the active viewport keeps it.
        bool activatedViewport = false;
        if ((event.GetEventType() == EventType::MouseButtonPressed || event.GetEventType() == EventType::MouseScrolled) && m_MouseCaptureViewport < 0)
        {
            const int hovered = HoveredViewport();
            if (hovered >= 0 && hovered != m_ActiveViewport)
            {
                ActivateViewport(hovered);
                activatedViewport = true;
            }
        }

        if (m_SceneState == SceneState::Edit && m_ViewportHovered)
        {
            const ImVec2 mouse = ImGui::GetMousePos();
            m_EditorCamera.SetCursorPosition({ mouse.x - m_ViewportBounds[0].x, mouse.y - m_ViewportBounds[0].y });
            m_EditorCamera.OnEvent(event);
        }

        EventDispatcher dispatcher(event);
        dispatcher.Dispatch<ExternalFileDropEvent>([this](ExternalFileDropEvent& drop)
        {
            if (m_ContentBrowserPanel)
                m_ContentBrowserPanel->OnExternalFileDrop(drop.GetPath());
            return false;
        });
        dispatcher.Dispatch<KeyPressedEvent>(Nox_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
        // The click that activated a viewport only does that (its picking under the cursor is not there yet).
        if (!activatedViewport)
            dispatcher.Dispatch<MouseButtonPressedEvent>(Nox_BIND_EVENT_FN(EditorLayer::OnMouseButtonPressed));
    }

    void EditorLayer::OnUpdate(Timestep ts)
    {
        if (!Project::GetActive())
            return; // closing: no project was opened
        // A scene being opened: a slice of its entities each frame (8 ms), swapped in once complete.
        if (m_SceneLoad && m_SceneLoad->Step(8.0))
            FinishSceneLoad();
        UpdatePrefabModeDirty();
        m_ScriptBuilder.Update();

        // Game scripts read the mouse only over the viewport and the keys only while it is the target: a click in the hierarchy
        // or a key typed in a text field is not the game's.
        Input::SetGameInputEnabled(m_ViewportFocused || m_ViewportHovered, m_ViewportHovered);

        // Deleting entities, switching scenes, or leaving Play can leave meshes/textures unreferenced.
        // Sweep at the start of the next frame, before anything re-requests them this frame.
        if (m_EditorScene && m_EditorScene->ConsumeAssetReferencesChanged())
            m_UnloadUnusedAssetsRequested = true;
        if (m_ActiveScene && m_ActiveScene != m_EditorScene && m_ActiveScene->ConsumeAssetReferencesChanged())
            m_UnloadUnusedAssetsRequested = true;
        // Loads still streaming in during the last sweep have finished: what nothing uses any more goes too.
        if (Project::GetActive()->GetEditorAssetManager()->ConsumeLoadsSettledAfterSweep())
            m_UnloadUnusedAssetsRequested = true;
        if (m_UnloadUnusedAssetsRequested)
        {
            NOX_PROFILE_SCOPE("Unload Unused Assets");
            m_UnloadUnusedAssetsRequested = false;
            UnloadUnusedAssets();
        }


        // Import Into Level requests whose assets finished cooking: their scene goes into the level being edited.
        for (const ModelInstance::LevelDescription& level : Project::GetActive()->GetEditorAssetManager()->ConsumeLevelImports())
        {
            if (m_EditorScene)
            {
                Entity imported = ModelInstance::SpawnLevel(*m_EditorScene, level);
                if (m_ActiveScene == m_EditorScene)
                    m_SceneHierarchyPanel.SetSelectedEntity(imported);
            }
        }
        m_ActiveScene->OnViewportResize(m_ViewportSize.x, m_ViewportSize.y);

        // zero sized framebuffer is invalid
        if (m_ViewportSize.x > 0.0f && m_ViewportSize.y > 0.0f)
        {
            // Verify if the viewport has a new size and resize the RenderTarget accordingly.
            // Whole pixels: the panel's size is a float (docking and DPI scaling make it fractional), and comparing it with
            // the renderer's integer size was never equal, so the viewport "changed" every frame and the renderer rebuilt
            // its targets, dropped every history and flushed the GPU again and again (every ~120 ms), which showed up as
            // jitter while the camera moved -- timings looked fine because the profiler stats reset with each rebuild.
            const NRI::Extent2D wantedSize{ static_cast<uint32_t>(m_ViewportSize.x), static_cast<uint32_t>(m_ViewportSize.y) };
            NRI::Extent2D viewportSize = m_Renderer->getViewPortSize();
            if (wantedSize.width != viewportSize.width || wantedSize.height != viewportSize.height)
            {
                m_Renderer->onViewportSizeChange(wantedSize);
                m_EditorCamera.SetViewportSize(static_cast<float>(wantedSize.width), static_cast<float>(wantedSize.height));
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

                    m_EditorCamera.OnUpdate(ts, m_MouseCaptureViewport == m_ActiveViewport);

                    m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
                    break;
                }
            case SceneState::Simulate:
                {
                    m_EditorCamera.OnUpdate(ts, m_MouseCaptureViewport == m_ActiveViewport);

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
        // The other viewports' cameras, drawn after the main view this frame; the asset thumbnail in progress.
        SubmitExtraViewports(ts);
        AssetPreviewRenderer::Get().Update();

        {
            NOX_PROFILE_SCOPE("Asset Manager Update");
            Project::GetActive()->GetEditorAssetManager()->Update();
        }
    }

    void EditorLayer::OnRender()
    {
        if (!Project::GetActive())
            return; // closing: no project was opened
    }

    void EditorLayer::OnImGuiRender()
    {
        if (!Project::GetActive())
            return; // closing: no project was opened

        // Through ImGui's own cursor state instead of raw SDL calls (Input::SetCursorVisible): those fought
        // ImGui_ImplSDL3_NewFrame's own per-frame cursor update and lost unpredictably (flicker). Telling ImGui itself
        // "no cursor" for this frame is the way the backend expects to be told, so there is nothing left to race against.
        // ShouldHideCursor(), not just "is flying": UE5 doesn't hide it on the click alone, only once the mouse actually moves.
        if (m_EditorCamera.ShouldHideCursor())
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);

        /*--
        * IMGUI Docking
        * Create a dockspace and dock the viewport and settings window.
        * The central node is named "Viewport", which can be used later with Begin("Viewport")
        * to render the final image.
        -*/

        const ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingInCentralNode;

        // Before the dockspace, which fits above it.
        UI_StatusBar();
        UI_PrefabExitPrompt();
        UI_SceneLoadProgress();
        UI_PackageGame();

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
        if (!ImGui::DockBuilderGetNode(dockID)->IsSplitNode() && !ImGui::FindWindowByName("Viewports"))
        {
            ImGui::DockBuilderDockWindow("Viewports", dockID); // Dock the viewport area (UI_Viewports) to the central node
            ImGui::DockBuilderGetCentralNode(dockID)->LocalFlags |= ImGuiDockNodeFlags_NoTabBar; // Remove "Tab" from the central node
            ImGuiID leftID = ImGui::DockBuilderSplitNode(dockID, ImGuiDir_Left, 0.2f, nullptr, &dockID); // Split the central node
            ImGui::DockBuilderDockWindow("Settings", leftID); // Dock "Settings" to the left node
        }
        // A layout saved before the viewport area existed (the one "Viewport" window sat in the central node): the area takes that
        // place, and UI_Viewports moves the viewport into it (the one-viewport layout, applied while the area's dockspace is new).
        else if (!ImGui::FindWindowByName("Viewports") && !ImGui::FindWindowSettingsByID(ImHashStr("Viewports")))
        {
            if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockID))
                ImGui::DockBuilderDockWindow("Viewports", central->ID);
        }

        // [optional] Show the menu bar
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("New Project..."))
                {
                    NewProject();
                }
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

                if (ImGui::MenuItem("Package Game..."))
                {
                    if (m_PackageOutput[0] == '\0' && Project::GetActive())
                    {
                        const std::filesystem::path defaultOutput = Project::GetProjectDirectory() / "Build" / Project::GetActive()->GetConfig().Name;
                        snprintf(m_PackageOutput, sizeof(m_PackageOutput), "%s", defaultOutput.generic_string().c_str());
                    }
                    m_PackageResult.clear();
                    m_ShowPackageGame = true;
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Exit"))
                    Application::Shutdown();

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit"))
            {
                const bool editing = m_SceneState == SceneState::Edit;
                const std::string undoLabel = m_History.CanUndo() ? "Undo " + m_History.GetUndoName() : std::string("Undo");
                const std::string redoLabel = m_History.CanRedo() ? "Redo " + m_History.GetRedoName() : std::string("Redo");
                if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, editing && m_History.CanUndo()))
                    Undo();
                if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y / Ctrl+Shift+Z", false, editing && m_History.CanRedo()))
                    Redo();
                ImGui::Separator();
                ImGui::TextDisabled("History: %zu steps, %.1f MB", m_History.GetStateCount() > 0 ? m_History.GetStateCount() - 1 : 0,
                                    static_cast<double>(m_History.GetMemoryBytes()) / (1024.0 * 1024.0));
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Create"))
            {
                if (ImGui::MenuItem("Empty Entity") && m_ActiveScene)
                    m_ActiveScene->CreateEntity("Empty Entity");
                if (ImGui::BeginMenu("Shapes"))
                {
                    for (uint8_t index = 0; index < static_cast<uint8_t>(PrimitiveShape::Count); ++index)
                    {
                        const PrimitiveShape shape = static_cast<PrimitiveShape>(index);
                        if (ImGui::MenuItem(PrimitiveShapes::GetName(shape)))
                            CreateShape(shape, std::string());
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Window"))
            {
                bool renderGraphOpen = m_RenderGraphPanel->IsOpen();
                if (ImGui::MenuItem("Render Graph", nullptr, &renderGraphOpen))
                    m_RenderGraphPanel->SetOpen(renderGraphOpen);
                bool profilerOpen = m_ProfilerPanel.IsOpen();
                if (ImGui::MenuItem("Profiler", nullptr, &profilerOpen))
                    m_ProfilerPanel.SetOpen(profilerOpen);

                // Up to four viewports, each with its own camera (docs/Future.md item 5). A layout snaps them into place; a
                // viewport's tab can then be dragged anywhere and the splitters resized.
                if (ImGui::BeginMenu("Viewports"))
                {
                    if (ImGui::MenuItem("1 Viewport"))
                        m_PendingViewportLayout = static_cast<int>(ViewportLayout::One);
                    if (ImGui::MenuItem("2 Side by Side"))
                        m_PendingViewportLayout = static_cast<int>(ViewportLayout::TwoSideBySide);
                    if (ImGui::MenuItem("2 Stacked"))
                        m_PendingViewportLayout = static_cast<int>(ViewportLayout::TwoStacked);
                    if (ImGui::MenuItem("3 Viewports"))
                        m_PendingViewportLayout = static_cast<int>(ViewportLayout::Three);
                    if (ImGui::MenuItem("4 Viewports (2 x 2)"))
                        m_PendingViewportLayout = static_cast<int>(ViewportLayout::Four);
                    ImGui::Separator();
                    for (int index = 1; index < kMaxViewports; ++index)
                    {
                        bool open = m_Viewports[index].Open;
                        if (ImGui::MenuItem(m_Viewports[index].Title, nullptr, &open))
                        {
                            if (open)
                                OpenViewport(index);
                            else
                                m_Viewports[index].Open = false;
                        }
                    }
                    ImGui::Separator();
                    for (int index = 0; index < kMaxViewports; ++index)
                        UI_ViewportOptionsMenu(index);
                    ImGui::Separator();
                    ImGui::TextDisabled("Click a viewport to use it; drag its tab to move it");
                    ImGui::EndMenu();
                }

                // Temporary entry point (docs/Animation_Graph_Architecture_Plan_2026.md Step 3); Step 4 replaces
                // this with double-clicking a graph asset reference.
                if (ImGui::BeginMenu("Animation Graphs"))
                {
                    auto assetManager = Project::GetActive()->GetEditorAssetManager();
                    const auto& registry = assetManager->GetAssetRegistry();
                    bool any = false;
                    for (const auto& [handle, metadata] : registry)
                    {
                        if (metadata.Type != AssetType::AnimationGraph)
                            continue;
                        any = true;
                        if (ImGui::MenuItem(metadata.FilePath.stem().string().c_str()))
                            OpenNodeGraphEditor(handle);
                    }
                    if (!any)
                        ImGui::TextDisabled("No .nanimgraph assets found");
                    ImGui::EndMenu();
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Script"))
            {
                if (ImGui::MenuItem("Build and Reload", "Ctrl+B", false, !m_ScriptBuilder.IsBuilding()))
                    m_ScriptBuilder.RequestBuild();
                if (ImGui::MenuItem("Reload assembly", "Ctrl+R"))
                {
                    ScriptEngine::ReloadAssembly();
                }
                ImGui::MenuItem("Build on Save", nullptr, &m_ScriptBuilder.AutoBuild);
                bool waitForDebugger = ScriptEngine::GetWaitForDebugger();
                if (ImGui::MenuItem("Wait for Debugger on Play", nullptr, &waitForDebugger))
                    ScriptEngine::SetWaitForDebugger(waitForDebugger);
                ImGui::SetItemTooltip("Play waits (up to 60 s) until a .NET debugger attaches to the editor, then breaks once.\n"
                                      "Visual Studio: Debug > Attach to Process, NoxEditor, code type Managed (.NET Core).\n"
                                      "Rider: Run > Attach to Process. Breakpoints in the scripts then hit (the .pdb is loaded with them).");
                ImGui::SetItemTooltip("Saving a .cs file of the project builds its scripts and reloads them a moment later.");
                if (!m_ScriptBuilder.GetStatus().empty())
                    ImGui::TextDisabled("%s", m_ScriptBuilder.GetStatus().c_str());

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

        UI_Viewports();

        // Extra ImGui windows can be added in OnImGuiRender() layer, like the demo window.
        // ImGui::ShowDemoWindow();

        m_SceneHierarchyPanel.OnImGuiRender();
        m_ContentBrowserPanel->OnImGuiRender();
        m_RenderGraphPanel->OnImGuiRender();
        m_ProfilerPanel.OnImGuiRender();

        for (auto& panel : m_NodeGraphEditors)
            panel->OnImGuiRender();
        std::erase_if(m_NodeGraphEditors, [](const Scope<NodeGraphEditorPanel>& panel) { return !panel->IsOpen(); });
        for (auto& panel : m_MaterialEditors)
            panel->OnImGuiRender();
        std::erase_if(m_MaterialEditors, [](const Scope<MaterialEditorPanel>& panel) { return !panel->IsOpen(); });
        for (auto& panel : m_AnimationClipPanels)
            panel->OnImGuiRender();
        std::erase_if(m_AnimationClipPanels, [](const Scope<AnimationClipPanel>& panel) { return !panel->IsOpen(); });
        for (auto& panel : m_TimelinePanels)
            panel->OnImGuiRender(m_ActiveScene, m_SceneHierarchyPanel.GetSelectedEntity(), m_SceneState == SceneState::Edit);
        std::erase_if(m_TimelinePanels, [](const Scope<TimelinePanel>& panel)
        {
            if (panel->IsOpen())
                return false;
            panel->EndPreview(); // what the targets had comes back
            return true;
        });

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

        // Render quality presets (docs/Hybrid_Renderer_Plan_2026-10.md §2), saved in the project file.
        {
            ProjectConfig& config = Project::GetActive()->GetConfig();
            const Renderer::RenderQuality recommended = m_Renderer->GetRecommendedQuality();
            const Renderer::RenderQuality current = m_Renderer->GetCurrentQuality();
            const bool automatic = config.RenderQuality == "Auto";
            char preview[64];
            if (automatic && current == recommended)
                snprintf(preview, sizeof(preview), "Auto (%s)", Renderer::GetQualityName(recommended));
            else
                snprintf(preview, sizeof(preview), "%s", Renderer::GetQualityName(current));

            auto applyQuality = [this](Renderer::RenderQuality quality)
            {
                m_Renderer->ApplyQualityPreset(quality);
                // An ortho view stays raster only; the preset comes back when it is left.
                if (m_EditorCamera.IsOrthographic())
                {
                    m_OrthoSaved = { true, m_Renderer->isDLSSEnabled(), m_Renderer->getRayTracingEnabled(), m_Renderer->isPathTracingEnabled() };
                    m_Renderer->setDLSSEnabled(false);
                    m_Renderer->setRayTracingEnabled(false);
                    m_Renderer->setPathTracingEnabled(false);
                }
            };

            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::BeginCombo("Render Quality", preview))
            {
                char autoLabel[64];
                snprintf(autoLabel, sizeof(autoLabel), "Auto (%s for this GPU)", Renderer::GetQualityName(recommended));
                if (ImGui::Selectable(autoLabel, automatic))
                {
                    config.RenderQuality = "Auto";
                    applyQuality(recommended);
                    Project::SaveActive();
                }
                for (Renderer::RenderQuality quality : { Renderer::RenderQuality::Low, Renderer::RenderQuality::Medium, Renderer::RenderQuality::High })
                {
                    if (ImGui::Selectable(Renderer::GetQualityName(quality), !automatic && config.RenderQuality == Renderer::GetQualityName(quality)))
                    {
                        config.RenderQuality = Renderer::GetQualityName(quality);
                        applyQuality(quality);
                        Project::SaveActive();
                    }
                }
                if (ImGui::Selectable("Custom (keep these settings, no preset at start)", config.RenderQuality == "Custom"))
                {
                    config.RenderQuality = "Custom";
                    Project::SaveActive();
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Every preset is ray traced (no shadow maps):\n"
                                  "  Low: RT shadows of the sun, screen-probe GI (24 px probes), SSAO, DLSS Performance (the fallback for smaller RT GPUs)\n"
                                  "  Medium: + RT reflections, screen-probe GI (16 px), DLSS Balanced\n"
                                  "  High: + RTAO, RT glass, DLSS Quality (the closest to the path tracer)\n"
                                  "The project opens with it (saved in the .nproj). Changing a setting below makes it Custom for this session.");

            // Saved on top of the preset (the .nproj's Renderer map): the game starts with them too.
            if (ImGui::TreeNode("Project Settings (saved, on top of the preset)"))
            {
                ProjectRendererSettings& saved = config.Renderer;
                bool changed = false;
                auto choice = [&](const char* label, int32_t& value, std::initializer_list<std::pair<int32_t, const char*>> options)
                {
                    const char* current = "Preset's";
                    for (const auto& [optionValue, name] : options)
                        if (optionValue == value) current = name;
                    ImGui::SetNextItemWidth(180.0f);
                    if (ImGui::BeginCombo(label, current))
                    {
                        if (ImGui::Selectable("Preset's", value < 0)) { value = -1; changed = true; }
                        for (const auto& [optionValue, name] : options)
                            if (ImGui::Selectable(name, value == optionValue)) { value = optionValue; changed = true; }
                        ImGui::EndCombo();
                    }
                };
                choice("Ray Tracing Resolution##Project", saved.RayTracingResolution, { { 1, "Full" }, { 2, "Half" } });
                choice("Diffuse GI##Project", saved.DiffuseGI, { { 0, "Environment (IBL)" }, { 1, "DDGI" }, { 2, "ReSTIR GI" }, { 3, "Screen Probes" } });
                choice("Shadowed Lights##Project", saved.ShadowedLights, { { 0, "All" }, { 1, "1" }, { 2, "2" }, { 4, "4" }, { 8, "8" } });
                choice("Order-Independent Transparency##Project", saved.OIT, { { 0, "Off" }, { 1, "On" } });
                choice("Temporal Anti-Aliasing##Project", saved.TAA, { { 0, "Off" }, { 1, "On" } });

                const char* windowModes[] = { "Windowed", "Borderless", "Fullscreen" };
                int windowMode = static_cast<int>(ParseWindowMode(config.WindowMode));
                ImGui::SetNextItemWidth(180.0f);
                if (ImGui::Combo("Game Window", &windowMode, windowModes, IM_ARRAYSIZE(windowModes)))
                {
                    config.WindowMode = windowModes[windowMode];
                    Project::SaveActive();
                }
                ImGui::SetItemTooltip("How the packaged game opens (NoxRuntime): Borderless covers the screen at the desktop's resolution,\n"
                                      "Fullscreen switches the display (exclusive). Alt+Enter toggles a window in the game.");
                if (changed)
                {
                    m_Renderer->ApplyProjectRendererSettings(saved);
                    Project::SaveActive();
                }
                ImGui::TreePop();
            }
            ImGui::Separator();
        }

        if (ImGui::Button("Dump Frame Graph"))
        {
            // Next to Nox.log (opened relative to the working directory); open the .dot files with GraphViz.
            const std::filesystem::path directory = std::filesystem::current_path();
            if (m_ActiveScene && m_ActiveScene->DumpSystemGraphs(directory))
                NOX_CORE_INFO("Frame graph written: {}", (directory / "SceneUpdate.dot").string());
        }
        ImGui::Separator();
        
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
            "16: Indirect Diffuse GI Only (Screen Probes / DDGI / ReSTIR GI)",
            "17: DDGI Probe Grid Spheres",
            "18: Path Tracer (1-SPP Raw)",
            "19: Path Tracer (Progressive Ground Truth)",
            "20: Texture Streaming Mips (red: needs more, green: as needed, blue: more than needed)",
            "21: Cluster LOD Level (green: original, then yellow, orange, red, magenta, blue)",
            "22: Ambient Occlusion (SSAO/RTAO, white = unoccluded)"
        };
        int currentMode = static_cast<int>(m_Renderer->getDebugMode());
        if (ImGui::Combo("PBR Debug View", &currentMode, debugModeNames, IM_ARRAYSIZE(debugModeNames)))
        {
            m_Renderer->setDebugMode(static_cast<uint32_t>(currentMode));
        }

        // Texture streaming (§5.12): what the streamed textures hold and want, and a smaller pool to force mip reduction.
        if (ImGui::CollapsingHeader("Texture Streaming"))
        {
            TextureStreamer& streamer = Project::GetActive()->GetEditorAssetManager()->GetTextureStreamer();
            const TextureStreamer::Stats& stats = streamer.GetStats();
            constexpr double MB = 1024.0 * 1024.0;
            ImGui::Text("Streamed textures: %zu", stats.Textures);
            ImGui::Text("Resident %.0f MB | wanted %.0f MB | pool %.0f MB", stats.ResidentBytes / MB, stats.WantedBytes / MB,
                        stats.PoolBytes == UINT64_MAX ? 0.0 : stats.PoolBytes / MB);
            int poolOverrideMB = static_cast<int>(streamer.GetPoolOverride() / (1024 * 1024));
            if (ImGui::SliderInt("Pool Override (MB, 0 = budget)", &poolOverrideMB, 0, 4096))
                streamer.SetPoolOverride(static_cast<uint64_t>(poolOverrideMB) * 1024 * 1024);
        }
        
        static const char* tonemapModeNames[] = {
            "0: None (Clamped Linear)",
            "1: ACES (Narkowicz)",
            "2: ACES (Hill)",
            "3: ACES (Hill + Exposure Boost)",
            "4: Khronos PBR Neutral",
            "5: Reinhard",
            "6: Reinhard Modified",
            "7: Heji Hable ALU",
            "8: Hable UC2 (RTXPT Standard)",
            "9: ACES (RTXPT Standard)"
        };
        int currentTonemap = static_cast<int>(m_Renderer->getTonemapMode());
        if (ImGui::Combo("Tonemapping Mode", &currentTonemap, tonemapModeNames, IM_ARRAYSIZE(tonemapModeNames)))
        {
            m_Renderer->setTonemapMode(static_cast<uint32_t>(currentTonemap));
        }
        
        float exposure = m_Renderer->getExposure();
        // Logarithmic up to 1000 (+10 EV): a moonlit night is ~9 stops under daylight (or the camera's Auto Exposure).
        if (ImGui::SliderFloat("Exposure", &exposure, 0.01f, 1000.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
        {
            m_Renderer->setExposure(exposure);
        }
        bool autoExposure = m_Renderer->getViewportAutoExposure();
        if (ImGui::Checkbox("Auto Exposure", &autoExposure))
            m_Renderer->setViewportAutoExposure(autoExposure);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Adapts the exposure to the image, like the eye: needed to see a night lit by the moon or lamps\n"
                              "(they are ~10 stops darker than daylight). On whatever the scene camera's Auto Exposure says.");

        float gamma = m_Renderer->getGamma();
        if (ImGui::SliderFloat("Gamma", &gamma, 0.5f, 3.5f, "%.2f"))
        {
            m_Renderer->setGamma(gamma);
        }

        // There was no way to change this without editing the hardcoded constructor value in
        // EditorLayer's OnAttach and recompiling - this is that missing control.
        // The slider is horizontal FOV (Unreal's convention -- its CameraComponent FOV is horizontal): at a wide aspect ratio,
        // the same *vertical* FOV number covers a much wider horizontal angle, which is what made 120 look so extreme (that was
        // ~144 degrees horizontal on a 16:9 view, an actual fisheye, not a bug). EditorCamera itself still works in vertical FOV
        // throughout (the projection matrix, DLSS, the gizmo) -- only this slider converts, through the current aspect ratio.
        const float aspect = std::max(m_EditorCamera.GetAspectRatio(), 0.01f);
        float horizontalFov = glm::degrees(2.0f * std::atan(std::tan(glm::radians(m_EditorCamera.GetFOV()) * 0.5f) * aspect));
        if (ImGui::SliderFloat("Camera FOV", &horizontalFov, 10.0f, 120.0f, "%.1f"))
        {
            const float verticalFov = glm::degrees(2.0f * std::atan(std::tan(glm::radians(horizontalFov) * 0.5f) / aspect));
            m_EditorCamera.SetFOV(verticalFov);
        }

        float iblAmbient = m_Renderer->getScaleIBLAmbient();
        if (ImGui::SliderFloat("IBL Ambient Scale", &iblAmbient, 0.0f, 5.0f, "%.2f"))
        {
            m_Renderer->setScaleIBLAmbient(iblAmbient);
        }

        // What the current mode makes these settings: the path tracer shades glass, blending and anti-aliasing itself; DLSS
        // anti-aliases (TAA and the jitter switch then do nothing). Settings that cannot apply are shown locked, with why.
        const bool pathTracing = m_Renderer->isPathTracingEnabled();
        const bool dlssActive = m_Renderer->isDLSSSupported() && m_Renderer->isDLSSEnabled() && m_Renderer->getUpscaleMode() != NRI::UpscaleMode::Off;
        auto lockedReason = [](const char* reason)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", reason);
        };

        ImGui::BeginDisabled(pathTracing);
        bool refraction = m_Renderer->getRefractionEnabled();
        if (ImGui::Checkbox("Glass Refraction", &refraction))
            m_Renderer->setRefractionEnabled(refraction);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Transmissive materials (glass) show the scene behind them bent by their IOR and thickness and blurred\n"
                              "by their roughness (a copy of the lit scene with blur mips, only while glass is in view).\n"
                              "Off: transmission only makes them see-through.");
        ImGui::BeginDisabled(!refraction);
        ImGui::Indent();
        bool rtRefraction = m_Renderer->getRTRefractionEnabled();
        if (ImGui::Checkbox("Ray Traced Glass", &rtRefraction))
            m_Renderer->setRTRefractionEnabled(rtRefraction);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Smooth glass traces what is behind it like the path tracer: bent in at the surface and out at the\n"
                              "mesh's far side (glass with a volume thickness), then through the scene, so objects off screen or\n"
                              "hidden behind others show too. Rough glass keeps the blurred screen-space refraction. Needs ray tracing.");
        ImGui::Unindent();
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (pathTracing)
            lockedReason("the path tracer refracts itself");

        const bool rayTracing = m_Renderer->getRayTracingEnabled();
        ImGui::BeginDisabled(pathTracing || rayTracing);
        bool oit = m_Renderer->getOITEnabled();
        if (ImGui::Checkbox("Order-Independent Transparency", &oit))
            m_Renderer->setOITEnabled(oit);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Raster transparency only: transparent surfaces and alpha-blended particles blend in any order\n"
                              "(weighted blended OIT), so overlapping layers (smoke, foliage cards) stop popping. It is an\n"
                              "approximation: overlapping layers average out, so solid-looking transparents (glass in front of\n"
                              "glass, thick leaves) look see-through. Off (sorted back to front) is usually better for glass.\n"
                              "Raster pipeline only: not used with ray tracing (hybrid sorts glass and traces through it) or by\n"
                              "the path tracer.");
        ImGui::EndDisabled();
        if (pathTracing)
            lockedReason("path tracer");
        else if (rayTracing)
            lockedReason("raster only: ray tracing is on");

        ImGui::BeginDisabled(dlssActive || pathTracing);
        bool taa = m_Renderer->getTAAEnabled();
        if (ImGui::Checkbox("Temporal Anti-Aliasing", &taa))
            m_Renderer->setTAAEnabled(taa);
        ImGui::EndDisabled();
        if (dlssActive)
            lockedReason("DLSS anti-aliases");
        else if (pathTracing)
            lockedReason("path tracer accumulates");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Smooth edges without DLSS: the camera shifts by a fraction of a pixel each frame and the frames are\n"
                              "blended along the motion vectors. Used while DLSS is off or not supported (DLSS / DLAA does this\n"
                              "itself) and not with the path tracer, which accumulates on its own.");
        if (taa && m_Renderer->isTAAActive())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(active)");
        }

        // DLSS and TAA jitter the camera themselves (always on while they run): the switch only matters without them.
        const bool jitterForced = dlssActive || m_Renderer->isTAAActive();
        ImGui::BeginDisabled(jitterForced);
        bool jitter = jitterForced || m_Renderer->getCameraJitterEnabled();
        if (ImGui::Checkbox("Camera Subpixel Jitter (R2)", &jitter))
        {
            m_Renderer->setCameraJitterEnabled(jitter);
        }
        ImGui::EndDisabled();
        if (jitterForced)
            lockedReason(dlssActive ? "on: DLSS needs it" : "on: TAA needs it");
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
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Also the ray tracing resolution: every ray traced effect (shadows, reflections, GI, AO, glass)\n"
                                      "runs at the render resolution and DLSS upscales the result. Per axis: DLAA 100%%, Quality 67%%,\n"
                                      "Balanced 58%%, Performance 50%%, Ultra Performance 33%% -- Performance traces a quarter of DLAA's rays.");

                bool stableDepth = m_Renderer->getStableEditorDepth();
                if (ImGui::Checkbox("Stable Overlays", &stableDepth))
                    m_Renderer->setStableEditorDepth(stableDepth);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Draws the scene's depth and entity IDs again without the camera jitter, so the grid, gizmo lines,\n"
                                      "collider wireframes and picking hold still while DLSS is on (a depth-only pass of the scene).\n"
                                      "Off: they come from the jittered render and crawl along edges.");
                
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
            // Ambient Occlusion: SSAO (raster-only) and RTAO (needs Hybrid Ray Tracing, below) both only ever feed
            // the same occlusion term -- see the comment on Renderer::m_ssaoEnabled -- so this is one set of controls
            // for whichever is active, not two separate sections to keep in sync.
            ImGui::SeparatorText("Ambient Occlusion");
            bool ssaoEnabled = m_Renderer->getSSAOEnabled();
            if (ImGui::Checkbox("SSAO", &ssaoEnabled))
                m_Renderer->setSSAOEnabled(ssaoEnabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Screen-space contact/ambient occlusion. Mutually exclusive with RTAO (below, under Hybrid Ray Tracing).");
            if (m_Renderer->getSSAOEnabled() || m_Renderer->getRTAOEnabled())
            {
                ImGui::Indent();
                float aoRadius = WorldUnits::ToCentimeters(m_Renderer->getSSAORadius());
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::SliderFloat("Radius (cm)", &aoRadius, 1.0f, 200.0f, "%.1f"))
                    m_Renderer->setSSAORadius(WorldUnits::FromCentimeters(aoRadius));
                float aoPower = m_Renderer->getSSAOPower();
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::SliderFloat("Power", &aoPower, 0.5f, 4.0f, "%.2f"))
                    m_Renderer->setSSAOPower(aoPower);
                ImGui::Unindent();
            }
            ImGui::Separator();

            // Hybrid Ray Tracing options (only visible when not in full Path Tracing)
            bool rtEnabled = m_Renderer->getRayTracingEnabled();
            if (ImGui::Checkbox("Enable Hybrid Ray Tracing", &rtEnabled))
            {
                m_Renderer->setRayTracingEnabled(rtEnabled);
            }

            if (rtEnabled)
            {
                ImGui::Indent();
                static const char* rtResolutionNames[] = { "Full (every pixel)", "Half (a quarter of the rays)" };
                const bool rrActive = m_Renderer->isDLSSSupported() && m_Renderer->isDLSSEnabled() &&
                                      m_Renderer->getUpscaleMode() != NRI::UpscaleMode::Off &&
                                      m_Renderer->isDLSSRayReconstructionSupported() && m_Renderer->isDLSSRayReconstructionEnabled();
                // Ray Reconstruction wants its own per-pixel noise: the renderer traces at full resolution under it.
                ImGui::BeginDisabled(rrActive);
                int rtResolution = (m_Renderer->getRayTracingResolutionDivisor() > 1 && !rrActive) ? 1 : 0;
                if (ImGui::Combo("Ray Tracing Resolution", &rtResolution, rtResolutionNames, IM_ARRAYSIZE(rtResolutionNames)))
                    m_Renderer->setRayTracingResolutionDivisor(rtResolution == 1 ? 2u : 1u);
                ImGui::EndDisabled();
                if (rrActive)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(full: Ray Reconstruction)");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Only the ray traced effects, not the image: textures, geometry and DLSS stay as they are.\n"
                                      "Half: RT reflections, RT sun shadows and RTAO trace one pixel of every 2 x 2 block, a different\n"
                                      "one each frame; the others are filled from their neighbours on the same surface before the\n"
                                      "denoisers, which accumulate the four over frames. About a quarter of their cost, a little\n"
                                      "softer. The GI has its own setting (Screen Probe Spacing); the other lights' shadow rays stay\n"
                                      "per pixel.");
                bool rtShadows = m_Renderer->getRayTracingShadows();
                if (ImGui::Checkbox("Ray Tracing Shadows", &rtShadows))
                {
                    m_Renderer->setRayTracingShadows(rtShadows);
                }
                if (rtShadows)
                {
                    int shadowLights = static_cast<int>(m_Renderer->getShadowRayLightLimit());
                    if (ImGui::SliderInt("Shadowed Lights per Pixel", &shadowLights, 0, 8, shadowLights == 0 ? "All" : "%d"))
                        m_Renderer->setShadowRayLightLimit(static_cast<uint32_t>(shadowLights));
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Each pixel traces shadow rays only for the lights that light it the most; the weaker ones\n"
                                          "light it without a shadow. The sun's soft shadow is always traced. All: every light in range.");
                }

                bool rtReflections = m_Renderer->getRayTracingReflections();
                if (ImGui::Checkbox("Ray Tracing Reflections", &rtReflections))
                {
                    m_Renderer->setRayTracingReflections(rtReflections);
                }

                bool rtaoEnabled = m_Renderer->getRTAOEnabled();
                if (ImGui::Checkbox("Ray Tracing Ambient Occlusion (RTAO)", &rtaoEnabled))
                {
                    m_Renderer->setRTAOEnabled(rtaoEnabled);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Traces real rays for ambient occlusion instead of SSAO's screen-space reprojection -- turning this on turns SSAO off (see Ambient Occlusion, above). Same radius/power settings, so the two are directly comparable.");

                bool rtCulling = m_Renderer->getRayTracingCulling();
                if (ImGui::Checkbox("Ray Tracing Culling", &rtCulling))
                    m_Renderer->setRayTracingCulling(rtCulling);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Leaves far, small objects out of what rays can hit (Unreal's ray tracing culling): an object\n"
                                      "farther than the radius whose bounds look smaller than the angle is not in the TLAS. Within the\n"
                                      "radius, or larger than the angle, everything stays -- in front of the camera or behind it.\n"
                                      "Over the ray tracing memory budget, the BLAS of meshes not traced for a while are freed and\n"
                                      "built again when needed.");
                if (rtCulling)
                {
                    float cullRadiusMeters = WorldUnits::ToMeters(m_Renderer->getRayTracingCullRadius());
                    if (ImGui::DragFloat("Culling Radius (m)", &cullRadiusMeters, 1.0f, 0.0f, 100000.0f, "%.0f"))
                        m_Renderer->setRayTracingCullRadius(WorldUnits::FromMeters(cullRadiusMeters));
                    float cullAngle = m_Renderer->getRayTracingCullAngle();
                    if (ImGui::SliderFloat("Culling Angle (deg)", &cullAngle, 0.0f, 10.0f, "%.2f"))
                        m_Renderer->setRayTracingCullAngle(cullAngle);
                }
                ImGui::TextDisabled("TLAS: %u instances, %u culled, %u BLAS evicted", m_Renderer->getTlasInstanceCount(),
                                    m_Renderer->getTlasCulledCount(), m_Renderer->getEvictedBlasCount());

                ImGui::BeginDisabled(!m_Renderer->isAsyncComputeAvailable());
                bool asyncCompute = m_Renderer->getAsyncComputeEnabled();
                if (ImGui::Checkbox("Async Compute (Screen-Probe GI)", &asyncCompute))
                    m_Renderer->setAsyncComputeEnabled(asyncCompute);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(m_Renderer->isAsyncComputeAvailable()
                                          ? "The screen-probe GI runs on the GPU's compute queue, beside the SSAO / RTAO / RT shadow passes,\n"
                                            "which do not touch what it reads and writes; the reflections and the lighting wait for it.\n"
                                            "Off: everything runs on the graphics queue, one after another. The image is the same either\n"
                                            "way; compare the GPU frame time, and see both queues in Window > Profiler."
                                          : "This GPU has no compute-only queue family: compute runs on the graphics queue.");
                if (m_Renderer->isAsyncComputeAvailable() && asyncCompute)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%u passes on the compute queue)", m_Renderer->getAsyncComputePassCount());
                }

                // Diffuse Global Illumination (GI)
                static const char* giModeNames[] = {
                    "Off (IBL Ambient)",
                    "DDGI (Probe Volumes)",
                    "ReSTIR GI (Screen-Space Resampling via RTXDI)",
                    "Screen Probes (Lumen-style final gather)"
                };
                int currentGIMode = static_cast<int>(m_Renderer->getDiffuseGIMode());
                if (ImGui::Combo("Diffuse Global Illumination", &currentGIMode, giModeNames, IM_ARRAYSIZE(giModeNames)))
                {
                    m_Renderer->setDiffuseGIMode(static_cast<uint32_t>(currentGIMode));
                    // The screen probes' rays read DDGI at their hits (the second bounce): it runs under them.
                    m_Renderer->setDDGIEnabled(currentGIMode == 1 || currentGIMode == 3);

                    // "PBR Debug View" mode 16 ("Indirect Diffuse GI Only") is a second, independent
                    // switch that also keeps ReSTIR GI's full pipeline running when no GI technique is
                    // chosen (Renderer::isReSTIRGIRequested) -- leaving it stuck on 16 silently keeps paying
                    // for TLAS build + all three ReSTIR GI passes + NRD GI denoise every frame with
                    // no visual indication why. Clear it whenever GI mode no longer needs it.
                    if (currentGIMode == 0 && m_Renderer->getDebugMode() == 16)
                    {
                        m_Renderer->setDebugMode(0);
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Select indirect diffuse lighting technique:\n- Off: Constant or Cubemap IBL\n- DDGI: Irradiance probe volume grid\n- ReSTIR GI: RTXDI Spatiotemporal reservoir resampling (screen-space indirect diffuse)\n- Screen Probes: what Lumen does for its final gather -- probes on the visible surfaces trace the scene,\n  DDGI underneath gives the second bounce. Smooth, no light through walls (the default of every preset)");
                }

                uint32_t activeDebugMode = m_Renderer->getDebugMode();

                // Screen-probe GI settings
                if (m_Renderer->getDiffuseGIMode() == 3)
                {
                    ImGui::Indent();
                    const NRI::Extent2D probes = m_Renderer->getScreenProbeCounts();
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.8f, 1.0f), "Screen probes: %ux%u (%u probes, 64 rays each)", probes.width, probes.height,
                                       probes.width * probes.height);

                    static const char* tileNames[] = { "8 px (finest, 4x the rays of 16)", "12 px", "16 px", "24 px", "32 px (fastest)" };
                    static const uint32_t tileSizes[] = { 8, 12, 16, 24, 32 };
                    uint32_t& tileSize = m_Renderer->getScreenProbeTileSize();
                    int tileIndex = 2;
                    for (int i = 0; i < IM_ARRAYSIZE(tileSizes); ++i)
                    {
                        if (tileSizes[i] == tileSize)
                            tileIndex = i;
                    }
                    if (ImGui::Combo("Screen Probe Spacing", &tileIndex, tileNames, IM_ARRAYSIZE(tileNames)))
                        tileSize = tileSizes[tileIndex];
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("One probe per this many pixels each way (render resolution). Finer: more detail in corners and on small\n"
                                          "objects, and more rays (cost grows with the square).");

                    uint32_t& filterPasses = m_Renderer->getScreenProbeFilterPasses();
                    int filterInt = static_cast<int>(std::min(filterPasses, 3u));
                    if (ImGui::SliderInt("Spatial Filter Passes", &filterInt, 0, 3))
                        filterPasses = static_cast<uint32_t>(filterInt);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Each pass averages a probe with its neighbours on the same surface: smoother, less local.");

                    uint32_t& maxHistory = m_Renderer->getScreenProbeMaxHistory();
                    int historyInt = static_cast<int>(std::clamp(maxHistory, 1u, 64u));
                    if (ImGui::SliderInt("Temporal Frames", &historyInt, 1, 64))
                        maxHistory = static_cast<uint32_t>(historyInt);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Frames accumulated at most per pixel: more is smoother and slower to follow changes in the light\n"
                                          "(a large change still shortens it on its own).");

                    if (ImGui::Button("Reset Screen Probe History"))
                        m_Renderer->resetScreenProbeHistory();
                    ImGui::TextDisabled("Second bounce: the DDGI cascades (settings below)");
                    ImGui::Unindent();
                }

                // ReSTIR GI Settings (RTXDI)
                if (m_Renderer->isReSTIRGIRequested())
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
                if (m_Renderer->getDiffuseGIMode() == 1 || m_Renderer->getDiffuseGIMode() == 3 || activeDebugMode == 17)
                {
                    ImGui::Indent();
                    uint32_t totalProbes = m_Renderer->getDDGIProbeCountTotal();
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Grid: %ux%ux%u (%u probes, %u rays/probe)",
                        m_Renderer->getDDGIProbeCountX(),
                        m_Renderer->getDDGIProbeCountY(),
                        m_Renderer->getDDGIProbeCountZ(),
                        totalProbes,
                        m_Renderer->getDDGIRaysPerProbe());

                    bool& follow = m_Renderer->getDDGIFollowCamera();
                    ImGui::Checkbox("Follow Camera", &follow);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("The probe volume is centered on the camera and scrolls with it in whole cells, each probe keeping its\n"
                                          "history. Beyond the volume the environment's light takes over. Off: a fixed grid (origin and spacing\n"
                                          "below, in world units).");
                    if (follow)
                    {
                        glm::vec3& spacingMeters = m_Renderer->getDDGISpacingMeters();
                        ImGui::DragFloat3("Probe Spacing", glm::value_ptr(spacingMeters), 0.05f, 0.25f, 50.0f, "%.2f m");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Between the probes of the first (finest) cascade.");
                        uint32_t& cascades = m_Renderer->getDDGICascadeSetting();
                        int cascadeCount = static_cast<int>(std::clamp(cascades, 1u, 4u));
                        if (ImGui::SliderInt("Cascades", &cascadeCount, 1, 4))
                            cascades = static_cast<uint32_t>(cascadeCount);
                        float& cascadeScale = m_Renderer->getDDGICascadeScale();
                        ImGui::SliderFloat("Cascade Scale", &cascadeScale, 1.5f, 4.0f, "%.1fx");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Nested volumes around the camera, each this much wider (and coarser) than the one inside it: fine\n"
                                              "detail near you, light on buildings across the street and beyond. Far cascades update less often.");
                        const glm::vec3 cells = glm::vec3(m_Renderer->getDDGIProbeCountX() - 1, m_Renderer->getDDGIProbeCountY() - 1,
                                                          m_Renderer->getDDGIProbeCountZ() - 1);
                        const glm::vec3 first = spacingMeters * cells;
                        const glm::vec3 last = first * std::pow(std::max(cascadeScale, 1.5f), static_cast<float>(cascadeCount - 1));
                        ImGui::TextDisabled("Around the camera: %.0f x %.0f x %.0f m fine, %.0f x %.0f x %.0f m in all", first.x, first.y, first.z,
                                            last.x, last.y, last.z);
                    }
                    else
                    {
                        glm::vec3& origin = m_Renderer->getDDGIGridOrigin();
                        ImGui::DragFloat3("Grid Origin", glm::value_ptr(origin), 0.1f);

                        glm::vec3& spacing = m_Renderer->getDDGIGridSpacing();
                        ImGui::DragFloat3("Grid Spacing", glm::value_ptr(spacing), 0.05f, 0.2f, 10000.0f);
                    }

                    static const char* strideNames[] = { "Every probe, every frame", "Half of them a frame", "A quarter a frame" };
                    uint32_t& stride = m_Renderer->getDDGIUpdateStride();
                    int strideIndex = stride >= 4 ? 2 : (stride >= 2 ? 1 : 0);
                    if (ImGui::Combo("Probe Updates", &strideIndex, strideNames, IM_ARRAYSIZE(strideNames)))
                        stride = strideIndex == 0 ? 1u : (strideIndex == 1 ? 2u : 4u);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How many probes trace rays each frame. Fewer is cheaper; light changes settle a little more slowly.\n"
                                          "Probes the volume just scrolled over always update at once.");

                    ImGui::Checkbox("Probe Relocation", &m_Renderer->getDDGIRelocation());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Probes that end up inside walls or right against a surface move (up to 45 %% of a cell) to\n"
                                          "free space nearby, so the light next to thin walls and floors neither goes dark nor leaks.");

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

                    if (ImGui::Button("Reset Volume to Defaults"))
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
                    "Brute-Force Loop (every light, default)",
                    "ReSTIR DI (Screen-Space Resampled, RTXDI)"
                };
                int currentDirectLightingMode = static_cast<int>(m_Renderer->getDirectLightingMode());
                if (ImGui::Combo("Direct Lighting", &currentDirectLightingMode, directLightingModeNames, IM_ARRAYSIZE(directLightingModeNames)))
                {
                    m_Renderer->setDirectLightingMode(static_cast<uint32_t>(currentDirectLightingMode));
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Brute-force loop: every light in range, each with its own shadow ray per pixel (the sun's from the\n"
                                      "denoised shadow mask). Exact and noise-free; the cost grows with the number of lights that reach a pixel.\n"
                                      "ReSTIR DI: one light per pixel resampled from all of them, one shadow ray, then denoised -- the\n"
                                      "many-light technique (UE5's MegaLights is the same idea); worth it with hundreds of overlapping lights.\n"
                                      "Switch \"PBR Debug View\" to \"8: Direct Lights Only\" to compare the two in isolation.");
                }

                if (m_Renderer->getDirectLightingMode() == 1)
                {
                    ImGui::Indent();
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "ReSTIR DI (RTXDI): presampled lights (RIS tiles, ReGIR grid), temporal + spatial reuse");

                    uint32_t& numLocal = m_Renderer->getReSTIRDINumLocalLightSamples();
                    int numLocalInt = static_cast<int>(numLocal);
                    if (ImGui::SliderInt("Local Light Samples", &numLocalInt, 1, 16))
                        numLocal = static_cast<uint32_t>(numLocalInt);

                    uint32_t& numInfinite = m_Renderer->getReSTIRDINumInfiniteLightSamples();
                    int numInfiniteInt = static_cast<int>(numInfinite);
                    if (ImGui::SliderInt("Infinite (Directional) Light Samples", &numInfiniteInt, 1, 2))
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

            if (m_Renderer->isReSTIRGIRequested())
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

        UpdateHistory();
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

        // Prefab Mode: the prefab's name and its Save / Exit buttons take the toolbar (nothing plays while a prefab is edited).
        if (m_PrefabMode.Active)
        {
            ImGui::SetCursorPos(ImVec2(10.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.45f, 0.7f, 1.0f, 1.0f), "Prefab Mode: %s%s%s", m_PrefabMode.Name.c_str(), m_PrefabMode.Variant ? " (variant)" : "",
                               m_PrefabMode.Dirty ? "  *" : "");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.35f, 0.55f, 1.0f));
            if (ImGui::Button("Save"))
                SavePrefabMode();
            ImGui::SameLine();
            if (ImGui::Button("Save & Exit"))
            {
                SavePrefabMode();
                ExitPrefabMode();
            }
            ImGui::SameLine();
            if (ImGui::Button("Exit"))
                RequestExitPrefabMode(); // asks first when there are unsaved changes
            ImGui::PopStyleColor();
        }

        bool hasPlayButton = !m_PrefabMode.Active && (m_SceneState == SceneState::Edit || m_SceneState == SceneState::Play);
        bool hasSimulateButton = !m_PrefabMode.Active && (m_SceneState == SceneState::Edit || m_SceneState == SceneState::Simulate);
        bool hasPauseButton = !m_PrefabMode.Active && m_SceneState != SceneState::Edit;

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

    bool EditorLayer::AnyNodeGraphEditorWantsInput() const
    {
        return std::any_of(m_NodeGraphEditors.begin(), m_NodeGraphEditors.end(),
            [](const Scope<NodeGraphEditorPanel>& panel) { return panel->WantsInput(); });
    }

    bool EditorLayer::AnyNodeGraphEditorHovered() const
    {
        return std::any_of(m_NodeGraphEditors.begin(), m_NodeGraphEditors.end(),
            [](const Scope<NodeGraphEditorPanel>& panel) { return panel->IsHovered(); });
    }

    bool EditorLayer::OnKeyPressed(KeyPressedEvent& e)
    {
        // 1. Abort if the user is typing in an ImGui text field
        if (ImGui::GetIO().WantTextInput)
            return false;

        // A graph editor window owns the keyboard while it's focused or hovered (Delete removes its selected
        // node, not the scene entity selected elsewhere; Q/W/E/R aren't gizmo switches there).
        if (AnyNodeGraphEditorWantsInput())
            return false;

        // Shortcuts
        if (e.IsRepeat())
        {
            return false;
        }


        // The modifiers held when the key went down, not the live keyboard state: by the time the event is handled (once per
        // frame) a fast Ctrl+Shift+Z can already have let go of Shift, which read as Ctrl+Z and undid instead of redoing.
        const uint16_t modifiers = e.GetModifiers();
        bool control = (modifiers & SDL_KMOD_CTRL) != 0;
        bool shift = (modifiers & SDL_KMOD_SHIFT) != 0;

        // Shortcuts change the scene outside any ImGui item (Delete, Ctrl+D, End): the history looks for a change this frame.
        m_HistoryCheckRequested = true;

        // Letter shortcuts follow the keyboard layout, like in every editor: Ctrl+Z is the key labeled Z on a German QWERTZ
        // keyboard too. The event carries the physical scancode, which names the US position (Z and Y are swapped on QWERTZ,
        // so undo and redo came out reversed); a letter key is mapped back to the scancode of the letter it types. The fly
        // camera's WASD stays physical on purpose (Input::IsKeyPressed).
        int keyCode = e.GetKeyCode();
        const SDL_Keycode layoutKey = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(keyCode), SDL_KMOD_NONE, false);
        if (layoutKey >= SDLK_A && layoutKey <= SDLK_Z)
            keyCode = SDL_SCANCODE_A + static_cast<int>(layoutKey - SDLK_A);

        switch (keyCode)
        {
        case SDL_SCANCODE_Z:
            {
                // Ctrl+Z undo, Ctrl+Shift+Z redo (text fields keep their own undo: WantTextInput returned above).
                if (control)
                {
                    if (shift)
                        Redo();
                    else
                        Undo();
                }
                break;
            }
        case SDL_SCANCODE_Y:
            {
                if (control)
                    Redo();
                break;
            }
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

        // The gizmo mode is a toolbar button now (UI_ViewportOverlay), not Q/W/E/R: those double as WASD-fly's strafe/forward and
        // the fly-up key, so a key held to move the camera can no longer also switch what the gizmo does.
        case SDL_SCANCODE_R:
            if (control)
            {
                ScriptEngine::ReloadAssembly();
            }
            break;
        case SDL_SCANCODE_B:
            if (control)
                m_ScriptBuilder.RequestBuild();
            break;
        case SDL_SCANCODE_KP_1:
            if (m_ViewportHovered)
                SetViewMode(control ? EditorViewMode::Back : EditorViewMode::Front);
            break;
        case SDL_SCANCODE_KP_3:
            if (m_ViewportHovered)
                SetViewMode(control ? EditorViewMode::Left : EditorViewMode::Right);
            break;
        case SDL_SCANCODE_KP_7:
            if (m_ViewportHovered)
                SetViewMode(control ? EditorViewMode::Bottom : EditorViewMode::Top);
            break;
        case SDL_SCANCODE_KP_5:
            if (m_ViewportHovered)
                SetViewMode(EditorViewMode::Perspective);
            break;
        case SDL_SCANCODE_M:
            {
                if (m_ViewportHovered && !control)
                {
                    m_MeasureTool = !m_MeasureTool;
                    if (!m_MeasureTool)
                        m_MeasureStage = 0;
                }
                break;
            }
        case SDL_SCANCODE_ESCAPE:
            {
                // Esc: first drops the line being measured, then leaves the tool.
                if (m_MeasureTool)
                {
                    if (m_MeasureStage != 0)
                        m_MeasureStage = 0;
                    else
                        m_MeasureTool = false;
                }
                break;
            }
        case SDL_SCANCODE_END:
            {
                // Unreal's "Snap to Floor".
                if (Application::Get().GetLayer<ImGuiLayer>()->GetActiveWidgetID() == 0)
                    SnapSelectionToFloor();
                break;
            }
        case SDL_SCANCODE_F:
            {
                // Unreal's "Frame Selected" -- the same thing double-clicking an entity in the Outliner does.
                if (m_ViewportHovered && !control)
                    FocusOnEntity(m_SceneHierarchyPanel.GetSelectedEntity());
                break;
            }
        case SDL_SCANCODE_DELETE:
            {
                if (Application::Get().GetLayer<ImGuiLayer>()->GetActiveWidgetID() == 0)
                {
                    // Everything selected, not just the last one. Resolved by UUID: deleting a parent already removes its
                    // selected children.
                    std::vector<UUID> toDelete;
                    for (Entity selected : m_SceneHierarchyPanel.GetSelectedEntities())
                    {
                        // An entity of a prefab instance can be deleted too: the instance remembers it (an override).
                        if (selected)
                            toDelete.push_back(selected.GetUUID());
                    }
                    m_SceneHierarchyPanel.ClearSelection();
                    for (UUID id : toDelete)
                    {
                        if (Entity entity = m_ActiveScene->GetEntityByUUID(id))
                            m_ActiveScene->DestroyEntity(entity);
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
        if (AnyNodeGraphEditorHovered())
            return false; // a click in the graph window must not also pick the scene entity under the viewport

        if (event.GetMouseButton() == SDL_BUTTON_LEFT)
        {
            // The measure tool takes the clicks (no selecting) while it is on.
            if (m_MeasureTool && m_ViewportHovered && !Input::IsKeyPressed(SDL_SCANCODE_LALT))
            {
                MeasureClick();
                return false;
            }

            if (m_ViewportHovered && !ImGuizmo::IsOver() && !Input::IsKeyPressed(SDL_SCANCODE_LALT))
            {
                bool shift = Input::IsKeyPressed(SDL_SCANCODE_LSHIFT) || Input::IsKeyPressed(SDL_SCANCODE_RSHIFT);
                bool control = Input::IsKeyPressed(SDL_SCANCODE_LCTRL) || Input::IsKeyPressed(SDL_SCANCODE_RCTRL);

                // A click on an entity of a prefab instance picks the instance -- the outermost one when instances sit inside each other
                // (Ctrl+click picks the entity itself).
                Entity picked = m_HoveredEntity;
                while (picked && !control && picked.HasComponent<PrefabNodeComponent>())
                {
                    Entity instance = m_ActiveScene->GetEntityByUUID(picked.GetComponent<PrefabNodeComponent>().Instance);
                    if (!instance)
                        break;
                    picked = instance;
                }

                if (picked)
                {
                    if (shift)
                        m_SceneHierarchyPanel.SelectRange(picked);
                    else if (control)
                        m_SceneHierarchyPanel.ToggleSelectedEntity(picked);
                    else
                        m_SceneHierarchyPanel.SetSelectedEntity(picked);
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

        if (m_SceneState != SceneState::Play && m_ShowGrid)
            DrawWorldGrid();
        if (m_SceneState != SceneState::Play && m_ShowReferenceFigure)
        {
            // Under the cursor: where the cursor points on the surfaces (the y = 0 plane past them), kept while the mouse is elsewhere.
            glm::vec3 underCursor;
            if (m_ReferenceFigureAtCursor && m_ViewportHovered && PickWorldPoint(underCursor))
                m_ReferenceFigurePosition = underCursor;
            DrawReferenceFigure(m_ReferenceFigureAtCursor ? m_ReferenceFigurePosition : glm::vec3(0.0f));
        }

        // Rect lights are not geometry: their rectangle and a stroke the way they shine (the entity's -Z) show where they are.
        if (m_SceneState != SceneState::Play)
        {
            auto rectLights = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, RectLightComponent>();
            for (auto entity : rectLights)
            {
                auto [wtc, light] = rectLights.get<WorldTransformComponent, RectLightComponent>(entity);
                const glm::mat4& world = wtc.WorldMatrix;
                const glm::vec3 centre = glm::vec3(world[3]);
                const glm::vec3 halfX = glm::vec3(world[0]) * (0.5f * light.Width);
                const glm::vec3 halfY = glm::vec3(world[1]) * (0.5f * light.Height);
                const glm::vec3 corners[4] = { centre - halfX - halfY, centre + halfX - halfY, centre + halfX + halfY, centre - halfX + halfY };
                const glm::vec4 colour(glm::mix(light.Color, glm::vec3(1.0f, 0.9f, 0.4f), 0.5f), 1.0f);
                for (int i = 0; i < 4; ++i)
                    m_Renderer2D->DrawXRayLine(corners[i], corners[(i + 1) % 4], colour);
                const glm::vec3 forward = -glm::vec3(world[2]);
                if (glm::length(forward) > 1e-6f)
                    m_Renderer2D->DrawXRayLine(centre, centre + glm::normalize(forward) * 0.5f * (glm::length(halfX) + glm::length(halfY)), colour);
            }
            // Disc lights likewise: their circle and the way they shine.
            auto discLights = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, DiscLightComponent>();
            for (auto entity : discLights)
            {
                auto [wtc, light] = discLights.get<WorldTransformComponent, DiscLightComponent>(entity);
                const glm::mat4& world = wtc.WorldMatrix;
                const glm::vec3 centre = glm::vec3(world[3]);
                const float scale = glm::max(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])));
                const glm::vec3 axisX = glm::length(glm::vec3(world[0])) > 1e-6f ? glm::normalize(glm::vec3(world[0])) * light.Radius * scale : glm::vec3(0.0f);
                const glm::vec3 axisY = glm::length(glm::vec3(world[1])) > 1e-6f ? glm::normalize(glm::vec3(world[1])) * light.Radius * scale : glm::vec3(0.0f);
                const glm::vec4 colour(glm::mix(light.Color, glm::vec3(1.0f, 0.9f, 0.4f), 0.5f), 1.0f);
                constexpr int Segments = 32;
                for (int i = 0; i < Segments; ++i)
                {
                    const float a0 = glm::two_pi<float>() * static_cast<float>(i) / Segments;
                    const float a1 = glm::two_pi<float>() * static_cast<float>(i + 1) / Segments;
                    m_Renderer2D->DrawXRayLine(centre + axisX * std::cos(a0) + axisY * std::sin(a0), centre + axisX * std::cos(a1) + axisY * std::sin(a1), colour);
                }
                const glm::vec3 forward = -glm::vec3(world[2]);
                if (glm::length(forward) > 1e-6f)
                    m_Renderer2D->DrawXRayLine(centre, centre + glm::normalize(forward) * light.Radius * scale, colour);
            }
        }

        // Level Streaming Volumes: their boxes while editing and simulating (Unreal shows its volumes in the editor), in orange.
        if (m_SceneState != SceneState::Play)
        {
            constexpr glm::vec4 kVolumeColor(1.0f, 0.55f, 0.1f, 0.9f);
            constexpr int edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
            auto view = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, LevelStreamingVolumeComponent>();
            for (auto entity : view)
            {
                auto [wtc, volume] = view.get<WorldTransformComponent, LevelStreamingVolumeComponent>(entity);
                glm::vec3 corners[8];
                for (int i = 0; i < 8; ++i)
                {
                    const glm::vec3 sign((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
                    corners[i] = glm::vec3(wtc.WorldMatrix * glm::vec4(sign * volume.HalfExtents, 1.0f));
                }
                for (const auto& edge : edges)
                    m_Renderer2D->DrawXRayLine(corners[edge[0]], corners[edge[1]], kVolumeColor);
            }
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
            DrawPhysicsColliders3D();
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

    namespace
    {
        // Most recent first, one path per line, in the user's settings folder (not the project's).
        std::filesystem::path RecentProjectsFile()
        {
            char* prefPath = SDL_GetPrefPath("Nox", "NoxEditor");
            if (!prefPath)
                return {};
            std::filesystem::path file = std::filesystem::path(prefPath) / "RecentProjects.txt";
            SDL_free(prefPath);
            return file;
        }

        std::vector<std::filesystem::path> LoadRecentProjects()
        {
            std::vector<std::filesystem::path> projects;
            std::ifstream file(RecentProjectsFile());
            std::string line;
            while (std::getline(file, line))
            {
                if (!line.empty())
                    projects.emplace_back(line);
            }
            return projects;
        }

        void RememberRecentProject(const std::filesystem::path& project)
        {
            const std::filesystem::path file = RecentProjectsFile();
            if (file.empty())
                return;
            std::error_code error;
            const std::filesystem::path absolute = std::filesystem::absolute(project, error).lexically_normal();
            std::vector<std::filesystem::path> projects = LoadRecentProjects();
            std::erase_if(projects, [&](const std::filesystem::path& recent) { return recent == absolute; });
            projects.insert(projects.begin(), absolute);
            if (projects.size() > 10)
                projects.resize(10);
            std::ofstream out(file, std::ios::trunc);
            for (const std::filesystem::path& recent : projects)
                out << recent.generic_string() << "\n";
        }

        // The editor run from its build folder inside the repository: the sample project there.
        std::filesystem::path FindSampleProject()
        {
            std::filesystem::path candidate = SDL_GetBasePath() ? std::filesystem::path(SDL_GetBasePath()) : std::filesystem::path();
            for (int depth = 0; depth < 8 && !candidate.empty(); ++depth)
            {
                if (std::filesystem::exists(candidate / "Facerun/Facerun.nproj"))
                    return candidate / "Facerun/Facerun.nproj";
                if (candidate == candidate.parent_path())
                    break;
                candidate = candidate.parent_path();
            }
            return {};
        }

        // A project's files: <Name>.nproj, Assets/ (Scenes/, Scripts/) and an empty asset registry. Centimeters (100 units per
        // meter) like the engine's default; the Start Scene is set once a scene is saved.
        bool CreateProjectFiles(const std::filesystem::path& projectFile)
        {
            const std::filesystem::path directory = projectFile.parent_path();
            std::error_code error;
            std::filesystem::create_directories(directory / "Assets/Scenes", error);
            std::filesystem::create_directories(directory / "Assets/Scripts", error);
            if (error)
            {
                NOX_CORE_ERROR("New Project: cannot create the folders of '{}': {}", projectFile.string(), error.message());
                return false;
            }
            const std::filesystem::path registry = directory / "Assets/AssetRegistry.nxr";
            if (!std::filesystem::exists(registry))
                std::ofstream(registry) << "AssetRegistry: []\n";

            const std::string name = projectFile.stem().string();
            std::ofstream out(projectFile, std::ios::trunc);
            out << "Project:\n"
                << "  Name: " << name << "\n"
                << "  StartScene: 0\n"
                << "  AssetDirectory: \"Assets\"\n"
                << "  AssetRegistryPath: \"AssetRegistry.nxr\"\n"
                << "  ScriptModulePath: \"Scripts/Binaries/" << name << ".dll\"\n"
                << "  WorldUnitsPerMeter: 100\n"
                << "  RenderQuality: Auto\n";
            return static_cast<bool>(out);
        }
    }

    bool EditorLayer::NewProject()
    {
        std::filesystem::path path = Utility::SaveFile("Nox Project\0nproj\0");
        if (path.empty())
            return false;
        if (path.extension() != ".nproj")
            path += ".nproj";
        if (std::filesystem::exists(path))
        {
            NOX_CORE_ERROR("New Project: '{}' already exists (open it instead)", path.string());
            return false;
        }
        return CreateProjectFiles(path) && OpenProject(path);
    }

    bool EditorLayer::OpenProject()
    {
        const std::string filepath = Utility::OpenFile("Nox Project\0nproj\0");
        if (filepath.empty())
            return false;
        return OpenProject(filepath);
    }

    bool EditorLayer::OpenStartupProject()
    {
        for (const std::filesystem::path& recent : LoadRecentProjects())
        {
            if (std::filesystem::exists(recent) && OpenProject(recent))
                return true;
        }
        if (const std::filesystem::path sample = FindSampleProject(); !sample.empty() && OpenProject(sample))
            return true;

        while (true)
        {
            const SDL_MessageBoxButtonData buttons[] = {
                { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Open Project..." },
                { 0, 1, "New Project..." },
                { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Quit" },
            };
            SDL_MessageBoxData box{};
            box.flags = SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
            box.window = Application::Get().getWindow()->getHandle();
            box.title = "Nox Editor";
            box.message = "No project is open. Open an existing project (.nproj) or make a new one.";
            box.numbuttons = SDL_arraysize(buttons);
            box.buttons = buttons;
            int choice = 2;
            if (!SDL_ShowMessageBox(&box, &choice) || choice == 2)
                return false;
            if (choice == 0 ? OpenProject() : NewProject())
                return true;
        }
    }

    bool EditorLayer::OpenProject(const std::filesystem::path& path)
    {
        const bool hadProject = Project::GetActive() != nullptr;
        if (Project::Load(path))
        {
            RememberRecentProject(path);
            m_EditorCamera.ApplyWorldUnitDefaults(); // the project's world unit is only known now (U6)
            m_Renderer->ApplyQualitySetting(Project::GetActive()->GetConfig().RenderQuality);
            m_Renderer->ApplyProjectRendererSettings(Project::GetActive()->GetConfig().Renderer);
            Project::GetActive()->GetEditorAssetManager()->Init();
            // The project's scripts (at startup ScriptEngine::Initialize loads them, after this).
            if (ScriptEngine::IsInitialized())
                ScriptEngine::LoadProjectAssembly();
            m_ScriptBuilder.SetProject(ScriptEngine::GetProjectScriptsProject());

            AssetHandle startScene = Project::GetActive()->GetConfig().StartScene;
            if (startScene)
                OpenScene(startScene);
            else if (hadProject)
                NewScene(); // nothing of the project before stays open

            m_ContentBrowserPanel = CreateScope<ContentBrowserPanel>(Project::GetActive());
            m_ContentBrowserPanel->SetOpenAssetCallback([this](AssetHandle handle) { OpenAsset(handle); });
            m_ContentBrowserPanel->SetCreatePrefabCallback([this](UUID dragged, const std::filesystem::path& folder) { return CreatePrefab(dragged, folder); });
            m_SceneHierarchyPanel.SetCreatePrefabCallback([this](UUID entity)
            {
                // Into the folder the Content Browser shows, like a prefab dragged into it.
                if (m_ContentBrowserPanel)
                {
                    const AssetHandle created = CreatePrefab(entity, m_ContentBrowserPanel->CurrentFolder());
                    if (created != 0)
                        m_ContentBrowserPanel->ShowNewAsset(created);
                }
            });
            m_ContentBrowserPanel->SetCreateVariantCallback([this](AssetHandle base, const std::filesystem::path& folder) { return CreateVariant(base, folder); });
            // The browser's reference checks see what the open scenes use right now, unsaved changes included.
            // Replace References in the open scene: through its text (the same load as an undo step), every token of the old
            // handle becomes the new one.
            m_ContentBrowserPanel->SetReplaceInScenesCallback([this](AssetHandle from, AssetHandle to)
            {
                if (!m_EditorScene || m_SceneState != SceneState::Edit)
                    return;
                SceneSerializer serializer(m_EditorScene);
                std::string text = serializer.SerializeToText();
                const std::string fromText = std::to_string(static_cast<uint64_t>(from));
                const std::string toText = std::to_string(static_cast<uint64_t>(to));
                bool changed = false;
                for (size_t at = text.find(fromText); at != std::string::npos; at = text.find(fromText, at))
                {
                    const bool digitBefore = at > 0 && std::isdigit(static_cast<unsigned char>(text[at - 1]));
                    const bool digitAfter = at + fromText.size() < text.size() && std::isdigit(static_cast<unsigned char>(text[at + fromText.size()]));
                    if (digitBefore || digitAfter)
                    {
                        at += fromText.size();
                        continue;
                    }
                    text.replace(at, fromText.size(), toText);
                    at += toText.size();
                    changed = true;
                }
                if (changed)
                {
                    RestoreHistoryState(text);
                    m_HistoryCheckRequested = true;
                }
            });
            m_ContentBrowserPanel->SetSceneReferencesCallback([this](std::unordered_set<AssetHandle>& handles)
            {
                if (m_EditorScene)
                    m_EditorScene->CollectAssetReferences(handles);
                if (m_ActiveScene && m_ActiveScene != m_EditorScene)
                    m_ActiveScene->CollectAssetReferences(handles);
            });
            return true;
        }
        NOX_CORE_ERROR("Could not open the project '{}'", path.string());
        return false;
    }

    void EditorLayer::SaveProject()
    {
        //Project::SaveActive();
    }

    void EditorLayer::NewScene()
    {
        SaveEditorCameras();
        EndTimelinePreviews();
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
        EndTimelinePreviews();

        if (m_PrefabMode.Active)
        {
            // Asks first when the prefab has unsaved changes, then opens the scene.
            RequestExitPrefabMode([this, handle]() { OpenScene(handle); });
            return;
        }
        if (m_SceneState != SceneState::Edit)
        {
            OnSceneStop();
        }

        // Read and built without stopping the editor (a large scene took seconds of frozen window): the file is parsed on a thread,
        // the entities created a slice a frame (OnUpdate), and the scene swapped in once complete. Another scene opened meanwhile
        // replaces this load.
        m_SceneLoad = CreateScope<SceneLoader>(Project::GetActiveAssetDirectory() / Project::GetActive()->GetEditorAssetManager()->GetFilePath(handle));
        m_SceneLoadHandle = handle;
    }

    void EditorLayer::FinishSceneLoad()
    {
        Scope<SceneLoader> load = std::move(m_SceneLoad);
        const AssetHandle handle = std::exchange(m_SceneLoadHandle, AssetHandle(0));
        if (!load || load->Failed())
            return; // the loader logged why; the scene shown stays
        if (m_PrefabMode.Active)
        {
            NOX_CORE_WARN("'{}' finished loading while a prefab is open: not opened", load->GetPath().filename().string());
            return;
        }
        if (m_SceneState != SceneState::Edit)
            OnSceneStop(); // Play / Simulate started while it loaded: they ran on the scene being left

        Ref<Scene> newScene = load->GetScene();
        if (m_ViewportSize.x > 0 && m_ViewportSize.y > 0)
            newScene->OnViewportResize(m_ViewportSize.x, m_ViewportSize.y);

        m_EditorScene = newScene;
        m_SceneHierarchyPanel.SetContext(m_EditorScene);

        m_ActiveScene = m_EditorScene;
        SaveEditorCameras(); // the scene being left
        m_EditorScenePath = Project::GetActive()->GetEditorAssetManager()->GetFilePath(handle);
        RestoreEditorCameras();
        m_UnloadUnusedAssetsRequested = true;
#if NOX_PROFILE_STATS
        Profiler::Get().ResetStats();
#endif
    }

    void EditorLayer::UI_SceneLoadProgress()
    {
        if (!m_SceneLoad)
            return;
        // A small window at the bottom of the screen; the editor stays usable meanwhile.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y - 60.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.9f);
        if (ImGui::Begin("##SceneLoad", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
        {
            ImGui::Text("Opening %s", m_SceneLoad->GetPath().filename().string().c_str());
            ImGui::ProgressBar(m_SceneLoad->GetProgress(), ImVec2(320.0f, 0.0f), m_SceneLoad->GetStage().c_str());
        }
        ImGui::End();
    }

    namespace
    {
        std::filesystem::path EditorCamerasFile()
        {
            return Project::GetProjectDirectory() / "Cache" / "EditorCameras.txt";
        }

        // Lines: <scene path>|<viewport>|<focal x y z> <distance> <pitch> <yaw> <view mode> <ortho half height>
        std::vector<std::string> ReadEditorCameraLines()
        {
            std::vector<std::string> lines;
            std::ifstream in(EditorCamerasFile());
            for (std::string line; std::getline(in, line);)
            {
                if (!line.empty())
                    lines.push_back(line);
            }
            return lines;
        }
    }

    void EditorLayer::SaveEditorCameras()
    {
        if (!Project::GetActive() || m_EditorScenePath.empty())
            return;
        const std::string scene = m_EditorScenePath.generic_string();
        std::vector<std::string> lines = ReadEditorCameraLines();
        std::erase_if(lines, [&](const std::string& line) { return line.rfind(scene + "|", 0) == 0; });
        for (int index = 0; index < kMaxViewports; ++index)
        {
            const ViewportSlot& slot = m_Viewports[index];
            if (index != m_ActiveViewport && !slot.CameraSet)
                continue;
            const EditorCamera& camera = index == m_ActiveViewport ? m_EditorCamera : slot.Camera;
            const glm::vec3 focal = camera.GetFocalPoint();
            char line[512];
            snprintf(line, sizeof(line), "%s|%d|%.6g %.6g %.6g %.6g %.6g %.6g %d %.6g", scene.c_str(), index, focal.x, focal.y, focal.z,
                     camera.GetDistance(), camera.GetPitch(), camera.GetYaw(), static_cast<int>(camera.GetViewMode()), camera.GetOrthoHalfHeight());
            lines.push_back(line);
        }
        std::error_code error;
        std::filesystem::create_directories(EditorCamerasFile().parent_path(), error);
        std::ofstream out(EditorCamerasFile(), std::ios::trunc);
        for (const std::string& line : lines)
            out << line << "\n";
    }

    void EditorLayer::RestoreEditorCameras()
    {
        if (!Project::GetActive() || m_EditorScenePath.empty())
            return;
        const std::string prefix = m_EditorScenePath.generic_string() + "|";
        for (const std::string& line : ReadEditorCameraLines())
        {
            if (line.rfind(prefix, 0) != 0)
                continue;
            int index = -1, mode = 0;
            glm::vec3 focal(0.0f);
            float distance = 0.0f, pitch = 0.0f, yaw = 0.0f, orthoHalfHeight = 10.0f;
            if (sscanf(line.c_str() + prefix.size(), "%d|%f %f %f %f %f %f %d %f", &index, &focal.x, &focal.y, &focal.z, &distance, &pitch, &yaw,
                       &mode, &orthoHalfHeight) != 9 || index < 0 || index >= kMaxViewports || mode < 0 || mode > static_cast<int>(EditorViewMode::Right))
                continue;
            EditorCamera& camera = index == m_ActiveViewport ? m_EditorCamera : m_Viewports[index].Camera;
            camera.SetViewMode(static_cast<EditorViewMode>(mode));
            camera.SetFocalPoint(focal);
            camera.SetDistance(distance);
            camera.SetOrientation(pitch, yaw);
            if (camera.IsOrthographic())
                camera.SetOrthoHalfHeight(orthoHalfHeight);
            m_Viewports[index].CameraSet = true;
        }
    }

    void EditorLayer::UI_StatusBar()
    {
        // What is still streaming in (§5.11.5): background loads (cooking on a first import), bytes on their way to the
        // GPU, and meshes that draw but wait for their BLAS.
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
        if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(), flags))
        {
            if (ImGui::BeginMenuBar())
            {
                if (ImGui::GetTime() - m_StatusBarRefreshTime >= StatusBarRefreshSeconds)
                {
                    const auto assetManager = Project::GetActive()->GetEditorAssetManager();
                    m_StatusBarCounts = {
                        .Loading = assetManager->GetLoadingCount(),
                        .Streaming = assetManager->GetStreamingCount(),
                        .PendingMB = static_cast<double>(assetManager->GetPendingUploadBytes()) / (1024.0 * 1024.0),
                        .BlasBuilds = m_Renderer->GetPendingBlasBuilds()
                    };
                    m_StatusBarRefreshTime = ImGui::GetTime();
                }
                // Model imports cooking in the background (Do Not Combine): a progress bar until the last mesh is done.
                const EditorAssetManager::ImportProgress import = Project::GetActive()->GetEditorAssetManager()->GetImportProgress();
                if (import.Imports > 0)
                {
                    char overlay[64];
                    if (import.Total == 0)
                        snprintf(overlay, sizeof(overlay), "Importing: reading the file...");
                    else
                        snprintf(overlay, sizeof(overlay), "Importing meshes %u / %u", import.Done, import.Total);
                    ImGui::ProgressBar(import.Total == 0 ? 0.0f : static_cast<float>(import.Done) / static_cast<float>(import.Total),
                                       ImVec2(220.0f, 0.0f), overlay);
                    ImGui::SameLine();
                }
                const StatusBarCounts& counts = m_StatusBarCounts;
                if (counts.Loading == 0 && counts.Streaming == 0 && counts.BlasBuilds == 0 && import.Imports == 0)
                    ImGui::TextDisabled("Ready");
                else
                    ImGui::Text("Loading %zu asset(s)  |  Streaming %zu texture(s)  |  %.1f MB to upload  |  %zu BLAS build(s) pending",
                                counts.Loading, counts.Streaming, counts.PendingMB, counts.BlasBuilds);
                ImGui::EndMenuBar();
            }
        }
        ImGui::End();
    }

    // Places model assets into the scene being edited at `point`, filed under `folder` ("" = the top level): a single asset is
    // one model instance, several use each asset's file layout (per-mesh assets) -- either way the instance turns into plain
    // flat entities once loaded. Used by the viewport (drop point on the ray) and the hierarchy panel (a folder row).
    float EditorLayer::LocationSnapWorldUnits() const
    {
        return WorldUnits::FromCentimeters(kLocationSnapsCm[m_LocationSnapIndex]);
    }

    glm::vec3 EditorLayer::SnapLocation(const glm::vec3& point) const
    {
        if (!m_SnapEnabled)
            return point;
        const float step = LocationSnapWorldUnits();
        return glm::round(point / step) * step;
    }

    // The snap switch and steps, floating at the top left of the viewport (its own window: a click on it is not a click in the scene).

    void EditorLayer::UI_Viewports()
    {
        // The area the viewports dock into: a window in the middle of the editor with a dockspace of its own, so a layout preset
        // rebuilds only this area and the other panels keep their places.
        const ImGuiID viewportDockSpace = ImHashStr("NoxViewportDockSpace");
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("Viewports", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse);
        ImGui::PopStyleVar();
        if (m_PendingViewportLayout >= 0 || !ImGui::DockBuilderGetNode(viewportDockSpace))
        {
            const int layout = m_PendingViewportLayout >= 0 ? m_PendingViewportLayout : static_cast<int>(ViewportLayout::One);
            m_PendingViewportLayout = -1;
            ApplyViewportLayout(static_cast<ViewportLayout>(layout), viewportDockSpace, ImGui::GetContentRegionAvail());
        }
        ImGui::DockSpace(viewportDockSpace, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
        ImGui::End();

        // The viewports open last session (once imgui.ini was read, before the first frame), then what to write back.
        if (s_ViewportIni.Loaded)
        {
            s_ViewportIni.Loaded = false;
            for (int index = 0; index < kMaxViewports; ++index)
            {
                ViewportSlot& slot = m_Viewports[index];
                slot.ShowFlags = s_ViewportIni.Flags[index];
                slot.ShowGrid = s_ViewportIni.Grid[index] != 0;
                slot.ResolutionIndex = s_ViewportIni.Resolution[index];
                slot.Realtime = s_ViewportIni.Realtime[index] != 0;
                if (index > 0 && s_ViewportIni.Open[index])
                    OpenViewport(index);
            }
        }
        for (int index = 0; index < kMaxViewports; ++index)
        {
            const ViewportSlot& slot = m_Viewports[index];
            s_ViewportIni.Open[index] = slot.Open;
            s_ViewportIni.Flags[index] = slot.ShowFlags;
            s_ViewportIni.Grid[index] = slot.ShowGrid ? 1 : 0;
            s_ViewportIni.Resolution[index] = slot.ResolutionIndex;
            s_ViewportIni.Realtime[index] = slot.Realtime ? 1 : 0;
            if (index == m_ActiveViewport)
                s_ViewportIni.View[index] = static_cast<int>(m_EditorCamera.GetViewMode());
            else if (slot.CameraSet)
                s_ViewportIni.View[index] = static_cast<int>(slot.Camera.GetViewMode());
        }

        // A closed viewport hands the tools back to the first one.
        if (!m_Viewports[m_ActiveViewport].Open)
            ActivateViewport(0);

        for (int index = 0; index < kMaxViewports; ++index)
        {
            if (!m_Viewports[index].Open)
                continue;
            if (index == m_ActiveViewport)
                UI_ActiveViewport();
            else
                UI_ExtraViewport(index);
        }
        for (int index = 0; index < kMaxViewports; ++index)
        {
            if (!m_Viewports[index].Open)
            {
                m_Viewports[index].Hovered = false;
                m_Viewports[index].Size = { 0, 0 };
                if (m_Renderer->GetExtraViewTexture(static_cast<uint32_t>(index)))
                    m_Renderer->ReleaseExtraView(static_cast<uint32_t>(index));
            }
        }

        // Input reaches the layer (the camera, picking, the click that activates another viewport) while a viewport is under the mouse.
        Application::Get().GetImGuiLayer()->BlockEvents(HoveredViewport() < 0);

        // Assets dragged from the Content Browser over another viewport: it becomes the active one, so the drop lands in it.
        if (const ImGuiPayload* payload = ImGui::GetDragDropPayload())
        {
            const int hovered = HoveredViewport();
            if (hovered >= 0 && hovered != m_ActiveViewport && (payload->IsDataType("CONTENT_BROWSER_ITEM") || payload->IsDataType("CONTENT_BROWSER_ITEMS")))
                ActivateViewport(hovered);
        }

        // The viewport a mouse button went down in keeps the mouse until every button is up again: a drag that leaves it still
        // steers its camera, and a right-click in another panel never flies the camera.
        const bool anyButtonDown = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                                   ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        const bool anyButtonClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                                      ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
        if (!anyButtonDown)
            m_MouseCaptureViewport = -1;
        else if (m_MouseCaptureViewport < 0 && anyButtonClicked)
            m_MouseCaptureViewport = HoveredViewport();

        UI_ViewportOverlay();
    }

    void EditorLayer::UI_ActiveViewport()
    {
        ViewportSlot& slot = m_Viewports[m_ActiveViewport];
        // We define "viewport" with no padding an retrieve the rendering area
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool visible = ImGui::Begin(slot.Title, m_ActiveViewport == 0 ? nullptr : &slot.Open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        if (!visible)
        {
            // Behind another tab: nothing of it is shown, hovered or picked.
            m_ViewportFocused = m_ViewportHovered = false;
            slot.Hovered = false;
            slot.Size = { 0, 0 };
            ImGui::End();
            return;
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
                // Whole pixels, like the render target: a fractional size resamples the image by a fraction of a pixel.
                ImGui::Image(textureID, ImVec2(std::floor(viewportPanelSize.x), std::floor(viewportPanelSize.y)));
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
                else if (type == AssetType::Mesh || type == AssetType::StaticMesh || type == AssetType::MeshSource || type == AssetType::Prefab)
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

                        // The model instance appears at once; its nodes spawn once the model has loaded (and
                        // been cooked, on a first import) in the background (§5.11.5).
                        const AssetMetadata& metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
                        std::string entityName = metadata.FilePath.filename().stem().string();
                        if (entityName.empty())
                            entityName = "Model";
                        Entity root = m_ActiveScene->CreateEntity(entityName);
                        if (type == AssetType::Prefab)
                        {
                            auto& prefabInstance = root.AddComponent<PrefabInstanceComponent>();
                            prefabInstance.Prefab = handle;
                            prefabInstance.InitTransform = true;
                        }
                        else
                            root.AddComponent<ModelInstanceComponent>().Model = handle;
                        // A mesh's import scale (static or skeletal) is baked into its cooked geometry, bind pose and animation
                        // clips (MeshImporter::ApplyImportScale / ApplySkeletonScale / ApplyAnimationScale, like Unreal's Import
                        // Uniform Scale): the entity starts at Scale 1.0, "as authored".
                        AssetManager::RequestAsset(handle);
                        m_SceneHierarchyPanel.SetSelectedEntity(root);
                        m_PlacementPreview.Root = root;

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
                                    transform.Translation = SnapLocation(rayOrigin + rayDirection * distance);
                                    m_PlacementPreview.Root.MarkTransformDirty();
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

            // Several assets dragged together (Ctrl+A in the Content Browser): every model lands under one group at the
            // drop point, each at the place its file had it (per-mesh assets carry the file's instance transforms).
            if (const ImGuiPayload* multiPayload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEMS"))
            {
                const size_t count = multiPayload->DataSize / sizeof(AssetHandle);
                const AssetHandle* handles = static_cast<const AssetHandle*>(multiPayload->Data);

                glm::vec3 dropPoint(0.0f);
                const glm::vec2 viewportSize = m_ViewportBounds[1] - m_ViewportBounds[0];
                if (viewportSize.x > 0.0f && viewportSize.y > 0.0f)
                {
                    const ImVec2 imguiMouse = ImGui::GetMousePos();
                    const glm::vec2 ndc = {
                        ((imguiMouse.x - m_ViewportBounds[0].x) / viewportSize.x) * 2.0f - 1.0f,
                        1.0f - ((imguiMouse.y - m_ViewportBounds[0].y) / viewportSize.y) * 2.0f
                    };
                    const glm::mat4 inverseViewProjection = glm::inverse(m_EditorCamera.GetGizmoProjection() * m_EditorCamera.GetGizmoView());
                    glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndc, -1.0f, 1.0f);
                    glm::vec4 farPoint = inverseViewProjection * glm::vec4(ndc, 1.0f, 1.0f);
                    nearPoint /= nearPoint.w;
                    farPoint /= farPoint.w;
                    const glm::vec3 rayOrigin = glm::vec3(nearPoint);
                    const glm::vec3 rayDirection = glm::normalize(glm::vec3(farPoint - nearPoint));

                    // Same plane as a single drop: through the origin, facing the editor camera.
                    const glm::vec3 planeNormal = m_EditorCamera.GetForwardDirection();
                    const float denominator = glm::dot(rayDirection, planeNormal);
                    if (std::abs(denominator) > 0.0001f)
                    {
                        const float distance = glm::dot(-rayOrigin, planeNormal) / denominator;
                        if (distance >= 0.0f)
                            dropPoint = SnapLocation(rayOrigin + rayDirection * distance);
                    }
                }

                // Like Unreal: no group entity, no folder -- every piece lands at the top level of the outliner.
                PlaceAssets(std::vector<AssetHandle>(handles, handles + count), dropPoint, std::string());
            }
            ImGui::EndDragDropTarget();
        }

        // Gizmos
        //maybe be a callback you subscribe to instead
        Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
        if (selectedEntity && m_GizmoType != -1)
        {
            ImGuizmo::SetOrthographic(m_EditorCamera.IsOrthographic());
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
            // The toolbar switch, inverted while Ctrl is held; the steps are the ones chosen there.
            const bool snap = m_SnapEnabled != Input::IsKeyPressed(SDL_SCANCODE_LCTRL);
            float snapValue = LocationSnapWorldUnits();
            if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
                snapValue = kRotationSnaps[m_RotationSnapIndex];
            else if (m_GizmoType == ImGuizmo::OPERATION::SCALE)
                snapValue = kScaleSnaps[m_ScaleSnapIndex];

            float snapValues[3] = {snapValue, snapValue, snapValue};

            glm::mat4 deltaMatrix(1.0f);
            const glm::mat4 beforeManipulate = worldTransform;

            ImGuizmo::Manipulate(glm::value_ptr(cameraView), glm::value_ptr(cameraProjection),
                                 static_cast<ImGuizmo::OPERATION>(m_GizmoType), ImGuizmo::LOCAL,
                                 glm::value_ptr(worldTransform), glm::value_ptr(deltaMatrix), snap ? snapValues : nullptr);

            // Snapping feedback while a translation is dragged (the drag's start is the pose in its first frame).
            const bool usingGizmo = ImGuizmo::IsUsing();
            if (usingGizmo && !m_GizmoWasUsing)
                m_GizmoDragStart = beforeManipulate;
            m_GizmoWasUsing = usingGizmo;
            if (usingGizmo && snap && m_GizmoType == ImGuizmo::OPERATION::TRANSLATE)
                DrawTranslationSnapFeedback(cameraView, cameraProjection, worldTransform);

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

        UI_MeasureOverlay();

        slot.Bounds[0] = m_ViewportBounds[0];
        slot.Bounds[1] = m_ViewportBounds[1];
        slot.Size = m_ViewportSize;
        slot.Hovered = m_ViewportHovered;

        // With more than one viewport the active one is framed (Unreal's highlight).
        if (OpenViewportCount() > 1)
        {
            ImGui::GetWindowDrawList()->AddRect(ImVec2(m_ViewportBounds[0].x + 1.0f, m_ViewportBounds[0].y + 1.0f),
                                                ImVec2(m_ViewportBounds[1].x - 1.0f, m_ViewportBounds[1].y - 1.0f), IM_COL32(255, 170, 40, 255), 0.0f, 0, 2.0f);
        }

        ImGui::End(); // End viewport
    }

    void EditorLayer::UI_ExtraViewport(int index)
    {
        ViewportSlot& slot = m_Viewports[index];
        ImGui::SetNextWindowSize(ImVec2(640.0f, 400.0f), ImGuiCond_FirstUseEver);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool visible = ImGui::Begin(slot.Title, index == 0 ? nullptr : &slot.Open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        slot.Hovered = false;
        if (!visible)
        {
            slot.Size = { 0, 0 };
            ImGui::End();
            return;
        }

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        slot.Bounds[0] = { origin.x, origin.y };
        slot.Bounds[1] = { origin.x + available.x, origin.y + available.y };
        slot.Size = { static_cast<uint32_t>(std::max(available.x, 0.0f)), static_cast<uint32_t>(std::max(available.y, 0.0f)) };
        // Also while something is dragged (an asset from the Content Browser activates the viewport it is dragged over).
        slot.Hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

        // Rendered this frame by the renderer (SubmitExtraViewports, in OnUpdate) at the size the window had last frame.
        auto* texture = slot.Submitted ? m_Renderer->GetExtraViewTexture(static_cast<uint32_t>(index)) : nullptr;
        if (texture && texture->getImTextureID() && available.x >= 1.0f && available.y >= 1.0f)
            ImGui::Image(texture->getImTextureID(), ImVec2(std::floor(available.x), std::floor(available.y)));

        ImDrawList* draw = ImGui::GetWindowDrawList();

        // Where the gizmo of the selected entity is (its axes, not draggable here: a click makes this the active viewport, where it is).
        Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
        if (texture && selectedEntity && m_GizmoType != -1 && m_SceneState != SceneState::Play && selectedEntity.HasComponent<WorldTransformComponent>())
        {
            const glm::mat4 world = selectedEntity.GetComponent<WorldTransformComponent>().WorldMatrix;
            const glm::mat4 viewProjection = slot.Camera.GetProjection() * slot.Camera.GetViewMatrix();
            auto toScreen = [&](const glm::vec3& point, ImVec2& out) -> bool
            {
                const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
                if (clip.w <= 1e-6f)
                    return false;
                const glm::vec2 ndc = glm::vec2(clip) / clip.w;
                out = ImVec2(origin.x + (ndc.x * 0.5f + 0.5f) * available.x, origin.y + (0.5f - ndc.y * 0.5f) * available.y);
                return true;
            };
            const glm::vec3 center = glm::vec3(world[3]);
            ImVec2 centerOnScreen;
            if (toScreen(center, centerOnScreen))
            {
                const ImU32 axisColors[3] = { IM_COL32(230, 60, 60, 220), IM_COL32(80, 210, 80, 220), IM_COL32(70, 120, 240, 220) };
                const float length = std::min(available.x, available.y) * 0.09f;
                const float probe = WorldUnits::FromMeters(0.01f);
                for (int axis = 0; axis < 3; ++axis)
                {
                    const glm::vec3 direction = glm::length(glm::vec3(world[axis])) > 0.0f ? glm::normalize(glm::vec3(world[axis])) : glm::vec3(0.0f);
                    ImVec2 probeOnScreen;
                    if (!toScreen(center + direction * probe, probeOnScreen))
                        continue;
                    ImVec2 delta(probeOnScreen.x - centerOnScreen.x, probeOnScreen.y - centerOnScreen.y);
                    const float deltaLength = std::sqrt(delta.x * delta.x + delta.y * delta.y);
                    if (deltaLength < 1e-4f)
                        continue; // the axis points at the camera
                    const ImVec2 end(centerOnScreen.x + delta.x / deltaLength * length, centerOnScreen.y + delta.y / deltaLength * length);
                    draw->AddLine(centerOnScreen, end, axisColors[axis], 2.0f);
                    if (m_GizmoType == ImGuizmo::OPERATION::SCALE)
                        draw->AddRectFilled(ImVec2(end.x - 3.0f, end.y - 3.0f), ImVec2(end.x + 3.0f, end.y + 3.0f), axisColors[axis]);
                    else if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
                        draw->AddCircle(end, 4.0f, axisColors[axis], 0, 2.0f);
                    else
                        draw->AddCircleFilled(end, 3.5f, axisColors[axis]);
                }
                draw->AddCircleFilled(centerOnScreen, 3.0f, IM_COL32(255, 255, 255, 200));
            }
        }

        // Its view in the corner (with what it shows besides lit geometry). A click or a scroll in it makes it the active viewport
        // (OnEvent), with every tool.
        std::string viewLabel = kViewModeNames[static_cast<int>(slot.Camera.GetViewMode())];
        if (slot.ShowFlags & Renderer::ExtraViewUnlit)
            viewLabel += " | Unlit";
        if (slot.ShowFlags & Renderer::ExtraViewWireframe)
            viewLabel += " | Wireframe";
        if (slot.ResolutionIndex > 0)
            viewLabel += slot.ResolutionIndex == 1 ? " | 1/2 res" : " | 1/4 res";
        if (!slot.Realtime)
            viewLabel += " | Realtime off";
        const char* viewName = viewLabel.c_str();
        const ImVec2 textSize = ImGui::CalcTextSize(viewName);
        const ImVec2 labelMin(origin.x + 8.0f, origin.y + 8.0f);
        draw->AddRectFilled(labelMin, ImVec2(labelMin.x + textSize.x + 12.0f, labelMin.y + textSize.y + 6.0f), IM_COL32(0, 0, 0, 160), 3.0f);
        draw->AddText(ImVec2(labelMin.x + 6.0f, labelMin.y + 3.0f), IM_COL32(230, 230, 230, 255), viewName);

        ImGui::End();
    }

    void EditorLayer::ApplyViewportLayout(ViewportLayout layout, ImGuiID dockSpace, ImVec2 size)
    {
        // Everything docked in the area is undocked, the area split for the layout, and each viewport docked into its cell. The
        // first viewport keeps the area's central node (the splits put the new cells right / below), so it is never left empty.
        int count = 1;
        switch (layout)
        {
        case ViewportLayout::One: count = 1; break;
        case ViewportLayout::TwoSideBySide:
        case ViewportLayout::TwoStacked: count = 2; break;
        case ViewportLayout::Three: count = 3; break;
        case ViewportLayout::Four: count = 4; break;
        }

        ImGui::DockBuilderRemoveNode(dockSpace);
        ImGui::DockBuilderAddNode(dockSpace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockSpace, ImVec2(std::max(size.x, 64.0f), std::max(size.y, 64.0f)));

        ImGuiID cells[kMaxViewports] = { dockSpace, 0, 0, 0 };
        switch (layout)
        {
        case ViewportLayout::One:
            break;
        case ViewportLayout::TwoSideBySide:
            ImGui::DockBuilderSplitNode(dockSpace, ImGuiDir_Right, 0.5f, &cells[1], &cells[0]);
            break;
        case ViewportLayout::TwoStacked:
            ImGui::DockBuilderSplitNode(dockSpace, ImGuiDir_Down, 0.5f, &cells[1], &cells[0]);
            break;
        case ViewportLayout::Three:
        {
            // A big one on the left, two stacked on the right.
            ImGuiID right = 0;
            ImGui::DockBuilderSplitNode(dockSpace, ImGuiDir_Right, 0.4f, &right, &cells[0]);
            ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.5f, &cells[2], &cells[1]);
            break;
        }
        case ViewportLayout::Four:
        {
            // 2 x 2: 1 2 on top, 3 4 below.
            ImGuiID left = 0, right = 0;
            ImGui::DockBuilderSplitNode(dockSpace, ImGuiDir_Right, 0.5f, &right, &left);
            ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.5f, &cells[2], &cells[0]);
            ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.5f, &cells[3], &cells[1]);
            break;
        }
        }

        for (int index = 0; index < kMaxViewports; ++index)
        {
            if (index < count)
            {
                OpenViewport(index);
                ImGui::DockBuilderDockWindow(m_Viewports[index].Title, cells[index]);
            }
            else
                m_Viewports[index].Open = false;
        }
        // A single viewport has no tab bar, like the editor always had.
        if (layout == ViewportLayout::One)
        {
            if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockSpace))
                node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
        }
        ImGui::DockBuilderFinish(dockSpace);
    }

    void EditorLayer::OpenViewport(int index)
    {
        ViewportSlot& slot = m_Viewports[index];
        if (!slot.CameraSet)
        {
            // A new viewport starts from the active camera, in a view of its own: perspective, top, front, right (or the one it had
            // last session).
            slot.Camera = m_EditorCamera;
            slot.Camera.SetViewMode(static_cast<EditorViewMode>(s_ViewportIni.View[index]));
            slot.CameraSet = true;
        }
        slot.Open = true;
    }

    void EditorLayer::ActivateViewport(int index)
    {
        if (index < 0 || index >= kMaxViewports || index == m_ActiveViewport)
            return;

        ViewportSlot& previous = m_Viewports[m_ActiveViewport];
        ViewportSlot& next = m_Viewports[index];
        if (!next.CameraSet)
            OpenViewport(index);

        const bool wasOrthographic = m_EditorCamera.IsOrthographic();
        previous.Camera = m_EditorCamera;
        previous.CameraSet = true;
        m_EditorCamera = next.Camera;
        m_ActiveViewport = index;

        // The tools follow at once: this frame's update runs before the window is drawn again.
        m_ViewportBounds[0] = next.Bounds[0];
        m_ViewportBounds[1] = next.Bounds[1];
        if (next.Size.x > 0 && next.Size.y > 0)
        {
            m_ViewportSize = next.Size;
            m_EditorCamera.SetViewportSize(static_cast<float>(next.Size.x), static_cast<float>(next.Size.y));
        }
        m_ViewportHovered = next.Hovered;
        m_HoveredEntity = Entity();
        m_IsBoxSelecting = false;
        SyncOrthoRenderSettings(wasOrthographic);
        m_Renderer->ResetCameraHistory();
    }

    int EditorLayer::HoveredViewport() const
    {
        for (int index = 0; index < kMaxViewports; ++index)
        {
            if (m_Viewports[index].Open && m_Viewports[index].Hovered)
                return index;
        }
        return -1;
    }

    int EditorLayer::OpenViewportCount() const
    {
        int count = 0;
        for (const ViewportSlot& slot : m_Viewports)
            count += slot.Open ? 1 : 0;
        return count;
    }

    void EditorLayer::SubmitExtraViewports(Timestep ts)
    {
        const float cell = std::max(LocationSnapWorldUnits(), WorldUnits::FromMeters(1.0f));
        for (int index = 0; index < kMaxViewports; ++index)
        {
            ViewportSlot& slot = m_Viewports[index];
            slot.Submitted = false;
            if (index == m_ActiveViewport || !slot.Open || slot.Size.x == 0 || slot.Size.y == 0)
            {
                slot.DrawnSize = { 0, 0 }; // drawn afresh when it is an extra view again
                continue;
            }

            const float width = static_cast<float>(slot.Size.x);
            const float height = static_cast<float>(slot.Size.y);
            if (slot.Camera.GetAspectRatio() != width / height)
                slot.Camera.SetViewportSize(width, height);
            // Its own easing (an ortho zoom set before it was put aside); no mouse input reaches it.
            slot.Camera.OnUpdate(ts, false);

            // Lower resolution (M3): the image is drawn smaller and stretched over the window.
            const uint32_t divisor = 1u << std::clamp(slot.ResolutionIndex, 0, 2);
            const glm::uvec2 drawSize = glm::max(slot.Size / divisor, glm::uvec2(1));
            uint32_t flags = slot.ShowFlags;

            // Realtime off (UE's viewport option): drawn again only when what it shows changed -- its camera, size or options, the
            // scene (an edit, undo or redo), the selection, a gizmo drag in progress -- and every two seconds (assets streaming in).
            // In Play it is always realtime.
            const glm::mat4 viewProjection = slot.Camera.GetProjection() * slot.Camera.GetViewMatrix();
            size_t selection = m_SceneHierarchyPanel.GetSelectedEntities().size();
            for (const Entity& selected : m_SceneHierarchyPanel.GetSelectedEntities())
                selection = selection * 31 + static_cast<uint32_t>(selected);
            slot.SinceDrawn += ts.GetSeconds();
            if (!slot.Realtime && m_SceneState == SceneState::Edit && slot.DrawnSize == drawSize && slot.DrawnFlags == flags &&
                slot.DrawnViewProjection == viewProjection && slot.DrawnHistory == m_History.GetPosition() && slot.DrawnSelection == selection &&
                !ImGuizmo::IsUsing() && slot.SinceDrawn < 2.0f && m_Renderer->GetExtraViewTexture(static_cast<uint32_t>(index)))
            {
                slot.Submitted = true; // the last image stays on screen
                continue;
            }
            slot.DrawnSize = drawSize;
            slot.DrawnFlags = flags;
            slot.DrawnViewProjection = viewProjection;
            slot.DrawnHistory = m_History.GetPosition();
            slot.DrawnSelection = selection;
            slot.SinceDrawn = 0.0f;

            // The grid as DrawWorldGrid would draw it for this camera.
            shaderio::PushConstantGrid grid{};
            const bool showGrid = m_SceneState != SceneState::Play && m_ShowGrid && slot.ShowGrid;
            if (showGrid)
            {
                const EditorViewMode mode = slot.Camera.GetViewMode();
                const int axis = (mode == EditorViewMode::Front || mode == EditorViewMode::Back) ? 2 : (mode == EditorViewMode::Left || mode == EditorViewMode::Right) ? 0 : 1;
                grid = slot.Camera.IsOrthographic() ? Renderer2D::MakeGrid(slot.Camera.GetFocalPoint(), cell, axis, slot.Camera.GetOrthoHalfHeight())
                                                    : Renderer2D::MakeGrid(slot.Camera.GetPosition(), cell, 1, slot.Camera.GetDistance());
            }
            if (m_SceneState == SceneState::Play)
                flags &= ~Renderer::ExtraViewOutline;
            m_Renderer->SubmitExtraView(static_cast<uint32_t>(index), slot.Camera, NRI::Extent2D{ drawSize.x, drawSize.y }, showGrid ? &grid : nullptr, false, flags);
            slot.Submitted = true;
        }
    }

    void EditorLayer::UI_ViewportOptionsMenu(int index)
    {
        ViewportSlot& slot = m_Viewports[index];
        const std::string label = std::string(slot.Title) + " Options";
        if (!ImGui::BeginMenu(label.c_str()))
            return;
        ImGui::TextDisabled("While it is not the active viewport (that one is the full render)");
        auto flag = [&](const char* name, uint32_t bit, bool inverted, const char* tooltip)
        {
            bool on = ((slot.ShowFlags & bit) != 0) != inverted;
            if (ImGui::MenuItem(name, nullptr, &on))
                slot.ShowFlags = (on != inverted) ? (slot.ShowFlags | bit) : (slot.ShowFlags & ~bit);
            ImGui::SetItemTooltip("%s", tooltip);
        };
        bool lit = (slot.ShowFlags & Renderer::ExtraViewUnlit) == 0;
        if (ImGui::MenuItem("Lit", nullptr, lit))
            slot.ShowFlags &= ~Renderer::ExtraViewUnlit;
        if (ImGui::MenuItem("Unlit", nullptr, !lit))
            slot.ShowFlags |= Renderer::ExtraViewUnlit;
        ImGui::SetItemTooltip("Base colors without lighting or exposure.");
        ImGui::Separator();
        flag("Wireframe", Renderer::ExtraViewWireframe, false, "The triangles' edges over the image.");
        flag("Sky", Renderer::ExtraViewNoSky, true, "The sky behind the geometry (off: black).");
        flag("Particles", Renderer::ExtraViewNoParticles, true, "The scene's particle systems.");
        flag("Selection Outline", Renderer::ExtraViewOutline, false, "The orange outline of the selected entities.");
        ImGui::MenuItem("Grid", nullptr, &slot.ShowGrid);
        ImGui::Separator();
        const char* resolutions[] = { "Full Resolution", "Half Resolution", "Quarter Resolution" };
        for (int resolution = 0; resolution < 3; ++resolution)
        {
            if (ImGui::MenuItem(resolutions[resolution], nullptr, slot.ResolutionIndex == resolution))
                slot.ResolutionIndex = resolution;
        }
        ImGui::Separator();
        ImGui::MenuItem("Realtime", nullptr, &slot.Realtime);
        ImGui::SetItemTooltip("Off: drawn again only when its camera, the scene or the selection change (always on in Play).");
        ImGui::EndMenu();
    }

    void EditorLayer::SyncOrthoRenderSettings(bool wasOrthographic)
    {
        const bool isOrthographic = m_EditorCamera.IsOrthographic();
        if (isOrthographic && !wasOrthographic)
        {
            m_OrthoSaved = { true, m_Renderer->isDLSSEnabled(), m_Renderer->getRayTracingEnabled(), m_Renderer->isPathTracingEnabled() };
            m_Renderer->setDLSSEnabled(false);
            m_Renderer->setRayTracingEnabled(false);
            m_Renderer->setPathTracingEnabled(false);
        }
        else if (!isOrthographic && wasOrthographic && m_OrthoSaved.Valid)
        {
            m_Renderer->setDLSSEnabled(m_OrthoSaved.Dlss);
            m_Renderer->setRayTracingEnabled(m_OrthoSaved.RayTracing);
            m_Renderer->setPathTracingEnabled(m_OrthoSaved.PathTracing);
            m_OrthoSaved.Valid = false;
        }
    }

    void EditorLayer::UI_ViewportOverlay()
    {
        // On the active viewport, while it shows.
        if (m_Viewports[m_ActiveViewport].Size.x == 0 || m_Viewports[m_ActiveViewport].Size.y == 0)
            return;
        ImGui::SetNextWindowPos(ImVec2(m_ViewportBounds[0].x + 10.0f, m_ViewportBounds[0].y + 10.0f));
        ImGui::SetNextWindowBgAlpha(0.65f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking;
        if (!ImGui::Begin("##ViewportOverlay", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        // The gizmo mode: buttons, not Q/W/E/R (WASD-fly needs those keys for movement instead -- see EditorCamera::OnUpdate).
        {
            auto gizmoButton = [this](const char* label, const char* tooltip, int mode)
            {
                const bool active = m_GizmoType == mode;
                if (active)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
                if (ImGui::Button(label, ImVec2(28.0f, 0.0f)))
                    m_GizmoType = mode;
                if (active)
                    ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tooltip);
                ImGui::SameLine();
            };
            gizmoButton("Sel", "Select only, no gizmo", -1);
            gizmoButton("Mov", "Move", ImGuizmo::OPERATION::TRANSLATE);
            gizmoButton("Rot", "Rotate", ImGuizmo::OPERATION::ROTATE);
            gizmoButton("Scl", "Scale", ImGuizmo::OPERATION::SCALE);
            ImGui::NewLine();
        }

        ImGui::Checkbox("Snap", &m_SnapEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Snap moving, rotating, scaling and placing to the steps on the right. Hold Ctrl to switch it for one move.");

        auto stepCombo = [](const char* id, int& index, const auto& values, const char* format, float width)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(width);
            char preview[32];
            snprintf(preview, sizeof(preview), format, values[index]);
            if (ImGui::BeginCombo(id, preview))
            {
                for (int i = 0; i < static_cast<int>(std::size(values)); ++i)
                {
                    char label[32];
                    snprintf(label, sizeof(label), format, values[i]);
                    if (ImGui::Selectable(label, i == index))
                        index = i;
                }
                ImGui::EndCombo();
            }
        };
        stepCombo("##locationSnap", m_LocationSnapIndex, kLocationSnapsCm, "%g cm", 90.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Location step");
        stepCombo("##rotationSnap", m_RotationSnapIndex, kRotationSnaps, "%g deg", 80.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rotation step");
        stepCombo("##scaleSnap", m_ScaleSnapIndex, kScaleSnaps, "%g", 60.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scale step");

        {
            const char* viewNames[] = { "Perspective", "Top", "Bottom", "Front", "Back", "Left", "Right" };
            int view = static_cast<int>(m_EditorCamera.GetViewMode());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::Combo("##viewMode", &view, viewNames, 7))
                SetViewMode(static_cast<EditorViewMode>(view));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("View: Perspective, or an orthographic view along an axis (raster only). Numpad 7 / 1 / 3 (Ctrl: the opposite side), 5 = perspective");
        }
        ImGui::SameLine();
        {
            int figure = !m_ShowReferenceFigure ? 0 : m_ReferenceFigureAtCursor ? 2 : 1;
            const char* figureModes[] = { "No Figure", "Figure at Origin", "Figure at Cursor" };
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::Combo("##figure", &figure, figureModes, 3))
            {
                m_ShowReferenceFigure = figure != 0;
                m_ReferenceFigureAtCursor = figure == 2;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A 180 cm figure to judge sizes: at the world origin, or standing on the surface under the cursor");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Grid", &m_ShowGrid);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The world grid, with the cells of the location step");
        ImGui::SameLine();
        bool wireframe = m_Renderer->getWireframeOverlay();
        if (ImGui::Checkbox("Wireframe", &wireframe))
            m_Renderer->setWireframeOverlay(wireframe);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Every mesh's triangle edges over the image (how dense the geometry is)");
        ImGui::SameLine();
        if (ImGui::SmallButton("To Floor"))
            SnapSelectionToFloor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drop the selection onto the surface below it (End)");
        ImGui::SameLine();
        if (ImGui::SmallButton("To Grid"))
            SnapSelectionToGrid();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Move the selection to the nearest grid point of the location step (its position, not its size)");

        ImGui::SameLine();
        if (ImGui::Checkbox("Measure", &m_MeasureTool) && !m_MeasureTool)
            m_MeasureStage = 0;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Measure (M): click two points, the distance is written on the line. Esc leaves it.");

        // How lengths are written (sizes in the Inspector, the measure tool).
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        const char* displayNames[] = { "cm", "m", "auto" };
        int display = static_cast<int>(WorldUnits::GetDisplayUnit());
        if (ImGui::Combo("##displayUnit", &display, displayNames, 3))
            WorldUnits::SetDisplayUnit(static_cast<WorldUnits::DisplayUnit>(display));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Unit lengths are shown in");

        ImGui::End();
    }

    void EditorLayer::DrawReferenceFigure(const glm::vec3& position)
    {
        // Sizes in cm, converted to world units; the figure stands on `position`, facing -Z, 180 cm tall.
        auto cm = [](float centimeters) { return WorldUnits::FromCentimeters(centimeters); };
        const glm::vec4 color(1.0f, 0.62f, 0.2f, 0.9f);

        auto box = [&](float minX, float maxX, float minY, float maxY, float minZ, float maxZ)
        {
            const glm::vec3 low = position + glm::vec3(cm(minX), cm(minY), cm(minZ));
            const glm::vec3 high = position + glm::vec3(cm(maxX), cm(maxY), cm(maxZ));
            const glm::vec3 corners[8] = {
                { low.x, low.y, low.z }, { high.x, low.y, low.z }, { high.x, low.y, high.z }, { low.x, low.y, high.z },
                { low.x, high.y, low.z }, { high.x, high.y, low.z }, { high.x, high.y, high.z }, { low.x, high.y, high.z } };
            constexpr int edges[12][2] = { {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7} };
            for (const auto& edge : edges)
                m_Renderer2D->DrawThinLine(corners[edge[0]], corners[edge[1]], color);
        };

        box(-18.0f, -1.0f, 0.0f, 88.0f, -10.0f, 10.0f);   // legs
        box(1.0f, 18.0f, 0.0f, 88.0f, -10.0f, 10.0f);
        box(-22.0f, 22.0f, 88.0f, 143.0f, -11.0f, 11.0f); // torso
        box(-31.0f, -22.0f, 83.0f, 143.0f, -5.0f, 5.0f);  // arms
        box(22.0f, 31.0f, 83.0f, 143.0f, -5.0f, 5.0f);
        box(-4.0f, 4.0f, 143.0f, 151.0f, -4.0f, 4.0f);    // neck
        box(-10.0f, 10.0f, 151.0f, 180.0f, -12.0f, 12.0f); // head, the top at 180 cm
    }

    // Decomposes a world matrix's scale the same way JoltPhysics3DScene::CreateBody does, so a collider's wireframe here is sized
    // exactly like the shape Jolt actually simulates (a non-uniform scale on a box collider shows as a non-uniform box; a sphere or
    // a capsule's radius, which Jolt cannot scale non-uniformly, uses the largest axis, matching CreateBody's own formula).
    static glm::vec3 DecomposeWorldScale(const glm::mat4& worldMatrix)
    {
        return glm::vec3(
            glm::length(glm::vec3(worldMatrix[0])),
            glm::length(glm::vec3(worldMatrix[1])),
            glm::length(glm::vec3(worldMatrix[2])));
    }

    // The entity's world position and rotation, without its scale (a collider's own Offset is a fixed world-unit distance from the
    // body's origin, not stretched by the entity's Scale -- JoltPhysics3DScene::CreateBody never multiplies Offset by worldScale,
    // only the shape's own extents/radius). Assumes no shear (an ordinary Translate * Rotate * Scale world matrix).
    static glm::mat4 RigidPart(const glm::mat4& worldMatrix)
    {
        glm::mat4 result = worldMatrix;
        result[0] = glm::vec4(glm::normalize(glm::vec3(worldMatrix[0])), 0.0f);
        result[1] = glm::vec4(glm::normalize(glm::vec3(worldMatrix[1])), 0.0f);
        result[2] = glm::vec4(glm::normalize(glm::vec3(worldMatrix[2])), 0.0f);
        return result;
    }

    void EditorLayer::DrawPhysicsColliders3D()
    {
        constexpr glm::vec4 kColliderColor(0.2f, 1.0f, 0.3f, 0.9f);
        constexpr int kCircleSegments = 24;

        // `transform` is the rigid (no-scale) part of an entity's world matrix; `radius`/`local` are already in world-unit length
        // (the shape's own extent x worldScale, done by the caller -- see JoltPhysics3DScene::CreateBody for the same math).
        auto drawCircle = [this, kColliderColor](const glm::mat4& transform, const glm::vec3& localCentre, float radius, int axis)
        {
            const int a = (axis + 1) % 3, b = (axis + 2) % 3;
            glm::vec3 previous(0.0f);
            for (int i = 0; i <= kCircleSegments; ++i)
            {
                const float t = glm::two_pi<float>() * static_cast<float>(i) / kCircleSegments;
                glm::vec3 local = localCentre;
                local[a] += std::cos(t) * radius;
                local[b] += std::sin(t) * radius;
                const glm::vec3 point = glm::vec3(transform * glm::vec4(local, 1.0f));
                if (i > 0)
                    m_Renderer2D->DrawXRayLine(previous, point, kColliderColor);
                previous = point;
            }
        };

        // Box: a non-uniform Scale shears the half extents (matching Jolt's box shape, which can be non-uniform) but never the Offset.
        auto boxView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, BoxCollider3DComponent>();
        for (auto entity : boxView)
        {
            auto [wtc, box] = boxView.get<WorldTransformComponent, BoxCollider3DComponent>(entity);
            const glm::vec3 halfExtents = box.HalfExtents * DecomposeWorldScale(wtc.WorldMatrix);
            const glm::mat4 transform = RigidPart(wtc.WorldMatrix);
            glm::vec3 corners[8];
            for (int i = 0; i < 8; ++i)
            {
                const glm::vec3 sign((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
                corners[i] = glm::vec3(transform * glm::vec4(box.Offset + sign * halfExtents, 1.0f));
            }
            constexpr int edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
            for (const auto& edge : edges)
                m_Renderer2D->DrawXRayLine(corners[edge[0]], corners[edge[1]], kColliderColor);
        }

        // Sphere: Jolt's sphere shape is always uniform, sized by the largest axis of the entity's scale (JoltPhysics3DScene::CreateBody).
        auto sphereView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, SphereCollider3DComponent>();
        for (auto entity : sphereView)
        {
            auto [wtc, sphere] = sphereView.get<WorldTransformComponent, SphereCollider3DComponent>(entity);
            const glm::vec3 worldScale = DecomposeWorldScale(wtc.WorldMatrix);
            const float radius = sphere.Radius * std::max({ worldScale.x, worldScale.y, worldScale.z });
            const glm::mat4 transform = RigidPart(wtc.WorldMatrix);
            for (int axis = 0; axis < 3; ++axis)
                drawCircle(transform, sphere.Offset, radius, axis);
        }

        // Capsule: two circles at the cap centres plus four verticals along the entity's local Y (JoltPhysics3DScene::CreateCharacter
        // does the equivalent for the character controller, not a component here -- this is CapsuleCollider3DComponent only).
        auto capsuleView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, CapsuleCollider3DComponent>();
        for (auto entity : capsuleView)
        {
            auto [wtc, capsule] = capsuleView.get<WorldTransformComponent, CapsuleCollider3DComponent>(entity);
            const glm::vec3 worldScale = DecomposeWorldScale(wtc.WorldMatrix);
            const float halfHeight = capsule.HalfHeight * worldScale.y;
            const float radius = capsule.Radius * std::max(worldScale.x, worldScale.z);
            const glm::mat4 transform = RigidPart(wtc.WorldMatrix);
            const glm::vec3 topCentre = capsule.Offset + glm::vec3(0.0f, halfHeight, 0.0f);
            const glm::vec3 bottomCentre = capsule.Offset - glm::vec3(0.0f, halfHeight, 0.0f);
            drawCircle(transform, topCentre, radius, 1);
            drawCircle(transform, bottomCentre, radius, 1);
            for (int i = 0; i < 4; ++i)
            {
                const float t = glm::half_pi<float>() * static_cast<float>(i);
                const glm::vec3 rim(std::cos(t) * radius, 0.0f, std::sin(t) * radius);
                m_Renderer2D->DrawXRayLine(glm::vec3(transform * glm::vec4(topCentre + rim, 1.0f)), glm::vec3(transform * glm::vec4(bottomCentre + rim, 1.0f)), kColliderColor);
            }
        }

        // A terrain/heightfield collider goes here later, as its own block: read its component, draw a wireframe grid of its sampled
        // heights (or its own bounds) the same way, no changes needed above.
    }

    void EditorLayer::FocusOnEntity(Entity entity)
    {
        if (!entity)
            return;

        glm::vec3 centre;
        float radius;
        const WorldBounds bounds = ComputeWorldBounds(*m_ActiveScene, entity);
        if (bounds.Valid)
        {
            centre = (bounds.Min + bounds.Max) * 0.5f;
            radius = glm::length(bounds.Size()) * 0.5f;
        }
        else if (entity.HasComponent<WorldTransformComponent>())
        {
            centre = glm::vec3(entity.GetComponent<WorldTransformComponent>().WorldMatrix[3]);
            radius = 0.0f; // Focus() floors this to a sensible minimum
        }
        else
            return;

        m_EditorCamera.Focus(centre, radius);
    }

    void EditorLayer::DrawWorldGrid()
    {
        // Godot's editor grid (GridMesh.slang). Its smallest cell is one meter (Godot's 1 m) or the snap step when that is bigger, so a
        // 10 cm snap does not turn the grid into a haze; the cell size does not change while you zoom (only in far jumps).
        // In an ortho view the grid lies on the plane the view looks at and the level follows the ortho size (Godot does the same).
        const float cell = std::max(LocationSnapWorldUnits(), WorldUnits::FromMeters(1.0f));
        if (m_EditorCamera.IsOrthographic())
            m_Renderer2D->DrawGrid(m_EditorCamera.GetFocalPoint(), cell, GridPlaneAxis(), m_EditorCamera.GetOrthoHalfHeight());
        else
            // The orbit distance, not the camera's height above the ground: height alone jumps just from tilting to look straight
            // down (position.y then equals the orbit distance instead of ~0), which made the grid collapse to its coarsest cells
            // on nothing more than a rotation. Orbit distance only changes when you actually zoom, so the grid now looks the same
            // however you're angled at it, like UE5's.
            m_Renderer2D->DrawGrid(m_EditorCamera.GetPosition(), cell, 1, m_EditorCamera.GetDistance());
    }

    int EditorLayer::GridPlaneAxis() const
    {
        switch (m_EditorCamera.GetViewMode())
        {
        case EditorViewMode::Front:
        case EditorViewMode::Back: return 2;
        case EditorViewMode::Left:
        case EditorViewMode::Right: return 0;
        default: return 1;
        }
    }

    void EditorLayer::SetViewMode(EditorViewMode mode)
    {
        const bool wasOrthographic = m_EditorCamera.IsOrthographic();
        m_EditorCamera.SetViewMode(mode);
        SyncOrthoRenderSettings(wasOrthographic);
    }

    void EditorLayer::DrawTranslationSnapFeedback(const glm::mat4& view, const glm::mat4& projection, const glm::mat4& current)
    {
        // A measuring line from where the drag began to where the object is now; it grows while dragging and jumps by the snap step.
        DrawMeasureLine(view, projection, glm::vec3(m_GizmoDragStart[3]), glm::vec3(current[3]), LocationSnapWorldUnits() * 0.05f);
    }

    void EditorLayer::DrawMeasureLine(const glm::mat4& view, const glm::mat4& projection, const glm::vec3& start, const glm::vec3& end, float minimumLength)
    {
        const float distance = glm::length(end - start);
        if (distance < minimumLength)
            return;

        const glm::mat4 viewProjection = projection * view;
        const float width = m_ViewportBounds[1].x - m_ViewportBounds[0].x;
        const float height = m_ViewportBounds[1].y - m_ViewportBounds[0].y;
        auto project = [&](const glm::vec3& point, ImVec2& out) -> bool
        {
            const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
            if (clip.w <= 0.05f)
                return false;
            const glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w;
            out = ImVec2(m_ViewportBounds[0].x + (ndc.x * 0.5f + 0.5f) * width, m_ViewportBounds[0].y + (1.0f - (ndc.y * 0.5f + 0.5f)) * height);
            return true;
        };

        ImVec2 a, b;
        if (!project(start, a) || !project(end, b))
            return;
        ImVec2 direction(b.x - a.x, b.y - a.y);
        const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
        if (length < 1.0f)
            return;
        direction = ImVec2(direction.x / length, direction.y / length);
        const ImVec2 across(-direction.y * 9.0f, direction.x * 9.0f);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 color = IM_COL32(255, 225, 70, 255);
        draw->AddLine(a, b, IM_COL32(0, 0, 0, 160), 4.0f);
        draw->AddLine(a, b, color, 2.0f);
        draw->AddLine(ImVec2(a.x - across.x, a.y - across.y), ImVec2(a.x + across.x, a.y + across.y), color, 2.0f);
        draw->AddLine(ImVec2(b.x - across.x, b.y - across.y), ImVec2(b.x + across.x, b.y + across.y), color, 2.0f);

        const std::string text = WorldUnits::Format(distance);
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        const ImVec2 middle((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const ImVec2 min(middle.x - size.x * 0.5f - 4.0f, middle.y - size.y * 0.5f - 2.0f);
        const ImVec2 max(middle.x + size.x * 0.5f + 4.0f, middle.y + size.y * 0.5f + 2.0f);
        draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, 190), 3.0f);
        draw->AddText(ImVec2(min.x + 4.0f, min.y + 2.0f), color, text.c_str());
    }

    // The world point under the cursor: the depth the renderer read back for the picked pixel (reverse-Z), unprojected like the lighting
    // shaders do, else the ground plane.
    bool EditorLayer::PickWorldPoint(glm::vec3& out) const
    {
        // The pixel -> NDC mapping uses the size the renderer draws at (what the shaders divide the pixel by), not the ImGui panel.
        const NRI::Extent2D outputSize = m_Renderer->getViewPortSize();
        const glm::vec2 viewportSize(static_cast<float>(outputSize.width), static_cast<float>(outputSize.height));
        if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f)
            return false;

        int32_t pickedX = 0, pickedY = 0;
        float depth = 0.0f;
        const bool hasDepth = m_Renderer->getPickedDepth(pickedX, pickedY, depth, 4);

        glm::vec2 pixel;
        if (hasDepth)
            pixel = glm::vec2(static_cast<float>(pickedX) + 0.5f, static_cast<float>(pickedY) + 0.5f);
        else
        {
            const ImVec2 mouse = ImGui::GetMousePos();
            pixel = glm::vec2(mouse.x - m_ViewportBounds[0].x, mouse.y - m_ViewportBounds[0].y);
        }

        const glm::vec2 ndc = glm::vec2((pixel.x / viewportSize.x) * 2.0f - 1.0f, 1.0f - (pixel.y / viewportSize.y) * 2.0f);
        const glm::mat4 inverseViewProjection = glm::inverse(m_EditorCamera.GetGizmoProjection() * m_EditorCamera.GetGizmoView());
        glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndc, -1.0f, 1.0f);
        glm::vec4 farPoint = inverseViewProjection * glm::vec4(ndc, 1.0f, 1.0f);
        nearPoint /= nearPoint.w;
        farPoint /= farPoint.w;

        const glm::vec3 origin = glm::vec3(nearPoint);
        const glm::vec3 direction = glm::normalize(glm::vec3(farPoint - nearPoint));

        if (hasDepth && depth > 0.0f)
        {
            // The way the lighting shaders do it: the pixel's NDC position with the stored depth through the inverse of the renderer's own
            // projection * view (not the gizmo's), in double precision.
            const glm::dmat4 inverse = glm::inverse(glm::dmat4(m_EditorCamera.GetProjection()) * glm::dmat4(m_EditorCamera.GetViewMatrix()));
            glm::dvec4 world = inverse * glm::dvec4(ndc.x, ndc.y, depth, 1.0);
            if (std::abs(world.w) > 1e-12)
            {
                out = glm::vec3(world / world.w);
                return true;
            }
        }

        // Nothing drawn: the plane the grid lies on.
        const int axis = GridPlaneAxis();
        if (std::abs(direction[axis]) > 1e-4f)
        {
            const float distance = -origin[axis] / direction[axis];
            if (distance > 0.0f)
            {
                out = origin + direction * distance;
                return true;
            }
        }
        return false;
    }

    void EditorLayer::MeasureClick()
    {
        glm::vec3 point;
        if (!PickWorldPoint(point))
            return;
        if (m_SnapEnabled != Input::IsKeyPressed(SDL_SCANCODE_LCTRL))
        {
            const float step = LocationSnapWorldUnits();
            point = glm::round(point / step) * step;
        }

        if (m_MeasureStage == 1)
        {
            m_MeasureB = point;
            m_MeasureStage = 2;
        }
        else
        {
            m_MeasureA = point;
            m_MeasureStage = 1;
        }
    }

    // Inside the viewport window: the line being measured, a ring where the cursor's point is.
    void EditorLayer::UI_MeasureOverlay()
    {
        if (!m_MeasureTool && m_MeasureStage == 0)
            return;

        const glm::mat4 view = m_EditorCamera.GetGizmoView();
        const glm::mat4 projection = m_EditorCamera.GetGizmoProjection();

        glm::vec3 cursor{ 0.0f };
        bool hasCursor = false;
        if (m_MeasureTool && m_ViewportHovered)
        {
            hasCursor = PickWorldPoint(cursor);
            if (hasCursor && m_SnapEnabled != Input::IsKeyPressed(SDL_SCANCODE_LCTRL))
            {
                const float step = LocationSnapWorldUnits();
                cursor = glm::round(cursor / step) * step;
            }
        }

        if (m_MeasureStage == 1 && hasCursor)
            DrawMeasureLine(view, projection, m_MeasureA, cursor, 0.0f);
        else if (m_MeasureStage == 2)
            DrawMeasureLine(view, projection, m_MeasureA, m_MeasureB, 0.0f);

        if (hasCursor)
        {
            const glm::vec4 clip = projection * view * glm::vec4(cursor, 1.0f);
            if (clip.w > 0.05f)
            {
                const glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w;
                const float width = m_ViewportBounds[1].x - m_ViewportBounds[0].x;
                const float height = m_ViewportBounds[1].y - m_ViewportBounds[0].y;
                const ImVec2 screen(m_ViewportBounds[0].x + (ndc.x * 0.5f + 0.5f) * width, m_ViewportBounds[0].y + (1.0f - (ndc.y * 0.5f + 0.5f)) * height);
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddCircle(screen, 6.0f, IM_COL32(0, 0, 0, 200), 16, 3.0f);
                draw->AddCircle(screen, 6.0f, IM_COL32(255, 225, 70, 255), 16, 1.5f);

            }
        }
    }

    void EditorLayer::SnapSelectionToFloor()
    {
        const auto& selection = m_SceneHierarchyPanel.GetSelectedEntities();
        if (selection.empty())
            return;

        // The tops of the selection (an entity whose parent is selected moves with it) and everything below them: none of it is floor.
        std::vector<Entity> tops;
        for (Entity entity : selection)
        {
            if (!entity)
                continue;
            const bool parentSelected = entity.HasComponent<RelationshipComponent>() && entity.GetComponent<RelationshipComponent>().Parent != 0 &&
                                        m_SceneHierarchyPanel.IsSelected(m_ActiveScene->GetEntityByUUID(entity.GetComponent<RelationshipComponent>().Parent));
            if (!parentSelected)
                tops.push_back(entity);
        }
        std::unordered_set<UUID> excluded;
        std::function<void(Entity)> exclude = [&](Entity entity)
        {
            excluded.insert(entity.GetUUID());
            if (!entity.HasComponent<RelationshipComponent>())
                return;
            for (UUID childID : entity.GetComponent<RelationshipComponent>().Children)
            {
                if (Entity child = m_ActiveScene->GetEntityByUUID(childID))
                    exclude(child);
            }
        };
        for (Entity top : tops)
            exclude(top);

        for (Entity top : tops)
        {
            const WorldBounds mover = ComputeWorldBounds(*m_ActiveScene, top);
            if (!mover.Valid)
                continue;
            const float centreY = 0.5f * (mover.Min.y + mover.Max.y);

            // The surface below: the highest top of another mesh that overlaps this one in the ground plane and lies below its middle
            // (bounding boxes: right for floors, tables, boxes; a slope or a curved surface counts as its flat top).
            float floorY = 0.0f; // nothing below: the world ground
            bool found = false;
            for (auto handle : m_ActiveScene->GetAllEntitiesWith<MeshComponent, WorldTransformComponent>())
            {
                Entity candidate(handle, m_ActiveScene.get());
                if (excluded.contains(candidate.GetUUID()))
                    continue;
                const WorldBounds bounds = ComputeWorldBounds(*m_ActiveScene, candidate, false);
                if (!bounds.Valid)
                    continue;
                const bool overlapsX = bounds.Min.x < mover.Max.x && bounds.Max.x > mover.Min.x;
                const bool overlapsZ = bounds.Min.z < mover.Max.z && bounds.Max.z > mover.Min.z;
                if (!overlapsX || !overlapsZ || bounds.Max.y > centreY)
                    continue;
                if (!found || bounds.Max.y > floorY)
                {
                    floorY = bounds.Max.y;
                    found = true;
                }
            }

            // The real surface below (Renderer::RequestRaycast, a frame or two later): five rays down from just under the object,
            // at its centre and towards its corners; the highest surface they find is the floor -- a slope, a curved rock, the
            // inside of a bowl, not their bounding boxes. Nothing found (ray tracing off, nothing below): the boxes' answer.
            struct SnapRays
            {
                UUID Entity;
                float BoundsFloor = 0.0f;
                float OriginY = 0.0f;
                int Pending = 0;
                bool Hit = false;
                float NearestDistance = 0.0f;
            };
            auto snap = std::make_shared<SnapRays>();
            snap->Entity = top.GetUUID();
            snap->BoundsFloor = floorY;
            snap->OriginY = mover.Min.y - WorldUnits::FromMeters(0.01f);
            const glm::vec3 centre = 0.5f * (mover.Min + mover.Max);
            const glm::vec3 halfInset = 0.25f * (mover.Max - mover.Min);
            const glm::vec2 offsets[5] = { { 0.0f, 0.0f }, { -1.0f, -1.0f }, { 1.0f, -1.0f }, { -1.0f, 1.0f }, { 1.0f, 1.0f } };
            snap->Pending = 5;
            for (const glm::vec2& offset : offsets)
            {
                const glm::vec3 origin(centre.x + offset.x * halfInset.x, snap->OriginY, centre.z + offset.y * halfInset.z);
                m_Renderer->RequestRaycast(origin, glm::vec3(0.0f, -1.0f, 0.0f), WorldUnits::FromMeters(1000.0f), [this, snap](bool hit, float distance)
                {
                    if (hit && (!snap->Hit || distance < snap->NearestDistance))
                    {
                        snap->Hit = true;
                        snap->NearestDistance = distance;
                    }
                    if (--snap->Pending > 0)
                        return;
                    Entity entity = m_ActiveScene ? m_ActiveScene->GetEntityByUUID(snap->Entity) : Entity{};
                    if (!entity || !entity.HasComponent<WorldTransformComponent>())
                        return;
                    const WorldBounds now = ComputeWorldBounds(*m_ActiveScene, entity);
                    if (!now.Valid)
                        return;
                    const float floor = snap->Hit ? snap->OriginY - snap->NearestDistance : snap->BoundsFloor;
                    const float drop = floor - now.Min.y;
                    entity.SetWorldTransform(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, drop, 0.0f)) * entity.GetComponent<WorldTransformComponent>().WorldMatrix);
                    m_HistoryCheckRequested = true; // one undo step
                });
            }
        }
    }

    void EditorLayer::SnapSelectionToGrid()
    {
        // Unreal's "Snap Origin to Grid": every selected entity whose parent is not selected too (children follow their parent).
        const float step = LocationSnapWorldUnits();
        if (step <= 0.0f)
            return;
        for (Entity entity : m_SceneHierarchyPanel.GetSelectedEntities())
        {
            if (!entity || !entity.HasComponent<WorldTransformComponent>())
                continue;
            const bool parentSelected = entity.HasComponent<RelationshipComponent>() && entity.GetComponent<RelationshipComponent>().Parent != 0 &&
                                        m_SceneHierarchyPanel.IsSelected(m_ActiveScene->GetEntityByUUID(entity.GetComponent<RelationshipComponent>().Parent));
            if (parentSelected)
                continue;
            glm::mat4 world = entity.GetComponent<WorldTransformComponent>().WorldMatrix;
            const glm::vec3 position(world[3]);
            world[3] = glm::vec4(glm::round(position / step) * step, 1.0f);
            entity.SetWorldTransform(world);
        }
        m_HistoryCheckRequested = true;
    }

    AssetHandle EditorLayer::CreateVariant(AssetHandle baseHandle, const std::filesystem::path& relativeFolder)
    {
        Ref<Prefab> base = AssetManager::GetAsset<Prefab>(baseHandle);
        if (!base)
            return 0;

        // "<Base> Variant": the name the Content Browser then lets the user change.
        std::string name = base->Name + " Variant";
        constexpr char forbidden[] = { 92, 47, 58, 42, 63, 34, 60, 62, 124, 0 }; // \ / : * ? " < > |
        for (char& character : name)
        {
            if (std::strchr(forbidden, character) != nullptr)
                character = '_';
        }

        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        std::filesystem::path relativePath = relativeFolder / (name + ".nprefab");
        for (int suffix = 2; std::filesystem::exists(assetDirectory / relativePath); ++suffix)
            relativePath = relativeFolder / (name + " " + std::to_string(suffix) + ".nprefab");

        if (!PrefabImporter::CreateVariantFile(relativePath.stem().string(), base->Root, baseHandle, assetDirectory / relativePath))
            return 0;
        return Project::GetActive()->GetEditorAssetManager()->RegisterExistingFile(relativePath, AssetType::Prefab);
    }

    AssetHandle EditorLayer::CreatePrefab(UUID dragged, const std::filesystem::path& relativeFolder)
    {
        Entity draggedEntity = m_ActiveScene->GetEntityByUUID(dragged);
        if (!draggedEntity)
            return 0;

        // The dragged entity, or the whole selection when it is part of it. The dragged one goes first: it is the pivot the
        // others are positioned against.
        std::vector<Entity> entities{ draggedEntity };
        if (m_SceneHierarchyPanel.IsSelected(draggedEntity))
        {
            for (Entity selected : m_SceneHierarchyPanel.GetSelectedEntities())
            {
                if (selected && !(selected == draggedEntity))
                    entities.push_back(selected);
            }
        }

        // The default name the Content Browser then lets the user change (like a new file in Windows).
        std::string name = entities.size() == 1 ? draggedEntity.GetName() : std::string("New Prefab");
        for (char& character : name)
        {
            if (std::strchr("\\/:*?\"<>|", character) != nullptr)
                character = '_';
        }
        if (name.empty())
            name = "New Prefab";

        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        std::filesystem::path relativePath = relativeFolder / (name + ".nprefab");
        for (int suffix = 2; std::filesystem::exists(assetDirectory / relativePath); ++suffix)
            relativePath = relativeFolder / (name + " " + std::to_string(suffix) + ".nprefab");

        if (!SceneSerializer::SerializePrefab(*m_ActiveScene, entities, relativePath.stem().string(), assetDirectory / relativePath))
            return 0;
        return Project::GetActive()->GetEditorAssetManager()->RegisterExistingFile(relativePath, AssetType::Prefab);
    }

    void EditorLayer::PlaceAssets(const std::vector<AssetHandle>& handles, const glm::vec3& point, const std::string& folder)
    {
        m_SceneHierarchyPanel.ClearSelection();
        for (AssetHandle handle : handles)
        {
            const AssetType type = AssetManager::GetAssetType(handle);
            if (type == AssetType::Prefab)
            {
                // A prefab instance: its entities spawn from the .nprefab once it is loaded (PrefabInstance::SpawnPending).
                const AssetMetadata& prefabMetadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
                std::string prefabName = prefabMetadata.FilePath.filename().stem().string();
                Entity prefabRoot = m_ActiveScene->CreateEntity(prefabName.empty() ? "Prefab" : prefabName);
                auto& prefabInstance = prefabRoot.AddComponent<PrefabInstanceComponent>();
                prefabInstance.Prefab = handle;
                prefabInstance.InitTransform = true;
                prefabRoot.GetComponent<TransformComponent>().Translation = point;
                if (!folder.empty())
                    prefabRoot.AddComponent<FolderComponent>(folder);
                AssetManager::RequestAsset(handle);
                continue;
            }
            if (type != AssetType::Mesh && type != AssetType::StaticMesh)
                continue;

            const AssetMetadata& metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
            std::string entityName = metadata.FilePath.filename().stem().string();
            Entity root = m_ActiveScene->CreateEntity(entityName.empty() ? "Model" : entityName);
            auto& instance = root.AddComponent<ModelInstanceComponent>();
            instance.Model = handle;
            // Several assets dragged together (this function): each keeps its own baked position from the source file,
            // offset by the shared drop point (ModelInstance combines them: rootLocal * modelSpace) -- reconstructing
            // the file's original relative layout, not stacking every dragged piece on the exact same point.
            instance.AtFileLayout = true;
            auto& rootTransform = root.GetComponent<TransformComponent>();
            rootTransform.Translation = point;
            // Baked into the cooked geometry now, static or skeletal alike (see the other drag-in path above).
            if (!folder.empty())
                root.AddComponent<FolderComponent>(folder);
            AssetManager::RequestAsset(handle);
        }
    }

    AssetHandle EditorLayer::GetShapeAsset(PrimitiveShape shape)
    {
        Ref<Project> project = Project::GetActive();
        if (!project)
            return 0;

        // Like UE's engine BasicShapes, but in the project: one asset per shape, shared by every placement, whose material can
        // be edited like any imported one. A glTF already in the folder (made earlier) is imported again rather than rewritten.
        auto assetManager = project->GetEditorAssetManager();
        const std::string name = PrimitiveShapes::GetName(shape);
        const std::filesystem::path meshPath = std::filesystem::path("Shapes") / (name + ".nsmesh");
        if (const AssetHandle existing = assetManager->FindHandleByPath(meshPath, AssetType::StaticMesh); existing != 0)
            return existing;

        const std::filesystem::path sourcePath = std::filesystem::path("Shapes") / (name + ".gltf");
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        if (!std::filesystem::exists(assetDirectory / sourcePath) && PrimitiveShapes::WriteGltf(shape, assetDirectory / "Shapes").empty())
        {
            NOX_CORE_ERROR("Could not write the {} shape into {}", name, (assetDirectory / "Shapes").generic_string());
            return 0;
        }

        MeshImportSettings settings;
        settings.ImportSkeletalMeshes = false;
        settings.ImportAnimations = false;
        settings.StaticCombine = MeshCombineMode::CombineAll; // one asset, registered right away (Do Not Combine registers when its cook ends)
        settings.ImportScale = WorldUnits::PerSourceUnit(WorldUnits::SourceUnit::Meters); // the glTF is in meters, baked at cook time
        if (!assetManager->ImportModel(sourcePath, meshPath, settings))
            return 0;
        return assetManager->FindHandleByPath(meshPath, AssetType::StaticMesh);
    }

    void EditorLayer::CreateShape(PrimitiveShape shape, const std::string& folder)
    {
        if (!m_ActiveScene)
            return;
        const AssetHandle mesh = GetShapeAsset(shape);
        if (mesh == 0)
            return;
        // Placed like a model dropped on the hierarchy: at the point the editor camera orbits. One mesh node, so it spawns as a
        // plain mesh entity named after the shape once the asset has loaded.
        PlaceAssets({ mesh }, m_EditorCamera.GetPosition() + m_EditorCamera.GetForwardDirection() * m_EditorCamera.GetDistance(), folder);
    }

    void EditorLayer::UI_PackageGame()
    {
        if (m_ShowPackageGame)
        {
            ImGui::OpenPopup("Package Game");
            m_ShowPackageGame = false;
        }
        ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("Package Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;

        ImGui::TextWrapped("A folder that runs the game on its own: the runtime (NoxRuntime, renamed after the project), the engine's "
                           "libraries, scripts and shaders, and only the assets the scenes need, cooked, without their sources.");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(480.0f);
        ImGui::InputText("Output folder", m_PackageOutput, sizeof(m_PackageOutput));
        ImGui::Checkbox("Every scene (not only the start scene)", &m_PackageAllScenes);
        ImGui::Checkbox("Pack files (Content.npak)", &m_PackagePackFiles);
        ImGui::Checkbox("Precompiled shaders only (no Slang)", &m_PackagePrecompiledShaders);
        ImGui::Checkbox("Self-contained .NET", &m_PackageSelfContainedDotNet);
        ImGui::SetItemTooltip("Copies the .NET 9 runtime into the game (about 70 MB): it runs where .NET is not installed.");
        ImGui::SetItemTooltip("The game uses the shader cache the editor wrote (every pipeline it created) and ships without the\n"
                              "shader compiler. A shader the editor never compiled would fail: off ships Slang to compile it.");
        ImGui::SetItemTooltip("The assets in one pack file the game reads from (only its table of contents stays in memory).\n"
                              "Off: a folder of loose files, easier to inspect.");
        ImGui::SetItemTooltip("The start scene and what it uses is the game; tick this when scripts load other scenes.");
        ImGui::Spacing();

        if (ImGui::Button("Package", ImVec2(120.0f, 0.0f)))
        {
            EndTimelinePreviews();
            SaveScene(); // the scene as edited is what ships
            PackageSettings settings;
            settings.OutputDirectory = std::filesystem::path(m_PackageOutput);
            settings.AllScenes = m_PackageAllScenes;
            settings.PackFiles = m_PackagePackFiles;
            settings.PrecompiledShadersOnly = m_PackagePrecompiledShaders;
            settings.SelfContainedDotNet = m_PackageSelfContainedDotNet;
            const PackageReport report = PackageGame(settings);
            if (!report.Success)
                m_PackageResult = "Failed: " + report.Error;
            else
            {
                char summary[512];
                snprintf(summary, sizeof(summary), "Done: %zu assets, %zu files, %.1f MB.\nRun %s", report.Assets, report.Files,
                         static_cast<double>(report.Bytes) / (1024.0 * 1024.0), report.Executable.generic_string().c_str());
                m_PackageResult = summary;
                for (const std::string& warning : report.Warnings)
                    m_PackageResult += "\n- " + warning;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        if (!m_PackageResult.empty())
        {
            ImGui::Separator();
            ImGui::BeginChild("##package_result", ImVec2(600.0f, 160.0f), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", m_PackageResult.c_str());
            ImGui::EndChild();
        }
        ImGui::EndPopup();
    }

    void EditorLayer::EndTimelinePreviews()
    {
        for (auto& panel : m_TimelinePanels)
            panel->EndPreview();
    }

    void EditorLayer::UpdateHistory()
    {
        // An interaction ends when the ImGui item that was held (a drag, a text field, a button, a menu item, a drag-drop source)
        // lets go, when the gizmo is released, or when a shortcut ran: that is when an edit is complete and becomes one step.
        const bool itemActive = ImGui::IsAnyItemActive();
        const bool gizmoUsing = ImGuizmo::IsUsing();
        EditorHistory::FrameInfo frame;
        frame.InteractionActive = itemActive || gizmoUsing;
        frame.GizmoEnded = m_HistoryGizmoWasUsing && !gizmoUsing;
        // A key pressed during a drag (Ctrl toggles snapping) waits for the drag to end, so the drag stays one step.
        frame.InteractionEnded = !frame.InteractionActive &&
                                 ((m_HistoryItemWasActive && !itemActive) || frame.GizmoEnded || m_HistoryCheckRequested);
        m_HistoryItemWasActive = itemActive;
        m_HistoryGizmoWasUsing = gizmoUsing;
        if (!frame.InteractionActive)
            m_HistoryCheckRequested = false;

        // Play and Simulate change a copy of the scene that is thrown away on Stop: nothing to undo there.
        if (m_SceneState != SceneState::Edit || !m_ActiveScene || !Project::GetActive())
            return;

        // A timeline preview poses its targets without editing the scene: nothing to record until it ends (and puts them back).
        for (const auto& panel : m_TimelinePanels)
        {
            if (panel->IsPreviewing())
                return;
        }

        frame.AssetsLoading = Project::GetActive()->GetEditorAssetManager()->GetLoadingCount() > 0;
        if (Entity selected = m_SceneHierarchyPanel.GetSelectedEntity())
            frame.Watched = selected.GetUUID();
        m_History.Update(m_ActiveScene, frame);
    }

    void EditorLayer::Undo()
    {
        // Not in the middle of a drag: the step being dragged is not taken yet.
        if (m_SceneState != SceneState::Edit || !m_History.CanUndo() || ImGuizmo::IsUsing() || ImGui::IsAnyItemActive())
            return;
        const std::string name = m_History.GetUndoName();
        RestoreHistoryState(m_History.StepBack());
        NOX_CORE_INFO("Undo: {}", name);
    }

    void EditorLayer::Redo()
    {
        if (m_SceneState != SceneState::Edit || !m_History.CanRedo() || ImGuizmo::IsUsing() || ImGui::IsAnyItemActive())
            return;
        const std::string name = m_History.GetRedoName();
        RestoreHistoryState(m_History.StepForward());
        NOX_CORE_INFO("Redo: {}", name);
    }

    void EditorLayer::RestoreHistoryState(const std::string& text)
    {
        NOX_PROFILE_SCOPE("Undo Restore");

        // Loaded like a scene file into a new scene, which replaces the edited one (what OpenScene does): every entity keeps its
        // UUID, model and prefab instances spawn again from their (already loaded) assets.
        Ref<Scene> restored = CreateRef<Scene>();
        SceneSerializer serializer(restored);
        if (!serializer.DeserializeText(text))
        {
            NOX_CORE_ERROR("Undo / redo: the history state could not be read; the scene is left as it is");
            return;
        }

        std::vector<UUID> selection;
        for (Entity selected : m_SceneHierarchyPanel.GetSelectedEntities())
        {
            if (selected)
                selection.push_back(selected.GetUUID());
        }

        m_HoveredEntity = Entity();
        restored->OnViewportResize(static_cast<uint32_t>(m_ViewportSize.x), static_cast<uint32_t>(m_ViewportSize.y));
        m_EditorScene = restored;
        m_ActiveScene = restored;
        m_SceneHierarchyPanel.SetContext(restored);
        for (UUID id : selection)
        {
            if (Entity entity = restored->GetEntityByUUID(id))
                m_SceneHierarchyPanel.ToggleSelectedEntity(entity);
        }
        m_UnloadUnusedAssetsRequested = true;
        m_History.OnSceneRestored(restored);
    }

    void EditorLayer::SaveScene()
    {
        EndTimelinePreviews(); // the scene as it is, not posed by a preview (which resumes next frame)
        if (m_PrefabMode.Active)
        {
            SavePrefabMode(); // Ctrl+S in Prefab Mode saves the prefab, never the level
            return;
        }
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
        EndTimelinePreviews();
        if (m_PrefabMode.Active)
        {
            SavePrefabMode();
            return;
        }
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

    void EditorLayer::OpenPrefabMode(AssetHandle handle)
    {
        if (m_PrefabMode.Active)
        {
            NOX_CORE_WARN("Prefab Mode is already open for '{}': exit it first", m_PrefabMode.Name);
            return;
        }
        if (m_SceneState != SceneState::Edit)
            OnSceneStop();

        const AssetMetadata metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
        Ref<Prefab> prefab = PrefabImporter::ImportPrefab(handle, metadata);
        if (!prefab)
            return;

        Ref<Scene> prefabScene = CreateRef<Scene>();
        // A variant is edited as an instance of its base carrying the variant's changes; any other prefab as its own entities.
        const bool isVariant = prefab->Base != 0;
        Entity root = isVariant ? PrefabInstance::LoadVariantForEditing(*prefabScene, *prefab) : PrefabInstance::LoadForEditing(*prefabScene, *prefab);
        if (!root)
        {
            NOX_CORE_ERROR("Prefab '{}' has no root entity", metadata.FilePath.generic_string());
            return;
        }

        // Light for the isolated scene: the level's environment and sun, as preview entities. They are not below the root, so
        // they are never saved into the prefab. A level without an environment gets the project's default one.
        bool hasEnvironment = false;
        if (m_EditorScene)
        {
            for (auto handleInLevel : m_EditorScene->GetAllEntitiesWith<EnvironmentLightComponent>())
            {
                Entity source(handleInLevel, m_EditorScene.get());
                Entity preview = prefabScene->CreateEntity("Preview Environment");
                preview.AddComponent<EnvironmentLightComponent>(source.GetComponent<EnvironmentLightComponent>());
                hasEnvironment = true;
            }
            for (auto handleInLevel : m_EditorScene->GetAllEntitiesWith<DirectionalLightComponent>())
            {
                Entity source(handleInLevel, m_EditorScene.get());
                Entity preview = prefabScene->CreateEntity("Preview Sun");
                preview.GetComponent<TransformComponent>() = source.GetComponent<TransformComponent>();
                preview.AddComponent<DirectionalLightComponent>(source.GetComponent<DirectionalLightComponent>());
            }
        }
        if (!hasEnvironment)
        {
            Entity preview = prefabScene->CreateEntity("Preview Environment");
            preview.AddComponent<EnvironmentLightComponent>().TexturePath = "EnvironmentMaps/shanghai_bund_4k_cube_bc6u.dds";
        }

        m_PrefabMode.Active = true;
        m_PrefabMode.Variant = isVariant;
        m_PrefabMode.Prefab = handle;
        m_PrefabMode.Name = metadata.FilePath.stem().string();
        m_PrefabMode.ReturnScene = m_EditorScene;
        m_PrefabMode.ReturnScenePath = m_EditorScenePath;
        m_PrefabMode.Root = root.GetUUID();

        m_HoveredEntity = Entity();
        m_EditorScene = prefabScene;
        m_ActiveScene = prefabScene;
        // Prefab Mode gets its own history; the level's waits until it is back.
        m_LevelHistory = std::move(m_History);
        m_History.Clear();
        m_EditorScenePath.clear(); // nothing saves the level over the prefab
        m_SceneHierarchyPanel.SetContext(m_EditorScene);
        m_SceneHierarchyPanel.SetSelectedEntity(root);
        m_UnloadUnusedAssetsRequested = true;
    }

    void EditorLayer::SavePrefabMode()
    {
        if (!m_PrefabMode.Active)
            return;
        Entity root = m_EditorScene->GetEntityByUUID(m_PrefabMode.Root);
        if (!root)
        {
            NOX_CORE_ERROR("Prefab '{}': its root entity was deleted, nothing to save", m_PrefabMode.Name);
            return;
        }

        const AssetMetadata metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(m_PrefabMode.Prefab);
        if (m_PrefabMode.Variant)
        {
            // The variant file holds what this instance of its base differs in, and the entities added below it.
            Ref<Prefab> variantFile = PrefabImporter::ImportPrefab(m_PrefabMode.Prefab, metadata);
            if (!variantFile || !PrefabInstance::SaveVariant(*m_EditorScene, root, *variantFile, Project::GetActiveAssetDirectory() / metadata.FilePath))
            {
                NOX_CORE_ERROR("Could not save the variant '{}' (its base has to be loaded)", m_PrefabMode.Name);
                return;
            }
        }
        else if (!SceneSerializer::SerializePrefab(*m_EditorScene, { root }, m_PrefabMode.Name, Project::GetActiveAssetDirectory() / metadata.FilePath))
        {
            return;
        }

        // The instances take their differences from the prefab first (they are told apart against the old content), then the loaded
        // asset takes the new content (spawns read it on the main thread) and every instance in the level spawns again from it.
        if (m_PrefabMode.ReturnScene)
            PrefabInstance::CaptureOverrides(*m_PrefabMode.ReturnScene, m_PrefabMode.Prefab);
        if (Ref<Prefab> fresh = PrefabImporter::ImportPrefab(m_PrefabMode.Prefab, metadata))
        {
            if (Prefab* loaded = AssetManager::FindLoadedAsset<Prefab>(m_PrefabMode.Prefab))
            {
                loaded->Name = fresh->Name;
                loaded->Root = fresh->Root;
                loaded->Entities = fresh->Entities;
                loaded->Base = fresh->Base;
                loaded->Overrides = fresh->Overrides;
                loaded->Structure = fresh->Structure;
                loaded->Nested = fresh->Nested;
            }
        }
        if (m_PrefabMode.ReturnScene)
            PrefabInstance::RespawnAll(*m_PrefabMode.ReturnScene, m_PrefabMode.Prefab);

        // Its Content Browser thumbnail is drawn again.
        AssetPreviewRenderer::Get().Invalidate(m_PrefabMode.Prefab);

        // Saved: what is in the scene now is the reference.
        m_PrefabMode.Snapshot = CurrentPrefabText();
        m_PrefabMode.SnapshotTaken = !m_PrefabMode.Snapshot.empty();
        m_PrefabMode.Dirty = false;
    }

    std::string EditorLayer::CurrentPrefabText()
    {
        if (!m_PrefabMode.Active)
            return {};
        Entity root = m_EditorScene->GetEntityByUUID(m_PrefabMode.Root);
        if (!root)
            return {};

        if (m_PrefabMode.Variant)
        {
            Prefab variant;
            variant.Name = m_PrefabMode.Name;
            return PrefabInstance::BuildVariant(*m_EditorScene, root, variant) ? PrefabImporter::PrefabToText(variant) : std::string();
        }
        return SceneSerializer::PrefabToText(*m_EditorScene, { root }, m_PrefabMode.Name);
    }

    // Every frame in Prefab Mode: the reference text is taken once everything in the scene has spawned (an instance spawns a frame after
    // it is placed, the ones inside it a frame later); after that the text is compared every few frames for the "modified" mark.
    void EditorLayer::UpdatePrefabModeDirty()
    {
        if (!m_PrefabMode.Active)
            return;

        if (!m_PrefabMode.SnapshotTaken)
        {
            // A few frames pass first: components settle in their first updates (a camera takes the viewport's aspect ratio).
            if (m_PrefabMode.FramesSinceCheck++ < 5)
                return;
            for (auto handle : m_EditorScene->GetAllEntitiesWith<PrefabInstanceComponent>())
            {
                if (!m_EditorScene->GetAllEntitiesWith<PrefabInstanceComponent>().get<PrefabInstanceComponent>(handle).Spawned)
                    return;
            }
            m_PrefabMode.Snapshot = CurrentPrefabText();
            m_PrefabMode.SnapshotTaken = !m_PrefabMode.Snapshot.empty();
            m_PrefabMode.FramesSinceCheck = 0;
            return;
        }

        if (++m_PrefabMode.FramesSinceCheck >= 20)
        {
            m_PrefabMode.FramesSinceCheck = 0;
            m_PrefabMode.Dirty = CurrentPrefabText() != m_PrefabMode.Snapshot;
        }
    }

    void EditorLayer::RequestExitPrefabMode(std::function<void()> afterwards)
    {
        if (!m_PrefabMode.Active)
        {
            if (afterwards)
                afterwards();
            return;
        }

        // Compared now, not by the last periodic check: a change made a moment ago counts.
        const bool dirty = m_PrefabMode.SnapshotTaken && CurrentPrefabText() != m_PrefabMode.Snapshot;
        if (!dirty)
        {
            ExitPrefabMode();
            if (afterwards)
                afterwards();
            return;
        }
        m_AfterPrefabExit = std::move(afterwards);
        m_ShowPrefabExitPrompt = true;
    }

    void EditorLayer::UI_PrefabExitPrompt()
    {
        if (m_ShowPrefabExitPrompt)
        {
            ImGui::OpenPopup("Unsaved prefab changes");
            m_ShowPrefabExitPrompt = false;
        }
        if (!ImGui::BeginPopupModal("Unsaved prefab changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;

        ImGui::Text("'%s' has changes that are not saved.", m_PrefabMode.Name.c_str());
        ImGui::Separator();
        if (ImGui::Button("Save"))
        {
            SavePrefabMode();
            ExitPrefabMode();
            std::function<void()> afterwards = std::move(m_AfterPrefabExit);
            m_AfterPrefabExit = nullptr;
            ImGui::CloseCurrentPopup();
            if (afterwards)
                afterwards();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save"))
        {
            ExitPrefabMode();
            std::function<void()> afterwards = std::move(m_AfterPrefabExit);
            m_AfterPrefabExit = nullptr;
            ImGui::CloseCurrentPopup();
            if (afterwards)
                afterwards();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_AfterPrefabExit = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void EditorLayer::ExitPrefabMode()
    {
        if (!m_PrefabMode.Active)
            return;

        m_HoveredEntity = Entity();
        m_EditorScene = m_PrefabMode.ReturnScene;
        m_ActiveScene = m_EditorScene;
        m_History = std::move(m_LevelHistory);
        m_LevelHistory.Clear();
        m_EditorScenePath = m_PrefabMode.ReturnScenePath;
        m_PrefabMode = {};
        m_SceneHierarchyPanel.SetContext(m_EditorScene);
        m_SceneHierarchyPanel.ClearSelection();
        m_UnloadUnusedAssetsRequested = true;
    }

    void EditorLayer::OpenAsset(AssetHandle handle)
    {
        auto opener = m_AssetOpeners.find(AssetManager::GetAssetType(handle));
        if (opener != m_AssetOpeners.end())
            opener->second(handle);
    }

    void EditorLayer::OpenNodeGraphEditor(AssetHandle handle)
    {
        for (auto& panel : m_NodeGraphEditors)
        {
            if (panel->GetGraphAsset() == handle)
            {
                panel->SetOpen(true);
                return;
            }
        }
        m_NodeGraphEditors.push_back(CreateScope<NodeGraphEditorPanel>(handle));
    }

    void EditorLayer::OpenMaterialEditor(AssetHandle handle)
    {
        for (auto& panel : m_MaterialEditors)
        {
            if (panel->GetMaterial() == handle)
            {
                ImGui::SetWindowFocus(panel->GetTitle().c_str()); // already open: bring it to the front
                return;
            }
        }
        m_MaterialEditors.push_back(CreateScope<MaterialEditorPanel>(handle));
    }

    void EditorLayer::OnScenePlay()
    {
        EndTimelinePreviews(); // Play copies the scene as it is
        SetViewMode(EditorViewMode::Perspective);
        if (m_SceneState == SceneState::Simulate)
            OnSceneStop();

        m_SceneState = SceneState::Play;

        m_HoveredEntity = Entity(); // points into the scene being replaced
        m_ActiveScene = Scene::Copy(m_EditorScene);
        m_ActiveScene->OnRuntimeStart();

        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
    }

    void EditorLayer::OnSceneSimulate()
    {
        EndTimelinePreviews();
        if (m_SceneState == SceneState::Play)
            OnSceneStop();

        m_SceneState = SceneState::Simulate;

        m_HoveredEntity = Entity(); // points into the scene being replaced
        m_ActiveScene = Scene::Copy(m_EditorScene);
        m_ActiveScene->OnSimulationStart();

        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
    }

    void EditorLayer::OnSceneStop()
    {
        NOX_CORE_ASSERT(m_SceneState == SceneState::Play || m_SceneState == SceneState::Simulate, "OnSceneStop failed no sceneState match");

        if (m_SceneState == SceneState::Play)
            m_ActiveScene->OnRuntimeStop();
        else if (m_SceneState == SceneState::Simulate)
            m_ActiveScene->OnSimulationStop();

        m_SceneState = SceneState::Edit;

        m_HoveredEntity = Entity(); // points into the play scene that is destroyed here
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
        if (m_PrefabMode.ReturnScene) // the level waits while a prefab is edited: its assets stay
            m_PrefabMode.ReturnScene->CollectAssetReferences(referencedAssets);
        AssetPreviewRenderer::Get().CollectAssetReferences(referencedAssets); // what the asset previews draw right now

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
