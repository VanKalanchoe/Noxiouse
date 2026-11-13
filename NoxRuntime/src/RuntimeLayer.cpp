#include "RuntimeLayer.h"
#include "NoxCore/Audio/AudioMixer.h"

#include <iostream>
#include <thread>
#include <unordered_set>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_keyboard.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/Window.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/Components.h"
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
        m_Headless = Application::IsHeadless();

        const std::filesystem::path projectFile = FindProjectFile();
        if (projectFile.empty() || !Project::Load(projectFile, true))
        {
            NOX_CORE_ERROR("NoxRuntime: no project to play ({}). Usage: NoxRuntime [--server] [--project=]<project.nproj>",
                           projectFile.empty() ? std::string("none found") : projectFile.generic_string());
            Quit();
            return;
        }
        NOX_CORE_INFO("NoxRuntime: {} '{}'", m_Headless ? "serving" : "playing", projectFile.generic_string());
        if (m_Renderer)
        {
            // The project's quality preset ("Auto": the one this GPU should run, the fallback on smaller RT cards).
            m_Renderer->ApplyQualitySetting(Project::GetActive()->GetConfig().RenderQuality);
            m_Renderer->ApplyProjectRendererSettings(Project::GetActive()->GetConfig().Renderer);
        }
        AudioMixer::LoadProjectMixer(false); // its buses, effects and snapshots

        // The project's window mode (Borderless by default); "--windowed" on the command line keeps a window (testing).
        if (!m_Headless)
        {
            const ApplicationCommandLineArgs& args = Application::Get().GetSpecification().CommandLineArgs;
            const bool windowed = args.HasSwitch("--windowed");
            if (!windowed)
                Application::Get().getWindow()->SetMode(ParseWindowMode(Project::GetActive()->GetConfig().WindowMode));
        }

        // The RuntimeAssetManager (Project::Load(path, true)): the cooked assets as they are, nothing on disk is ever written.
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

        if (m_Headless)
        {
            // The console: lines typed into the server's terminal, handed to the main loop (UpdateServerConsole). The thread blocks
            // in the read; it is detached and ends with the process.
            m_Console = std::make_shared<ConsoleInput>();
            std::thread([console = m_Console]()
            {
                std::string line;
                while (std::getline(std::cin, line))
                {
                    std::lock_guard lock(console->Mutex);
                    console->Lines.push_back(line);
                }
            }).detach();
            NOX_CORE_INFO("Server console: 'status', 'quit' (or Ctrl+C)");
        }
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
        // At most once a second (the editor's rule): a streamed world asks for it with nearly every cell and every load.
        if (m_UnloadUnusedAssetsRequested && std::chrono::steady_clock::now() - m_LastUnloadSweep >= std::chrono::seconds(1))
        {
            NOX_PROFILE_SCOPE("Unload Unused Assets");
            m_UnloadUnusedAssetsRequested = false;
            m_LastUnloadSweep = std::chrono::steady_clock::now();
            UnloadUnusedAssets();
        }

        if (m_Renderer)
        {
            // The window is the viewport: the renderer's output is the swapchain, the scene's cameras take its aspect.
            const NRI::Extent2D outputSize = m_Renderer->getOutputSize();
            if (outputSize.width > 0 && outputSize.height > 0)
            {
                m_Renderer->onViewportSizeChange(outputSize); // no-op while the size is unchanged
                m_Scene->OnViewportResize(outputSize.width, outputSize.height);
            }
            m_Scene->SetRenderer(m_Renderer);
            m_Scene->SetRenderer2D(m_Renderer->getRenderer2D());
        }
        {
            NOX_PROFILE_SCOPE("Scene Update");
            m_Scene->OnUpdateRuntime(ts);
        }

        {
            NOX_PROFILE_SCOPE("Asset Manager Update");
            assetManager->Update(); // publishes the background loads that finished
        }

        if (m_Headless)
            UpdateServerConsole(static_cast<float>(ts));
    }

    void RuntimeLayer::UpdateServerConsole(float ts)
    {
        ++m_FramesSinceStatus;
        m_WorstFrameSinceStatus = std::max(m_WorstFrameSinceStatus, ts);
        m_StatusTimer += ts;
        if (m_StatusTimer >= 10.0f)
            LogServerStatus();

        std::deque<std::string> lines;
        if (m_Console)
        {
            std::lock_guard lock(m_Console->Mutex);
            lines.swap(m_Console->Lines);
        }
        for (const std::string& line : lines)
        {
            if (line == "quit" || line == "exit")
            {
                NOX_CORE_INFO("Server: quitting");
                Quit();
            }
            else if (line == "status")
                LogServerStatus();
            else if (!line.empty())
                NOX_CORE_WARN("Server console: unknown command '{}' (status, quit)", line);
        }
    }

    void RuntimeLayer::LogServerStatus()
    {
        if (!m_Scene)
            return;
        const uint64_t tick = m_Scene->GetTick();
        const float seconds = std::max(m_StatusTimer, 1e-3f);
        size_t entities = 0;
        for ([[maybe_unused]] auto entity : m_Scene->GetAllEntitiesWith<IDComponent>())
            ++entities;
        NOX_CORE_INFO("Server status: tick {} ({:.1f} ticks/s), {:.1f} updates/s, worst update {:.1f} ms, {} entities", tick,
                      static_cast<double>(tick - m_TickAtStatus) / seconds, static_cast<double>(m_FramesSinceStatus) / seconds,
                      m_WorstFrameSinceStatus * 1000.0f, entities);
        m_TickAtStatus = tick;
        m_StatusTimer = 0.0f;
        m_FramesSinceStatus = 0;
        m_WorstFrameSinceStatus = 0.0f;
    }

    void RuntimeLayer::OnEvent(Event& event)
    {
        // Alt+Enter: windowed <-> full screen. Esc: quit.
        EventDispatcher dispatcher(event);
        dispatcher.Dispatch<KeyPressedEvent>([](KeyPressedEvent& pressed)
        {
            if (pressed.GetKeyCode() == SDL_SCANCODE_RETURN && (pressed.GetModifiers() & SDL_KMOD_ALT) != 0 && !pressed.IsRepeat())
            {
                Application::Get().getWindow()->ToggleFullscreen();
                return true;
            }
            if (pressed.GetKeyCode() == SDL_SCANCODE_ESCAPE && !pressed.IsRepeat())
            {
                Quit();
                return true;
            }
            return false;
        });
    }

    std::filesystem::path RuntimeLayer::FindProjectFile() const
    {
        const ApplicationCommandLineArgs& args = Application::Get().GetSpecification().CommandLineArgs;
        if (const char* project = args.ProjectFile())
            return std::filesystem::path(project);

        // A packaged game keeps its project in Game/ next to the executable (File > Package Game in the editor).
        const std::filesystem::path workingDirectory = std::filesystem::current_path();
        const std::filesystem::path executableDirectory(SDL_GetBasePath());
        for (const std::filesystem::path& directory : { workingDirectory, executableDirectory, executableDirectory / "Game", workingDirectory / ".." / "Facerun" })
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
        if (m_Renderer)
        {
            m_Scene->SetRenderer(m_Renderer);
            m_Scene->SetRenderer2D(m_Renderer->getRenderer2D());
            const NRI::Extent2D outputSize = m_Renderer->getOutputSize();
            m_Scene->OnViewportResize(outputSize.width, outputSize.height);
        }
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
