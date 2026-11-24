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
    class ScriptBuilder
    {
    public:
        ScriptBuilder();
        ~ScriptBuilder();

        void SetProject(const std::filesystem::path& csproj);
        void Update();
        void RequestBuild();

        bool IsBuilding() const { return m_Build.valid(); }
        bool IsBuildPending() const { return m_BuildRequested || (AutoBuild && m_Changed); }
        const std::string& GetStatus() const { return m_Status; }
        bool AutoBuild = true;

        struct BuildResult
        {
            uint64_t Number = 0;
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
        std::atomic<int64_t> m_ChangedAt{ 0 };
        bool m_BuildRequested = false;
        std::future<int> m_Build;
        std::chrono::steady_clock::time_point m_BuildStarted;
        BuildResult m_LastResult;
        std::filesystem::path m_Log;
        std::string m_Status;
    };
}
