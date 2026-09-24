#include "Application.h"

#define SDL_MAIN_USE_CALLBACKS 1  /* use the callbacks instead of main() */
#include <imgui_impl_sdl3.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <ranges>

#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Profiling/StatsOverlayLayer.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Events/WindowEvents.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/ImGui/ImGuiLayer.h"
#include "NoxCore/Tasks/JobSystem.h"
#include "NoxCore/Animation/AnimationGraphNodes.h"

namespace Nox
{
    static Application* s_Application = nullptr;

    Application::Application(ApplicationSpecification specification) : m_Specification(std::move(specification))
    {
        NOX_CORE_INFO("Application Start");

        s_Application = this;

        m_JobSystem = std::make_unique<JobSystem>();

        if (!m_Specification.WorkingDirectory.empty())
            std::filesystem::current_path(m_Specification.WorkingDirectory);

        if (m_Specification.WindowSpec.Ttile.empty())
            m_Specification.WindowSpec.Ttile = m_Specification.Name;

        m_Window = std::make_shared<Window>(m_Specification.WindowSpec);

        renderer = std::make_unique<Renderer>(m_Window, m_Specification.isEditor);

        // ImGui
        if (m_Specification.isEditor)
            m_ImGuiLayer = PushLayer<ImGuiLayer>(*renderer);

#if NOX_PROFILE_STATS
        // Nox Stats overlay (F3) for every app, drawn with the engine's screen-space text.
        PushLayer<StatsOverlayLayer>(*renderer);
#endif
    }

    Application::~Application()
    {
        NOX_CORE_INFO("Application Shutdown");

        IsEngineShuttingDown = true;

        PopLayer<ImGuiLayer>();

        s_Application = nullptr;
    }

    void Application::Shutdown()
    {
        NOX_CORE_INFO("Application Shutdown");

        SDL_Event quitEvent;
        SDL_zero(quitEvent); // Zero-initialize
        quitEvent.type = SDL_EVENT_QUIT; // Set event type
        SDL_PushEvent(&quitEvent);
    }

    void Application::Run(AppState& applicationState)
    {
        {
            NOX_PROFILE_SCOPE("Frame");

            const double currentTime = GetTime();
            Timestep timestep = static_cast<float>(currentTime - m_LastFrameTime);
            m_LastFrameTime = currentTime;

            // Main layer update here
            {
                NOX_PROFILE_SCOPE("Layers OnUpdate");
                for (const std::unique_ptr<Layer>& layer : m_LayerStack)
                    layer->OnUpdate(timestep);
            }

            // NOTE: rendering can be done elsewhere (eg. render thread)
            {
                NOX_PROFILE_SCOPE("Layers OnRender");
                for (const std::unique_ptr<Layer>& layer : m_LayerStack)
                    layer->OnRender();
            }

            if (applicationState.app->GetSpecification().isEditor)
            {
                NOX_PROFILE_SCOPE("ImGui Build");
                m_ImGuiLayer->Begin();
                for (const std::unique_ptr<Layer>& layer : m_LayerStack)
                    layer->OnImGuiRender();
                m_ImGuiLayer->End();
            }

            renderer->drawFrame();

            // Frame sync point (§5.3): no frame graph task runs past this.
            m_JobSystem->ResetFrameArenas();
        }

        NOX_PROFILE_FRAME();
    }

    Application& Application::Get()
    {
        assert(s_Application);
        return *s_Application;
    }

    void Application::RaiseEvent(Event& event)
    {
        for (auto& layer : std::views::reverse(m_LayerStack))
        {
            layer->OnEvent(event);
            if (event.Handled)
                break;
        }
    }

    double Application::GetTime()
    {
        // Nanosecond ticks in double seconds: SDL_GetTicks() (milliseconds) quantized the frame delta, and float
        // seconds lose sub-millisecond precision after a few hours of uptime.
        return static_cast<double>(SDL_GetTicksNS()) / 1'000'000'000.0;
    }

    std::string Application::GetExecutableRootPath()
    {
        return SDL_GetBasePath();
    }
}

static bool minimized = false;

/* This function runs once at startup. */
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
    SDL_SetAppMetadata("Nox Engine", "1.0", "com.example.renderer-clear");

    Nox::Log::Init(); // todo: make it disalable when shipping

    // Node-graph domains register their node types once, here, regardless of editor vs. future standalone
    // runtime (NodeTypeRegistry has no de-duplication, so this must run exactly once).
    Nox::RegisterAnimationGraphNodeTypes();

    Nox::ApplicationCommandLineArgs args;
    args.Count = argc;
    args.Args = argv;

    auto applicationState = new Nox::AppState();
    applicationState->app = Nox::CreateApplication(args);
    *appstate = applicationState;

    return SDL_APP_CONTINUE; /* carry on with the program! */
}

