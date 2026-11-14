#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "ScriptDiagnostics.h"

namespace Utils { class NOXWatcher; }

namespace Nox
{
    // Builds the project's C# scripts (dotnet build of its .csproj) and hot-reloads them (ScriptEngine::ReloadAssembly): on
    // request, or by itself a moment after a .cs file under the project's scripts changed (debounced: saving several files
    // builds once). The build runs on its own thread; the reload happens on the main thread once it succeeded.
    class ScriptBuilder
    {
    public:
        ScriptBuilder();
        ~ScriptBuilder();

        // The project's scripts (ScriptEngine::GetProjectScriptsProject); empty stops watching.
        void SetProject(const std::filesystem::path& csproj);
        // Once a frame: starts a due build, reloads after a finished one.
        void Update();
        void RequestBuild();

        bool IsBuilding() const { return m_Build.valid(); }
        // A build asked for (RequestBuild) or due (a change) that has not started yet.
        bool IsBuildPending() const { return m_BuildRequested || (AutoBuild && m_Changed); }
        const std::string& GetStatus() const { return m_Status; }
        bool AutoBuild = true;

        // The last finished build: whether it compiled, whether the scripts were reloaded, its errors and warnings, the full output.
        struct BuildResult
        {
            uint64_t Number = 0;  // 1, 2, 3 ... per finished build (0: none yet)
            bool Succeeded = false;
            bool Reloaded = false;
            double Seconds = 0.0;
            std::vector<ScriptDiagnostic> Diagnostics;
            std::filesystem::path Log;
        };
        const BuildResult& GetLastResult() const { return m_LastResult; }

    private:
        void StartBuild();

        std::filesystem::path m_Project;
        std::unique_ptr<Utils::NOXWatcher> m_Watcher;
        std::atomic<bool> m_Changed{ false };
        std::atomic<int64_t> m_ChangedAt{ 0 }; // steady_clock ticks of the last change
        bool m_BuildRequested = false;
        std::future<int> m_Build;
        std::chrono::steady_clock::time_point m_BuildStarted;
        BuildResult m_LastResult;
        std::filesystem::path m_Log;
        std::string m_Status;
    };
}
