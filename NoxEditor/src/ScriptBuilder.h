#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>

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
        const std::string& GetStatus() const { return m_Status; }
        bool AutoBuild = true;

    private:
        void StartBuild();

        std::filesystem::path m_Project;
        std::unique_ptr<Utils::NOXWatcher> m_Watcher;
        std::atomic<bool> m_Changed{ false };
        std::atomic<int64_t> m_ChangedAt{ 0 }; // steady_clock ticks of the last change
        bool m_BuildRequested = false;
        std::future<int> m_Build;
        std::filesystem::path m_Log;
        std::string m_Status;
    };
}
