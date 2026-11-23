#include "Window.h"

#include <algorithm>
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
        
        float main_scale = m_Specification.ScaleByDisplay ? SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()) : 1.0f;
        SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (m_Specification.IsResizeable)
            flags |= SDL_WINDOW_RESIZABLE;
        if (m_Specification.Borderless)
            flags |= SDL_WINDOW_BORDERLESS;
        if (m_Specification.IsVulkan)
            flags |= SDL_WINDOW_VULKAN;
        const int initialW = std::max(1, static_cast<int>(m_Specification.Width * main_scale));
        const int initialH = std::max(1, static_cast<int>(m_Specification.Height * main_scale));
        m_window = SDL_CreateWindow(m_Specification.Ttile.empty() ? "Nox" : m_Specification.Ttile.c_str(), initialW, initialH, flags);
        if (!m_window)
        {
            SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        }
        else
        {
            SDL_SetWindowMinimumSize(m_window, m_Specification.MinWidth, m_Specification.MinHeight);
            if (m_Specification.HasPosition)
                SDL_SetWindowPosition(m_window, m_Specification.PosX, m_Specification.PosY);
        }
    }

    Window::~Window()
    {
        if (m_Specification.DestroyOnDestruct && m_window)
        {
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
        }
    }

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
        if (m_window)
            SDL_GetWindowSizeInPixels(m_window, &width, &height);
        else
            width = height = 0;
    }

    void Window::GetSize(int& width, int& height) const
    {
        if (m_window)
            SDL_GetWindowSize(m_window, &width, &height);
        else
            width = height = 0;
    }

    void Window::GetPosition(int& x, int& y) const
    {
        if (m_window)
            SDL_GetWindowPosition(m_window, &x, &y);
        else
            x = y = 0;
    }

    void Window::SetPosition(int x, int y)
    {
        if (m_window)
            SDL_SetWindowPosition(m_window, x, y);
    }

    void Window::SetSize(int width, int height)
    {
        if (m_window)
            SDL_SetWindowSize(m_window, std::max(width, 1), std::max(height, 1));
    }

    void Window::SetTitle(const std::string& title)
    {
        if (m_window)
            SDL_SetWindowTitle(m_window, title.c_str());
    }

    void Window::Focus()
    {
        if (m_window)
            SDL_RaiseWindow(m_window);
    }

    uint32_t Window::GetWindowID() const
    {
        return m_window ? SDL_GetWindowID(m_window) : 0;
    }

    float Window::GetContentScale() const
    {
        return m_window ? SDL_GetWindowDisplayScale(m_window) : 1.0f;
    }

    NriNativeWindowHandle Window::GetNativeWindowHandle() const
    {
        return MakeNriWindow(m_window);
    }

    NriNativeWindowHandle Window::MakeNriWindow(SDL_Window* window)
    {
        NriNativeWindowHandle handle{};
        if (!window)
        {
            handle.type = NRI_WINDOW_SYSTEM_HEADLESS;
            return handle;
        }
        const SDL_PropertiesID props = SDL_GetWindowProperties(window);
#ifdef _WIN32
        handle.type = NRI_WINDOW_SYSTEM_WIN32;
        handle.hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        handle.hinstance = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);
#else
        handle.type = NRI_WINDOW_SYSTEM_HEADLESS;
#endif
        return handle;
    }
}
