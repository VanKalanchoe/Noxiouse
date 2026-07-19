#include "Application.h"

#define SDL_MAIN_USE_CALLBACKS 1  /* use the callbacks instead of main() */
#include <imgui_impl_sdl3.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <ranges>

#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Events/WindowEvents.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/ImGui/ImGuiLayer.h"


namespace Nox
{
    static Application* s_Application = nullptr;

    Application::Application(ApplicationSpecification specification) : m_Specification(std::move(specification))
    {
        s_Application = this;
        
        if (m_Specification.WindowSpec.Ttile.empty())
            m_Specification.WindowSpec.Ttile = m_Specification.Name;
        
        m_Window = std::make_shared<Window>(m_Specification.WindowSpec);
        
        renderer = std::make_unique<Renderer>(m_Window, m_Specification.isEditor);
        
        // ImGui
        if (m_Specification.isEditor)
            m_ImGuiLayer = PushLayer<ImGuiLayer>(*renderer);
    }

    Application::~Application()
    {
        IsEngineShuttingDown = true;
        
        PopLayer<ImGuiLayer>();
        
        s_Application = nullptr;
    }
    
    void Application::Shutdown()
    {
        SDL_Event quitEvent;
        SDL_zero(quitEvent); // Zero-initialize
        quitEvent.type = SDL_EVENT_QUIT; // Set event type
        SDL_PushEvent(&quitEvent);
    }

    void Application::Run(const AppState& applicationState) const
    {
        float currentTime = GetTime();
        float timestep = glm::clamp(currentTime - applicationState.lastTime, 0.001f, 0.1f);
        
        // Main layer update here
        for ( const std::unique_ptr<Layer>& layer : m_LayerStack )
            layer->OnUpdate(timestep);
        
        // NOTE: rendering can be done elsewhere (eg. render thread)
        for (const std::unique_ptr<Layer>& layer : m_LayerStack)
            layer->OnRender();
        
        if (applicationState.app->GetSpecification().isEditor)
        {
            m_ImGuiLayer->Begin();
            for (const std::unique_ptr<Layer>& layer : m_LayerStack)
                layer->OnImGuiRender();
            m_ImGuiLayer->End();
        }
        
        renderer->drawFrame();
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

    float Application::GetTime()
    {
        return static_cast<float>(SDL_GetTicks()) / 1000.0f;
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
    
    Nox::ApplicationCommandLineArgs args;
    args.Count = argc;
    args.Args = argv;
    
    auto applicationState = new Nox::AppState();
    applicationState->app = Nox::CreateApplication(args);
    applicationState->lastTime = Nox::Application::GetTime();
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
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: /* framebuffer resize swapchain */
        {
            applicationState->app->GetRenderer()->resizeWindow();
            
            int width, height;
            applicationState->app->getWindow()->getSizeInPixels(width, height);
            Nox::WindowResizeEvent event(width, height); //maybe uint32_t in the future cast ?
            applicationState->app->RaiseEvent(event);
            return SDL_APP_CONTINUE;
        }
    case SDL_EVENT_WINDOW_MINIMIZED: minimized = true; return SDL_APP_CONTINUE;;
    case SDL_EVENT_WINDOW_RESTORED: minimized = false; return SDL_APP_CONTINUE;;
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
            
            SDL_Scancode scan = currentEvent->key.scancode; // maybe keycode better ?
            bool repeat = (currentEvent->key.repeat != 0);

            Nox::KeyPressedEvent event(scan, repeat);
            applicationState->app->RaiseEvent(event);
                        
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
