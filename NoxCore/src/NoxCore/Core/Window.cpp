#include "Window.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_log.h>

namespace Nox
{
    Window::Window(WindowSpecification specification) : m_Specification(std::move(specification))
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        }
        
        float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        m_window = SDL_CreateWindow("examples/renderer/clear", m_Specification.Width * main_scale, m_Specification.Height * main_scale, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!m_window)
        {
            SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        }
        
        SDL_SetWindowMinimumSize(m_window, 640, 480);
    }

    Window::~Window() = default;
    
    void Window::getSizeInPixels(int& width, int& height)
    {
        SDL_GetWindowSizeInPixels(m_window, &width, &height);
    }
}
