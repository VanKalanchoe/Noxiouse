#include "EditorV2Layer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
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
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <stb_image_write.h>
#include "json.hpp"

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/SceneImporter.h"
#include "NoxCore/Asset/TextureImporter.h"
#include "NoxCore/Audio/AudioMixer.h"
#include "NoxCore/Console/Console.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Input.h"
#include "NoxCore/Core/Window.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Network/NetReplication.h"
#include "NoxCore/Network/NetSession.h"
#include "NoxCore/Physics/MeshColliderGeometry.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/CustomLitShaders.h"
#include "NoxCore/Renderer/MaterialGraph.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/EntityBounds.h"
#include "NoxCore/Scene/ModelInstance.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/SceneSerializer.h"
#include "NoxCore/Scripting/ScriptEngine.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Core/UILayer.h"
#include "NoxCore/UI/Layout/UIDockSpace.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UIImage.h"
#include "NoxCore/UI/Widgets/UISlider.h"
#include "NoxCore/UI/Widgets/UITextInput.h"
#include "NoxCore/Utils/Utils.h"
#include "ProjectManager/ProcessLauncher.h"
#include "ProjectManager/ProjectLibrary.h"

namespace Nox
{
    namespace
    {
        constexpr const char* kLayoutPath = "assets/layouts/EditorV2.nlayout";
        constexpr const char* kViewportPrefsPath = "assets/layouts/EditorV2Viewports.txt";
        constexpr const char* kViewportTitles[4] = { "Viewport", "Viewport 2", "Viewport 3", "Viewport 4" };
        constexpr int kDefaultSlotViewModes[4] = { 0, 1, 3, 6 };

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

        for (int i = 0; i < kMaxViewports; ++i)
        {
            m_Viewports[i].Title = kViewportTitles[i];
            m_Viewports[i].Camera = m_EditorCamera;
            m_Viewports[i].Camera.SetViewMode(static_cast<EditorViewMode>(kDefaultSlotViewModes[i]));
        }
        m_Viewports[0].Open = true;
        LoadViewportPrefs();

        m_IconPlay = TextureImporter::LoadTexture2D("assets/Icons/PlayButton.ktx2", { .flip = true, .generateMips = false });
        m_IconStop = TextureImporter::LoadTexture2D("assets/Icons/StopButton.ktx2", { .flip = true, .generateMips = false });
        m_IconPause = TextureImporter::LoadTexture2D("assets/Icons/PauseButton.ktx2", { .flip = true, .generateMips = false });
        m_IconSimulate = TextureImporter::LoadTexture2D("assets/Icons/SimulateButton.ktx2", { .flip = true, .generateMips = false });
        m_IconStep = TextureImporter::LoadTexture2D("assets/Icons/StepButton.ktx2", { .flip = true, .generateMips = false });

        m_EditorScene = CreateRef<Scene>();
        m_ActiveScene = m_EditorScene;
        m_SceneHierarchyPanel.SetFocusEntityCallback([this](Entity entity) { FocusOnEntity(entity); });
        m_SceneHierarchyPanel.SetCreateShapeCallback([this](PrimitiveShape shape, const std::string& folder) { CreateShape(shape, folder); });
        m_SceneHierarchyPanel.SetSceneMutatedCallback([this]() { m_HistoryCheckRequested = true; });
        m_SceneHierarchyPanel.SetContext(m_ActiveScene);

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
        if (m_CursorHidden)
        {
            SDL_ShowCursor();
            m_CursorHidden = false;
        }
        EndMultiplayerPlay();
        SaveEditorCameras();
        SaveViewportPrefs();
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
        m_HoveredEntity = Entity();
        m_EditorScene = CreateRef<Scene>();
        m_ActiveScene = m_EditorScene;
        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
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
        m_HoveredEntity = Entity();
        m_EditorScene = newScene;
        m_ActiveScene = m_EditorScene;
        m_EditorScenePath = Project::GetActive()->GetEditorAssetManager()->GetFilePath(handle);
        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
        RefreshSceneLoadOverlay();

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

    void EditorV2Layer::PlaceAssets(const std::vector<AssetHandle>& handles, const glm::dvec3& point, const std::string& folder)
    {
        if (!m_ActiveScene || !Project::GetActive())
            return;
        m_SceneHierarchyPanel.ClearSelection();
        Entity lastCreated;
        for (AssetHandle handle : handles)
        {
            const AssetType type = AssetManager::GetAssetType(handle);
            if (type == AssetType::Prefab)
            {
                const AssetMetadata& prefabMetadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
                std::string prefabName = prefabMetadata.FilePath.filename().stem().string();
                Entity prefabRoot = m_ActiveScene->CreateEntity(prefabName.empty() ? "Prefab" : prefabName);
                auto& prefabInstance = prefabRoot.AddComponent<PrefabInstanceComponent>();
                prefabInstance.Prefab = handle;
                prefabInstance.InitTransform = true;
                prefabRoot.GetComponent<TransformComponent>().Translation = point;
                if (!folder.empty())
                    prefabRoot.AddComponent<FolderComponent>(folder);
                prefabRoot.MarkTransformDirty();
                AssetManager::RequestAsset(handle);
                lastCreated = prefabRoot;
                continue;
            }
            if (type != AssetType::Mesh && type != AssetType::StaticMesh)
                continue;

            const AssetMetadata& metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
            std::string entityName = metadata.FilePath.filename().stem().string();
            Entity root = m_ActiveScene->CreateEntity(entityName.empty() ? "Model" : entityName);
            auto& instance = root.AddComponent<ModelInstanceComponent>();
            instance.Model = handle;
            instance.AtFileLayout = true;
            root.GetComponent<TransformComponent>().Translation = point;
            if (!folder.empty())
                root.AddComponent<FolderComponent>(folder);
            root.MarkTransformDirty();
            AssetManager::RequestAsset(handle);
            lastCreated = root;
        }
        if (lastCreated)
            m_SceneHierarchyPanel.SetSelectedEntity(lastCreated);
        m_SceneHierarchyPanel.MarkDirty();
        m_HistoryCheckRequested = true;
    }

    void EditorV2Layer::CreateShape(PrimitiveShape shape, const std::string& folder)
    {
        if (!m_ActiveScene)
            return;
        const AssetHandle mesh = GetShapeAsset(shape);
        if (mesh == 0)
            return;
        PlaceAssets({ mesh }, m_EditorCamera.GetPosition() + glm::dvec3(m_EditorCamera.GetForwardDirection() * m_EditorCamera.GetDistance()), folder);
    }

    void EditorV2Layer::FocusOnEntity(Entity entity)
    {
        if (!entity || !m_ActiveScene)
            return;

        glm::dvec3 centre;
        glm::vec3 boxSize{ 0.0f };
        const WorldBounds bounds = ComputeWorldBounds(*m_ActiveScene, entity);
        if (bounds.Valid)
        {
            centre = bounds.Center();
            boxSize = bounds.Size();
        }
        else if (entity.HasComponent<WorldTransformComponent>())
        {
            centre = entity.GetComponent<WorldTransformComponent>().GetPosition();
        }
        else
        {
            return;
        }

        m_ViewportTools.ViewTurn.Active = false;

        if (m_EditorCamera.IsOrthographic())
        {
            const float orthoRadius = bounds.Valid
                ? std::max({ boxSize.x, boxSize.y, boxSize.z }) * 0.5f
                : 0.0f;
            m_EditorCamera.Focus(centre, orthoRadius);
            return;
        }

        const float tanHalfFov = std::max(std::tan(glm::radians(m_EditorCamera.GetFOV()) * 0.5f), 0.01f);
        const float aspect = std::max(m_EditorCamera.GetAspectRatio(), 0.25f);
        float focusDist = WorldUnits::FromMeters(2.2f);
        if (bounds.Valid)
        {
            const float distV = (0.5f * boxSize.y * 1.18f) / tanHalfFov;
            const float distH = (0.5f * std::max(boxSize.x, boxSize.z) * 1.18f) / (tanHalfFov * aspect);
            focusDist = std::max({ distV, distH, WorldUnits::FromCentimeters(45.0f) });
        }
        const double entityRadius = bounds.Valid ? 0.5 * static_cast<double>(glm::length(boxSize)) : 0.0;

        // Avoid placing the camera behind an occluding mesh (e.g. looking sideways through a nearby ramp/wall).
        std::unordered_set<UUID> excluded;
        std::function<void(Entity)> excludeSubtree = [&](Entity e)
        {
            excluded.insert(e.GetUUID());
            if (!e.HasComponent<RelationshipComponent>())
                return;
            for (UUID cid : e.GetComponent<RelationshipComponent>().Children)
            {
                if (Entity c = m_ActiveScene->GetEntityByUUID(cid))
                    excludeSubtree(c);
            }
        };
        excludeSubtree(entity);

        struct Occluder
        {
            glm::dmat4 InvWorld;
            glm::vec3 LocalMin;
            glm::vec3 LocalMax;
        };
        std::vector<Occluder> occluders;
        auto meshView = m_ActiveScene->GetAllEntitiesWith<MeshComponent, WorldTransformComponent>();
        for (auto handle : meshView)
        {
            Entity cand(handle, m_ActiveScene.get());
            if (!cand || excluded.contains(cand.GetUUID()) || !m_ActiveScene->IsActiveInHierarchy(cand))
                continue;
            const WorldBounds wb = ComputeWorldBounds(*m_ActiveScene, cand, false);
            if (!wb.Valid)
                continue;
            if (wb.Max.y <= (bounds.Valid ? bounds.Min.y + boxSize.y * 0.15 : centre.y))
                continue;
            glm::vec3 lMin, lMax;
            if (!ComputeLocalMeshBounds(cand, lMin, lMax))
                continue;
            const glm::dmat4& w = cand.GetComponent<WorldTransformComponent>().WorldMatrix;
            if (std::abs(glm::determinant(w)) < 1e-12)
                continue;
            occluders.push_back({ glm::inverse(w), lMin, lMax });
        }

        auto clearFraction = [&](float pitch, float yaw, float dist) -> double
        {
            const glm::quat q(glm::vec3(-pitch, -yaw, 0.0f));
            const glm::vec3 fwd = glm::rotate(q, glm::vec3(0.0f, 0.0f, -1.0f));
            const glm::dvec3 camPos = centre - glm::dvec3(fwd) * static_cast<double>(dist);
            const double startT = (dist > 1e-3f) ? std::clamp((entityRadius * 0.55) / static_cast<double>(dist), 0.02, 0.5) : 0.02;

            double bestT = 1.0;
            for (const Occluder& occ : occluders)
            {
                const glm::dvec3 oA = glm::dvec3(occ.InvWorld * glm::dvec4(centre, 1.0));
                const glm::dvec3 oB = glm::dvec3(occ.InvWorld * glm::dvec4(camPos, 1.0));
                const glm::dvec3 oD = oB - oA;

                double tMin = startT;
                double tMax = bestT;
                bool hit = true;
                for (int axis = 0; axis < 3; ++axis)
                {
                    const double b0 = static_cast<double>(occ.LocalMin[axis]);
                    const double b1 = static_cast<double>(occ.LocalMax[axis]);
                    if (std::abs(oD[axis]) < 1e-9)
                    {
                        if (oA[axis] < b0 || oA[axis] > b1) { hit = false; break; }
                    }
                    else
                    {
                        double t1 = (b0 - oA[axis]) / oD[axis];
                        double t2 = (b1 - oA[axis]) / oD[axis];
                        if (t1 > t2) std::swap(t1, t2);
                        tMin = std::max(tMin, t1);
                        tMax = std::min(tMax, t2);
                        if (tMin > tMax) { hit = false; break; }
                    }
                }
                if (hit && tMin < bestT)
                    bestT = tMin;
            }
            return bestT;
        };

        float targetPitch = m_EditorCamera.GetPitch();
        float targetYaw = m_EditorCamera.GetYaw();
        double bestFrac = clearFraction(targetPitch, targetYaw, focusDist);
        if (bestFrac < 0.999)
        {
            float frontYaw = targetYaw;
            if (entity.HasComponent<WorldTransformComponent>())
            {
                const glm::mat4 w = m_Renderer
                    ? m_Renderer->ToRenderSpace(entity.GetComponent<WorldTransformComponent>().WorldMatrix)
                    : glm::mat4(entity.GetComponent<WorldTransformComponent>().WorldMatrix);
                const glm::vec3 entFwd = glm::vec3(w[2]);
                if (glm::length(glm::vec2(entFwd.x, entFwd.z)) > 1e-4f)
                    frontYaw = -std::atan2(entFwd.x, entFwd.z);
            }

            const float candidatePitches[] = { std::clamp(targetPitch, 0.08f, 0.45f), 0.22f, 0.45f };
            const float yawOffsets[] = { 0.0f, 0.55f, -0.55f, 1.1f, -1.1f, 1.57f, -1.57f, 2.2f, -2.2f, 3.14159f };
            float bestCandidatePitch = targetPitch;
            float bestCandidateYaw = targetYaw;
            double bestCandidateFrac = bestFrac;

            auto tryCandidates = [&](float baseYaw)
            {
                for (float p : candidatePitches)
                {
                    for (float dy : yawOffsets)
                    {
                        const float y = baseYaw + dy;
                        const double f = clearFraction(p, y, focusDist);
                        if (f > bestCandidateFrac)
                        {
                            bestCandidateFrac = f;
                            bestCandidatePitch = p;
                            bestCandidateYaw = y;
                            if (bestCandidateFrac >= 0.999)
                                return true;
                        }
                    }
                }
                return false;
            };

            if (!tryCandidates(targetYaw))
                tryCandidates(frontYaw);

            if (bestCandidateFrac > bestFrac + 0.05)
            {
                targetPitch = bestCandidatePitch;
                targetYaw = bestCandidateYaw;
                bestFrac = bestCandidateFrac;
                m_EditorCamera.SetOrientation(targetPitch, targetYaw);
            }
            if (bestFrac < 0.999)
                focusDist = std::max(focusDist * static_cast<float>(bestFrac) * 0.88f, WorldUnits::FromCentimeters(40.0f));
        }

        m_EditorCamera.SetFocalPoint(centre);
        m_EditorCamera.SetDistance(focusDist);
    }

