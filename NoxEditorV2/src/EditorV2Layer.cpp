#include "EditorV2Layer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <system_error>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_video.h>
#include <glm/gtc/quaternion.hpp>
#include <stb_image_write.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Audio/AudioMixer.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Input.h"
#include "NoxCore/Core/Window.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/SceneSerializer.h"
#include "NoxCore/Scripting/ScriptEngine.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UILayer.h"
#include "NoxCore/UI/Layout/UIDockSpace.h"
#include "NoxCore/UI/Widgets/UIImage.h"
#include "NoxCore/Utils/Utils.h"
#include "ProjectManager/ProcessLauncher.h"
#include "ProjectManager/ProjectLibrary.h"

namespace Nox
{
    namespace
    {
        constexpr const char* kLayoutPath = "assets/layouts/EditorV2.nlayout";
    }

    EditorV2Layer::EditorV2Layer(std::filesystem::path projectFile, bool createBasicScene)
        : Layer("EditorV2Layer")
        , m_EditorCamera(30.0f, 1.778f, 0.01f, 1000.0f)
        , m_CreateBasicScene(createBasicScene)
    {
        m_ActiveScene = CreateRef<Scene>();

        if (!projectFile.empty())
            NOX_CORE_INFO("Loading project {}", projectFile.generic_string());
        if ((projectFile.empty() || !OpenProject(projectFile)) && !OpenStartupProject(projectFile))
        {
            SDL_Event event{};
            event.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&event);
        }

        if (m_ApplyBasicSceneView)
            ApplyBasicSceneView();