/* This function runs when a new event (mouse input, keypresses, etc) occurs. */
SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* currentEvent)
{
    auto applicationState = static_cast<Nox::AppState*>(appstate);

    if (applicationState->app->GetSpecification().isEditor)
        ImGui_ImplSDL3_ProcessEvent(currentEvent);

    switch (currentEvent->type)
    {
    case SDL_EVENT_QUIT: return SDL_APP_SUCCESS; /* end the program, reporting success to the OS. */
    case SDL_EVENT_WINDOW_RESIZED:
        {
            applicationState->app->GetRenderer()->resizeWindow();

            int width, height;
            applicationState->app->getWindow()->getSizeInPixels(width, height);

            Nox::WindowResizeEvent event(width, height); //maybe uint32_t in the future cast ?
            applicationState->app->RaiseEvent(event);

            return SDL_APP_CONTINUE;
        }
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: /* framebuffer resize swapchain */
        {
            applicationState->app->GetRenderer()->resizeWindow();

            int width, height;
            applicationState->app->getWindow()->getSizeInPixels(width, height);

            Nox::WindowResizeEvent event(width, height); //maybe uint32_t in the future cast ?
            applicationState->app->RaiseEvent(event);

            return SDL_APP_CONTINUE;
        }
    case SDL_EVENT_WINDOW_MINIMIZED:
        {
            minimized = true;
            return SDL_APP_CONTINUE;
        }
    case SDL_EVENT_WINDOW_RESTORED:
        {
            minimized = false;
            return SDL_APP_CONTINUE;
        }
    case SDL_EVENT_MOUSE_MOTION:
        {
            SDL_MouseMotionEvent motion = currentEvent->motion;

            int x = motion.x; // X position in **pixels** relative to the window
            int y = motion.y; // Y position in pixels
            int dx = motion.xrel; // Delta X since last event
            int dy = motion.yrel; // Delta Y since last event

            Nox::MouseMovedEvent event(x, y);
            applicationState->app->RaiseEvent(event);

            return SDL_APP_CONTINUE;
        }
    case SDL_EVENT_MOUSE_WHEEL:
        {
            Nox::MouseScrolledEvent event(currentEvent->wheel.x, currentEvent->wheel.y);
            applicationState->app->RaiseEvent(event);

            return SDL_APP_CONTINUE;
        }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        {
            Uint8 sdlButton = currentEvent->button.button;

            Nox::MouseButtonPressedEvent event(sdlButton);
            applicationState->app->RaiseEvent(event);

            return SDL_APP_CONTINUE;
        }
    case SDL_EVENT_KEY_DOWN:
        {
            if (currentEvent->key.scancode == SDL_SCANCODE_ESCAPE) return SDL_APP_SUCCESS;
            
            if (currentEvent->key.scancode == SDL_SCANCODE_F)
            {
                // Freezes the culling view (Renderer::setFrozen captures it again on every freeze).
                applicationState->app->GetRenderer()->setFrozen(!applicationState->app->GetRenderer()->getFrozen());
            }

            SDL_Scancode scan = currentEvent->key.scancode; // maybe keycode better ?
            bool repeat = (currentEvent->key.repeat != 0);

            Nox::KeyPressedEvent event(scan, repeat);
            applicationState->app->RaiseEvent(event);

            return SDL_APP_CONTINUE;
        }
    case SDL_EVENT_DROP_FILE:
        {
            if (currentEvent->drop.data)
            {
                Nox::ExternalFileDropEvent event(currentEvent->drop.data);
                applicationState->app->RaiseEvent(event);
            }
            return SDL_APP_CONTINUE;
        }
    }

    return SDL_APP_CONTINUE; /* carry on with the program! */
}

/* This function runs once per frame, and is the heart of the program. */
SDL_AppResult SDL_AppIterate(void* appstate)
{
    if (minimized)
    {
        SDL_Delay(10);
        return SDL_APP_CONTINUE;
    }

    auto applicationState = static_cast<Nox::AppState*>(appstate);

    applicationState->app->Run(*applicationState);

    return SDL_APP_CONTINUE; /* carry on with the program! */
}

/* This function runs once at shutdown. */
void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
    /* SDL will clean up the window for us. */

    if (!appstate)
        return;

    auto applicationState = static_cast<Nox::AppState*>(appstate);

    if (applicationState->app)
    {
        delete applicationState->app;
        applicationState->app = nullptr; // prevents double delete ????
    }

    delete applicationState;
}