    void EditorV2Layer::SnapSelectionToFloor()
    {
        if (!m_ActiveScene || !m_Renderer)
            return;
        const auto& selection = m_SceneHierarchyPanel.GetSelectedEntities();
        if (selection.empty())
            return;

        std::vector<Entity> tops;
        for (Entity entity : selection)
        {
            if (!entity)
                continue;
            const bool parentSelected = entity.HasComponent<RelationshipComponent>() &&
                                        entity.GetComponent<RelationshipComponent>().Parent != 0 &&
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
            const double centreY = 0.5 * (mover.Min.y + mover.Max.y);

            double floorY = 0.0;
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

            struct SnapRays
            {
                UUID Entity = 0;
                double BoundsFloor = 0.0;
                double OriginY = 0.0;
                int Pending = 0;
                bool Hit = false;
                float NearestDistance = 0.0f;
            };
            auto snap = std::make_shared<SnapRays>();
            snap->Entity = top.GetUUID();
            snap->BoundsFloor = floorY;
            snap->OriginY = mover.Min.y - WorldUnits::FromMeters(0.01f);
            const glm::dvec3 centre = mover.Center();
            const glm::dvec3 halfInset = 0.25 * (mover.Max - mover.Min);
            const glm::vec2 offsets[5] = { { 0.0f, 0.0f }, { -1.0f, -1.0f }, { 1.0f, -1.0f }, { -1.0f, 1.0f }, { 1.0f, 1.0f } };
            snap->Pending = 5;
            for (const glm::vec2& offset : offsets)
            {
                const glm::dvec3 origin(centre.x + offset.x * halfInset.x, snap->OriginY, centre.z + offset.y * halfInset.z);
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
                    const double floor = snap->Hit ? snap->OriginY - snap->NearestDistance : snap->BoundsFloor;
                    const double drop = floor - now.Min.y;
                    entity.SetWorldTransform(glm::translate(glm::dmat4(1.0), glm::dvec3(0.0, drop, 0.0)) *
                                             entity.GetComponent<WorldTransformComponent>().WorldMatrix);
                    m_HistoryCheckRequested = true;
                });
            }
        }
    }

    void EditorV2Layer::SnapSelectionToGrid()
    {
        if (!m_ActiveScene)
            return;
        const float step = m_ViewportTools.LocationSnapWorldUnits();
        if (step <= 0.0f)
            return;
        for (Entity entity : m_SceneHierarchyPanel.GetSelectedEntities())
        {
            if (!entity || !entity.HasComponent<WorldTransformComponent>())
                continue;
            const bool parentSelected = entity.HasComponent<RelationshipComponent>() &&
                                        entity.GetComponent<RelationshipComponent>().Parent != 0 &&
                                        m_SceneHierarchyPanel.IsSelected(m_ActiveScene->GetEntityByUUID(entity.GetComponent<RelationshipComponent>().Parent));
            if (parentSelected)
                continue;
            glm::dmat4 world = entity.GetComponent<WorldTransformComponent>().WorldMatrix;
            const glm::dvec3 position(world[3]);
            const double gridStep = step;
            world[3] = glm::dvec4(glm::round(position / gridStep) * gridStep, 1.0);
            entity.SetWorldTransform(world);
        }
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
        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
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
        for (int index = 0; index < kMaxViewports; ++index)
        {
            const ViewportSlot& slot = m_Viewports[index];
            if (index != m_ActiveViewport && !slot.CameraSet)
                continue;
            const EditorCamera& camera = index == m_ActiveViewport ? m_EditorCamera : slot.Camera;
            const glm::dvec3 focal = camera.GetFocalPoint();
            char line[512];
            std::snprintf(line, sizeof(line), "%s|%d|%.17g %.17g %.17g %.6g %.6g %.6g %d %.6g",
                          scene.c_str(), index, focal.x, focal.y, focal.z,
                          camera.GetDistance(), camera.GetPitch(), camera.GetYaw(),
                          static_cast<int>(camera.GetViewMode()), camera.GetOrthoHalfHeight());
            lines.push_back(line);
        }
        std::error_code error;
        std::filesystem::create_directories(EditorCamerasFile().parent_path(), error);
        std::ofstream out(EditorCamerasFile(), std::ios::trunc);
        for (const std::string& entry : lines)
            out << entry << "\n";
    }

    bool EditorV2Layer::RestoreEditorCameras()
    {
        bool activeRestored = false;
        if (!Project::GetActive() || m_EditorScenePath.empty())
            return activeRestored;
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
                       &mode, &orthoHalfHeight) != 9 || index < 0 || index >= kMaxViewports ||
                mode < 0 || mode > static_cast<int>(EditorViewMode::Right))
                continue;
            EditorCamera& camera = index == m_ActiveViewport ? m_EditorCamera : m_Viewports[index].Camera;
            camera.SetViewMode(static_cast<EditorViewMode>(mode));
            camera.SetFocalPoint(focal);
            camera.SetDistance(distance);
            camera.SetOrientation(pitch, yaw);
            if (camera.IsOrthographic())
                camera.SetOrthoHalfHeight(orthoHalfHeight);
            m_Viewports[index].CameraSet = true;
            activeRestored |= (index == m_ActiveViewport);
        }
        return activeRestored;
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
        frame.InteractionActive = m_ViewportTools.GizmoUsing;
        frame.InteractionEnded = m_HistoryCheckRequested || (m_ViewportTools.GizmoWasUsing && !m_ViewportTools.GizmoUsing);
        m_ViewportTools.GizmoWasUsing = m_ViewportTools.GizmoUsing;
        m_HistoryCheckRequested = false;
        frame.AssetsLoading = Project::GetActive()->GetEditorAssetManager()->GetLoadingCount() > 0;
        if (Entity watched = m_SceneHierarchyPanel.GetSelectedEntity())
            frame.Watched = watched.GetUUID();
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
        std::vector<UUID> selection;
        for (Entity selected : m_SceneHierarchyPanel.GetSelectedEntities())
        {
            if (selected)
                selection.push_back(selected.GetUUID());
        }

        m_HoveredEntity = Entity();
        restored->AdoptWorldState(*m_EditorScene);
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

        if (m_PlayNet.Mode != PlayNetMode::Standalone)
        {
            if (!m_PlayNet.SaveWithoutAsking)
            {
                m_OpenSaveAndPlay = true;
                return;
            }
            SaveScene();
            BeginMultiplayerPlay();
            RefreshToolBar();
            return;
        }

        m_SceneState = SceneState::Play;
        StartPlayScene(Scene::NetMode::Standalone);
        RefreshToolBar();
    }

    void EditorV2Layer::StartPlayScene(Scene::NetMode netMode)
    {
        m_HoveredEntity = Entity();
        m_ActiveScene = Scene::Copy(m_EditorScene);
        m_ActiveScene->SetNetMode(netMode);
        if (m_PlaySession && m_PlaySession->GetReplication())
            m_PlaySession->GetReplication()->SetScene(m_ActiveScene.get());
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
        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
    }

    bool EditorV2Layer::LaunchPlayer(const std::vector<std::string>& arguments, bool console, const char* what)
    {
#ifdef _WIN32
        const std::filesystem::path runtime = std::filesystem::path(Application::GetExecutableRootPath()) / "NoxRuntime.exe";
#else
        const std::filesystem::path runtime = std::filesystem::path(Application::GetExecutableRootPath()) / "NoxRuntime";
#endif
        std::error_code exists;
        if (!std::filesystem::exists(runtime, exists))
        {
            NOX_CORE_ERROR("Multiplayer Play: NoxRuntime is not next to the editor ({}); build the NoxRuntime target", runtime.generic_string());
            return false;
        }
        auto process = std::make_unique<ChildProcess>();
        std::string error;
        if (!process->Launch(runtime, arguments, console, error))
        {
            NOX_CORE_ERROR("Multiplayer Play: could not start {} ({})", what, error);
            return false;
        }
        NOX_CORE_INFO("Multiplayer Play: started {}", what);
        m_PlayProcesses.push_back(std::move(process));
        return true;
    }

    std::filesystem::path EditorV2Layer::EditorSceneAssetPath() const
    {
        if (m_EditorScenePath.empty() || !Project::GetActive())
            return {};
        if (!m_EditorScenePath.is_absolute())
            return m_EditorScenePath.lexically_normal();
        std::error_code error;
        const std::filesystem::path relative = std::filesystem::relative(m_EditorScenePath, Project::GetActiveAssetDirectory(), error);
        if (error || relative.empty() || *relative.begin() == "..")
            return {};
        return relative.lexically_normal();
    }

    AssetHandle EditorV2Layer::EditorSceneHandle() const
    {
        const std::filesystem::path relative = EditorSceneAssetPath();
        if (relative.empty())
            return 0;
        return Project::GetActive()->GetEditorAssetManager()->ResolvePath(relative, AssetType::Scene);
    }

    void EditorV2Layer::BeginMultiplayerPlay()
    {
        if (m_EditorScenePath.empty() || !Project::GetActive())
        {
            NOX_CORE_WARN("Multiplayer Play: save the scene first (the other players load it from disk)");
            return;
        }
        std::filesystem::path projectFile;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(Project::GetProjectDirectory(), error))
        {
            if (entry.is_regular_file(error) && entry.path().extension() == ".nproj")
            {
                projectFile = entry.path();
                break;
            }
        }
        const std::string scene = EditorSceneAssetPath().generic_string();
        const AssetHandle sceneHandle = EditorSceneHandle();
        if (projectFile.empty())
        {
            NOX_CORE_ERROR("Multiplayer Play: no .nproj file in {}", Project::GetProjectDirectory().generic_string());
            return;
        }
        if (scene.empty() || sceneHandle == 0)
        {
            NOX_CORE_ERROR("Multiplayer Play: the scene '{}' is not an asset of the project (save it under {})",
                           m_EditorScenePath.generic_string(), Project::GetActiveAssetDirectory().generic_string());
            return;
        }

        static const char* emulationNames[] = { "off", "average", "bad" };
        const std::string emulation = emulationNames[std::clamp(m_PlayNet.Emulation, 0, 2)];
        Net::NetEmulation emulationValues;
        Net::NetEmulation::FromPreset(emulation, emulationValues);
        const uint16_t port = static_cast<uint16_t>(std::clamp(m_PlayNet.Port, 1, 65535));

        SDL_Rect usable{ 0, 0, 1920, 1080 };
        SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
        const int windowWidth = std::min(usable.w / 2, 1280);
        const int windowHeight = std::min(usable.h / 2, 720);
        auto playerArguments = [&](int player)
        {
            const int slot = (player - 2) % 4;
            return std::vector<std::string>{
                projectFile.string(),
                "--connect=127.0.0.1:" + std::to_string(port),
                "--connect-retry=30",
                "--windowed",
                "--name=Player" + std::to_string(player),
                "--scene=" + scene,
                "--net-emulation=" + emulation,
                std::format("--window-size={},{}", windowWidth, windowHeight),
                std::format("--window-pos={},{}", usable.x + (slot % 2) * windowWidth, usable.y + (slot / 2) * windowHeight)
            };
        };
        auto withDebug = [this](std::vector<std::string> arguments)
        {
            if (m_PlayNet.DebugTrace)
                arguments.push_back("--net-debug");
            return arguments;
        };

        Net::NetSessionConfig config;
        config.Port = port;
        config.Emulation = emulationValues;
        config.PlayerName = "Player1";
        config.DebugTrace = m_PlayNet.DebugTrace;
        m_PlaySessionEnded = false;

        if (m_PlayNet.Mode == PlayNetMode::ListenServer)
        {
            config.Role = Net::SessionRole::ListenServer;
            m_PlaySession = std::make_unique<Net::NetSession>(config);
            m_PlaySession->FillWelcome = [this, sceneHandle](Net::NetWelcome& welcome)
            {
                welcome.ServerTick = m_ActiveScene ? m_ActiveScene->GetTick() : 0;
                welcome.TickRate = static_cast<uint16_t>(Scene::TickRate);
                welcome.SceneHandle = static_cast<uint64_t>(sceneHandle);
            };
            if (!m_PlaySession->Start())
            {
                NOX_CORE_ERROR("Multiplayer Play: port {} cannot be opened (another server running? change it in the Play settings)", port);
                m_PlaySession.reset();
                return;
            }
            m_SceneState = SceneState::Play;
            StartPlayScene(Scene::NetMode::Server);
        }
        else
        {
            if (!LaunchPlayer(withDebug({ projectFile.string(), "--server", "--port=" + std::to_string(port),
                                          "--scene=" + scene, "--net-emulation=" + emulation }),
                              true, "the dedicated server"))
                return;
            config.Role = Net::SessionRole::Client;
            config.Address = "127.0.0.1:" + std::to_string(port);
            config.ConnectRetrySeconds = 30.0;
            m_PlaySession = std::make_unique<Net::NetSession>(config);
            m_PlaySession->OnJoined = [this, sceneHandle](AssetHandle serverScene)
            {
                if (serverScene != sceneHandle)
                    return false;
                m_PlayWaitingForServer = false;
                StartPlayScene(Scene::NetMode::Client);
                return true;
            };
            m_PlaySession->OnLeft = [this](Net::DisconnectReason reason, const std::string& text, bool)
            {
                NOX_CORE_WARN("Multiplayer Play: the session ended -- {} ({})", Net::ToString(reason), text);
                m_PlaySessionEnded = true;
            };
            m_SceneState = SceneState::Play;
            m_PlayWaitingForServer = true;
            if (!m_PlaySession->Start())
                m_PlaySessionEnded = true;
        }

        for (int player = 2; player <= std::clamp(m_PlayNet.Players, 1, 8); ++player)
            LaunchPlayer(withDebug(playerArguments(player)), false, std::format("player {}", player).c_str());
    }

    void EditorV2Layer::EndMultiplayerPlay()
    {
        m_PlayProcesses.clear();
        m_PlaySession.reset();
        m_PlaySessionEnded = false;
    }

    void EditorV2Layer::ShowPlaySettingsOverlay(UI::UIElement* anchor)
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer || !anchor)
            return;

        UI::UIContext& ctx = uiLayer->GetContext();
        auto backdrop = UI::MakeUI<UI::UIAnchorBox>();
        backdrop->WidthMode = UI::UISizeMode::Fill;
        backdrop->HeightMode = UI::UISizeMode::Fill;
        backdrop->HitTestMode = UI::UIHitTestMode::Self;

        auto card = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 8.0f);
        card->StyleClass = "Surface";
        card->DrawBackground = true;
        card->HitTestMode = UI::UIHitTestMode::Self;
        card->Padding = UI::UIThickness::All(12.0f);
        card->WidthMode = UI::UISizeMode::Fixed;
        card->HeightMode = UI::UISizeMode::Auto;
        card->FixedSize = { 310.0f, 0.0f };

        const UI::UIRect anchorBounds = anchor->GetBounds();
        float leftX = anchorBounds.Min.x;
        float topY = anchorBounds.Max.y + 4.0f;
        if (const auto& root = ctx.GetRoot())
        {
            const UI::UIRect& rootBounds = root->GetBounds();
            if (rootBounds.Width() > 330.0f)
                leftX = std::clamp(leftX, 8.0f, rootBounds.Max.x - 320.0f);
            if (rootBounds.Height() > 360.0f)
                topY = std::clamp(topY, 8.0f, rootBounds.Max.y - 340.0f);
        }
        card->AnchorMin = { 0.0f, 0.0f };
        card->AnchorMax = { 0.0f, 0.0f };
        card->AnchorOffsets = { leftX, topY, 0.0f, 0.0f };

        auto title = UI::Label("Multiplayer Play Settings").Get();
        card->AddChild(title);
        card->AddChild(UI::Separator().Get());

        auto modeLabel = UI::Label("Net Mode").Get();
        card->AddChild(modeLabel);
        std::array<Ref<UI::UIButton>, 3> modeBtns;
        std::array<Ref<UI::UIButton>, 3> emuBtns;
        Ref<UI::UISlider> playersSlider;
        Ref<UI::UILabel> playersLabel;
        Ref<UI::UITextInput> portInput;
        Ref<UI::UICheckBox> saveCheck;
        Ref<UI::UICheckBox> debugCheck;

        auto syncControls = [this, &modeBtns, &emuBtns, &playersSlider, &playersLabel, &portInput, &saveCheck, &debugCheck]() mutable
        {
            const bool netEnabled = m_PlayNet.Mode != PlayNetMode::Standalone;
            for (int i = 0; i < 3; ++i)
            {
                if (modeBtns[i])
                    modeBtns[i]->StyleVariant = (static_cast<int>(m_PlayNet.Mode) == i) ? "primary" : "";
                if (emuBtns[i])
                {
                    emuBtns[i]->StyleVariant = (m_PlayNet.Emulation == i) ? "primary" : "";
                    emuBtns[i]->SetEnabled(netEnabled);
                }
            }
            if (playersLabel)
                playersLabel->Text = std::format("Number of Players: {}", m_PlayNet.Players);
            if (playersSlider)
                playersSlider->SetEnabled(netEnabled);
            if (portInput)
                portInput->SetEnabled(netEnabled);
            if (saveCheck)
                saveCheck->SetEnabled(netEnabled);
            if (debugCheck)
                debugCheck->SetEnabled(netEnabled);
        };

        const char* modeLabels[3] = { "Standalone", "Listen Server", "Client" };
        auto modeRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
        modeRow->WidthMode = UI::UISizeMode::Fill;
        modeRow->HeightMode = UI::UISizeMode::Auto;
        for (int i = 0; i < 3; ++i)
        {
            modeBtns[i] = UI::MakeUI<UI::UIButton>(modeLabels[i]);
            modeBtns[i]->WidthMode = UI::UISizeMode::Fill;
            modeBtns[i]->Padding = UI::UIThickness::Symmetric(6.0f, 4.0f);
            modeRow->AddChild(modeBtns[i]);
        }
        card->AddChild(modeRow);

        playersLabel = UI::Label(std::format("Number of Players: {}", m_PlayNet.Players)).Get();
        card->AddChild(playersLabel);
        playersSlider = UI::MakeUI<UI::UISlider>(static_cast<float>(m_PlayNet.Players), 1.0f, 8.0f);
        playersSlider->Step = 1.0f;
        playersSlider->Format = "{:.0f}";
        playersSlider->WidthMode = UI::UISizeMode::Fill;
        playersSlider->OnChanged = [this, playersLabel](float value)
        {
            m_PlayNet.Players = std::clamp(static_cast<int>(std::round(value)), 1, 8);
            if (playersLabel)
                playersLabel->Text = std::format("Number of Players: {}", m_PlayNet.Players);
        };
        card->AddChild(playersSlider);

        card->AddChild(UI::Label("Network Emulation").Get());
        const char* emuLabels[3] = { "Off", "Average", "Bad" };
        auto emuRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
        emuRow->WidthMode = UI::UISizeMode::Fill;
        emuRow->HeightMode = UI::UISizeMode::Auto;
        for (int i = 0; i < 3; ++i)
        {
            emuBtns[i] = UI::MakeUI<UI::UIButton>(emuLabels[i]);
            emuBtns[i]->WidthMode = UI::UISizeMode::Fill;
            emuBtns[i]->Padding = UI::UIThickness::Symmetric(6.0f, 4.0f);
            emuRow->AddChild(emuBtns[i]);
        }
        card->AddChild(emuRow);

        auto portRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 8.0f);
        portRow->WidthMode = UI::UISizeMode::Fill;
        portRow->HeightMode = UI::UISizeMode::Auto;
        portRow->AddChild(UI::Label("Port").VAlign(UI::UIAlignment::Center).Get());
        portInput = UI::MakeUI<UI::UITextInput>(std::to_string(m_PlayNet.Port), "7777");
        portInput->WidthMode = UI::UISizeMode::Fill;
        portInput->OnChanged = [this](const std::string& txt)
        {
            const int parsed = std::atoi(txt.c_str());
            if (parsed > 0)
                m_PlayNet.Port = std::clamp(parsed, 1, 65535);
        };
        portRow->AddChild(portInput);
        card->AddChild(portRow);

        saveCheck = UI::MakeUI<UI::UICheckBox>("Save the scene without asking", m_PlayNet.SaveWithoutAsking);
        saveCheck->OnChanged = [this](bool v) { m_PlayNet.SaveWithoutAsking = v; };
        card->AddChild(saveCheck);

        debugCheck = UI::MakeUI<UI::UICheckBox>("Network debug log (--net-debug)", m_PlayNet.DebugTrace);
        debugCheck->OnChanged = [this](bool v) { m_PlayNet.DebugTrace = v; };
        card->AddChild(debugCheck);

        auto closeRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 6.0f);
        closeRow->WidthMode = UI::UISizeMode::Fill;
        closeRow->HeightMode = UI::UISizeMode::Auto;
        auto closeSpacer = UI::MakeUI<UI::UIElement>();
        closeSpacer->WidthMode = UI::UISizeMode::Fill;
        auto closeBtn = UI::MakeUI<UI::UIButton>("Done", [&ctx]() { ctx.ClearOverlay(); });
        closeBtn->Padding = UI::UIThickness::Symmetric(12.0f, 4.0f);
        closeRow->AddChild(closeSpacer);
        closeRow->AddChild(closeBtn);
        card->AddChild(closeRow);

        for (int i = 0; i < 3; ++i)
        {
            modeBtns[i]->OnClick = [this, i, modeBtns, emuBtns, playersSlider, playersLabel, portInput, saveCheck, debugCheck]()
            {
                m_PlayNet.Mode = static_cast<PlayNetMode>(i);
                const bool netEnabled = m_PlayNet.Mode != PlayNetMode::Standalone;
                for (int j = 0; j < 3; ++j)
                {
                    modeBtns[j]->StyleVariant = (i == j) ? "primary" : "";
                    emuBtns[j]->StyleVariant = (m_PlayNet.Emulation == j) ? "primary" : "";
                    emuBtns[j]->SetEnabled(netEnabled);
                }
                playersLabel->Text = std::format("Number of Players: {}", m_PlayNet.Players);
                playersSlider->SetEnabled(netEnabled);
                portInput->SetEnabled(netEnabled);
                saveCheck->SetEnabled(netEnabled);
                debugCheck->SetEnabled(netEnabled);
            };
            emuBtns[i]->OnClick = [this, i, emuBtns]()
            {
                m_PlayNet.Emulation = i;
                for (int j = 0; j < 3; ++j)
                    emuBtns[j]->StyleVariant = (i == j) ? "primary" : "";
            };
        }

        syncControls();

        UI::UIElement* cardPtr = card.get();
        backdrop->OnMouseDown = [cardPtr, &ctx](const UI::UIPointerEvent& e)
        {
            if (!cardPtr->GetBounds().Contains(e.Position))
            {
                ctx.ClearOverlay();
                return true;
            }
            return false;
        };
        backdrop->AddChild(card);
        ctx.SetOverlay(backdrop);
    }

    void EditorV2Layer::ShowSaveAndPlayOverlay()
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer)
            return;

        UI::UIContext& ctx = uiLayer->GetContext();
        auto backdrop = UI::MakeUI<UI::UIAnchorBox>();
        backdrop->WidthMode = UI::UISizeMode::Fill;
        backdrop->HeightMode = UI::UISizeMode::Fill;
        backdrop->HitTestMode = UI::UIHitTestMode::Self;
        backdrop->DrawBackground = true;
        backdrop->StyleClass = "Backdrop";

        auto card = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 10.0f);
        card->StyleClass = "Surface";
        card->DrawBackground = true;
        card->HitTestMode = UI::UIHitTestMode::Self;
        card->Padding = UI::UIThickness::All(16.0f);
        card->WidthMode = UI::UISizeMode::Fixed;
        card->HeightMode = UI::UISizeMode::Auto;
        card->FixedSize = { 390.0f, 0.0f };
        card->AnchorMin = { 0.5f, 0.5f };
        card->AnchorMax = { 0.5f, 0.5f };
        card->AnchorOffsets = { -195.0f, -95.0f, 195.0f, 95.0f };

        card->AddChild(UI::Label("Save and Play").Get());
        auto desc = UI::Label("Multiplayer Play starts the other players (NoxRuntime) on the saved scene: the scene is saved first.").Get();
        desc->WordWrap = true;
        card->AddChild(desc);

        auto dontAsk = UI::CheckBox("Don't ask again (this session)", m_PlayNet.SaveWithoutAsking,
                                    [this](bool checked) { m_PlayNet.SaveWithoutAsking = checked; }).Get();
        card->AddChild(dontAsk);

        auto btnRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 8.0f);
        btnRow->WidthMode = UI::UISizeMode::Fill;
        btnRow->HeightMode = UI::UISizeMode::Auto;
        auto spacer = UI::MakeUI<UI::UIElement>();
        spacer->WidthMode = UI::UISizeMode::Fill;
        auto saveBtn = UI::MakeUI<UI::UIButton>("Save and Play", [this, &ctx]()
        {
            ctx.ClearOverlay();
            SaveScene();
            BeginMultiplayerPlay();
            RefreshToolBar();
        });
        saveBtn->StyleVariant = "primary";
        saveBtn->Padding = UI::UIThickness::Symmetric(12.0f, 5.0f);

        auto cancelBtn = UI::MakeUI<UI::UIButton>("Cancel", [this, &ctx]()
        {
            m_PlayNet.SaveWithoutAsking = false;
            ctx.ClearOverlay();
        });
        cancelBtn->Padding = UI::UIThickness::Symmetric(12.0f, 5.0f);

        btnRow->AddChild(spacer);
        btnRow->AddChild(saveBtn);
        btnRow->AddChild(cancelBtn);
        card->AddChild(btnRow);

        backdrop->AddChild(card);
        ctx.SetOverlay(backdrop);
    }

    void EditorV2Layer::OnSceneSimulate()
    {
        m_EditorSceneReferencesValid = false;
        if (m_SceneState == SceneState::Play)
            OnSceneStop();
        m_SceneState = SceneState::Simulate;
        m_HoveredEntity = Entity();
        m_ActiveScene = Scene::Copy(m_EditorScene);
        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
        m_ActiveScene->SetRenderer(m_Renderer);
        m_ActiveScene->SetRenderer2D(m_Renderer2D);
        const glm::dvec3 streamingSource = m_EditorCamera.GetPosition();
        m_ActiveScene->OnSimulationStart(&streamingSource);
        RefreshToolBar();
    }

    void EditorV2Layer::OnSceneStop()
    {
        const bool started = !(m_SceneState == SceneState::Play && m_PlayWaitingForServer);
        EndMultiplayerPlay();
        m_PlayWaitingForServer = false;
        if (m_SceneState == SceneState::Play && started)
            m_ActiveScene->OnRuntimeStop();
        else if (m_SceneState == SceneState::Simulate)
            m_ActiveScene->OnSimulationStop();

        m_SceneState = SceneState::Edit;
        if (Input::IsRelativeMouseMode())
            Input::SetRelativeMouseMode(false);
        Input::ClearInjected();
        m_HoveredEntity = Entity();
        m_ActiveScene = m_EditorScene;
        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
        m_UnloadUnusedAssetsRequested = true;
        RefreshToolBar();
    }

    void EditorV2Layer::OnScenePause()
    {
        if (m_SceneState != SceneState::Edit && m_ActiveScene)
            m_ActiveScene->SetPaused(true);
    }

    void EditorV2Layer::LoadViewportPrefs()
    {
        std::ifstream in(kViewportPrefsPath);
        if (!in.is_open())
            return;
        for (std::string line; std::getline(in, line);)
        {
            int index = 0, open = 0, view = 0, grid = 1, resolution = 0, realtime = 1, cameraPos = 0;
            unsigned flags = Renderer::ExtraViewOutline;
            const int read = sscanf(line.c_str(), "Viewport%d=%d,%d,%u,%d,%d,%d,%d",
                                    &index, &open, &view, &flags, &grid, &resolution, &realtime, &cameraPos);
            if (read >= 3 && index >= 0 && index < kMaxViewports && view >= 0 && view <= static_cast<int>(EditorViewMode::Right))
            {
                ViewportSlot& slot = m_Viewports[index];
                slot.Open = (index == 0) || (open != 0);
                slot.Camera.SetViewMode(static_cast<EditorViewMode>(view));
                if (read >= 7)
                {
                    slot.ShowFlags = flags;
                    slot.ShowGrid = (grid != 0);
                    slot.ResolutionIndex = std::clamp(resolution, 0, 2);
                    slot.Realtime = (realtime != 0);
                    slot.ShowCameraPosition = (read == 8 && cameraPos != 0);
                }
                if (index == 0)
                    m_EditorCamera.SetViewMode(static_cast<EditorViewMode>(view));
            }
        }
    }

    void EditorV2Layer::SaveViewportPrefs() const
    {
        std::error_code error;
        std::filesystem::path p(kViewportPrefsPath);
        if (!p.parent_path().empty())
            std::filesystem::create_directories(p.parent_path(), error);
        std::ofstream out(kViewportPrefsPath, std::ios::trunc);
        if (!out.is_open())
            return;
        for (int index = 0; index < kMaxViewports; ++index)
        {
            const ViewportSlot& slot = m_Viewports[index];
            const int view = static_cast<int>(index == m_ActiveViewport ? m_EditorCamera.GetViewMode() : slot.Camera.GetViewMode());
            out << "Viewport" << index << "="
                << (slot.Open ? 1 : 0) << ","
                << view << ","
                << slot.ShowFlags << ","
                << (slot.ShowGrid ? 1 : 0) << ","
                << slot.ResolutionIndex << ","
                << (slot.Realtime ? 1 : 0) << ","
                << (slot.ShowCameraPosition ? 1 : 0) << "\n";
        }
    }

    void EditorV2Layer::OpenViewport(int index)
    {
        if (index < 0 || index >= kMaxViewports)
            return;
        ViewportSlot& slot = m_Viewports[index];
        if (!slot.CameraSet)
        {
            const EditorViewMode keepMode = slot.Camera.GetViewMode();
            slot.Camera = m_EditorCamera;
            slot.Camera.SetViewMode(keepMode);
            slot.CameraSet = true;
        }
        slot.Open = true;

        if (index > 0)
        {
            if (m_ViewportSlotRoots[index] && m_ExtraViewportHosts[index] && index != m_ActiveViewport)
            {
                m_ViewportSlotRoots[index]->ClearChildren();
                m_ViewportSlotRoots[index]->AddChild(m_ExtraViewportHosts[index]);
            }
            if (UILayer* uiLayer = Application::Get().GetUILayer())
            {
                if (const auto& dock = uiLayer->GetDockSpaceRef())
                {
                    if (!dock->FindNodeByPanelTitle(slot.Title))
                    {
                        const UI::UIDockDropZone zone = (index == 2) ? UI::UIDockDropZone::Bottom : UI::UIDockDropZone::Right;
                        dock->AddPanel(slot.Title, m_ViewportSlotRoots[index], zone, "Viewport", 0.5f);
                    }
                }
            }
        }
    }

    void EditorV2Layer::CloseViewport(int index)
    {
        if (index <= 0 || index >= kMaxViewports)
            return;
        if (m_ActiveViewport == index)
            ActivateViewport(0);

        ViewportSlot& slot = m_Viewports[index];
        slot.Open = false;
        slot.Hovered = false;
        slot.Size = { 0, 0 };
        slot.DrawnSize = { 0, 0 };
        if (m_Renderer && m_Renderer->GetExtraViewTexture(static_cast<uint32_t>(index)))
            m_Renderer->ReleaseExtraView(static_cast<uint32_t>(index));
        if (m_ExtraViewportImages[index])
            m_ExtraViewportImages[index]->TextureDescriptorSlot = ~0u;

        if (UILayer* uiLayer = Application::Get().GetUILayer())
        {
            if (const auto& dock = uiLayer->GetDockSpaceRef())
                dock->RemovePanel(slot.Title);
        }
    }

    void EditorV2Layer::ActivateViewport(int index)
    {
        if (index < 0 || index >= kMaxViewports || index == m_ActiveViewport)
            return;

        ViewportSlot& previous = m_Viewports[m_ActiveViewport];
        ViewportSlot& next = m_Viewports[index];
        if (!next.Open)
            OpenViewport(index);
        if (!next.CameraSet)
        {
            const EditorViewMode keepMode = next.Camera.GetViewMode();
            next.Camera = m_EditorCamera;
            next.Camera.SetViewMode(keepMode);
            next.CameraSet = true;
        }

        const bool wasOrthographic = m_EditorCamera.IsOrthographic();
        const int prevIdx = m_ActiveViewport;
        previous.Camera = m_EditorCamera;
        previous.CameraSet = true;
        previous.DrawnSize = { 0, 0 };
        m_EditorCamera = next.Camera;
        m_ActiveViewport = index;

        if (m_ViewportSlotRoots[prevIdx] && m_ExtraViewportHosts[prevIdx])
        {
            m_ViewportSlotRoots[prevIdx]->ClearChildren();
            m_ViewportSlotRoots[prevIdx]->AddChild(m_ExtraViewportHosts[prevIdx]);
        }
        if (m_ViewportSlotRoots[index] && m_ActiveViewportHost)
        {
            m_ViewportSlotRoots[index]->ClearChildren();
            m_ViewportSlotRoots[index]->AddChild(m_ActiveViewportHost);
            const UI::UIRect& b = m_ViewportSlotRoots[index]->GetBounds();
            if (b.Width() > 1.0f && b.Height() > 1.0f)
                m_EditorCamera.SetViewportSize(b.Width(), b.Height());
        }

        m_HoveredEntity = Entity();
        m_ViewportTools.IsBoxSelecting = false;
        SyncOrthoRenderSettings(wasOrthographic);
        if (m_Renderer)
            m_Renderer->ResetCameraHistory();
        m_ViewportToolbar.Sync();
    }

    void EditorV2Layer::ApplyViewportLayout(ViewportLayout layout)
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer || !uiLayer->GetDockSpaceRef())
            return;

        const auto& dock = uiLayer->GetDockSpaceRef();
        int count = 1;
        switch (layout)
        {
        case ViewportLayout::One: count = 1; break;
        case ViewportLayout::TwoSideBySide:
        case ViewportLayout::TwoStacked: count = 2; break;
        case ViewportLayout::Three: count = 3; break;
        case ViewportLayout::Four: count = 4; break;
        }

        if (m_ActiveViewport >= count)
            ActivateViewport(0);

        for (int index = 1; index < kMaxViewports; ++index)
        {
            dock->RemovePanel(m_Viewports[index].Title);
            m_Viewports[index].Open = false;
            m_Viewports[index].Hovered = false;
            m_Viewports[index].Size = { 0, 0 };
            m_Viewports[index].DrawnSize = { 0, 0 };
            if (m_Renderer && m_Renderer->GetExtraViewTexture(static_cast<uint32_t>(index)))
                m_Renderer->ReleaseExtraView(static_cast<uint32_t>(index));
            if (m_ExtraViewportImages[index])
                m_ExtraViewportImages[index]->TextureDescriptorSlot = ~0u;
        }

        if (!dock->FindNodeByPanelTitle("Viewport") && m_ViewportSlotRoots[0])
            dock->AddPanel("Viewport", m_ViewportSlotRoots[0], UI::UIDockDropZone::Center, {}, 0.5f, UI::UIDockNodeFlags::CentralNode);

        auto prepareSlot = [this](int index)
        {
            ViewportSlot& slot = m_Viewports[index];
            if (!slot.CameraSet)
            {
                const EditorViewMode keepMode = slot.Camera.GetViewMode();
                slot.Camera = m_EditorCamera;
                slot.Camera.SetViewMode(keepMode);
                slot.CameraSet = true;
            }
            slot.Open = true;
            if (index != m_ActiveViewport && m_ViewportSlotRoots[index] && m_ExtraViewportHosts[index])
            {
                m_ViewportSlotRoots[index]->ClearChildren();
                m_ViewportSlotRoots[index]->AddChild(m_ExtraViewportHosts[index]);
            }
        };

        for (int i = 1; i < count; ++i)
            prepareSlot(i);

        switch (layout)
        {
        case ViewportLayout::One:
            break;
        case ViewportLayout::TwoSideBySide:
            dock->AddPanel(m_Viewports[1].Title, m_ViewportSlotRoots[1], UI::UIDockDropZone::Right, "Viewport", 0.5f);
            break;
        case ViewportLayout::TwoStacked:
            dock->AddPanel(m_Viewports[1].Title, m_ViewportSlotRoots[1], UI::UIDockDropZone::Bottom, "Viewport", 0.5f);
            break;
        case ViewportLayout::Three:
            dock->AddPanel(m_Viewports[1].Title, m_ViewportSlotRoots[1], UI::UIDockDropZone::Right, "Viewport", 0.4f);
            dock->AddPanel(m_Viewports[2].Title, m_ViewportSlotRoots[2], UI::UIDockDropZone::Bottom, m_Viewports[1].Title, 0.5f);
            break;
        case ViewportLayout::Four:
            dock->AddPanel(m_Viewports[1].Title, m_ViewportSlotRoots[1], UI::UIDockDropZone::Right, "Viewport", 0.5f);
            dock->AddPanel(m_Viewports[2].Title, m_ViewportSlotRoots[2], UI::UIDockDropZone::Bottom, "Viewport", 0.5f);
            dock->AddPanel(m_Viewports[3].Title, m_ViewportSlotRoots[3], UI::UIDockDropZone::Bottom, m_Viewports[1].Title, 0.5f);
            break;
        }
    }

    int EditorV2Layer::HoveredViewport() const
    {
        for (int index = 0; index < kMaxViewports; ++index)
        {
            if (m_Viewports[index].Open && m_Viewports[index].Hovered)
                return index;
        }
        return -1;
    }

    int EditorV2Layer::OpenViewportCount() const
    {
        int count = 0;
        for (const ViewportSlot& slot : m_Viewports)
            count += slot.Open ? 1 : 0;
        return count;
    }

    void EditorV2Layer::SubmitExtraViewports(Timestep ts)
    {
        if (!m_Renderer)
            return;
        const float cell = std::max(m_ViewportTools.LocationSnapWorldUnits(), WorldUnits::FromMeters(1.0f));
        for (int index = 0; index < kMaxViewports; ++index)
        {
            ViewportSlot& slot = m_Viewports[index];
            slot.Submitted = false;
            if (index == m_ActiveViewport || !slot.Open || slot.Size.x == 0 || slot.Size.y == 0)
            {
                slot.DrawnSize = { 0, 0 };
                continue;
            }

            const float width = static_cast<float>(slot.Size.x);
            const float height = static_cast<float>(slot.Size.y);
            if (slot.Camera.GetAspectRatio() != width / height)
                slot.Camera.SetViewportSize(width, height);
            slot.Camera.OnUpdate(ts, false);

            const uint32_t divisor = 1u << std::clamp(slot.ResolutionIndex, 0, 2);
            const glm::uvec2 drawSize = glm::max(slot.Size / divisor, glm::uvec2(1));
            uint32_t flags = slot.ShowFlags;

            const glm::mat4 viewProjection = slot.Camera.GetProjection() * slot.Camera.GetViewMatrix(m_Renderer->GetRenderOrigin());
            size_t selection = m_SceneHierarchyPanel.GetSelectedEntities().size();
            for (const Entity& selected : m_SceneHierarchyPanel.GetSelectedEntities())
                selection = selection * 31 + static_cast<uint32_t>(selected);
            slot.SinceDrawn += ts.GetSeconds();
            if (!slot.Realtime && m_SceneState == SceneState::Edit && slot.DrawnSize == drawSize && slot.DrawnFlags == flags &&
                slot.DrawnViewProjection == viewProjection && slot.DrawnHistory == m_History.GetPosition() && slot.DrawnSelection == selection &&
                !m_ViewportTools.GizmoUsing && slot.SinceDrawn < 2.0f && m_Renderer->GetExtraViewTexture(static_cast<uint32_t>(index)))
            {
                slot.Submitted = true;
                continue;
            }

            slot.DrawnSize = drawSize;
            slot.DrawnFlags = flags;
            slot.DrawnViewProjection = viewProjection;
            slot.DrawnHistory = m_History.GetPosition();
            slot.DrawnSelection = selection;
            slot.SinceDrawn = 0.0f;

            shaderio::PushConstantGrid grid{};
            const bool showGrid = (m_SceneState != SceneState::Play && m_ViewportTools.ShowGrid && slot.ShowGrid);
            if (showGrid)
            {
                const EditorViewMode mode = slot.Camera.GetViewMode();
                const int axis = (mode == EditorViewMode::Front || mode == EditorViewMode::Back) ? 2 :
                                 (mode == EditorViewMode::Left || mode == EditorViewMode::Right) ? 0 : 1;
                grid = slot.Camera.IsOrthographic()
                    ? Renderer2D::MakeGrid(slot.Camera.GetFocalPoint(), m_Renderer->GetRenderOrigin(), cell, axis, slot.Camera.GetOrthoHalfHeight())
                    : Renderer2D::MakeGrid(slot.Camera.GetPosition(), m_Renderer->GetRenderOrigin(), cell, 1, slot.Camera.GetDistance());
            }
            if (m_SceneState == SceneState::Play)
                flags &= ~Renderer::ExtraViewOutline;

            m_Renderer->SubmitExtraView(static_cast<uint32_t>(index), slot.Camera, NRI::Extent2D{ drawSize.x, drawSize.y }, showGrid ? &grid : nullptr, false, flags);
            slot.Submitted = true;
        }
    }

    std::vector<UI::UIMenuItem> EditorV2Layer::BuildViewportOptionsItems(int index)
    {
        ViewportSlot& slot = m_Viewports[index];
        auto toggleFlag = [&slot](uint32_t bit, bool inverted)
        {
            const bool on = ((slot.ShowFlags & bit) != 0) != inverted;
            slot.ShowFlags = (!on != inverted) ? (slot.ShowFlags | bit) : (slot.ShowFlags & ~bit);
        };
        const bool lit = (slot.ShowFlags & Renderer::ExtraViewUnlit) == 0;
        const bool wf = (slot.ShowFlags & Renderer::ExtraViewWireframe) != 0;
        const bool sky = (slot.ShowFlags & Renderer::ExtraViewNoSky) == 0;
        const bool particles = (slot.ShowFlags & Renderer::ExtraViewNoParticles) == 0;
        const bool outline = (slot.ShowFlags & Renderer::ExtraViewOutline) != 0;

        return {
            {"While it is not the active viewport (full render)", "", {}, false, false, false, true},
            {"Lit", "", [&slot]() { slot.ShowFlags &= ~Renderer::ExtraViewUnlit; }, false, true, lit},
            {"Unlit", "", [&slot]() { slot.ShowFlags |= Renderer::ExtraViewUnlit; }, false, true, !lit, false,
             "Base colors without lighting or exposure."},
            {"", "", {}, true},
            {"Wireframe", "", [toggleFlag]() mutable { toggleFlag(Renderer::ExtraViewWireframe, false); }, false, true, wf, false,
             "The triangles' edges over the image."},
            {"Sky", "", [toggleFlag]() mutable { toggleFlag(Renderer::ExtraViewNoSky, true); }, false, true, sky, false,
             "The sky behind the geometry (off: black)."},
            {"Particles", "", [toggleFlag]() mutable { toggleFlag(Renderer::ExtraViewNoParticles, true); }, false, true, particles, false,
             "The scene's particle systems."},
            {"Selection Outline", "", [toggleFlag]() mutable { toggleFlag(Renderer::ExtraViewOutline, false); }, false, true, outline, false,
             "The orange outline of the selected entities."},
            {"Grid", "", [&slot]() { slot.ShowGrid = !slot.ShowGrid; }, false, true, slot.ShowGrid},
            {"", "", {}, true},
            {"Full Resolution", "", [&slot]() { slot.ResolutionIndex = 0; }, false, true, slot.ResolutionIndex == 0},
            {"Half Resolution", "", [&slot]() { slot.ResolutionIndex = 1; }, false, true, slot.ResolutionIndex == 1},
            {"Quarter Resolution", "", [&slot]() { slot.ResolutionIndex = 2; }, false, true, slot.ResolutionIndex == 2},
            {"", "", {}, true},
            {"Camera Position", "", [&slot]() { slot.ShowCameraPosition = !slot.ShowCameraPosition; }, false, true, slot.ShowCameraPosition, false,
             "Show world-unit coordinates next to the axis indicator."},
            {"Realtime", "", [&slot]() { slot.Realtime = !slot.Realtime; }, false, true, slot.Realtime, false,
             "Off: drawn again only when its camera, the scene or the selection change."}
        };
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
        const float cell = std::max(m_ViewportTools.LocationSnapWorldUnits(), WorldUnits::FromMeters(1.0f));
        if (m_EditorCamera.IsOrthographic())
            m_Renderer2D->DrawGrid(m_EditorCamera.GetFocalPoint(), m_Renderer->GetRenderOrigin(), cell, GridPlaneAxis(), m_EditorCamera.GetOrthoHalfHeight());
        else
            m_Renderer2D->DrawGrid(m_EditorCamera.GetPosition(), m_Renderer->GetRenderOrigin(), cell, 1, m_EditorCamera.GetDistance());
    }

    void EditorV2Layer::DrawReferenceFigure(const glm::dvec3& worldPosition)
    {
        if (!m_Renderer2D || !m_Renderer)
            return;
        const glm::vec3 position = m_Renderer->ToRenderSpace(worldPosition);
        auto cm = [](float centimeters) { return WorldUnits::FromCentimeters(centimeters); };
        const glm::vec4 color(1.0f, 0.62f, 0.2f, 0.9f);

        auto box = [&](float minX, float maxX, float minY, float maxY, float minZ, float maxZ)
        {
            const glm::vec3 low = position + glm::vec3(cm(minX), cm(minY), cm(minZ));
            const glm::vec3 high = position + glm::vec3(cm(maxX), cm(maxY), cm(maxZ));
            const glm::vec3 corners[8] = {
                { low.x, low.y, low.z }, { high.x, low.y, low.z }, { high.x, low.y, high.z }, { low.x, low.y, high.z },
                { low.x, high.y, low.z }, { high.x, high.y, low.z }, { high.x, high.y, high.z }, { low.x, high.y, high.z }
            };
            constexpr int edges[12][2] = { {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7} };
            for (const auto& edge : edges)
                m_Renderer2D->DrawThinLine(corners[edge[0]], corners[edge[1]], color);
        };

        box(-18.0f, -1.0f, 0.0f, 88.0f, -10.0f, 10.0f);
        box(1.0f, 18.0f, 0.0f, 88.0f, -10.0f, 10.0f);
        box(-22.0f, 22.0f, 88.0f, 143.0f, -11.0f, 11.0f);
        box(-31.0f, -22.0f, 83.0f, 143.0f, -5.0f, 5.0f);
        box(22.0f, 31.0f, 83.0f, 143.0f, -5.0f, 5.0f);
        box(-4.0f, 4.0f, 143.0f, 151.0f, -4.0f, 4.0f);
        box(-10.0f, 10.0f, 151.0f, 180.0f, -12.0f, 12.0f);
    }

    namespace
    {
        glm::vec3 DecomposeWorldScale(const glm::mat4& worldMatrix)
        {
            return glm::vec3(
                glm::length(glm::vec3(worldMatrix[0])),
                glm::length(glm::vec3(worldMatrix[1])),
                glm::length(glm::vec3(worldMatrix[2])));
        }

        glm::mat4 RigidPart(const glm::mat4& worldMatrix)
        {
            glm::mat4 result = worldMatrix;
            for (int c = 0; c < 3; ++c)
            {
                const float l = glm::length(glm::vec3(worldMatrix[c]));
                if (l > 1e-6f)
                    result[c] = glm::vec4(glm::vec3(worldMatrix[c]) / l, 0.0f);
            }
            return result;
        }
    }

    void EditorV2Layer::DrawPhysicsColliders3D()
    {
        if (!m_ActiveScene || !m_Renderer || !m_Renderer2D)
            return;

        constexpr glm::vec4 kActiveColor(0.2f, 1.0f, 0.3f, 0.9f);
        constexpr glm::vec4 kInactiveColor(1.0f, 0.3f, 0.25f, 0.9f);
        constexpr glm::vec4 kCharacterColor(0.3f, 0.75f, 1.0f, 0.9f);
        constexpr int kCircleSegments = 32;

        auto colorOf = [this, kActiveColor, kInactiveColor](entt::entity handle)
        {
            for (Entity entity(handle, m_ActiveScene.get()); entity; entity = entity.GetParent())
            {
                if (entity.HasComponent<RigidBody3DComponent>())
                    return kActiveColor;
            }
            return kInactiveColor;
        };
        auto line = [this](const glm::vec3& a, const glm::vec3& b, const glm::vec4& color) { m_Renderer2D->DrawXRayLine(a, b, color); };

        auto drawCircle = [&](const glm::mat4& transform, const glm::vec3& centre, float radius, int axis, const glm::vec4& color)
        {
            const int a = (axis + 1) % 3, b = (axis + 2) % 3;
            glm::vec3 previous(0.0f);
            for (int i = 0; i <= kCircleSegments; ++i)
            {
                const float t = glm::two_pi<float>() * static_cast<float>(i) / kCircleSegments;
                glm::vec3 local = centre;
                local[a] += std::cos(t) * radius;
                local[b] += std::sin(t) * radius;
                const glm::vec3 point = glm::vec3(transform * glm::vec4(local, 1.0f));
                if (i > 0)
                    line(previous, point, color);
                previous = point;
            }
        };

        auto drawCapsule = [&](const glm::mat4& transform, const glm::vec3& centre, float halfHeight, float top, float bottom, const glm::vec4& color)
        {
            const float sine = halfHeight > 1e-6f ? std::clamp((bottom - top) / (2.0f * halfHeight), -1.0f, 1.0f) : 0.0f;
            const float tangent = std::asin(sine);
            const glm::vec3 topCentre = centre + glm::vec3(0.0f, halfHeight, 0.0f);
            const glm::vec3 bottomCentre = centre - glm::vec3(0.0f, halfHeight, 0.0f);
            auto at = [&](const glm::vec3& c, float radius, const glm::vec3& across, float angle)
            {
                return glm::vec3(transform * glm::vec4(c + radius * (std::cos(angle) * across + std::sin(angle) * glm::vec3(0.0f, 1.0f, 0.0f)), 1.0f));
            };
            auto arc = [&](const glm::vec3& c, float radius, const glm::vec3& across, float from, float to)
            {
                constexpr int kSegments = 16;
                glm::vec3 previous = at(c, radius, across, from);
                for (int i = 1; i <= kSegments; ++i)
                {
                    const glm::vec3 point = at(c, radius, across, from + (to - from) * static_cast<float>(i) / kSegments);
                    line(previous, point, color);
                    previous = point;
                }
            };
            for (const glm::vec3& across : { glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f) })
            {
                arc(topCentre, top, across, tangent, glm::pi<float>() - tangent);
                arc(bottomCentre, bottom, across, glm::pi<float>() - tangent, glm::two_pi<float>() + tangent);
                line(at(topCentre, top, across, tangent), at(bottomCentre, bottom, across, tangent), color);
                line(at(topCentre, top, across, glm::pi<float>() - tangent), at(bottomCentre, bottom, across, glm::pi<float>() - tangent), color);
            }
            drawCircle(transform, topCentre + glm::vec3(0.0f, top * sine, 0.0f), top * std::cos(tangent), 1, color);
            drawCircle(transform, bottomCentre + glm::vec3(0.0f, bottom * sine, 0.0f), bottom * std::cos(tangent), 1, color);
        };

        auto boxView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, BoxCollider3DComponent>();
        for (auto entity : boxView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, box] = boxView.get<WorldTransformComponent, BoxCollider3DComponent>(entity);
            const glm::vec4 color = colorOf(entity);
            const glm::vec3 halfExtents = box.HalfExtents * DecomposeWorldScale(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            const glm::mat4 transform = RigidPart(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            glm::vec3 corners[8];
            for (int i = 0; i < 8; ++i)
            {
                const glm::vec3 sign((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
                corners[i] = glm::vec3(transform * glm::vec4(box.Offset + sign * halfExtents, 1.0f));
            }
            constexpr int edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
            for (const auto& edge : edges)
                line(corners[edge[0]], corners[edge[1]], color);
        }

        auto sphereView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, SphereCollider3DComponent>();
        for (auto entity : sphereView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, sphere] = sphereView.get<WorldTransformComponent, SphereCollider3DComponent>(entity);
            const glm::vec4 color = colorOf(entity);
            const glm::vec3 worldScale = DecomposeWorldScale(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            const float radius = sphere.Radius * std::max({ worldScale.x, worldScale.y, worldScale.z });
            const glm::mat4 transform = RigidPart(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            for (int axis = 0; axis < 3; ++axis)
                drawCircle(transform, sphere.Offset, radius, axis, color);
        }

        auto capsuleView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, CapsuleCollider3DComponent>();
        for (auto entity : capsuleView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, capsule] = capsuleView.get<WorldTransformComponent, CapsuleCollider3DComponent>(entity);
            const glm::vec3 worldScale = DecomposeWorldScale(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            const float horizontal = std::max(worldScale.x, worldScale.z);
            const float top = capsule.Radius * horizontal;
            const float bottom = capsule.BottomRadius > 0.0f ? capsule.BottomRadius * horizontal : top;
            drawCapsule(RigidPart(m_Renderer->ToRenderSpace(wtc.WorldMatrix)), capsule.Offset, capsule.HalfHeight * worldScale.y, top, bottom, colorOf(entity));
        }

        auto characterView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, CharacterController3DComponent>();
        for (auto entity : characterView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, controller] = characterView.get<WorldTransformComponent, CharacterController3DComponent>(entity);
            const float radius = std::max(controller.Radius, WorldUnits::FromCentimeters(1.0f));
            const float halfHeight = 0.5f * std::max(controller.Height, 0.0f);
            const glm::mat4 transform = glm::translate(glm::mat4(1.0f), m_Renderer->ToRenderSpace(wtc.GetPosition()));
            drawCapsule(transform, glm::vec3(0.0f, halfHeight + radius, 0.0f), halfHeight, radius, radius, kCharacterColor);
        }

        auto cylinderView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, CylinderCollider3DComponent>();
        for (auto entity : cylinderView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, cylinder] = cylinderView.get<WorldTransformComponent, CylinderCollider3DComponent>(entity);
            const glm::vec4 color = colorOf(entity);
            const glm::vec3 worldScale = DecomposeWorldScale(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            const float halfHeight = cylinder.HalfHeight * worldScale.y;
            const float horizontal = std::max(worldScale.x, worldScale.z);
            const float top = cylinder.Radius * horizontal;
            const float bottom = (cylinder.BottomRadius > 0.0f ? cylinder.BottomRadius : cylinder.Radius) * horizontal;
            const glm::mat4 transform = RigidPart(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            const glm::vec3 topCentre = cylinder.Offset + glm::vec3(0.0f, halfHeight, 0.0f);
            const glm::vec3 bottomCentre = cylinder.Offset - glm::vec3(0.0f, halfHeight, 0.0f);
            if (top > 0.0f)
                drawCircle(transform, topCentre, top, 1, color);
            drawCircle(transform, bottomCentre, bottom, 1, color);
            for (int i = 0; i < 8; ++i)
            {
                const float t = glm::quarter_pi<float>() * static_cast<float>(i);
                const glm::vec3 direction(std::cos(t), 0.0f, std::sin(t));
                line(glm::vec3(transform * glm::vec4(topCentre + direction * top, 1.0f)),
                     glm::vec3(transform * glm::vec4(bottomCentre + direction * bottom, 1.0f)), color);
            }
        }

        glm::dvec3 viewPosition = m_EditorCamera.GetPosition();
        if (m_SceneState == SceneState::Play)
        {
            if (Entity camera = m_ActiveScene->GetPrimaryCameraEntity())
                viewPosition = camera.GetComponent<WorldTransformComponent>().GetPosition();
        }
        const glm::vec3 eye = m_Renderer->ToRenderSpace(viewPosition);
        auto planeView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, PlaneCollider3DComponent>();
        for (auto entity : planeView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, plane] = planeView.get<WorldTransformComponent, PlaneCollider3DComponent>(entity);
            const glm::vec4 color = colorOf(entity);
            const glm::vec3 worldScale = DecomposeWorldScale(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            const glm::mat4 transform = RigidPart(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            auto point = [&](float x, float y, float z) { return glm::vec3(transform * glm::vec4(plane.Offset + glm::vec3(x, y, z), 1.0f)); };
            if (plane.HalfSize.x > 0.0f && plane.HalfSize.y > 0.0f)
            {
                const float x = plane.HalfSize.x * worldScale.x, z = plane.HalfSize.y * worldScale.z;
                line(point(-x, 0, -z), point(x, 0, -z), color);
                line(point(x, 0, -z), point(x, 0, z), color);
                line(point(x, 0, z), point(-x, 0, z), color);
                line(point(-x, 0, z), point(-x, 0, -z), color);
                line(point(0, 0, 0), point(0, WorldUnits::FromMeters(1.0f), 0), color);
                continue;
            }
            glm::vec4 endlessColor = color;
            for (Entity owner(entity, m_ActiveScene.get()); owner; owner = owner.GetParent())
            {
                if (owner.HasComponent<RigidBody3DComponent>())
                {
                    if (owner.GetComponent<RigidBody3DComponent>().Type == RigidBody3DComponent::BodyType::Dynamic)
                        endlessColor = kInactiveColor;
                    break;
                }
            }
            const glm::vec3 origin = point(0, 0, 0);
            const glm::vec3 alongX = glm::vec3(transform[0]), alongZ = glm::vec3(transform[2]);
            const float step = WorldUnits::FromMeters(5.0f);
            const float reach = WorldUnits::FromMeters(50.0f);
            const float nearX = std::round(glm::dot(eye - origin, alongX) / step) * step;
            const float nearZ = std::round(glm::dot(eye - origin, alongZ) / step) * step;
            for (float offset = -reach; offset <= reach + 0.5f * step; offset += step)
            {
                line(point(nearX - reach, 0, nearZ + offset), point(nearX + reach, 0, nearZ + offset), endlessColor);
                line(point(nearX + offset, 0, nearZ - reach), point(nearX + offset, 0, nearZ + reach), endlessColor);
            }
            line(origin, point(0, WorldUnits::FromMeters(1.0f), 0), endlessColor);
        }

        auto animatorView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, AnimatorComponent>();
        for (auto entity : animatorView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, animator] = animatorView.get<WorldTransformComponent, AnimatorComponent>(entity);
            const AnimationGraphInstance& graph = animator.GraphInstance;
            if (graph.FootProbes.empty())
                continue;
            const glm::mat4 world = m_Renderer->ToRenderSpace(wtc.WorldMatrix);
            const float meter = WorldUnits::FromMeters(1.0f);
            for (const AnimFootProbe& probe : graph.FootProbes)
            {
                const glm::vec3 foot = glm::vec3(world * glm::vec4(probe.Position, 1.0f));
                const glm::vec3 start = foot + glm::vec3(0.0f, probe.Up * meter, 0.0f);
                const glm::vec3 end = foot - glm::vec3(0.0f, probe.Down * meter, 0.0f);
                const auto ground = graph.FootGround.find(probe.Bone);
                if (ground == graph.FootGround.end() || !ground->second.Hit)
                {
                    line(start, end, glm::vec4(1.0f, 0.25f, 0.2f, 1.0f));
                    continue;
                }
                const glm::vec3 hit = glm::vec3(world * glm::vec4(ground->second.Point, 1.0f));
                const glm::vec4 green(0.2f, 1.0f, 0.3f, 1.0f);
                line(start, hit, green);
                const float c = WorldUnits::FromCentimeters(4.0f);
                line(hit - glm::vec3(c, 0, 0), hit + glm::vec3(c, 0, 0), green);
                line(hit - glm::vec3(0, 0, c), hit + glm::vec3(0, 0, c), green);
                line(hit, hit + glm::vec3(world * glm::vec4(ground->second.Normal, 0.0f)) * (3.0f * c), green);
            }
        }

        auto meshView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, MeshCollider3DComponent>();
        for (auto entity : meshView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, mesh] = meshView.get<WorldTransformComponent, MeshCollider3DComponent>(entity);
            if (mesh.Mesh == 0)
                continue;
            bool dynamic = false;
            for (Entity owner(entity, m_ActiveScene.get()); owner; owner = owner.GetParent())
            {
                if (owner.HasComponent<RigidBody3DComponent>())
                {
                    dynamic = owner.GetComponent<RigidBody3DComponent>().Type == RigidBody3DComponent::BodyType::Dynamic;
                    break;
                }
            }
            const std::vector<glm::vec3>& lines = MeshColliderWireframe(mesh, mesh.Convex || dynamic);
            const glm::vec4 color = colorOf(entity);
            const glm::mat4 world = m_Renderer->ToRenderSpace(wtc.WorldMatrix);
            for (size_t i = 0; i + 1 < lines.size(); i += 2)
                line(glm::vec3(world * glm::vec4(lines[i], 1.0f)), glm::vec3(world * glm::vec4(lines[i + 1], 1.0f)), color);
        }
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
        }

        if (m_SceneState != SceneState::Play && m_ViewportTools.ShowGrid)
            DrawWorldGrid();

        if (m_SceneState != SceneState::Play && m_ViewportTools.ShowReferenceFigure)
        {
            glm::dvec3 underCursor;
            const bool vpHovered = m_ViewportCanvas && UI::HasState(m_ViewportCanvas->GetState(), UI::UIState::Hovered);
            if (m_ViewportTools.ReferenceFigureAtCursor && vpHovered && m_ViewportCanvas->PickWorldPoint(underCursor))
                m_ViewportTools.ReferenceFigurePosition = underCursor;
            DrawReferenceFigure(m_ViewportTools.ReferenceFigureAtCursor ? m_ViewportTools.ReferenceFigurePosition : glm::dvec3(0.0));
        }

        if (m_SceneState != SceneState::Play)
        {
            auto rectLights = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, RectLightComponent>();
            for (auto entity : rectLights)
            {
                auto [wtc, light] = rectLights.get<WorldTransformComponent, RectLightComponent>(entity);
                const glm::mat4 world = m_Renderer->ToRenderSpace(wtc.WorldMatrix);
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

            auto probes = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, ReflectionProbeComponent>();
            for (auto entity : probes)
            {
                auto [wtc, probe] = probes.get<WorldTransformComponent, ReflectionProbeComponent>(entity);
                const glm::mat4 world = m_Renderer->ToRenderSpace(wtc.WorldMatrix);
                glm::vec3 corners[8];
                for (int i = 0; i < 8; ++i)
                    corners[i] = glm::vec3(world * glm::vec4((i & 1) ? probe.Extents.x : -probe.Extents.x, (i & 2) ? probe.Extents.y : -probe.Extents.y,
                                                             (i & 4) ? probe.Extents.z : -probe.Extents.z, 1.0f));
                const glm::vec4 colour(0.4f, 0.8f, 1.0f, 1.0f);
                for (int i = 0; i < 8; ++i)
                    for (int axis = 1; axis < 8; axis <<= 1)
                        if (!(i & axis))
                            m_Renderer2D->DrawXRayLine(corners[i], corners[i | axis], colour);
            }

            auto volumes = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, IrradianceVolumeComponent>();
            for (auto entity : volumes)
            {
                auto [wtc, volume] = volumes.get<WorldTransformComponent, IrradianceVolumeComponent>(entity);
                const glm::mat4 world = m_Renderer->ToRenderSpace(wtc.WorldMatrix);
                glm::vec3 corners[8];
                for (int i = 0; i < 8; ++i)
                    corners[i] = glm::vec3(world * glm::vec4((i & 1) ? volume.Extents.x : -volume.Extents.x, (i & 2) ? volume.Extents.y : -volume.Extents.y,
                                                             (i & 4) ? volume.Extents.z : -volume.Extents.z, 1.0f));
                const glm::vec4 colour(1.0f, 0.75f, 0.3f, 1.0f);
                for (int i = 0; i < 8; ++i)
                    for (int axis = 1; axis < 8; axis <<= 1)
                        if (!(i & axis))
                            m_Renderer2D->DrawXRayLine(corners[i], corners[i | axis], colour);
            }

            auto discLights = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, DiscLightComponent>();
            for (auto entity : discLights)
            {
                auto [wtc, light] = discLights.get<WorldTransformComponent, DiscLightComponent>(entity);
                const glm::mat4 world = m_Renderer->ToRenderSpace(wtc.WorldMatrix);
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

            Entity selected = m_SceneHierarchyPanel.GetSelectedEntity();
            if (selected && selected.HasComponent<AudioSourceComponent>() && selected.HasComponent<WorldTransformComponent>() &&
                selected.GetComponent<AudioSourceComponent>().Spatial && !selected.HasComponent<InactiveInHierarchyComponent>())
            {
                const auto& source = selected.GetComponent<AudioSourceComponent>();
                const glm::vec3 centre = glm::vec3(m_Renderer->ToRenderSpace(selected.GetComponent<WorldTransformComponent>().WorldMatrix)[3]);
                auto sphere = [&](float meters, const glm::vec4& colour)
                {
                    const float radius = WorldUnits::FromMeters(meters);
                    constexpr int Segments = 48;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        const int a = (axis + 1) % 3, b = (axis + 2) % 3;
                        for (int i = 0; i < Segments; ++i)
                        {
                            const float t0 = glm::two_pi<float>() * static_cast<float>(i) / Segments;
                            const float t1 = glm::two_pi<float>() * static_cast<float>(i + 1) / Segments;
                            glm::vec3 p0 = centre, p1 = centre;
                            p0[a] += std::cos(t0) * radius; p0[b] += std::sin(t0) * radius;
                            p1[a] += std::cos(t1) * radius; p1[b] += std::sin(t1) * radius;
                            m_Renderer2D->DrawXRayLine(p0, p1, colour);
                        }
                    }
                };
                sphere(source.Spatialization.MinDistance, glm::vec4(0.55f, 0.85f, 1.0f, 0.9f));
                sphere(source.Spatialization.MaxDistance, glm::vec4(0.2f, 0.4f, 1.0f, 0.7f));
            }

            constexpr glm::vec4 kVolumeColor(1.0f, 0.55f, 0.1f, 0.9f);
            constexpr glm::vec4 kAudioVolumeColor(0.3f, 0.6f, 1.0f, 0.9f);
            constexpr int edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
            auto drawBox = [&](const WorldTransformComponent& wtc, const glm::vec3& halfExtents, const glm::vec4& color)
            {
                glm::vec3 corners[8];
                for (int i = 0; i < 8; ++i)
                {
                    const glm::vec3 sign((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
                    corners[i] = glm::vec3(m_Renderer->ToRenderSpace(wtc.WorldMatrix) * glm::vec4(sign * halfExtents, 1.0f));
                }
                for (const auto& edge : edges)
                    m_Renderer2D->DrawXRayLine(corners[edge[0]], corners[edge[1]], color);
            };
            auto streamView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, LevelStreamingVolumeComponent>();
            for (auto entity : streamView)
            {
                if (Entity(entity, m_ActiveScene.get()).HasComponent<InactiveInHierarchyComponent>())
                    continue;
                auto [wtc, volume] = streamView.get<WorldTransformComponent, LevelStreamingVolumeComponent>(entity);
                drawBox(wtc, volume.HalfExtents, kVolumeColor);
            }
            constexpr glm::vec4 kPostProcessVolumeColor(0.75f, 0.45f, 1.0f, 0.9f);
            auto postView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, PostProcessVolumeComponent>();
            for (auto entity : postView)
            {
                if (Entity(entity, m_ActiveScene.get()).HasComponent<InactiveInHierarchyComponent>())
                    continue;
                auto [wtc, volume] = postView.get<WorldTransformComponent, PostProcessVolumeComponent>(entity);
                if (!volume.Unbound)
                    drawBox(wtc, volume.HalfExtents, kPostProcessVolumeColor);
            }
            auto audioView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, AudioVolumeComponent>();
            for (auto entity : audioView)
            {
                if (Entity(entity, m_ActiveScene.get()).HasComponent<InactiveInHierarchyComponent>())
                    continue;
                auto [wtc, volume] = audioView.get<WorldTransformComponent, AudioVolumeComponent>(entity);
                drawBox(wtc, volume.HalfExtents, kAudioVolumeColor);
            }
            constexpr glm::vec4 kProbeVolumeColor(0.35f, 0.9f, 0.55f, 0.9f);
            auto probeView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, AcousticProbeVolumeComponent>();
            for (auto entity : probeView)
            {
                if (Entity(entity, m_ActiveScene.get()).HasComponent<InactiveInHierarchyComponent>())
                    continue;
                auto [wtc, volume] = probeView.get<WorldTransformComponent, AcousticProbeVolumeComponent>(entity);
                drawBox(wtc, volume.HalfExtents, kProbeVolumeColor);
                if (selected != Entity(entity, m_ActiveScene.get()) || volume.BakedData == 0 || AssetManager::RequestAsset(volume.BakedData) != AssetState::Ready)
                    continue;
                const Ref<AcousticData> data = AssetManager::GetAsset<AcousticData>(volume.BakedData);
                const float arm = WorldUnits::FromMeters(0.15f);
                for (const glm::vec3& probe : data->ProbePositions)
                {
                    const glm::vec3 p = glm::vec3(m_Renderer->ToRenderSpace(data->Origin + glm::dvec3(probe)));
                    m_Renderer2D->DrawXRayLine(p - glm::vec3(arm, 0, 0), p + glm::vec3(arm, 0, 0), kProbeVolumeColor);
                    m_Renderer2D->DrawXRayLine(p - glm::vec3(0, arm, 0), p + glm::vec3(0, arm, 0), kProbeVolumeColor);
                    m_Renderer2D->DrawXRayLine(p - glm::vec3(0, 0, arm), p + glm::vec3(0, 0, arm), kProbeVolumeColor);
                }
            }
        }

        if (m_ViewportTools.ShowPhysicsColliders)
        {
            {
                auto view = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, BoxCollider2DComponent>();
                for (auto entity : view)
                {
                    auto [wtc, bc2d] = view.get<WorldTransformComponent, BoxCollider2DComponent>(entity);
                    glm::mat4 transform = m_Renderer->ToRenderSpace(wtc.WorldMatrix) *
                                          glm::translate(glm::mat4(1.0f), glm::vec3(bc2d.Offset, 0.001f)) *
                                          glm::scale(glm::mat4(1.0f), glm::vec3(bc2d.Size * 2.0f, 1.0f));
                    m_Renderer2D->DrawRect(transform, glm::vec4(0, 1, 0, 1));
                }
            }
            {
                auto view = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, CircleCollider2DComponent>();
                for (auto entity : view)
                {
                    auto [wtc, cc2d] = view.get<WorldTransformComponent, CircleCollider2DComponent>(entity);
                    glm::mat4 transform = m_Renderer->ToRenderSpace(wtc.WorldMatrix) *
                                          glm::translate(glm::mat4(1.0f), glm::vec3(cc2d.Offset, 0.001f)) *
                                          glm::scale(glm::mat4(1.0f), glm::vec3(cc2d.Radius * 2.0f));
                    m_Renderer2D->DrawCircle(transform, glm::vec4(0, 1, 0, 1), 0.01f);
                }
            }
            DrawPhysicsColliders3D();
        }

        m_Renderer->EndScene();
    }

    void EditorV2Layer::RefreshToolBar()
    {
        const bool isPlay = m_SceneState == SceneState::Play;
        const bool isSim = m_SceneState == SceneState::Simulate;
        const bool isEdit = m_SceneState == SceneState::Edit;
        const bool isPaused = !isEdit && m_ActiveScene && m_ActiveScene->IsPaused();

        if (m_PlayBtnWrap)
            m_PlayBtnWrap->Visible = isEdit || isPlay;
        if (m_SimulateBtnWrap)
            m_SimulateBtnWrap->Visible = isEdit || isSim;
        if (m_PauseBtnWrap)
            m_PauseBtnWrap->Visible = !isEdit;
        if (m_StepBtnWrap)
            m_StepBtnWrap->Visible = !isEdit && isPaused;

        if (m_PlayBtnIcon)
        {
            const Ref<Texture2D>& tex = isPlay ? m_IconStop : m_IconPlay;
            m_PlayBtnIcon->TextureDescriptorSlot = tex ? tex->GetDescriptorIndexSlot() : ~0u;
        }
        if (m_SimulateBtnIcon)
        {
            const Ref<Texture2D>& tex = isSim ? m_IconStop : m_IconSimulate;
            m_SimulateBtnIcon->TextureDescriptorSlot = tex ? tex->GetDescriptorIndexSlot() : ~0u;
        }
        if (m_PlaySettingsCogBtn)
            m_PlaySettingsCogBtn->SetEnabled(isEdit);
        if (m_PlayNetStatusLabel)
        {
            if (m_PlaySession)
            {
                m_PlayNetStatusLabel->Visible = true;
                m_PlayNetStatusLabel->Text = m_PlayWaitingForServer
                    ? ("Waiting for dedicated server -- " + m_PlaySession->Describe())
                    : m_PlaySession->Describe();
            }
            else
            {
                m_PlayNetStatusLabel->Visible = false;
            }
        }
    }

    void EditorV2Layer::RefreshSceneLoadOverlay()
    {
        if (!m_SceneLoadBanner)
            return;
        if (!m_SceneLoad)
        {
            m_SceneLoadBanner->Visible = false;
            return;
        }
        m_SceneLoadBanner->Visible = true;
        if (m_SceneLoadTitleLabel)
            m_SceneLoadTitleLabel->Text = "Opening " + m_SceneLoad->GetPath().filename().string();
        if (m_SceneLoadStageLabel)
        {
            char pct[128];
            std::snprintf(pct, sizeof(pct), "%s (%.0f%%)", m_SceneLoad->GetStage().c_str(), m_SceneLoad->GetProgress() * 100.0f);
            m_SceneLoadStageLabel->Text = pct;
        }
        if (m_SceneLoadProgressFill)
            m_SceneLoadProgressFill->FixedSize.x = std::clamp(m_SceneLoad->GetProgress(), 0.0f, 1.0f) * 300.0f;
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

        if (m_OpenSaveAndPlay)
        {
            m_OpenSaveAndPlay = false;
            ShowSaveAndPlayOverlay();
        }

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
                {
                    if (Entity imported = ModelInstance::SpawnLevel(*m_EditorScene, level))
                    {
                        m_SceneHierarchyPanel.SetSelectedEntity(imported);
                        m_SceneHierarchyPanel.MarkDirty();
                    }
                }
            }

            assetManager->Update();
        }

        RefreshSceneLoadOverlay();

        if (!m_Renderer || !m_ActiveScene || !m_ViewportImage)
            return;

        const bool wantHideCursor = m_EditorCamera.ShouldHideCursor() || (m_SceneState != SceneState::Edit && Input::IsRelativeMouseMode());
        if (wantHideCursor != m_CursorHidden)
        {
            if (wantHideCursor)
                SDL_HideCursor();
            else
                SDL_ShowCursor();
            m_CursorHidden = wantHideCursor;
        }

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

        const UI::UIElement* targetEl = m_ViewportCanvas ? static_cast<const UI::UIElement*>(m_ViewportCanvas.get())
                                                         : static_cast<const UI::UIElement*>(m_ViewportImage.get());
        const bool hovered = targetEl && UI::HasState(targetEl->GetState(), UI::UIState::Hovered);
        const bool focused = targetEl && UI::HasState(targetEl->GetState(), UI::UIState::Focused);

        m_Viewports[m_ActiveViewport].Size = { wantW, wantH };
        m_Viewports[m_ActiveViewport].Hovered = hovered;
        for (int index = 0; index < kMaxViewports; ++index)
        {
            if (index == m_ActiveViewport)
                continue;
            ViewportSlot& slot = m_Viewports[index];
            if (slot.Open && m_ExtraViewportImages[index] && m_ExtraViewportCanvases[index])
            {
                const UI::UIRect& eb = m_ExtraViewportImages[index]->GetBounds();
                slot.Size = {
                    static_cast<uint32_t>(std::max(0.0f, std::floor(eb.Width()))),
                    static_cast<uint32_t>(std::max(0.0f, std::floor(eb.Height())))
                };
                slot.Hovered = UI::HasState(m_ExtraViewportCanvases[index]->GetState(), UI::UIState::Hovered);
            }
            else
            {
                slot.Size = { 0, 0 };
                slot.Hovered = false;
            }
        }

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
            if (m_PlaySession)
                m_PlaySession->Receive(static_cast<float>(ts));
            if (m_PlaySessionEnded)
            {
                OnSceneStop();
                break;
            }
            if (!m_PlayWaitingForServer)
                m_ActiveScene->OnUpdateRuntime(ts);
            if (m_PlaySession)
            {
                m_PlaySession->AfterScene(static_cast<float>(ts));
                m_PlaySession->Send();
            }
            break;
        }

        m_Viewports[m_ActiveViewport].Camera = m_EditorCamera;
        m_Viewports[m_ActiveViewport].CameraSet = true;

        if (m_ViewportCanvas)
        {
            const UI::UIRect& vpBounds = m_ViewportCanvas->GetBounds();
            glm::vec2 mouse = Input::GetMousePosition();
            if (UILayer* uiLayer = Application::Get().GetUILayer())
                mouse = uiLayer->GetContext().GetLastMousePos();

            const int mouseX = static_cast<int>(mouse.x - vpBounds.Min.x);
            const int mouseY = static_cast<int>(mouse.y - vpBounds.Min.y);
            const int vpW = static_cast<int>(vpBounds.Width());
            const int vpH = static_cast<int>(vpBounds.Height());
            if (mouseX >= 0 && mouseY >= 0 && mouseX < vpW && mouseY < vpH)
            {
                const int32_t pixelData = m_Renderer->getPickedEntityID();
                m_HoveredEntity = pixelData == -1 ? Entity() : Entity(static_cast<entt::entity>(pixelData), m_ActiveScene.get());

                if (!m_ViewportTools.IsBoxSelecting)
                {
                    if (hovered)
                        m_Renderer->setPickRequest(mouseX, mouseY, true);
                }
                else
                {
                    const glm::vec2 boxMin = {
                        std::min(m_ViewportTools.BoxSelectStart.x, m_ViewportTools.BoxSelectEnd.x) - vpBounds.Min.x,
                        std::min(m_ViewportTools.BoxSelectStart.y, m_ViewportTools.BoxSelectEnd.y) - vpBounds.Min.y
                    };
                    const glm::vec2 boxMax = {
                        std::max(m_ViewportTools.BoxSelectStart.x, m_ViewportTools.BoxSelectEnd.x) - vpBounds.Min.x,
                        std::max(m_ViewportTools.BoxSelectStart.y, m_ViewportTools.BoxSelectEnd.y) - vpBounds.Min.y
                    };
                    const int startX = std::max(0, static_cast<int>(boxMin.x));
                    const int startY = std::max(0, static_cast<int>(boxMin.y));
                    const int width = std::max(1, static_cast<int>(boxMax.x - boxMin.x));
                    const int height = std::max(1, static_cast<int>(boxMax.y - boxMin.y));
                    m_Renderer->setBoxPickRequest(startX, startY, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
                }
            }
        }

        if (m_ShowSceneHierarchy)
            m_SceneHierarchyPanel.Update();

        std::vector<int32_t> selectedIDs;
        for (const Entity& entity : m_SceneHierarchyPanel.GetSelectedEntities())
        {
            if (entity)
                selectedIDs.push_back(static_cast<int32_t>(static_cast<uint32_t>(entity)));
        }
        m_Renderer->SetSelectedEntityID(selectedIDs);

        OnOverlayRender();
        SubmitExtraViewports(ts);
        UpdateHistory();
        RefreshToolBar();
        m_ViewportToolbar.Sync();

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
        for (int index = 0; index < kMaxViewports; ++index)
        {
            if (!m_ExtraViewportImages[index])
                continue;
            if (index != m_ActiveViewport && m_Viewports[index].Open && m_Viewports[index].Submitted)
            {
                Texture2D* extraTex = m_Renderer->GetExtraViewTexture(static_cast<uint32_t>(index));
                m_ExtraViewportImages[index]->TextureDescriptorSlot = extraTex ? extraTex->GetDescriptorIndexSlot() : ~0u;
            }
            else
            {
                m_ExtraViewportImages[index]->TextureDescriptorSlot = ~0u;
            }
        }
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
            bool textInputFocused = false;
            if (UILayer* uiLayer = Application::Get().GetUILayer())
                textInputFocused = dynamic_cast<UI::UITextInput*>(uiLayer->GetContext().GetFocused()) != nullptr;

            const auto& key = static_cast<const KeyPressedEvent&>(event);
            if (!key.IsRepeat() && !textInputFocused)
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

                const UI::UIElement* vpEl = m_ViewportCanvas ? static_cast<const UI::UIElement*>(m_ViewportCanvas.get())
                                                             : static_cast<const UI::UIElement*>(m_ViewportImage.get());
                const bool viewportHovered = vpEl && UI::HasState(vpEl->GetState(), UI::UIState::Hovered);

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
                case SDL_SCANCODE_D:
                    if (control && m_SceneState == SceneState::Edit) { m_SceneHierarchyPanel.DuplicateSelection(); event.Handled = true; return; }
                    break;
                case SDL_SCANCODE_F:
                    if (control)
                    {
                        if (m_ShowSceneHierarchy)
                            m_SceneHierarchyPanel.FocusSearch();
                        event.Handled = true;
                        return;
                    }
                    if (m_SceneState == SceneState::Edit)
                    {
                        if (Entity selected = m_SceneHierarchyPanel.GetSelectedEntity())
                            FocusOnEntity(selected);
                        event.Handled = true;
                        return;
                    }
                    break;
                case SDL_SCANCODE_M:
                    if (viewportHovered && !control && m_SceneState == SceneState::Edit)
                    {
                        m_ViewportTools.MeasureTool = !m_ViewportTools.MeasureTool;
                        if (!m_ViewportTools.MeasureTool)
                            m_ViewportTools.MeasureStage = 0;
                        m_ViewportToolbar.Sync();
                        event.Handled = true;
                        return;
                    }
                    break;
                case SDL_SCANCODE_END:
                    if (m_SceneState == SceneState::Edit)
                    {
                        SnapSelectionToFloor();
                        event.Handled = true;
                        return;
                    }
                    break;
                case SDL_SCANCODE_B:
                    if (control) { m_ScriptBuilder.RequestBuild(); event.Handled = true; return; }
                    break;
                case SDL_SCANCODE_R:
                    if (control) { if (ScriptEngine::IsInitialized()) ScriptEngine::ReloadAssembly(); event.Handled = true; return; }
                    break;
                case SDL_SCANCODE_DELETE:
                    if (m_SceneState == SceneState::Edit && !m_SceneHierarchyPanel.GetSelectedEntities().empty())
                    {
                        m_SceneHierarchyPanel.DeleteSelection();
                        event.Handled = true;
                        return;
                    }
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
                    if (m_ViewportTools.MeasureTool)
                    {
                        if (m_ViewportTools.MeasureStage != 0)
                            m_ViewportTools.MeasureStage = 0;
                        else
                            m_ViewportTools.MeasureTool = false;
                        m_ViewportToolbar.Sync();
                        event.Handled = true;
                        return;
                    }
                    if (m_SceneState != SceneState::Edit) { OnSceneStop(); event.Handled = true; return; }
                    break;
                default:
                    break;
                }
            }
        }

        const UI::UIElement* scrollEl = m_ViewportSlotRoots[m_ActiveViewport]
            ? m_ViewportSlotRoots[m_ActiveViewport].get()
            : (m_ViewportCanvas ? static_cast<const UI::UIElement*>(m_ViewportCanvas.get())
                                : static_cast<const UI::UIElement*>(m_ViewportImage.get()));
        if (!scrollEl)
            return;

        if (event.GetEventType() == EventType::MouseScrolled && m_SceneState == SceneState::Edit)
        {
            const UI::UIElement* hoveredUi = nullptr;
            glm::vec2 mouse = Input::GetMousePosition();
            if (UILayer* uiLayer = Application::Get().GetUILayer())
            {
                hoveredUi = uiLayer->GetContext().GetHovered();
                mouse = uiLayer->GetContext().GetLastMousePos();
            }
            const bool activeViewportHovered =
                (m_ViewportCanvas && (hoveredUi == m_ViewportCanvas.get() || UI::HasState(m_ViewportCanvas->GetState(), UI::UIState::Hovered))) ||
                (m_ExtraViewportCanvases[m_ActiveViewport] && hoveredUi == m_ExtraViewportCanvases[m_ActiveViewport].get());
            if (activeViewportHovered)
            {
                const UI::UIRect& b = scrollEl->GetBounds();
                m_EditorCamera.SetCursorPosition({ mouse.x - b.Min.x, mouse.y - b.Min.y });
                m_EditorCamera.OnEvent(event);
            }
        }
        else if (event.GetEventType() == EventType::MouseButtonReleased)
        {
            const bool anyDown = Input::IsMouseButtonPressed(SDL_BUTTON_LEFT) ||
                                 Input::IsMouseButtonPressed(SDL_BUTTON_RIGHT) ||
                                 Input::IsMouseButtonPressed(SDL_BUTTON_MIDDLE);
            if (!anyDown)
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
        m_ViewportImage->AnchorMin = { 0.0f, 0.0f };
        m_ViewportImage->AnchorMax = { 1.0f, 1.0f };
        m_ViewportImage->AnchorOffsets = { 0.0f, 0.0f, 0.0f, 0.0f };
        m_ViewportImage->HitTestMode = UI::UIHitTestMode::None;

        m_ViewportCanvas = UI::MakeUI<ViewportCanvas>();
        m_ViewportCanvas->AnchorMin = { 0.0f, 0.0f };
        m_ViewportCanvas->AnchorMax = { 1.0f, 1.0f };
        m_ViewportCanvas->AnchorOffsets = { 0.0f, 0.0f, 0.0f, 0.0f };
        m_ViewportCanvas->HitTestMode = UI::UIHitTestMode::Self;
        m_ViewportCanvas->Focusable = true;
        m_ViewportCanvas->Bind({
            .Render = m_Renderer,
            .Camera = &m_EditorCamera,
            .ActiveScene = &m_ActiveScene,
            .Hierarchy = &m_SceneHierarchyPanel,
            .HoveredEntity = &m_HoveredEntity,
            .Tools = &m_ViewportTools,
            .IsPlayModeFn = [this]() { return m_SceneState == SceneState::Play; },
            .IsEditModeFn = [this]() { return m_SceneState == SceneState::Edit; },
            .ShowCameraPositionFn = [this]() { return m_Viewports[m_ActiveViewport].ShowCameraPosition; },
            .ShowActiveFrameFn = [this]() { return OpenViewportCount() > 1; },
            .GridPlaneAxisFn = [this]() { return GridPlaneAxis(); },
            .OnSceneMutated = [this]() { m_HistoryCheckRequested = true; },
            .OnCaptureChanged = [this](bool captured, int) { m_ViewportMouseCaptured = captured; }
        });

        Ref<UI::UIElement> overlayToolbar = m_ViewportToolbar.Build(m_Renderer, &m_EditorCamera, &m_ViewportTools, {
            .SetViewMode = [this](EditorViewMode mode) { SetViewMode(mode); },
            .SnapToFloor = [this]() { SnapSelectionToFloor(); },
            .SnapToGrid = [this]() { SnapSelectionToGrid(); },
            .GetShowCameraPosition = [this]() { return m_Viewports[m_ActiveViewport].ShowCameraPosition; },
            .SetShowCameraPosition = [this](bool show) { m_Viewports[m_ActiveViewport].ShowCameraPosition = show; }
        });

        // Floating scene load progress banner inside the bottom center of the viewport
        m_SceneLoadTitleLabel = UI::Label("Opening scene...").Get();
        m_SceneLoadStageLabel = UI::Label("").Get();
        m_SceneLoadStageLabel->UseColorOverride = true;
        m_SceneLoadStageLabel->ColorOverride = { 0.72f, 0.76f, 0.82f, 1.0f };

        auto progressTrack = UI::MakeUI<UI::UIElement>();
        progressTrack->WidthMode = UI::UISizeMode::Fixed;
        progressTrack->HeightMode = UI::UISizeMode::Fixed;
        progressTrack->FixedSize = { 300.0f, 6.0f };
        progressTrack->DrawBackground = true;
        progressTrack->StyleClass = "Surface";
        progressTrack->ClipChildren = true;

        m_SceneLoadProgressFill = UI::MakeUI<UI::UIElement>();
        m_SceneLoadProgressFill->WidthMode = UI::UISizeMode::Fixed;
        m_SceneLoadProgressFill->HeightMode = UI::UISizeMode::Fill;
        m_SceneLoadProgressFill->FixedSize = { 0.0f, 6.0f };
        m_SceneLoadProgressFill->DrawBackground = true;
        m_SceneLoadProgressFill->StyleClass = "Button";
        m_SceneLoadProgressFill->StyleVariant = "primary";
        progressTrack->AddChild(m_SceneLoadProgressFill);

        m_SceneLoadBanner = UI::VStack(4.0f, { m_SceneLoadTitleLabel, m_SceneLoadStageLabel, progressTrack })
            .Padding(10.0f, 8.0f)
            .Style("Surface")
            .Background(true)
            .Get();
        m_SceneLoadBanner->WidthMode = UI::UISizeMode::Auto;
        m_SceneLoadBanner->HeightMode = UI::UISizeMode::Auto;
        m_SceneLoadBanner->AnchorMin = { 0.5f, 1.0f };
        m_SceneLoadBanner->AnchorMax = { 0.5f, 1.0f };
        m_SceneLoadBanner->AnchorOffsets = { -160.0f, -96.0f, 160.0f, -20.0f };
        m_SceneLoadBanner->Visible = false;

        auto viewportHost = UI::MakeUI<UI::UIAnchorBox>();
        viewportHost->WidthMode = UI::UISizeMode::Fill;
        viewportHost->HeightMode = UI::UISizeMode::Fill;
        viewportHost->AnchorMin = { 0.0f, 0.0f };
        viewportHost->AnchorMax = { 1.0f, 1.0f };
        viewportHost->AnchorOffsets = { 0.0f, 0.0f, 0.0f, 0.0f };
        viewportHost->AddChild(m_ViewportImage);
        viewportHost->AddChild(m_ViewportCanvas);
        viewportHost->AddChild(overlayToolbar);
        viewportHost->AddChild(m_SceneLoadBanner);
        m_ActiveViewportHost = viewportHost;

        for (int index = 0; index < kMaxViewports; ++index)
        {
            auto extraImg = UI::MakeUI<UI::UIImage>();
            extraImg->AnchorMin = { 0.0f, 0.0f };
            extraImg->AnchorMax = { 1.0f, 1.0f };
            extraImg->AnchorOffsets = { 0.0f, 0.0f, 0.0f, 0.0f };
            extraImg->HitTestMode = UI::UIHitTestMode::None;
            m_ExtraViewportImages[index] = extraImg;

            auto extraCanvas = UI::MakeUI<ExtraViewportCanvas>();
            extraCanvas->AnchorMin = { 0.0f, 0.0f };
            extraCanvas->AnchorMax = { 1.0f, 1.0f };
            extraCanvas->AnchorOffsets = { 0.0f, 0.0f, 0.0f, 0.0f };
            extraCanvas->HitTestMode = UI::UIHitTestMode::Self;
            extraCanvas->Bind({
                .Index = index,
                .Render = m_Renderer,
                .Camera = &m_Viewports[index].Camera,
                .Hierarchy = &m_SceneHierarchyPanel,
                .GizmoMode = &m_ViewportTools.GizmoMode,
                .ShowCameraPosition = &m_Viewports[index].ShowCameraPosition,
                .ShowFlags = &m_Viewports[index].ShowFlags,
                .ResolutionIndex = &m_Viewports[index].ResolutionIndex,
                .Realtime = &m_Viewports[index].Realtime,
                .IsPlayModeFn = [this]() { return m_SceneState == SceneState::Play; },
                .OnActivate = [this](int idx) { ActivateViewport(idx); }
            });
            m_ExtraViewportCanvases[index] = extraCanvas;

            auto extraHost = UI::MakeUI<UI::UIAnchorBox>();
            extraHost->WidthMode = UI::UISizeMode::Fill;
            extraHost->HeightMode = UI::UISizeMode::Fill;
            extraHost->AnchorMin = { 0.0f, 0.0f };
            extraHost->AnchorMax = { 1.0f, 1.0f };
            extraHost->AnchorOffsets = { 0.0f, 0.0f, 0.0f, 0.0f };
            extraHost->AddChild(extraImg);
            extraHost->AddChild(extraCanvas);
            m_ExtraViewportHosts[index] = extraHost;

            auto root = UI::MakeUI<UI::UIAnchorBox>();
            root->WidthMode = UI::UISizeMode::Fill;
            root->HeightMode = UI::UISizeMode::Fill;
            root->AddChild(index == m_ActiveViewport ? m_ActiveViewportHost : m_ExtraViewportHosts[index]);
            m_ViewportSlotRoots[index] = root;
        }

        // Sleek Godot-style Play / Simulate / Pause / Step Toolbar (dockable without tab header via NoTabBar)
        class ToolbarDragHandle final : public UI::UIElement
        {
        public:
            ToolbarDragHandle(UI::UIDockSpace* dock, bool drawDots)
                : m_Dock(dock), m_DrawDots(drawDots)
            {
                HitTestMode = UI::UIHitTestMode::Self;
                if (drawDots)
                {
                    WidthMode = UI::UISizeMode::Fixed;
                    HeightMode = UI::UISizeMode::Fixed;
                    FixedSize = { 14.0f, 22.0f };
                    VAlign = UI::UIAlignment::Center;
                    Tooltip = "Drag to move / dock Toolbar";
                }
                else
                {
                    WidthMode = UI::UISizeMode::Fill;
                    HeightMode = UI::UISizeMode::Fill;
                }
            }

            bool OnPointerDown(const UI::UIPointerEvent& e) override
            {
                if (e.Button != 1)
                    return false;
                m_Pressed = true;
                m_PressPos = e.Position;
                return true;
            }

            bool OnPointerMove(const UI::UIPointerEvent& e) override
            {
                if (m_Pressed && m_Dock)
                {
                    const UI::UIVec2 d = e.Position - m_PressPos;
                    if (d.x * d.x + d.y * d.y > 36.0f)
                    {
                        m_Pressed = false;
                        m_Dock->BeginPanelDrag("Toolbar", e.Position);
                        return true;
                    }
                }
                return false;
            }

            bool OnPointerUp(const UI::UIPointerEvent& e) override
            {
                if (e.Button == 1)
                    m_Pressed = false;
                return false;
            }

        protected:
            void OnPaint(UI::UIDrawList& drawList, const UI::UITheme&) override
            {
                if (!m_DrawDots)
                    return;
                const UI::UIRect& b = GetBounds();
                const float cx = (b.Min.x + b.Max.x) * 0.5f;
                const float cy = (b.Min.y + b.Max.y) * 0.5f;
                const UI::UIColor dotCol = UI::HasState(GetState(), UI::UIState::Hovered)
                    ? UI::UIColor{ 0.78f, 0.84f, 0.94f, 0.90f }
                    : UI::UIColor{ 0.45f, 0.50f, 0.58f, 0.55f };
                for (int r = -1; r <= 1; ++r)
                {
                    for (int c = 0; c <= 1; ++c)
                    {
                        const glm::vec2 p(cx + (c == 0 ? -2.0f : 2.0f), cy + static_cast<float>(r) * 4.0f);
                        drawList.AddRoundedRect(p - glm::vec2(1.1f), p + glm::vec2(1.1f), dotCol, glm::vec4(1.1f));
                    }
                }
            }

        private:
            UI::UIDockSpace* m_Dock = nullptr;
            bool m_DrawDots = false;
            bool m_Pressed = false;
            UI::UIVec2 m_PressPos{ 0.0f, 0.0f };
        };
        auto makeToolbarIconBtn = [](const Ref<Texture2D>& icon, const std::string& tip,
                                     std::function<void()> onClick, Ref<UI::UIImage>& outImg) -> Ref<UI::UIElement>
        {
            auto img = UI::MakeUI<UI::UIImage>(icon ? icon->GetDescriptorIndexSlot() : ~0u, UI::UIVec2{ 18.0f, 18.0f });
            img->WidthMode = UI::UISizeMode::Fixed;
            img->HeightMode = UI::UISizeMode::Fixed;
            img->FixedSize = { 18.0f, 18.0f };
            img->HitTestMode = UI::UIHitTestMode::None;
            outImg = img;

            auto box = UI::HStack(0.0f, { img })
                .Padding(6.0f, 3.0f)
                .VAlign(UI::UIAlignment::Center)
                .Style("Button")
                .Background(true)
                .Tooltip(tip)
                .Get();
            box->HitTestMode = UI::UIHitTestMode::Self;
            box->Focusable = true;
            UI::UIStackBox* boxPtr = box.get();
            box->OnMouseUp = [onClick = std::move(onClick), boxPtr](const UI::UIPointerEvent& e)
            {
                if (e.Button == 1 && boxPtr->GetBounds().Contains(e.Position) && onClick)
                {
                    onClick();
                    return true;
                }
                return false;
            };
            return box;
        };

        m_PlayBtnWrap = makeToolbarIconBtn(m_IconPlay, "Play / Stop (Esc)", [this]()
        {
            if (m_SceneState == SceneState::Play)
                OnSceneStop();
            else
                OnScenePlay();
        }, m_PlayBtnIcon);

        m_SimulateBtnWrap = makeToolbarIconBtn(m_IconSimulate, "Simulate / Stop", [this]()
        {
            if (m_SceneState == SceneState::Simulate)
                OnSceneStop();
            else
                OnSceneSimulate();
        }, m_SimulateBtnIcon);

        Ref<UI::UIImage> pauseImg;
        m_PauseBtnWrap = makeToolbarIconBtn(m_IconPause, "Pause / Resume", [this]()
        {
            if (m_SceneState != SceneState::Edit && m_ActiveScene)
                m_ActiveScene->SetPaused(!m_ActiveScene->IsPaused());
            RefreshToolBar();
        }, pauseImg);

        Ref<UI::UIImage> stepImg;
        m_StepBtnWrap = makeToolbarIconBtn(m_IconStep, "Step one frame", [this]()
        {
            if (m_SceneState != SceneState::Edit && m_ActiveScene)
                m_ActiveScene->Step();
        }, stepImg);

        m_PlaySettingsCogBtn = UI::MakeUI<UIVectorIconButton>(
            EditorIconKind::Cogwheel,
            "Play Settings (Multiplayer: Net Mode, Players, Emulation)",
            [this]() { ShowPlaySettingsOverlay(m_PlaySettingsCogBtn.get()); });
        m_PlaySettingsCogBtn->VAlign = UI::UIAlignment::Center;

        m_PlayNetStatusLabel = UI::Label("").VAlign(UI::UIAlignment::Center).Margin(6.0f, 0.0f);
        m_PlayNetStatusLabel->UseColorOverride = true;
        m_PlayNetStatusLabel->ColorOverride = { 0.72f, 0.78f, 0.86f, 1.0f };
        m_PlayNetStatusLabel->Visible = false;

        auto dockPtr = uiLayer->GetDockSpace();
        auto tbGrip = UI::MakeUI<ToolbarDragHandle>(dockPtr, true);
        auto tbLeftSpacer = UI::MakeUI<ToolbarDragHandle>(dockPtr, false);
        auto tbRightSpacer = UI::MakeUI<ToolbarDragHandle>(dockPtr, false);

        m_ToolBar = UI::HStack(4.0f, {
            tbGrip,
            tbLeftSpacer,
            m_PlayBtnWrap,
            m_SimulateBtnWrap,
            m_PauseBtnWrap,
            m_StepBtnWrap,
            m_PlaySettingsCogBtn,
            m_PlayNetStatusLabel,
            tbRightSpacer
        }).Padding(6.0f, 3.0f).Style("Surface").Background(true).Get();
        m_ToolBar->WidthMode = UI::UISizeMode::Fill;
        m_ToolBar->HeightMode = UI::UISizeMode::Fill;
        RefreshToolBar();

        // Dock panels: Viewport Center -> Scene Hierarchy Left (full height) -> Toolbar Top of Viewport (no tab strip)
        uiLayer->AddPanel(
            "Viewport",
            m_ViewportSlotRoots[0],
            UI::UIDockDropZone::Center,
            {},
            0.5f,
            UI::UIDockNodeFlags::CentralNode
        );

        if (m_ShowSceneHierarchy)
        {
            uiLayer->AddPanel(
                "Scene Hierarchy",
                m_SceneHierarchyPanel.BuildUI(),
                UI::UIDockDropZone::Left,
                "Viewport",
                0.155f
            );
        }

        if (m_ShowToolBar)
        {
            uiLayer->AddPanel(
                "Toolbar",
                m_ToolBar,
                UI::UIDockDropZone::Top,
                "Viewport",
                0.038f,
                UI::UIDockNodeFlags::NoTabBar | UI::UIDockNodeFlags::NoDockingOver
            );
        }

        for (int index = 1; index < kMaxViewports; ++index)
        {
            if (m_Viewports[index].Open)
            {
                const UI::UIDockDropZone zone = (index == 2) ? UI::UIDockDropZone::Bottom : UI::UIDockDropZone::Right;
                uiLayer->AddPanel(m_Viewports[index].Title, m_ViewportSlotRoots[index], zone, "Viewport", 0.5f);
            }
        }

        m_StatusLabel = UI::Label("Ready").Margin(10.0f, 4.0f);

        if (loadSavedLayout)
        {
            bool shouldLoad = true;
            std::ifstream layoutFile(kLayoutPath);
            if (layoutFile.is_open())
            {
                std::stringstream ss;
                ss << layoutFile.rdbuf();
                const std::string saved = ss.str();
                if ((m_ShowSceneHierarchy && saved.find("Scene Hierarchy") == std::string::npos) ||
                    (m_ShowToolBar && (saved.find("Toolbar") == std::string::npos || saved.find("flags: 6") == std::string::npos)))
                    shouldLoad = false;
            }
            if (shouldLoad)
            {
                if (const auto& dock = uiLayer->GetDockSpaceRef())
                    dock->LoadLayout(kLayoutPath);
            }
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
                {"Save Layout", "", [this, uiLayer]()
                {
                    if (const auto& dock = uiLayer->GetDockSpaceRef())
                        dock->SaveLayout(kLayoutPath);
                    SaveViewportPrefs();
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
                        Entity created = m_ActiveScene->CreateEntity("Empty Entity");
                        m_SceneHierarchyPanel.SetSelectedEntity(created);
                        m_SceneHierarchyPanel.MarkDirty();
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
                {"1 Viewport", "", [this]() { ApplyViewportLayout(ViewportLayout::One); }},
                {"2 Side by Side", "", [this]() { ApplyViewportLayout(ViewportLayout::TwoSideBySide); }},
                {"2 Stacked", "", [this]() { ApplyViewportLayout(ViewportLayout::TwoStacked); }},
                {"3 Viewports", "", [this]() { ApplyViewportLayout(ViewportLayout::Three); }},
                {"4 Viewports (2 x 2)", "", [this]() { ApplyViewportLayout(ViewportLayout::Four); }},
                {"", "", {}, true}
            };
            for (int index = 1; index < kMaxViewports; ++index)
            {
                const bool open = m_Viewports[index].Open;
                viewportItems.push_back({
                    m_Viewports[index].Title,
                    "",
                    [this, index, open]()
                    {
                        if (open)
                            CloseViewport(index);
                        else
                            OpenViewport(index);
                    },
                    false, true, open
                });
            }
            viewportItems.push_back({"", "", {}, true});
            for (int index = 0; index < kMaxViewports; ++index)
            {
                viewportItems.push_back({
                    std::string(m_Viewports[index].Title) + " Options",
                    "",
                    {},
                    false, true, false, false, "",
                    BuildViewportOptionsItems(index)
                });
            }
            viewportItems.push_back({"", "", {}, true});
            viewportItems.push_back({"Click a viewport to use it; drag its tab to move it", "", {}, false, false, false, true});

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
                {"Scene Hierarchy", "", [this, uiLayer]()
                {
                    m_ShowSceneHierarchy = !m_ShowSceneHierarchy;
                    if (const auto& dock = uiLayer->GetDockSpaceRef())
                    {
                        if (m_ShowSceneHierarchy)
                            dock->AddPanel("Scene Hierarchy", m_SceneHierarchyPanel.BuildUI(), UI::UIDockDropZone::Left, "Viewport", 0.155f);
                        else
                            dock->RemovePanel("Scene Hierarchy");
                    }
                }, false, true, m_ShowSceneHierarchy},
                {"Toolbar", "", [this, uiLayer]()
                {
                    m_ShowToolBar = !m_ShowToolBar;
                    if (const auto& dock = uiLayer->GetDockSpaceRef())
                    {
                        if (m_ShowToolBar)
                            dock->AddPanel("Toolbar", m_ToolBar, UI::UIDockDropZone::Top, "Viewport", 0.038f,
                                           UI::UIDockNodeFlags::NoTabBar | UI::UIDockNodeFlags::NoDockingOver);
                        else
                            dock->RemovePanel("Toolbar");
                    }
                }, false, true, m_ShowToolBar},
                {"Render Graph", "", {}, false, false, false, false, "Panel not yet ported to NoxEditorV2"},
                {"Output Log", "", {}, false, false, false, false, "Panel not yet ported to NoxEditorV2"},
                {"Profiler", "", {}, false, false, false, false, "Panel not yet ported to NoxEditorV2"},
                {"World Partition", "", {}, false, false, false, false, "Panel not yet ported to NoxEditorV2"},
                {"Levels", "", {}, false, false, false, false, "Panel not yet ported to NoxEditorV2"},
                {"Mixer", "", {}, false, false, false, false, "Panel not yet ported to NoxEditorV2"},
                {"Viewports", "", {}, false, true, false, false, "", std::move(viewportItems)},
                {"Animation Graphs", "", {}, false, true, false, false, "", std::move(animGraphItems)},
                {"", "", {}, true},
                {"Reset Layout", "", [this]()
                {
                    for (int i = 1; i < kMaxViewports; ++i)
                        m_Viewports[i].Open = false;
                    m_ActiveViewport = 0;
                    m_ShowSceneHierarchy = true;
                    m_ShowToolBar = true;
                    BuildUI(false);
                }},
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