        BuildUI(true);
    }

    EditorV2Layer::~EditorV2Layer()
    {
        if (UILayer* uiLayer = Application::Get().GetUILayer())
        {
            if (const auto& dock = uiLayer->GetDockSpaceRef())
                dock->SaveLayout(kLayoutPath);
        }
        if (Project::GetActive() && Project::GetActive()->GetEditorAssetManager())
            Project::GetActive()->GetEditorAssetManager()->Shutdown();
    }

    bool EditorV2Layer::NewProject()
    {
        std::filesystem::path path = Utility::SaveFile("Nox Project\0nproj\0");
        if (path.empty())
            return false;
        ProjectLibrary::NewProjectSettings settings;
        settings.Name = path.stem().string();
        settings.Location = path.parent_path();
        std::string error;
        const std::filesystem::path projectFile = ProjectLibrary::CreateProject(settings, error);
        if (projectFile.empty())
        {
            NOX_CORE_ERROR("New Project: {}", error);
            return false;
        }
        m_CreateBasicScene = true;
        return OpenProject(projectFile);
    }

    bool EditorV2Layer::OpenProject()
    {
        const std::string filepath = Utility::OpenFile("Nox Project\0nproj\0");
        if (filepath.empty())
            return false;
        return OpenProject(filepath);
    }

    bool EditorV2Layer::OpenProject(const std::filesystem::path& path)
    {
        if (path.empty())
            return false;

        const bool hadProject = Project::GetActive() != nullptr;
        if (!Project::Load(path))
        {
            NOX_CORE_ERROR("Could not open the project '{}'", path.string());
            return false;
        }

        Renderer* renderer = Application::Get().GetRenderer();
        Ref<Project> project = Project::GetActive();

        ProjectLibrary::RememberRecentProject(path);
        if (auto win = Application::Get().getWindow(); win && win->getHandle() && project)
            SDL_SetWindowTitle(win->getHandle(), ("Nox Editor - " + project->GetConfig().Name).c_str());

        m_EditorCamera.ApplyWorldUnitDefaults();
        if (renderer && project)
        {
            renderer->ApplyQualitySetting(project->GetConfig().RenderQuality);
            renderer->ApplyProjectRendererSettings(project->GetConfig().Renderer);
        }
        if (project && project->GetEditorAssetManager())
            project->GetEditorAssetManager()->Init();
        AudioMixer::LoadProjectMixer(true);
        if (renderer)
            renderer->InitializeCustomLitShaders();

        const AssetHandle startScene = project ? project->GetConfig().StartScene : AssetHandle(0);
        if (startScene != 0)
        {
            const std::filesystem::path relative = project->GetEditorAssetManager()->GetFilePath(startScene);
            const std::filesystem::path scenePath = Project::GetActiveAssetDirectory() / relative;
            Ref<Scene> loadedScene = CreateRef<Scene>();
            SceneSerializer serializer(loadedScene);
            if (serializer.Deserialize(scenePath))
            {
                m_ActiveScene = loadedScene;
                m_ActiveScenePath = relative;
            }
        }
        else if (hadProject)
        {
            NewScene();
        }

        if (m_CreateBasicScene && project && project->GetConfig().StartScene == 0)
            CreateBasicScene();
        m_CreateBasicScene = false;

        {
            std::error_code error;
            m_ThumbnailPending = !std::filesystem::exists(ProjectLibrary::ThumbnailPath(Project::GetActiveProjectDirectory()), error);
            m_ThumbnailTimer = 0.0f;
        }
        return true;
    }

    bool EditorV2Layer::OpenStartupProject(const std::filesystem::path& failed)
    {
        const std::string message = failed.empty()
            ? std::string("No project is open. Open an existing project (.nproj) or make a new one.")
            : "The project " + failed.generic_string() + " could not be opened (the log says why).\n"
              "Open another project (.nproj) or make a new one.";
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
            box.message = message.c_str();
            box.numbuttons = SDL_arraysize(buttons);
            box.buttons = buttons;
            int choice = 2;
            if (!SDL_ShowMessageBox(&box, &choice) || choice == 2)
                return false;
            if (choice == 0 ? OpenProject() : NewProject())
                return true;
        }
    }

    void EditorV2Layer::OpenProjectManager()
    {
        const char* base = SDL_GetBasePath();
        const ApplicationCommandLineArgs& args = Application::Get().GetSpecification().CommandLineArgs;
        std::filesystem::path fileName = args.Count > 0 && args.Args[0]
            ? std::filesystem::path(args.Args[0]).filename()
            : std::filesystem::path("NoxEditorV2");
#ifdef _WIN32
        if (!fileName.has_extension())
            fileName += ".exe";
#endif
        const std::filesystem::path executable = std::filesystem::path(base ? base : "") / fileName;
        std::vector<std::string> arguments{ "--project-manager" };
        for (int index = 1; index < args.Count; ++index)
        {
            if (ApplicationCommandLineArgs::IsSwitch(args.Args[index]) &&
                std::string_view(args.Args[index]) != "--project-manager")
                arguments.emplace_back(args.Args[index]);
        }
        std::string error;
        if (!LaunchDetachedProcess(executable, arguments, error))
        {
            NOX_CORE_ERROR("Project Manager: cannot start {}: {}", executable.generic_string(), error);
            return;
        }
        NOX_CORE_INFO("Project Manager: started {} --project-manager; this editor closes", executable.generic_string());
        Application::Get().Shutdown();
    }

    void EditorV2Layer::NewScene()
    {
        m_ActiveScene = CreateRef<Scene>();
        m_ActiveScenePath.clear();
    }

    void EditorV2Layer::SaveScene()
    {
        if (!m_ActiveScene || !Project::GetActive())
            return;
        if (m_ActiveScenePath.empty())
        {
            SaveSceneAs();
            return;
        }
        const std::filesystem::path fullPath = m_ActiveScenePath.is_absolute()
            ? m_ActiveScenePath
            : Project::GetActiveAssetDirectory() / m_ActiveScenePath;
        std::error_code error;
        std::filesystem::create_directories(fullPath.parent_path(), error);
        SceneSerializer serializer(m_ActiveScene);
        serializer.Serialize(fullPath);
        AdoptStartSceneIfNone(fullPath);
        NOX_CORE_INFO("Saved scene {}", fullPath.generic_string());
    }

    void EditorV2Layer::SaveSceneAs()
    {
        if (!m_ActiveScene || !Project::GetActive())
            return;
        std::filesystem::path path = Utility::SaveFile("Nox Scene\0nox\0");
        if (path.empty())
            return;
        if (!path.has_extension())
            path += ".nox";
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        SceneSerializer serializer(m_ActiveScene);
        serializer.Serialize(path);

        const std::filesystem::path rel = std::filesystem::relative(path, Project::GetActiveAssetDirectory(), error);
        if (!error && !rel.empty() && !rel.native().starts_with(std::filesystem::path("..").native()))
            m_ActiveScenePath = rel;
        else
            m_ActiveScenePath = path;

        AdoptStartSceneIfNone(path);
        NOX_CORE_INFO("Saved scene {}", path.generic_string());
    }

    void EditorV2Layer::CreateShape(PrimitiveShape shape)
    {
        if (!m_ActiveScene)
            return;
        const AssetHandle mesh = GetShapeAsset(shape);
        if (mesh == 0)
            return;
        Entity entity = m_ActiveScene->CreateEntity(PrimitiveShapes::GetName(shape));
        auto& instance = entity.AddComponent<ModelInstanceComponent>();
        instance.Model = mesh;
        instance.AtFileLayout = true;
        entity.GetComponent<TransformComponent>().Translation =
            m_EditorCamera.GetPosition() + glm::dvec3(m_EditorCamera.GetForwardDirection() * m_EditorCamera.GetDistance());
        entity.MarkTransformDirty();
        AssetManager::RequestAsset(mesh);
    }

    AssetHandle EditorV2Layer::GetShapeAsset(PrimitiveShape shape)
    {
        Ref<Project> project = Project::GetActive();
        if (!project)
            return 0;

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
        settings.StaticCombine = MeshCombineMode::CombineAll;
        settings.ImportScale = WorldUnits::PerSourceUnit(WorldUnits::SourceUnit::Meters);
        if (!assetManager->ImportModel(sourcePath, meshPath, settings))
            return 0;
        return assetManager->FindHandleByPath(meshPath, AssetType::StaticMesh);
    }

    AssetHandle EditorV2Layer::RegisterSceneAsset(const std::filesystem::path& scenePath)
    {
        Ref<Project> project = Project::GetActive();
        if (!project || scenePath.empty())
            return 0;
        auto assets = project->GetEditorAssetManager();
        std::error_code error;
        const std::filesystem::path relative = std::filesystem::relative(scenePath, Project::GetActiveAssetDirectory(), error);
        if (error || relative.empty() || relative.native().starts_with(std::filesystem::path("..").native()))
            return 0;
        AssetHandle handle = assets->FindHandleByPath(relative, AssetType::Scene);
        if (handle == 0)
        {
            assets->ImportAsset(relative, relative, AssetType::Scene);
            handle = assets->FindHandleByPath(relative, AssetType::Scene);
        }
        return handle;
    }

    void EditorV2Layer::AdoptStartSceneIfNone(const std::filesystem::path& scenePath)
    {
        Ref<Project> project = Project::GetActive();
        if (!project || project->GetConfig().StartScene != 0)
            return;
        if (const AssetHandle handle = RegisterSceneAsset(scenePath); handle != 0)
        {
            project->GetConfig().StartScene = handle;
            Project::SaveActive();
            NOX_CORE_INFO("Start Scene: {} (the project opens with it; Settings > Start Scene changes it)", scenePath.filename().string());
        }
    }

    void EditorV2Layer::CreateBasicScene()
    {
        NewScene();
        Scene& scene = *m_ActiveScene;

        const std::string lighting = "Lighting";
        scene.AddFolder(lighting);

        Entity sky = scene.CreateEntity("Sky");
        sky.AddComponent<EnvironmentLightComponent>().Source = EnvironmentSource::ProceduralSky;
        sky.AddComponent<FolderComponent>(lighting);

        Entity sun = scene.CreateEntity("Sun");
        sun.AddComponent<FolderComponent>(lighting);
        sun.AddComponent<DirectionalLightComponent>().Intensity = DefaultSkySunIntensity;
        sun.GetComponent<TransformComponent>().Rotation = glm::eulerAngles(DefaultSkySunRotation());
        sun.MarkTransformDirty();

        if (const AssetHandle plane = GetShapeAsset(PrimitiveShape::Plane); plane != 0)
        {
            Entity floor = scene.CreateEntity("Floor");
            auto& instance = floor.AddComponent<ModelInstanceComponent>();
            instance.Model = plane;
            instance.AtFileLayout = true;
            floor.GetComponent<TransformComponent>().Scale = glm::vec3(50.0f, 1.0f, 50.0f);
            floor.MarkTransformDirty();
            floor.AddComponent<RigidBody3DComponent>().Type = RigidBody3DComponent::BodyType::Static;
            auto& box = floor.AddComponent<BoxCollider3DComponent>();
            box.HalfExtents = glm::vec3(WorldUnits::FromMeters(0.5f), WorldUnits::FromMeters(0.05f), WorldUnits::FromMeters(0.5f));
            box.Offset = glm::vec3(0.0f, -WorldUnits::FromMeters(0.05f), 0.0f);
            AssetManager::RequestAsset(plane);
        }

        Entity camera = scene.CreateEntity("Camera");
        camera.AddComponent<CameraComponent>().Primary = true;
        auto& cameraTransform = camera.GetComponent<TransformComponent>();
        cameraTransform.Translation = glm::dvec3(0.0, WorldUnits::FromMeters(2.0f), WorldUnits::FromMeters(8.0f));
        cameraTransform.Rotation = glm::vec3(glm::radians(-12.0f), 0.0f, 0.0f);
        camera.MarkTransformDirty();

        const std::filesystem::path path = Project::GetActiveAssetDirectory() / "Scenes" / "Main.nox";
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        {
            SceneSerializer serializer(m_ActiveScene);
            serializer.Serialize(path);
        }
        m_ActiveScenePath = "Scenes/Main.nox";
        AdoptStartSceneIfNone(path);
        m_ApplyBasicSceneView = true;
        ApplyBasicSceneView();
    }

    void EditorV2Layer::ApplyBasicSceneView()
    {
        m_EditorCamera.SetFocalPoint(glm::dvec3(0.0));
        m_EditorCamera.SetDistance(WorldUnits::FromMeters(15.0f));
        m_EditorCamera.SetOrientation(glm::radians(25.0f), glm::radians(35.0f));
    }

    void EditorV2Layer::CaptureProjectThumbnail()
    {
        Renderer* renderer = Application::Get().GetRenderer();
        Ref<Project> project = Project::GetActive();
        Ref<Texture2D> image = renderer ? renderer->GetSceneResourceRef() : nullptr;
        if (!project || !image || image->GetWidth() < 16 || image->GetHeight() < 16)
            return;
        const std::filesystem::path file = ProjectLibrary::ThumbnailPath(Project::GetActiveProjectDirectory());
        const bool bgra = renderer->IsSurfaceFormatBGRA();
        renderer->ReadbackTexture(std::move(image), [file, bgra](std::vector<uint8_t> pixels, uint32_t width, uint32_t height)
        {
            if (pixels.size() != static_cast<size_t>(width) * height * 4 || width == 0 || height == 0)
                return;
            constexpr uint32_t outWidth = 384, outHeight = 216;
            uint32_t cropWidth = width, cropHeight = static_cast<uint32_t>(width * 9ull / 16ull);
            if (cropHeight > height)
            {
                cropHeight = height;
                cropWidth = static_cast<uint32_t>(height * 16ull / 9ull);
            }
            const uint32_t x0 = (width - cropWidth) / 2, y0 = (height - cropHeight) / 2;
            std::vector<uint8_t> out(static_cast<size_t>(outWidth) * outHeight * 4);
            for (uint32_t y = 0; y < outHeight; ++y)
            {
                for (uint32_t x = 0; x < outWidth; ++x)
                {
                    const uint32_t sx0 = x0 + x * cropWidth / outWidth, sx1 = std::max(sx0 + 1, x0 + (x + 1) * cropWidth / outWidth);
                    const uint32_t sy0 = y0 + y * cropHeight / outHeight, sy1 = std::max(sy0 + 1, y0 + (y + 1) * cropHeight / outHeight);
                    uint32_t sum[3] = { 0, 0, 0 }, count = 0;
                    for (uint32_t sy = sy0; sy < sy1 && sy < height; ++sy)
                    {
                        for (uint32_t sx = sx0; sx < sx1 && sx < width; ++sx)
                        {
                            const uint8_t* texel = &pixels[(static_cast<size_t>(sy) * width + sx) * 4];
                            sum[0] += texel[0];
                            sum[1] += texel[1];
                            sum[2] += texel[2];
                            ++count;
                        }
                    }
                    uint8_t* target = &out[(static_cast<size_t>(y) * outWidth + x) * 4];
                    count = std::max(count, 1u);
                    target[0] = static_cast<uint8_t>(sum[bgra ? 2 : 0] / count);
                    target[1] = static_cast<uint8_t>(sum[1] / count);
                    target[2] = static_cast<uint8_t>(sum[bgra ? 0 : 2] / count);
                    target[3] = 255;
                }
            }
            std::error_code error;
            std::filesystem::create_directories(file.parent_path(), error);
            if (!stbi_write_png(file.string().c_str(), outWidth, outHeight, 4, out.data(), outWidth * 4))
                NOX_CORE_WARN("Project thumbnail: cannot write {}", file.generic_string());
        });
    }

    void EditorV2Layer::OnUpdate(Timestep ts)
    {
        if (!Project::GetActive())
            return;

        if (m_ThumbnailPending)
        {
            m_ThumbnailTimer += ts;
            if (m_ThumbnailTimer > 4.0f)
            {
                m_ThumbnailPending = false;
                CaptureProjectThumbnail();
            }
        }

        if (Project::GetActive()->GetEditorAssetManager())
            Project::GetActive()->GetEditorAssetManager()->Update();

        Renderer* renderer = Application::Get().GetRenderer();
        if (!renderer || !m_ActiveScene || !m_ViewportImage)
            return;

        const UI::UIRect& bounds = m_ViewportImage->GetBounds();
        const uint32_t wantW = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Width())));
        const uint32_t wantH = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Height())));
        if (wantW > 0 && wantH > 0)
        {
            m_ActiveScene->OnViewportResize(wantW, wantH);
            const NRI::Extent2D currentSize = renderer->getViewPortSize();
            if (wantW != currentSize.width || wantH != currentSize.height)
            {
                renderer->onViewportSizeChange({wantW, wantH});
                m_EditorCamera.SetViewportSize(static_cast<float>(wantW), static_cast<float>(wantH));
            }
        }

        m_ActiveScene->SetRenderer(renderer);
        m_ActiveScene->SetRenderer2D(renderer->getRenderer2D());

        const bool hovered = UI::HasState(m_ViewportImage->GetState(), UI::UIState::Hovered);
        m_EditorCamera.OnUpdate(ts, hovered || m_ViewportMouseCaptured);
        m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);

        m_FpsAccumulator += ts;
        ++m_FpsFrames;
        if (m_FpsAccumulator >= 0.25f)
        {
            if (m_FpsLabel)
            {
                char fpsBuf[32];
                const float fps = static_cast<float>(m_FpsFrames) / m_FpsAccumulator;
                std::snprintf(fpsBuf, sizeof(fpsBuf), "FPS: %.1f", fps);
                m_FpsLabel->Text = fpsBuf;
            }
            m_FpsAccumulator = 0.0f;
            m_FpsFrames = 0;
        }

        Texture2D* sceneTex = renderer->GetSceneResource();
        m_ViewportImage->TextureDescriptorSlot = sceneTex ? sceneTex->GetDescriptorIndexSlot() : ~0u;
    }

    void EditorV2Layer::OnEvent(Event& event)
    {
        if (event.GetEventType() == EventType::KeyPressed)
        {
            const auto& key = static_cast<const KeyPressedEvent&>(event);
            if (!key.IsRepeat())
            {
                const bool control = (key.GetModifiers() & SDL_KMOD_CTRL) != 0 ||
                                     Input::IsKeyPressed(SDL_SCANCODE_LCTRL) ||
                                     Input::IsKeyPressed(SDL_SCANCODE_RCTRL);
                const bool shift = (key.GetModifiers() & SDL_KMOD_SHIFT) != 0 ||
                                   Input::IsKeyPressed(SDL_SCANCODE_LSHIFT) ||
                                   Input::IsKeyPressed(SDL_SCANCODE_RSHIFT);

                int keyCode = key.GetKeyCode();
                const SDL_Keycode layoutKey = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(keyCode), SDL_KMOD_NONE, false);
                if (layoutKey >= SDLK_A && layoutKey <= SDLK_Z)
                    keyCode = SDL_SCANCODE_A + static_cast<int>(layoutKey - SDLK_A);

                if (control && keyCode == SDL_SCANCODE_N)
                {
                    NewScene();
                    event.Handled = true;
                    return;
                }
                if (control && keyCode == SDL_SCANCODE_O)
                {
                    OpenProject();
                    event.Handled = true;
                    return;
                }
                if (control && keyCode == SDL_SCANCODE_S)
                {
                    if (shift)
                        SaveSceneAs();
                    else
                        SaveScene();
                    event.Handled = true;
                    return;
                }
                if (control && keyCode == SDL_SCANCODE_R)
                {
                    if (ScriptEngine::IsInitialized())
                        ScriptEngine::ReloadAssembly();
                    event.Handled = true;
                    return;
                }
            }
        }

        if (!m_ViewportImage)
            return;

        if (event.GetEventType() == EventType::MouseScrolled &&
            UI::HasState(m_ViewportImage->GetState(), UI::UIState::Hovered))
        {
            m_EditorCamera.OnEvent(event);
        }
        else if (event.GetEventType() == EventType::MouseButtonReleased)
        {
            m_ViewportMouseCaptured = false;
        }
    }

    void EditorV2Layer::BuildUI(bool loadSavedLayout)
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer)
            return;

        if (const auto& dock = uiLayer->GetDockSpaceRef())
            dock->ClearPanels();

        m_ViewportImage = UI::MakeUI<UI::UIImage>();
        m_ViewportImage->WidthMode = UI::UISizeMode::Fill;
        m_ViewportImage->HeightMode = UI::UISizeMode::Fill;
        m_ViewportImage->HitTestMode = UI::UIHitTestMode::Self;
        m_ViewportImage->Focusable = true;
        m_ViewportImage->OnMouseDown = [this](const UI::UIPointerEvent&)
        {
            m_ViewportMouseCaptured = true;
            return true;
        };
        m_ViewportImage->OnMouseUp = [this](const UI::UIPointerEvent&)
        {
            m_ViewportMouseCaptured = false;
            return false;
        };

        uiLayer->AddPanel(
            "Viewport",
            m_ViewportImage,
            UI::UIDockDropZone::Center,
            {},
            0.5f,
            UI::UIDockNodeFlags::CentralNode
        );

        m_StatusLabel = UI::Label("Ready").Margin(10.0f, 4.0f);

        if (loadSavedLayout)
        {
            if (const auto& dock = uiLayer->GetDockSpaceRef())
                dock->LoadLayout(kLayoutPath);
        }

        auto menuBar = UI::MakeUI<UI::UIMenuBar>();
        menuBar->AddDynamicMenu("File", [this, uiLayer]() -> std::vector<UI::UIMenuItem>
        {
            return {
                {"New Project...", "", [this]() { NewProject(); }},
                {"Open Project...", "Ctrl+O", [this]() { OpenProject(); }},
                {"Project Manager...", "", [this]() { OpenProjectManager(); }, false, true, false, false,
                 "Close this project and show the Project Manager (save the scene first: like Exit, this does not ask)."},
                {"", "", {}, true},
                {"New Scene", "Ctrl+N", [this]() { NewScene(); }},
                {"Save Scene", "Ctrl+S", [this]() { SaveScene(); }},
                {"Save Scene As...", "Ctrl+Shift+S", [this]() { SaveSceneAs(); }},
                {"", "", {}, true},
                {"Package Game...", "", {}, false, false, false, false, "Packaging UI not yet ported to NoxEditorV2"},
                {"Package Server...", "", {}, false, false, false, false, "Packaging UI not yet ported to NoxEditorV2"},
                {"", "", {}, true},
                {"Save Layout", "", [uiLayer]()
                {
                    if (const auto& dock = uiLayer->GetDockSpaceRef())
                        dock->SaveLayout(kLayoutPath);
                }},
                {"Exit", "Alt+F4", []() { Application::Get().Shutdown(); }}
            };
        });

        menuBar->AddDynamicMenu("Edit", []() -> std::vector<UI::UIMenuItem>
        {
            return {
                {"Undo", "Ctrl+Z", {}, false, false},
                {"Redo", "Ctrl+Y / Ctrl+Shift+Z", {}, false, false},
                {"", "", {}, true},
                {"History: 0 steps, 0.0 MB", "", {}, false, false, false, true}
            };
        });

        menuBar->AddDynamicMenu("Create", [this]() -> std::vector<UI::UIMenuItem>
        {
            std::vector<UI::UIMenuItem> shapeItems;
            for (uint8_t index = 0; index < static_cast<uint8_t>(PrimitiveShape::Count); ++index)
            {
                const PrimitiveShape shape = static_cast<PrimitiveShape>(index);
                shapeItems.push_back({PrimitiveShapes::GetName(shape), "", [this, shape]() { CreateShape(shape); }});
            }

            return {
                {"Empty Entity", "", [this]() { if (m_ActiveScene) m_ActiveScene->CreateEntity("Empty Entity"); }},
                {"Shapes", "", {}, false, true, false, false, "", std::move(shapeItems)},
                {"", "", {}, true},
                {"Streaming Test World...", "", {}, false, false, false, false,
                 "A world partition larger than RAM and VRAM (panel not yet ported to NoxEditorV2)."}
            };
        });

