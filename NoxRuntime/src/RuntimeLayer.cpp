#include "RuntimeLayer.h"

#include <unordered_set>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scripting/ScriptEngine.h"

namespace Nox
{
    namespace
    {
        std::filesystem::path FirstProjectIn(const std::filesystem::path& directory)
        {
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(directory, error))
            {
                if (entry.is_regular_file(error) && entry.path().extension() == ".nproj")
                    return entry.path();
            }
            return {};
        }

        void Quit()
        {
            SDL_Event event{};
            event.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&event);
        }
    }

    RuntimeLayer::RuntimeLayer()
        : Layer("RuntimeLayer")
    {
        m_Renderer = Application::Get().GetRenderer();

        const std::filesystem::path projectFile = FindProjectFile();
        if (projectFile.empty() || !Project::Load(projectFile))
        {
            NOX_CORE_ERROR("NoxRuntime: no project to play ({}). Usage: NoxRuntime <project.nproj>",
                           projectFile.empty() ? std::string("none found") : projectFile.generic_string());
            Quit();
            return;
        }
        NOX_CORE_INFO("NoxRuntime: playing '{}'", projectFile.generic_string());
        // The project's quality preset ("Auto": the one this GPU should run, the fallback on smaller RT cards).
        m_Renderer->ApplyQualitySetting(Project::GetActive()->GetConfig().RenderQuality);

        // The editor's asset manager, read-only: assets are already cooked by the editor, and no file watcher runs (Init is not
        // called). R2 replaces it with a RuntimeAssetManager reading the runtime registry / pack files.
        if (!ScriptEngine::Initialize(SDL_GetBasePath()))
            NOX_CORE_ERROR("NoxRuntime: failed to initialize the .NET scripting backend (scripts will not run)");

        const AssetHandle startScene = Project::GetActive()->GetConfig().StartScene;
        if (startScene == 0)
        {
            NOX_CORE_ERROR("NoxRuntime: the project has no start scene (ProjectConfig StartScene)");
            Quit();
            return;
        }
        StartScene(startScene);
    }

    RuntimeLayer::~RuntimeLayer()
    {
        NOX_CORE_INFO("RuntimeLayer Shutdown");

        if (m_Scene)
            m_Scene->OnRuntimeStop();
        m_Scene = nullptr;
        ScriptEngine::Shutdown();

        Font::ReleaseDefault(); // the stats overlay's font, before the renderer goes (same as the editor)

        if (Project::GetActive() && Project::GetActive()->GetEditorAssetManager())
            Project::GetActive()->GetEditorAssetManager()->Shutdown();
    }

    void RuntimeLayer::OnUpdate(Timestep ts)
    {
        if (!m_Scene || !Project::GetActive())
            return;

        // Same unused-asset sweep as the editor: a script destroying entities can leave meshes and textures unreferenced.
        auto assetManager = Project::GetActive()->GetEditorAssetManager();
        if (m_Scene->ConsumeAssetReferencesChanged() || assetManager->ConsumeLoadsSettledAfterSweep())
            m_UnloadUnusedAssetsRequested = true;
        if (m_UnloadUnusedAssetsRequested)
        {
            NOX_PROFILE_SCOPE("Unload Unused Assets");
            m_UnloadUnusedAssetsRequested = false;
            UnloadUnusedAssets();
        }

        // The window is the viewport: the renderer's output is the swapchain, the scene's cameras take its aspect.
        const NRI::Extent2D outputSize = m_Renderer->getOutputSize();
        if (outputSize.width > 0 && outputSize.height > 0)
        {
            m_Renderer->onViewportSizeChange(outputSize); // no-op while the size is unchanged
            m_Scene->OnViewportResize(outputSize.width, outputSize.height);
        }

        m_Scene->SetRenderer(m_Renderer);
        m_Scene->SetRenderer2D(m_Renderer->getRenderer2D());
        {
            NOX_PROFILE_SCOPE("Scene Update");
            m_Scene->OnUpdateRuntime(ts);
        }

        {
            NOX_PROFILE_SCOPE("Asset Manager Update");
            assetManager->Update(); // publishes the background loads that finished
        }
    }

    std::filesystem::path RuntimeLayer::FindProjectFile() const
    {
        const ApplicationCommandLineArgs& args = Application::Get().GetSpecification().CommandLineArgs;
        if (args.Count > 1)
            return std::filesystem::path(args[1]);

        const std::filesystem::path workingDirectory = std::filesystem::current_path();
        for (const std::filesystem::path& directory : { workingDirectory, std::filesystem::path(SDL_GetBasePath()), workingDirectory / ".." / "Facerun" })
        {
            const std::filesystem::path project = FirstProjectIn(directory);
            if (!project.empty())
                return project;
        }
        return {};
    }

    void RuntimeLayer::StartScene(AssetHandle sceneHandle)
    {
        // A copy, like the editor's Play: the scene asset itself stays as it was loaded.
        Ref<Scene> sceneAsset = AssetManager::GetAsset<Scene>(sceneHandle);
        if (!sceneAsset)
        {
            NOX_CORE_ERROR("NoxRuntime: the start scene {} could not be loaded", static_cast<uint64_t>(sceneHandle));
            Quit();
            return;
        }

        m_Scene = Scene::Copy(sceneAsset);
        m_Scene->SetRenderer(m_Renderer);
        m_Scene->SetRenderer2D(m_Renderer->getRenderer2D());
        const NRI::Extent2D outputSize = m_Renderer->getOutputSize();
        m_Scene->OnViewportResize(outputSize.width, outputSize.height);
        m_Scene->OnRuntimeStart();
        m_UnloadUnusedAssetsRequested = true;
#if NOX_PROFILE_STATS
        Profiler::Get().ResetStats();
#endif
    }

    void RuntimeLayer::UnloadUnusedAssets()
    {
        std::unordered_set<AssetHandle> referencedAssets;
        m_Scene->CollectAssetReferences(referencedAssets);
        Project::GetActive()->GetEditorAssetManager()->UnloadUnusedAssets(referencedAssets);
    }
}
