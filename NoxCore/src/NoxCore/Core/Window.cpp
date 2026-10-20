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
        m_window = SDL_CreateWindow(m_Specification.Ttile.empty() ? "Nox" : m_Specification.Ttile.c_str(), m_Specification.Width * main_scale, m_Specification.Height * main_scale, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!m_window)
        {
            SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        }
        
        SDL_SetWindowMinimumSize(m_window, 640, 480);
    }

    Window::~Window() = default;

    WindowMode ParseWindowMode(const std::string& name, WindowMode fallback)
    {
        if (name == "Windowed") return WindowMode::Windowed;
        if (name == "Borderless") return WindowMode::Borderless;
        if (name == "Fullscreen") return WindowMode::Fullscreen;
        return fallback;
    }

    const char* GetWindowModeName(WindowMode mode)
    {
        switch (mode)
        {
        case WindowMode::Windowed: return "Windowed";
        case WindowMode::Fullscreen: return "Fullscreen";
        default: return "Borderless";
        }
    }

    void Window::SetMode(WindowMode mode)
    {
        if (!m_window)
            return;
        switch (mode)
        {
        case WindowMode::Windowed:
            SDL_SetWindowFullscreen(m_window, false);
            break;
        case WindowMode::Borderless:
            SDL_SetWindowFullscreenMode(m_window, nullptr); // null: the desktop's resolution, no mode change
            SDL_SetWindowFullscreen(m_window, true);
            break;
        case WindowMode::Fullscreen:
        {
            const SDL_DisplayMode* current = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_window));
            SDL_SetWindowFullscreenMode(m_window, current); // exclusive, in the display's current mode
            SDL_SetWindowFullscreen(m_window, true);
            break;
        }
        }
        m_Mode = mode;
        if (mode != WindowMode::Windowed)
            m_LastFullscreenMode = mode;
    }

    void Window::ToggleFullscreen()
    {
        SetMode(m_Mode == WindowMode::Windowed ? m_LastFullscreenMode : WindowMode::Windowed);
    }
    
    void Window::getSizeInPixels(int& width, int& height)
    {
        SDL_GetWindowSizeInPixels(m_window, &width, &height);
    }
}
