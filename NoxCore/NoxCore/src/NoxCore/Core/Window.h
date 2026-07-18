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
    
    class Window
    {
    public:
        Window(WindowSpecification specification = WindowSpecification());
        ~Window();
        
        void getSizeInPixels(int& width, int& height);

    public:
        virtual SDL_Window* getHandle() { return m_window; }
        
    private:
        WindowSpecification m_Specification;
        SDL_Window* m_window = nullptr;
    };
}
