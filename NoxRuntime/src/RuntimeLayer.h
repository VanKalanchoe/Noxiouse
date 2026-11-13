#pragma once
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "NoxCore/Core/Layer.h"
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Ref.h"

namespace Nox
{
    class Renderer;
    class Scene;

    // Plays a project's start scene the way the editor's Play button does (a copy of the scene asset, OnRuntimeStart,
    // OnUpdateRuntime every frame), without the editor around it: no panels, no editor camera, no picking, no undo snapshots,
    // no file watching. For testing a game at its real cost and as the base of the packaged player (R7). Headless (--server): the same
    // game as a dedicated server -- no rendering, a status line in the log every 10 s and a console on stdin (status, quit).
    class RuntimeLayer : public Layer
    {
    public:
        RuntimeLayer();
        ~RuntimeLayer() override;

        void OnUpdate(Timestep ts) override;
        void OnEvent(Event& event) override;

    private:
        // The project to play: the first command line argument, else the first .nproj in the working directory, next to the
        // executable, in Game/ next to it (a packaged game), or in ../Facerun (the sample project, from the editor's working
        // directory).
        std::filesystem::path FindProjectFile() const;
        void StartScene(AssetHandle sceneHandle);
        void UnloadUnusedAssets();
        // Headless: the console's lines (a thread reads stdin) and the periodic status line.
        void UpdateServerConsole(float ts);
        void LogServerStatus();

    private:
        Renderer* m_Renderer = nullptr; // null while headless
        Ref<Scene> m_Scene;
        bool m_UnloadUnusedAssetsRequested = false;
        std::chrono::steady_clock::time_point m_LastUnloadSweep{}; // the sweep runs at most once a second

        // Headless server (the console's lines are read on a thread of their own; shared with it, so it may outlive the layer).
        struct ConsoleInput
        {
            std::mutex Mutex;
            std::deque<std::string> Lines;
        };
        bool m_Headless = false;
        std::shared_ptr<ConsoleInput> m_Console;
        float m_StatusTimer = 0.0f;
        uint32_t m_FramesSinceStatus = 0;
        double m_WorkSinceStatus = 0.0;      // seconds worked by the updates since the last status line
        double m_WorstWorkSinceStatus = 0.0;
        uint64_t m_TickAtStatus = 0;
    };
}
