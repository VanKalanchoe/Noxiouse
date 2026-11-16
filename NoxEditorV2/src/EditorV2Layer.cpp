#include "EditorV2Layer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <unordered_set>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_video.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <stb_image_write.h>
#include "json.hpp"

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/SceneImporter.h"
#include "NoxCore/Audio/AudioMixer.h"
#include "NoxCore/Console/Console.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Input.h"
#include "NoxCore/Core/Window.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Network/NetReplication.h"
#include "NoxCore/Network/NetSession.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/CustomLitShaders.h"
#include "NoxCore/Renderer/MaterialGraph.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/ModelInstance.h"
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

        std::filesystem::path EditorCamerasFile()
        {
            return Project::GetProjectDirectory() / "Cache" / "EditorCameras.txt";
        }

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

    EditorV2Layer::EditorV2Layer(std::filesystem::path projectFile, bool createBasicScene)
        : Layer("EditorV2Layer")
        , m_EditorCamera(30.0f, 1.778f, 0.01f, 1000.0f)
        , m_CreateBasicScene(createBasicScene)
    {
        Application& app = Application::Get();
        m_Renderer = app.GetRenderer();
        m_Renderer2D = m_Renderer ? m_Renderer->getRenderer2D() : nullptr;

        m_EditorScene = CreateRef<Scene>();
        m_ActiveScene = m_EditorScene;

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

        if (!ScriptEngine::Initialize(SDL_GetBasePath(), true) && !ScriptEngine::IsInitialized())
            NOX_CORE_ERROR("Failed to initialize the .NET scripting backend");
        BuildScriptsIfNeverBuilt();

        Net::RegisterConsoleVariables();
        if (const char* exec = app.GetSpecification().CommandLineArgs.Value("--exec"))
            Console::Get().Execute(exec);

        BuildUI(true);
    }

    EditorV2Layer::~EditorV2Layer()
    {
        SaveEditorCameras();
        ScriptEngine::Shutdown();

        if (UILayer* uiLayer = Application::Get().GetUILayer())
        {
            if (const auto& dock = uiLayer->GetDockSpaceRef())
                dock->SaveLayout(kLayoutPath);
        }
        if (Project::GetActive() && Project::GetActive()->GetEditorAssetManager())
            Project::GetActive()->GetEditorAssetManager()->Shutdown();
    }

    void EditorV2Layer::BuildScriptsIfNeverBuilt()
    {
        if (!ScriptEngine::IsInitialized() || !Project::GetActive() || ScriptEngine::GetProjectScriptsProject().empty())
            return;
        std::filesystem::path core, game;
        if (!ScriptEngine::FindAssemblies(core, game))
            m_ScriptBuilder.RequestBuild();
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

        Ref<Project> project = Project::GetActive();
        ProjectLibrary::RememberRecentProject(path);
        if (auto win = Application::Get().getWindow(); win && win->getHandle() && project)
            SDL_SetWindowTitle(win->getHandle(), ("Nox Editor - " + project->GetConfig().Name).c_str());

        m_EditorCamera.ApplyWorldUnitDefaults();
        if (m_Renderer && project)
        {
            m_Renderer->ApplyQualitySetting(project->GetConfig().RenderQuality);
            m_Renderer->ApplyProjectRendererSettings(project->GetConfig().Renderer);
        }
        if (project && project->GetEditorAssetManager())
            project->GetEditorAssetManager()->Init();
        AudioMixer::LoadProjectMixer(true);
        if (m_Renderer)
            m_Renderer->InitializeCustomLitShaders();

        m_CustomLitSignature = CustomLitSignature();
        if (!m_CustomLitSignature.empty())
            WriteMaterialShaderEditorSettings();

        m_ScriptBuilder.SetProject(ScriptEngine::GetProjectScriptsProject());
        if (ScriptEngine::IsInitialized())
        {
            ScriptEngine::LoadProjectAssembly();
            BuildScriptsIfNeverBuilt();
        }

        const AssetHandle startScene = project ? project->GetConfig().StartScene : AssetHandle(0);
        if (startScene != 0)
            OpenScene(startScene);
        else if (hadProject)
            NewScene();

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
        if (m_SceneState != SceneState::Edit)
            OnSceneStop();
        SaveEditorCameras();
        m_EditorScene = CreateRef<Scene>();
        m_ActiveScene = m_EditorScene;
        m_EditorScenePath.clear();
        m_UnloadUnusedAssetsRequested = true;
#if NOX_PROFILE_STATS
        Profiler::Get().ResetStats();
#endif
    }

    void EditorV2Layer::OpenScene(AssetHandle handle)
    {
        if (handle == 0 || !Project::GetActive())
            return;
        if (m_SceneState != SceneState::Edit)
            OnSceneStop();
        const std::filesystem::path rel = Project::GetActive()->GetEditorAssetManager()->GetFilePath(handle);
        if (rel.empty())
            return;
        m_SceneLoad = CreateScope<SceneLoader>(Project::GetActiveAssetDirectory() / rel);
        m_SceneLoadHandle = handle;
    }

    void EditorV2Layer::FinishSceneLoad()
    {
        Scope<SceneLoader> load = std::move(m_SceneLoad);
        const AssetHandle handle = std::exchange(m_SceneLoadHandle, AssetHandle(0));
        if (!load || load->Failed() || !Project::GetActive())
            return;
        if (m_SceneState != SceneState::Edit)
            OnSceneStop();

        Ref<Scene> newScene = load->GetScene();
        newScene->LoadSubLevelsForEditing();
        if (m_ViewportImage)
        {
            const UI::UIRect& bounds = m_ViewportImage->GetBounds();
            const uint32_t w = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Width())));
            const uint32_t h = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Height())));
            if (w > 0 && h > 0)
                newScene->OnViewportResize(w, h);
        }

        SaveEditorCameras();
        m_EditorScene = newScene;
        m_ActiveScene = m_EditorScene;
        m_EditorScenePath = Project::GetActive()->GetEditorAssetManager()->GetFilePath(handle);

        const bool cameraRestored = RestoreEditorCameras();
        if (!cameraRestored && !m_FocusOnOpen && m_EditorScene->IsWorldPartitioned() && !m_EditorScene->GetWorldPartition().Cells.empty())
        {
            glm::dvec3 lowest(std::numeric_limits<double>::max()), highest(std::numeric_limits<double>::lowest());
            for (const auto& [coord, cell] : m_EditorScene->GetWorldPartition().Cells)
            {
                lowest = glm::min(lowest, cell.BoundsMin);
                highest = glm::max(highest, cell.BoundsMax);
            }
            m_FocusOnOpen = (lowest + highest) * 0.5;
        }
        if (m_FocusOnOpen)
        {
            m_EditorCamera.SetFocalPoint(*m_FocusOnOpen);
            m_EditorCamera.SetDistance(WorldUnits::FromMeters(180.0f));
            m_EditorCamera.SetOrientation(0.45f, 0.0f);
            m_FocusOnOpen.reset();
        }
        if (m_EditorScene->IsWorldPartitioned())
        {
            m_EditorScene->LoadWorldCells(
                m_EditorScene->WorldCellsAround(
                    m_EditorCamera.GetFocalPoint(),
                    WorldUnits::FromMeters(m_EditorScene->GetWorldPartition().Settings.LoadingRange)
                )
            );
        }
        m_UnloadUnusedAssetsRequested = true;
