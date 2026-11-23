#pragma once
#include <string>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>
#include <NRI/Extensions/NRIDeviceCreation.h>

namespace Nox
{
    struct WindowSpecification
    {
        std::string Ttile;
        uint32_t Width = 1280;
        uint32_t Height = 720;
        bool IsResizeable = true;
        bool VSync = true;
        bool IsVulkan = true;
        bool Borderless = false;
        bool DestroyOnDestruct = false;
        bool HasPosition = false;
        int PosX = 0;
        int PosY = 0;
        int MinWidth = 640;
        int MinHeight = 480;
        bool ScaleByDisplay = true;
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
        void GetSize(int& width, int& height) const;
        void GetPosition(int& x, int& y) const;
        void SetPosition(int x, int y);
        void SetSize(int width, int height);
        void SetTitle(const std::string& title);
        void Focus();
        uint32_t GetWindowID() const;
        float GetContentScale() const;

        void SetMode(WindowMode mode);
        WindowMode GetMode() const { return m_Mode; }
        // Alt+Enter: windowed <-> the last full screen mode (Borderless at first).
        void ToggleFullscreen();

    public:
        virtual SDL_Window* getHandle() { return m_window; }
        NriNativeWindowHandle GetNativeWindowHandle() const;

        static NriNativeWindowHandle MakeNriWindow(SDL_Window* window);
        
    private:
        WindowSpecification m_Specification;
        SDL_Window* m_window = nullptr;
        WindowMode m_Mode = WindowMode::Windowed;
        WindowMode m_LastFullscreenMode = WindowMode::Borderless;
    };
}
