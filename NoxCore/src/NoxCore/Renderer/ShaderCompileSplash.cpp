#include "ShaderCompileSplash.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include <SDL3/SDL.h>

namespace Nox
{
    ShaderCompileSplash::ShaderCompileSplash(std::filesystem::path countFile)
        : m_CountFile(std::move(countFile))
    {
        if (std::ifstream file(m_CountFile); file)
        {
            uint32_t expected = 0;
            if (file >> expected && expected > 0)
                m_Expected = expected;
        }
        m_Progress.OnCompile = [this](const std::string& source) { OnCompile(source); };
        m_Progress.OnPipelineCreated = [this]() { OnPipelineCreated(); };
        m_Progress.OnIdle = [this]() { OnIdle(); };
        m_MainThread = std::this_thread::get_id();
        NRI::SetShaderProgress(&m_Progress);
    }

    ShaderCompileSplash::~ShaderCompileSplash()
    {
        NRI::SetShaderProgress(nullptr);
        // How many pipelines this start created: the next start's 100 %.
        if (m_Pipelines > 0)
        {
            std::error_code error;
            std::filesystem::create_directories(m_CountFile.parent_path(), error);
            std::ofstream(m_CountFile, std::ios::trunc) << m_Pipelines << "\n";
        }
        if (m_Renderer)
            SDL_DestroyRenderer(m_Renderer);
        if (m_Window)
            SDL_DestroyWindow(m_Window);
    }

    void ShaderCompileSplash::OnCompile(const std::string& source)
    {
        {
            std::lock_guard lock(m_Mutex);
            ++m_Compiles;
            m_Current = std::filesystem::path(source).filename().string();
        }
        // On a worker: the main thread draws it while it waits (OnIdle).
        if (std::this_thread::get_id() != m_MainThread)
            return;
        EnsureWindow();
        Draw(true); // before the compile starts, so the name shown is the one it is busy with
    }

    void ShaderCompileSplash::EnsureWindow()
    {
        if (m_Window)
            return;
        {
            std::lock_guard lock(m_Mutex);
            if (m_Compiles == 0)
                return; // everything from the cache so far: no window
        }
        // The first real compile: from here on the start takes a while.
        m_Window = SDL_CreateWindow("Noxiouse - compiling shaders", 560, 150, SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP);
        if (m_Window)
        {
            SDL_SetWindowPosition(m_Window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            m_Renderer = SDL_CreateRenderer(m_Window, nullptr);
            SDL_ShowWindow(m_Window);
        }
    }

    void ShaderCompileSplash::OnPipelineCreated()
    {
        ++m_Pipelines;
        EnsureWindow();
        Draw(false);
    }

    void ShaderCompileSplash::OnIdle()
    {
        EnsureWindow();
        Draw(false);
    }

    void ShaderCompileSplash::Draw(bool force)
    {
        // The OS sees the program answer even while nothing is drawn.
        SDL_PumpEvents();
        if (!m_Renderer)
            return;
        const uint64_t now = SDL_GetTicks();
        if (!force && now - m_LastDrawTicks < 30)
            return;
        m_LastDrawTicks = now;

        int width = 560, height = 150;
        SDL_GetWindowSize(m_Window, &width, &height);
        SDL_SetRenderDrawColor(m_Renderer, 24, 24, 28, 255);
        SDL_RenderClear(m_Renderer);

        SDL_SetRenderDrawColor(m_Renderer, 230, 230, 230, 255);
        SDL_RenderDebugText(m_Renderer, 20.0f, 18.0f, "Noxiouse: compiling shaders");
        SDL_SetRenderDrawColor(m_Renderer, 150, 150, 150, 255);
        SDL_RenderDebugText(m_Renderer, 20.0f, 34.0f, "First start or changed shaders - cached afterwards.");

        // The bar: pipelines created against the last start's count (it may overshoot a little after new pipelines were added).
        const float fraction = std::clamp(static_cast<float>(m_Pipelines) / static_cast<float>(std::max(m_Expected, 1u)), 0.0f, 0.99f);
        const SDL_FRect frame{ 20.0f, 60.0f, static_cast<float>(width) - 40.0f, 22.0f };
        SDL_SetRenderDrawColor(m_Renderer, 50, 50, 58, 255);
        SDL_RenderFillRect(m_Renderer, &frame);
        const SDL_FRect bar{ frame.x + 2.0f, frame.y + 2.0f, (frame.w - 4.0f) * fraction, frame.h - 4.0f };
        SDL_SetRenderDrawColor(m_Renderer, 70, 140, 255, 255);
        SDL_RenderFillRect(m_Renderer, &bar);

        uint32_t compiles = 0;
        std::string current;
        {
            std::lock_guard lock(m_Mutex);
            compiles = m_Compiles;
            current = m_Current;
        }
        char line[160];
        std::snprintf(line, sizeof(line), "%u%%   pipeline %u of ~%u, %u shader files compiled", static_cast<uint32_t>(fraction * 100.0f), m_Pipelines,
                      m_Expected, compiles);
        SDL_SetRenderDrawColor(m_Renderer, 210, 210, 210, 255);
        SDL_RenderDebugText(m_Renderer, 20.0f, 96.0f, line);
        std::snprintf(line, sizeof(line), "Compiling %s (and others in parallel)", current.c_str());
        SDL_SetRenderDrawColor(m_Renderer, 150, 150, 150, 255);
        SDL_RenderDebugText(m_Renderer, 20.0f, 116.0f, line);
        SDL_RenderPresent(m_Renderer);
    }
}