#if NOX_PROFILE_STATS
        Profiler::Get().ResetStats();
#endif
    }

    void EditorV2Layer::SaveScene()
    {
        if (!m_ActiveScene || !Project::GetActive())
            return;
        if (AudioMixer::IsDirty())
            AudioMixer::Save();
        if (m_EditorScenePath.empty())
        {
            SaveSceneAs();
            return;
        }
        const std::filesystem::path fullPath = m_EditorScenePath.is_absolute()
            ? m_EditorScenePath
            : Project::GetActiveAssetDirectory() / m_EditorScenePath;
        std::error_code error;
        std::filesystem::create_directories(fullPath.parent_path(), error);
        SerializeScene(m_ActiveScene, fullPath);
        AdoptStartSceneIfNone(fullPath);
        CaptureProjectThumbnail();
    }

    void EditorV2Layer::SaveSceneAs()
    {
        if (!m_ActiveScene || !Project::GetActive())
            return;
        std::filesystem::path path = Utility::SaveFile("Nox Scene *.nox\0nox\0");
        if (path.empty())
            return;
        if (!path.has_extension())
            path += ".nox";
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        m_ActiveScene->CopyWorldCellsTo(path);
        SerializeScene(m_ActiveScene, path);

        const std::filesystem::path rel = std::filesystem::relative(path, Project::GetActiveAssetDirectory(), error);
        if (!error && !rel.empty() && !rel.native().starts_with(std::filesystem::path("..").native()))
            m_EditorScenePath = rel;
        else
            m_EditorScenePath = path;

        AdoptStartSceneIfNone(path);
        CaptureProjectThumbnail();
    }

    void EditorV2Layer::SerializeScene(const Ref<Scene>& scene, const std::filesystem::path& path)
    {
        SceneImporter::SaveScene(scene, path);
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
        m_HistoryCheckRequested = true;
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
        Scene& scene = *m_EditorScene;

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
        SerializeScene(m_EditorScene, path);
        m_EditorScenePath = path;
        AdoptStartSceneIfNone(path);
        m_HistoryCheckRequested = true;
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
        Ref<Project> project = Project::GetActive();
        Ref<Texture2D> image = m_Renderer ? m_Renderer->GetSceneResourceRef() : nullptr;
        if (!project || !image || image->GetWidth() < 16 || image->GetHeight() < 16)
            return;
        const std::filesystem::path file = ProjectLibrary::ThumbnailPath(Project::GetActiveProjectDirectory());
        const bool bgra = m_Renderer->IsSurfaceFormatBGRA();
        m_Renderer->ReadbackTexture(std::move(image), [file, bgra](std::vector<uint8_t> pixels, uint32_t width, uint32_t height)
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

    void EditorV2Layer::SaveEditorCameras()
    {
        if (!Project::GetActive() || m_EditorScenePath.empty())
            return;
        const std::string scene = m_EditorScenePath.generic_string();
        std::vector<std::string> lines = ReadEditorCameraLines();
        std::erase_if(lines, [&](const std::string& line) { return line.rfind(scene + "|", 0) == 0; });
        const glm::dvec3 focal = m_EditorCamera.GetFocalPoint();
        char line[512];
        std::snprintf(line, sizeof(line), "%s|0|%.17g %.17g %.17g %.6g %.6g %.6g %d %.6g",
                      scene.c_str(), focal.x, focal.y, focal.z,
                      m_EditorCamera.GetDistance(), m_EditorCamera.GetPitch(), m_EditorCamera.GetYaw(),
                      static_cast<int>(m_EditorCamera.GetViewMode()), m_EditorCamera.GetOrthoHalfHeight());
        lines.push_back(line);
        std::error_code error;
        std::filesystem::create_directories(EditorCamerasFile().parent_path(), error);
        std::ofstream out(EditorCamerasFile(), std::ios::trunc);
        for (const std::string& entry : lines)
            out << entry << "\n";
    }

    bool EditorV2Layer::RestoreEditorCameras()
    {
        if (!Project::GetActive() || m_EditorScenePath.empty())
            return false;
        const std::string prefix = m_EditorScenePath.generic_string() + "|";
        for (const std::string& line : ReadEditorCameraLines())
        {
            if (line.rfind(prefix, 0) != 0)
                continue;
            int index = -1, mode = 0;
            glm::dvec3 focal(0.0);
            float distance = 0.0f, pitch = 0.0f, yaw = 0.0f, orthoHalfHeight = 10.0f;
            if (sscanf(line.c_str() + prefix.size(), "%d|%lf %lf %lf %f %f %f %d %f",
                       &index, &focal.x, &focal.y, &focal.z, &distance, &pitch, &yaw,
                       &mode, &orthoHalfHeight) != 9 || index != 0 ||
                mode < 0 || mode > static_cast<int>(EditorViewMode::Right))
                continue;
            m_EditorCamera.SetViewMode(static_cast<EditorViewMode>(mode));
            m_EditorCamera.SetFocalPoint(focal);
            m_EditorCamera.SetDistance(distance);
            m_EditorCamera.SetOrientation(pitch, yaw);
            if (m_EditorCamera.IsOrthographic())
                m_EditorCamera.SetOrthoHalfHeight(orthoHalfHeight);
            return true;
        }
        return false;
    }

    void EditorV2Layer::WriteMaterialShaderEditorSettings()
    {
        const std::filesystem::path projectDirectory = Project::GetActiveProjectDirectory();
        const std::filesystem::path shaderFolder = std::filesystem::absolute("assets/shaders").lexically_normal();
        CustomLitShaders::WriteCodeEditorConfig(projectDirectory, shaderFolder);

        const std::filesystem::path settingsPath = projectDirectory / ".vscode" / "settings.json";
        const std::string folder = shaderFolder.generic_string();
        nlohmann::json settings = nlohmann::json::object();
        std::error_code error;
        if (std::filesystem::exists(settingsPath, error))
        {
            std::ifstream file(settingsPath, std::ios::binary);
            std::stringstream text;
            text << file.rdbuf();
            settings = nlohmann::json::parse(text.str(), nullptr, false);
            if (settings.is_discarded() || !settings.is_object())
                return;
        }
        nlohmann::json& paths = settings["slang.additionalSearchPaths"];
        if (!paths.is_array())
            paths = nlohmann::json::array();
        for (const nlohmann::json& path : paths)
        {
            if (path.is_string() && path.get<std::string>() == folder)
                return;
        }
        paths.push_back(folder);
        std::filesystem::create_directories(settingsPath.parent_path(), error);
        std::ofstream out(settingsPath, std::ios::binary | std::ios::trunc);
        out << settings.dump(4) << "\n";
    }

    std::string EditorV2Layer::CustomLitSignature() const
    {
        std::string signature;
        const Ref<Project> project = Project::GetActive();
        if (!project)
            return signature;
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        for (const auto& [handle, metadata] : project->GetEditorAssetManager()->GetAssetRegistry())
        {
            if (metadata.Type != AssetType::MaterialShader && metadata.Type != AssetType::MaterialGraph)
                continue;
            std::error_code error;
            const std::filesystem::path file = assetDirectory / metadata.FilePath;
            const auto written = std::filesystem::last_write_time(file, error);
            const auto size = error ? 0 : std::filesystem::file_size(file, error);
            signature += metadata.FilePath.generic_string() + '|' +
                         std::to_string(error ? 0 : written.time_since_epoch().count()) + '|' +
                         std::to_string(size) + '\n';
        }
        return signature;
    }

    void EditorV2Layer::PollCustomLitShaders()
    {
        if (!m_Renderer || !Project::GetActive())
            return;
        std::string signature = CustomLitSignature();
        if (signature == m_CustomLitSignature)
            return;
        m_CustomLitSignature = std::move(signature);
        bool complete = true;
        const std::vector<CustomLitSource> sources = CustomLitShaders::GatherProjectSources(complete);
        if (!sources.empty())
            WriteMaterialShaderEditorSettings();
        bool graphsComplete = true;
        const std::vector<ProjectMaterialGraph> graphs = GatherProjectMaterialGraphs(graphsComplete);
        NOX_CORE_INFO("Custom Lit: Material Shaders or material graphs changed, building them again");
        m_Renderer->SetCustomLitShaders(sources, graphs);
    }

    void EditorV2Layer::UpdateHistory()
    {
        if (m_SceneState != SceneState::Edit || !m_ActiveScene || !Project::GetActive())
            return;
        EditorHistory::FrameInfo frame;
        frame.InteractionEnded = m_HistoryCheckRequested;
        m_HistoryCheckRequested = false;
        frame.AssetsLoading = Project::GetActive()->GetEditorAssetManager()->GetLoadingCount() > 0;
        m_History.Update(m_ActiveScene, frame);
    }

    void EditorV2Layer::Undo()
    {
        if (m_SceneState != SceneState::Edit || !m_History.CanUndo())
            return;
        const std::string name = m_History.GetUndoName();
        RestoreHistoryState(m_History.StepBack());
        NOX_CORE_INFO("Undo: {}", name);
    }

    void EditorV2Layer::Redo()
    {
        if (m_SceneState != SceneState::Edit || !m_History.CanRedo())
            return;
        const std::string name = m_History.GetRedoName();
        RestoreHistoryState(m_History.StepForward());
        NOX_CORE_INFO("Redo: {}", name);
    }

    void EditorV2Layer::RestoreHistoryState(const std::string& text)
    {
        NOX_PROFILE_SCOPE("Undo Restore");
        Ref<Scene> restored = CreateRef<Scene>();
        SceneSerializer serializer(restored);
        if (!serializer.DeserializeText(text))
        {
            NOX_CORE_ERROR("Undo / redo: the history state could not be read; the scene is left as it is");
            return;
        }

        if (m_ViewportImage)
        {
            const UI::UIRect& bounds = m_ViewportImage->GetBounds();
            const uint32_t w = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Width())));
            const uint32_t h = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Height())));
            if (w > 0 && h > 0)
                restored->OnViewportResize(w, h);
        }
        restored->AdoptWorldState(*m_EditorScene);
        m_EditorScene = restored;
        m_ActiveScene = restored;
        m_UnloadUnusedAssetsRequested = true;
        m_History.OnSceneRestored(restored);
    }

    void EditorV2Layer::UnloadUnusedAssets()
    {
        if (!Project::GetActive() || !Project::GetActive()->GetEditorAssetManager())
            return;
        std::unordered_set<AssetHandle> referencedAssets;
        if (m_EditorScene && m_ActiveScene != m_EditorScene)
        {
            if (!m_EditorSceneReferencesValid)
            {
                m_EditorSceneReferences.clear();
                m_EditorScene->CollectAssetReferences(m_EditorSceneReferences);
                m_EditorSceneReferencesValid = true;
            }
            referencedAssets = m_EditorSceneReferences;
        }
        else if (m_EditorScene)
        {
            m_EditorScene->CollectAssetReferences(referencedAssets);
            m_EditorSceneReferencesValid = false;
        }
        if (m_ActiveScene && m_ActiveScene != m_EditorScene)
            m_ActiveScene->CollectAssetReferences(referencedAssets);
        Project::GetActive()->GetEditorAssetManager()->UnloadUnusedAssets(referencedAssets);
    }

    void EditorV2Layer::OnScenePlay()
    {
        m_EditorSceneReferencesValid = false;
        SetViewMode(EditorViewMode::Perspective);
        if (m_SceneState == SceneState::Simulate)
            OnSceneStop();
        m_SceneState = SceneState::Play;
        m_ActiveScene = Scene::Copy(m_EditorScene);
        m_ActiveScene->SetRenderer(m_Renderer);
        m_ActiveScene->SetRenderer2D(m_Renderer2D);
        if (m_ViewportImage)
        {
            const UI::UIRect& bounds = m_ViewportImage->GetBounds();
            const uint32_t w = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Width())));
            const uint32_t h = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Height())));
            if (w > 0 && h > 0)
                m_ActiveScene->OnViewportResize(w, h);
        }
        m_ActiveScene->OnRuntimeStart();
    }

    void EditorV2Layer::OnSceneSimulate()
    {
        m_EditorSceneReferencesValid = false;
        if (m_SceneState == SceneState::Play)
            OnSceneStop();
        m_SceneState = SceneState::Simulate;
        m_ActiveScene = Scene::Copy(m_EditorScene);
        const glm::dvec3 streamingSource = m_EditorCamera.GetPosition();
        m_ActiveScene->OnSimulationStart(&streamingSource);
    }

    void EditorV2Layer::OnSceneStop()
    {
        if (m_SceneState == SceneState::Play)
            m_ActiveScene->OnRuntimeStop();
        else if (m_SceneState == SceneState::Simulate)
            m_ActiveScene->OnSimulationStop();

        m_SceneState = SceneState::Edit;
        if (Input::IsRelativeMouseMode())
            Input::SetRelativeMouseMode(false);
        Input::ClearInjected();
        m_ActiveScene = m_EditorScene;
        m_UnloadUnusedAssetsRequested = true;
    }

    void EditorV2Layer::OnScenePause()
    {
        if (m_SceneState != SceneState::Edit && m_ActiveScene)
            m_ActiveScene->SetPaused(true);
    }

    int EditorV2Layer::GridPlaneAxis() const
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

    void EditorV2Layer::SetViewMode(EditorViewMode mode)
    {
        const bool wasOrthographic = m_EditorCamera.IsOrthographic();
        m_EditorCamera.SetViewMode(mode);
        SyncOrthoRenderSettings(wasOrthographic);
    }

    void EditorV2Layer::SyncOrthoRenderSettings(bool wasOrthographic)
    {
        if (!m_Renderer)
            return;
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

    void EditorV2Layer::DrawWorldGrid()
    {
        if (!m_Renderer2D || !m_Renderer)
            return;
        const float cell = WorldUnits::FromMeters(1.0f);
        if (m_EditorCamera.IsOrthographic())
            m_Renderer2D->DrawGrid(m_EditorCamera.GetFocalPoint(), m_Renderer->GetRenderOrigin(), cell, GridPlaneAxis(), m_EditorCamera.GetOrthoHalfHeight());
        else
            m_Renderer2D->DrawGrid(m_EditorCamera.GetPosition(), m_Renderer->GetRenderOrigin(), cell, 1, m_EditorCamera.GetDistance());
    }

    void EditorV2Layer::OnOverlayRender()
    {
        if (!m_Renderer || !m_Renderer2D || !m_ActiveScene)
            return;

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
            DrawWorldGrid();
        }

        m_Renderer->EndScene();
    }

    void EditorV2Layer::RefreshStatusBar()
    {
        if (!m_StatusLabel || !m_Renderer || !Project::GetActive() || !Project::GetActive()->GetEditorAssetManager())
            return;
        auto assetManager = Project::GetActive()->GetEditorAssetManager();
        const EditorAssetManager::ImportProgress import = assetManager->GetImportProgress();
        const size_t loading = assetManager->GetLoadingCount();
        const size_t streaming = assetManager->GetStreamingCount();
        const double pendingMB = static_cast<double>(assetManager->GetPendingUploadBytes()) / (1024.0 * 1024.0);
        const size_t blasBuilds = m_Renderer->GetPendingBlasBuilds();

        if (import.Imports > 0)
        {
            char buf[96];
            if (import.Total == 0)
                std::snprintf(buf, sizeof(buf), "Importing: reading the file...");
            else
                std::snprintf(buf, sizeof(buf), "Importing meshes %u / %u", import.Done, import.Total);
            m_StatusLabel->Text = buf;
        }
        else if (loading == 0 && streaming == 0 && blasBuilds == 0)
        {
            m_StatusLabel->Text = "Ready";
        }
        else
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "Loading %zu asset(s)  |  Streaming %zu texture(s)  |  %.1f MB to upload  |  %zu BLAS build(s) pending",
                          loading, streaming, pendingMB, blasBuilds);
            m_StatusLabel->Text = buf;
        }
    }

    void EditorV2Layer::OnUpdate(Timestep ts)
    {
        if (!Project::GetActive())
            return;

        if (m_ThumbnailPending && m_SceneState == SceneState::Edit && !m_SceneLoad)
        {
            m_ThumbnailTimer += ts;
            if (m_ThumbnailTimer > 4.0f)
            {
                m_ThumbnailPending = false;
                CaptureProjectThumbnail();
            }
        }

        if (m_SceneLoad && m_SceneLoad->Step(8.0))
            FinishSceneLoad();

        m_CustomLitPollTimer += ts;
        if (m_CustomLitPollTimer > 0.5f)
        {
            m_CustomLitPollTimer = 0.0f;
            PollCustomLitShaders();
        }

        if (m_TestWorldGeneration && m_TestWorldGeneration->Step())
        {
            const AssetHandle scene = m_TestWorldGeneration->Failed() ? AssetHandle(0) : m_TestWorldGeneration->GetScene();
            const glm::dvec3 middle = m_TestWorldGeneration->GetMiddle();
            m_TestWorldGeneration.reset();
            if (scene != 0)
            {
                m_FocusOnOpen = middle;
                OpenScene(scene);
            }
        }

        m_ScriptBuilder.Update();

        auto assetManager = Project::GetActive()->GetEditorAssetManager();
        if (assetManager)
        {
            if (m_EditorScene && m_EditorScene->ConsumeAssetReferencesChanged())
            {
                m_UnloadUnusedAssetsRequested = true;
                m_EditorSceneReferencesValid = false;
            }
            if (m_ActiveScene && m_ActiveScene != m_EditorScene && m_ActiveScene->ConsumeAssetReferencesChanged())
                m_UnloadUnusedAssetsRequested = true;
            if (assetManager->ConsumeLoadsSettledAfterSweep())
                m_UnloadUnusedAssetsRequested = true;
            if (m_UnloadUnusedAssetsRequested && std::chrono::steady_clock::now() - m_LastUnloadSweep >= std::chrono::seconds(1))
            {
                NOX_PROFILE_SCOPE("Unload Unused Assets");
                m_UnloadUnusedAssetsRequested = false;
                m_LastUnloadSweep = std::chrono::steady_clock::now();
                UnloadUnusedAssets();
            }

            for (const ModelInstance::LevelDescription& level : assetManager->ConsumeLevelImports())
            {
                if (m_EditorScene)
                    ModelInstance::SpawnLevel(*m_EditorScene, level);
            }

            assetManager->Update();
        }

        if (!m_Renderer || !m_ActiveScene || !m_ViewportImage)
            return;

        const UI::UIRect& bounds = m_ViewportImage->GetBounds();
        const uint32_t wantW = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Width())));
        const uint32_t wantH = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Height())));
        if (wantW > 0 && wantH > 0)
        {
            m_ActiveScene->OnViewportResize(wantW, wantH);
            const NRI::Extent2D currentSize = m_Renderer->getViewPortSize();
            if (wantW != currentSize.width || wantH != currentSize.height)
            {
                m_Renderer->onViewportSizeChange({wantW, wantH});
                m_EditorCamera.SetViewportSize(static_cast<float>(wantW), static_cast<float>(wantH));
            }
        }

        const bool hovered = UI::HasState(m_ViewportImage->GetState(), UI::UIState::Hovered);
        const bool focused = UI::HasState(m_ViewportImage->GetState(), UI::UIState::Focused);
        Input::SetGameInputEnabled(focused || hovered, hovered);

        m_ActiveScene->SetRenderer(m_Renderer);
        m_ActiveScene->SetRenderer2D(m_Renderer2D);

        switch (m_SceneState)
        {
        case SceneState::Edit:
            m_EditorCamera.OnUpdate(ts, hovered || m_ViewportMouseCaptured);
            m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
            break;
        case SceneState::Simulate:
            m_EditorCamera.OnUpdate(ts, hovered || m_ViewportMouseCaptured);
            m_ActiveScene->OnUpdateSimulation(ts, m_EditorCamera);
            break;
        case SceneState::Play:
            m_ActiveScene->OnUpdateRuntime(ts);
            break;
        }

        OnOverlayRender();
        UpdateHistory();

        m_StatusRefreshTimer += ts;
        if (m_StatusRefreshTimer >= 1.0f)
        {
            m_StatusRefreshTimer = 0.0f;
            RefreshStatusBar();
        }

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

        Texture2D* sceneTex = m_Renderer->GetSceneResource();
        m_ViewportImage->TextureDescriptorSlot = sceneTex ? sceneTex->GetDescriptorIndexSlot() : ~0u;
    }

    void EditorV2Layer::OnEvent(Event& event)
    {
        if (!Project::GetActive())
            return;

        if (event.GetEventType() == EventType::ExternalFileDrop)
        {
            const auto& drop = static_cast<const ExternalFileDropEvent&>(event);
            NOX_CORE_INFO("Dropped external file: {}", drop.GetPath());
        }

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

                const bool viewportHovered = m_ViewportImage && UI::HasState(m_ViewportImage->GetState(), UI::UIState::Hovered);

                switch (keyCode)
                {
                case SDL_SCANCODE_Z:
                    if (control) { if (shift) Redo(); else Undo(); event.Handled = true; return; }
                    break;
                case SDL_SCANCODE_Y:
                    if (control) { Redo(); event.Handled = true; return; }
                    break;
                case SDL_SCANCODE_N:
                    if (control) { NewScene(); event.Handled = true; return; }
                    break;
                case SDL_SCANCODE_O:
                    if (control) { OpenProject(); event.Handled = true; return; }
                    break;
                case SDL_SCANCODE_S:
                    if (control) { if (shift) SaveSceneAs(); else SaveScene(); event.Handled = true; return; }
                    break;
                case SDL_SCANCODE_B:
                    if (control) { m_ScriptBuilder.RequestBuild(); event.Handled = true; return; }
                    break;
                case SDL_SCANCODE_R:
                    if (control) { if (ScriptEngine::IsInitialized()) ScriptEngine::ReloadAssembly(); event.Handled = true; return; }
                    break;
                case SDL_SCANCODE_KP_1:
                    if (viewportHovered) { SetViewMode(control ? EditorViewMode::Back : EditorViewMode::Front); event.Handled = true; return; }
                    break;
                case SDL_SCANCODE_KP_3:
                    if (viewportHovered) { SetViewMode(control ? EditorViewMode::Left : EditorViewMode::Right); event.Handled = true; return; }
                    break;
                case SDL_SCANCODE_KP_7:
                    if (viewportHovered) { SetViewMode(control ? EditorViewMode::Bottom : EditorViewMode::Top); event.Handled = true; return; }
                    break;
                case SDL_SCANCODE_KP_5:
                    if (viewportHovered) { SetViewMode(EditorViewMode::Perspective); event.Handled = true; return; }
                    break;
                case SDL_SCANCODE_ESCAPE:
                    if (m_SceneState != SceneState::Edit) { OnSceneStop(); event.Handled = true; return; }
                    break;
                default:
                    break;
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
                {"Package Game...", "", {}, false, false, false, false, "Packaging dialog not yet ported to NoxEditorV2"},
                {"Package Server...", "", {}, false, false, false, false, "Packaging dialog not yet ported to NoxEditorV2"},
                {"", "", {}, true},
                {"Save Layout", "", [uiLayer]()
                {
                    if (const auto& dock = uiLayer->GetDockSpaceRef())
                        dock->SaveLayout(kLayoutPath);
                }},
                {"Exit", "Alt+F4", []() { Application::Get().Shutdown(); }}
            };
        });

        menuBar->AddDynamicMenu("Edit", [this]() -> std::vector<UI::UIMenuItem>
        {
            const bool editing = m_SceneState == SceneState::Edit;
            const std::string undoLabel = m_History.CanUndo() ? "Undo " + m_History.GetUndoName() : std::string("Undo");
            const std::string redoLabel = m_History.CanRedo() ? "Redo " + m_History.GetRedoName() : std::string("Redo");
            char historyText[96];
            const size_t steps = m_History.GetStateCount() > 0 ? m_History.GetStateCount() - 1 : 0;
            std::snprintf(historyText, sizeof(historyText), "History: %zu steps, %.1f MB",
                          steps, static_cast<double>(m_History.GetMemoryBytes()) / (1024.0 * 1024.0));
            return {
                {undoLabel, "Ctrl+Z", [this]() { Undo(); }, false, editing && m_History.CanUndo()},
                {redoLabel, "Ctrl+Y / Ctrl+Shift+Z", [this]() { Redo(); }, false, editing && m_History.CanRedo()},
                {"", "", {}, true},
                {historyText, "", {}, false, false, false, true}
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
                {"Empty Entity", "", [this]()
                {
                    if (m_ActiveScene)
                    {
                        m_ActiveScene->CreateEntity("Empty Entity");
                        m_HistoryCheckRequested = true;
                    }
                }},
                {"Shapes", "", {}, false, true, false, false, "", std::move(shapeItems)},
                {"", "", {}, true},
                {"Streaming Test World...", "", {}, false, false, false, false,
                 "A world partition larger than RAM and VRAM (settings dialog not yet ported to NoxEditorV2)."}
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
            std::vector<UI::UIMenuItem> items = {
                {"Build and Reload", "Ctrl+B", [this]() { m_ScriptBuilder.RequestBuild(); }, false, !m_ScriptBuilder.IsBuilding()},
                {"Reload assembly", "Ctrl+R", []() { ScriptEngine::ReloadAssembly(); }, false, canReload},
                {"Build on Save", "", [this]() { m_ScriptBuilder.AutoBuild = !m_ScriptBuilder.AutoBuild; }, false, true, m_ScriptBuilder.AutoBuild, false,
                 "Saving a .cs file of the project builds its scripts and reloads them a moment later."},
                {"Wait for Debugger on Play", "", [waitForDbg]() { ScriptEngine::SetWaitForDebugger(!waitForDbg); }, false, true, waitForDbg, false,
                 "Play waits (up to 60 s) until a .NET debugger attaches to the editor, then breaks once."}
            };
            if (!m_ScriptBuilder.GetStatus().empty())
            {
                items.push_back({"", "", {}, true});
                items.push_back({m_ScriptBuilder.GetStatus(), "", {}, false, false, false, true});
            }
            return items;
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