#if NOX_WITH_MCP
        menuBar->AddDynamicMenu("Tools", []() -> std::vector<UI::UIMenuItem>
        {
            return {
                {"MCP Server (not yet wired in V2)", "", {}, false, false, false, true}
            };
        });
#endif

        menuBar->AddDynamicMenu("Window", [this, uiLayer]() -> std::vector<UI::UIMenuItem>
        {
            std::vector<UI::UIMenuItem> viewportItems = {
                {"1 Viewport", "", {}, false, false},
                {"2 Side by Side", "", {}, false, false},
                {"2 Stacked", "", {}, false, false},
                {"3 Viewports", "", {}, false, false},
                {"4 Viewports (2 x 2)", "", {}, false, false},
                {"", "", {}, true},
                {"Viewport 2", "", {}, false, false},
                {"Viewport 3", "", {}, false, false},
                {"Viewport 4", "", {}, false, false},
                {"", "", {}, true},
                {"Click a viewport to use it", "", {}, false, false, false, true}
            };

            std::vector<UI::UIMenuItem> animGraphItems;
            if (Ref<Project> project = Project::GetActive(); project && project->GetEditorAssetManager())
            {
                for (const auto& [handle, metadata] : project->GetEditorAssetManager()->GetAssetRegistry())
                {
                    if (metadata.Type != AssetType::AnimationGraph)
                        continue;
                    animGraphItems.push_back({metadata.FilePath.stem().string(), "", {}, false, false, false, false,
                                              "Graph editor panel not yet ported to NoxEditorV2"});
                }
            }
            if (animGraphItems.empty())
                animGraphItems.push_back({"No .nanimgraph assets found", "", {}, false, false, false, true});

            return {
                {"Render Graph", "", {}, false, false, false, false, "Panel not yet ported to NoxEditorV2"},
                {"Output Log", "", {}, false, false, false, false, "Panel not yet ported to NoxEditorV2"},
                {"Profiler", "", {}, false, false, false, false, "Panel not yet ported to NoxEditorV2"},
                {"World Partition", "", {}, false, false, false, false, "Panel not yet ported to NoxEditorV2"},
                {"Levels", "", {}, false, false, false, false, "Panel not yet ported to NoxEditorV2"},
                {"Mixer", "", {}, false, false, false, false, "Panel not yet ported to NoxEditorV2"},
                {"Viewports", "", {}, false, true, false, false, "", std::move(viewportItems)},
                {"Animation Graphs", "", {}, false, true, false, false, "", std::move(animGraphItems)},
                {"", "", {}, true},
                {"Reset Layout", "", [this]() { BuildUI(false); }},
                {"Reload Theme", "", [uiLayer]() { uiLayer->ReloadTheme(); }}
            };
        });

        menuBar->AddDynamicMenu("Script", [this]() -> std::vector<UI::UIMenuItem>
        {
            const bool waitForDbg = ScriptEngine::GetWaitForDebugger();
            const bool canReload = ScriptEngine::IsInitialized();
            return {
                {"Build and Reload", "Ctrl+B", {}, false, false, false, false, "ScriptBuilder not yet ported to NoxEditorV2"},
                {"Reload assembly", "Ctrl+R", []() { ScriptEngine::ReloadAssembly(); }, false, canReload},
                {"Build on Save", "", [this]() { m_BuildScriptsOnSave = !m_BuildScriptsOnSave; }, false, true, m_BuildScriptsOnSave},
                {"Wait for Debugger on Play", "", [waitForDbg]() { ScriptEngine::SetWaitForDebugger(!waitForDbg); }, false, true, waitForDbg, false,
                 "Play waits (up to 60 s) until a .NET debugger attaches to the editor, then breaks once."}
            };
        });

        auto spacer = UI::MakeUI<UI::UIElement>();
        spacer->WidthMode = UI::UISizeMode::Fill;
        menuBar->AddChild(spacer);

        Renderer* renderer = Application::Get().GetRenderer();
        auto vsyncBox = UI::CheckBox("VSync", renderer ? renderer->getVSync() : false, [renderer](bool enabled)
        {
            if (renderer)
                renderer->setVSync(enabled);
        }).VAlign(UI::UIAlignment::Center).Margin(6.0f, 0.0f);
        menuBar->AddChild(vsyncBox);

        m_FpsLabel = UI::Label("FPS: --.-").VAlign(UI::UIAlignment::Center).Margin(10.0f, 0.0f);
        m_FpsLabel->UseColorOverride = true;
        m_FpsLabel->ColorOverride = { 0.72f, 0.76f, 0.82f, 1.0f };
        menuBar->AddChild(m_FpsLabel);

        uiLayer->SetRoot(
            UI::VStack(0.0f, {
                           menuBar,
                           uiLayer->GetDockSpaceRef(),
                           m_StatusLabel
                       })
            .Padding(0.0f)
            .Style("Background")
            .Background(true)
        );
    }
}
