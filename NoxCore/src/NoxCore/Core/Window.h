#pragma once
#include <string>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

namespace Nox
{
    struct WindowSpecification
    {
        std::string Ttile;
        uint32_t Width = 1280;
        uint32_t Height = 720;
        bool IsResizeable = true;
        bool VSync = true;
    };
    
    // Windowed; Borderless: covering the display at the desktop's resolution (no mode change, alt-tab is instant); Fullscreen:
    // exclusive, in the display's current mode.
    enum class WindowMode : uint8_t { Windowed, Borderless, Fullscreen };
    WindowMode ParseWindowMode(const std::string& name, WindowMode fallback = WindowMode::Borderless);
    const char* GetWindowModeName(WindowMode mode);

    class Window
    {
    public:
        Window(WindowSpecification specification = WindowSpecification());
        ~Window();
        
        void getSizeInPixels(int& width, int& height);

        void SetMode(WindowMode mode);
        WindowMode GetMode() const { return m_Mode; }
        // Alt+Enter: windowed <-> the last full screen mode (Borderless at first).
        void ToggleFullscreen();

    public:
        virtual SDL_Window* getHandle() { return m_window; }
        
    private:
        WindowSpecification m_Specification;
        SDL_Window* m_window = nullptr;
        WindowMode m_Mode = WindowMode::Windowed;
        WindowMode m_LastFullscreenMode = WindowMode::Borderless;
    };
}
