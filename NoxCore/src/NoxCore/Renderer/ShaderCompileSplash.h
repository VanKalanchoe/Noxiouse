#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

#include "NRI/ShaderCompiler.h"

struct SDL_Window;
struct SDL_Renderer;

namespace Nox
{
    // A small window with a progress bar while the renderer creates its pipelines at start and shaders have to be compiled (the
    // first start, or after shader changes): the program no longer looks frozen for that minute. It opens only once a shader
    // really compiles -- a start that loads every shader from the cache shows nothing. The bar counts pipelines against how many
    // the last start created (remembered in <file>), events are pumped so the OS does not mark the program as not responding.
    // Lives for the duration of the renderer's start (main thread).
    class ShaderCompileSplash
    {
    public:
        explicit ShaderCompileSplash(std::filesystem::path countFile);
        ~ShaderCompileSplash();

        ShaderCompileSplash(const ShaderCompileSplash&) = delete;
        ShaderCompileSplash& operator=(const ShaderCompileSplash&) = delete;

    private:
        void OnCompile(const std::string& source);
        void OnPipelineCreated();
        void Draw(bool force);

        std::filesystem::path m_CountFile;
        NRI::ShaderProgress m_Progress;
        SDL_Window* m_Window = nullptr;
        SDL_Renderer* m_Renderer = nullptr;
        uint32_t m_Pipelines = 0;
        uint32_t m_Expected = 120; // until a start has been counted
        uint32_t m_Compiles = 0;
        std::string m_Current;
        uint64_t m_LastDrawTicks = 0;
    };
}
