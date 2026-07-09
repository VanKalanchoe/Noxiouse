#define SDL_MAIN_USE_CALLBACKS 1  /* use the callbacks instead of main() */
#include <imgui_impl_sdl3.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "Renderer.h"

/* We will use this renderer to draw into this window every frame. */
static SDL_Window* window = NULL;
std::unique_ptr<Renderer> renderer;
constexpr uint32_t WIDTH = 800;
constexpr uint32_t HEIGHT = 600;
bool minimized = false;
/* This function runs once at startup. */
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
    SDL_SetAppMetadata("Example Renderer Clear", "1.0", "com.example.renderer-clear");

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    window = SDL_CreateWindow("examples/renderer/clear", WIDTH * main_scale, HEIGHT * main_scale, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
    {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    renderer = std::make_unique<Renderer>(*window);

    return SDL_APP_CONTINUE; /* carry on with the program! */
}

/* This function runs when a new event (mouse input, keypresses, etc) occurs. */
SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
    ImGui_ImplSDL3_ProcessEvent(event);
    
    switch (event->type)
    {
    case SDL_EVENT_QUIT: return SDL_APP_SUCCESS; /* end the program, reporting success to the OS. */
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: /* framebuffer resize swapchain */
        renderer->resizeWindow();
        break;
    case SDL_EVENT_WINDOW_MINIMIZED: minimized = true; break;
    case SDL_EVENT_WINDOW_RESTORED: minimized = false; break;
    case SDL_EVENT_KEY_DOWN:
        if (event->key.scancode == SDL_SCANCODE_ESCAPE) return SDL_APP_SUCCESS;
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
    renderer->drawFrame();

    return SDL_APP_CONTINUE; /* carry on with the program! */
}

/* This function runs once at shutdown. */
void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
    /* SDL will clean up the window for us. */
}
