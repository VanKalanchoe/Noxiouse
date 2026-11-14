#include "ScriptBuilder.h"

#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Scripting/ScriptEngine.h"
#include "NoxCore/Utils/NOXWatcher.h"

namespace Nox
{
    namespace
    {
        // A change this long ago and none since: saving several files (or an IDE's rename across files) builds once.
        constexpr std::chrono::milliseconds kDebounce{ 750 };

#ifdef NDEBUG
        constexpr const char* kConfiguration = "Release";
#else
        constexpr const char* kConfiguration = "Debug";
#endif
    }

    ScriptBuilder::ScriptBuilder() = default;
    ScriptBuilder::~ScriptBuilder()
    {
        m_Watcher.reset(); // its threads call back into this
        if (m_Build.valid())
            m_Build.wait();
    }

    void ScriptBuilder::SetProject(const std::filesystem::path& csproj)
    {
        if (csproj == m_Project)
            return;
        m_Project = csproj;
        m_Watcher.reset();
        m_Changed = false;
        if (m_Project.empty())
            return;
        m_Log = m_Project.parent_path() / "obj" / "NoxBuild.log";
        m_Watcher = std::make_unique<Utils::NOXWatcher>();
        m_Watcher->watch(m_Project.parent_path(), [this](const std::filesystem::path& path)
        {
            // Sources only: the build's own output (bin, obj) changes while it runs.
            if (path.extension() != ".cs")
                return;
            const std::string generic = path.generic_string();
            if (generic.find("/bin/") != std::string::npos || generic.find("/obj/") != std::string::npos)
                return;
            m_ChangedAt = std::chrono::steady_clock::now().time_since_epoch().count();
            m_Changed = true;
        });
    }

    void ScriptBuilder::RequestBuild()
    {
        m_BuildRequested = true;
    }

    void ScriptBuilder::StartBuild()
    {
        if (m_Project.empty())
        {
            m_Status = "No C# project in the project's Assets/Scripts";
            NOX_CORE_WARN("Scripts: {}", m_Status);
            return;
        }
        std::error_code ignored;
        std::filesystem::create_directories(m_Log.parent_path(), ignored);
        std::string command = "dotnet build \"" + m_Project.string() + "\" --configuration " + kConfiguration +
                              " -nologo -verbosity:quiet > \"" + m_Log.string() + "\" 2>&1";
#ifdef _WIN32
        command = "\"" + command + "\""; // cmd /c drops the outer quotes and keeps the inner ones
#endif
        m_Status = "Building " + m_Project.filename().string() + "...";
        NOX_CORE_INFO("Scripts: building {}", m_Project.generic_string());
        m_BuildStarted = std::chrono::steady_clock::now();
        m_Build = std::async(std::launch::async, [command]() { return std::system(command.c_str()); });
    }

    void ScriptBuilder::Update()
    {
        if (m_Build.valid())
        {
            if (m_Build.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            const int result = m_Build.get();
            // The compiler's messages as data (MSBuild prints each twice: in place and in its summary; kept once).
            std::string output;
            {
                std::ifstream log(m_Log, std::ios::binary);
                output.assign(std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>());
            }
            BuildResult built;
            built.Number = m_LastResult.Number + 1;
            built.Succeeded = result == 0;
            built.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_BuildStarted).count();
            built.Diagnostics = ParseScriptBuildOutput(output);
            built.Log = m_Log;
            size_t errors = 0, warnings = 0;
            for (const ScriptDiagnostic& diagnostic : built.Diagnostics)
                (diagnostic.Severity == "error" ? errors : warnings)++;
            if (built.Succeeded)
            {
                built.Reloaded = ScriptEngine::ReloadAssembly();
                m_Status = built.Reloaded ? "Scripts built and reloaded" : "Scripts built; the reload failed (see the log)";
                NOX_CORE_INFO("Scripts: {} ({:.1f} s{})", m_Status, built.Seconds, warnings ? std::format(", {} warnings", warnings) : std::string());
            }
            else
            {
                // The compiler's errors, into the editor's log: file(line,col) code message.
                for (const ScriptDiagnostic& diagnostic : built.Diagnostics)
                {
                    if (diagnostic.Severity != "error")
                        continue;
                    const std::string place = diagnostic.File.empty() ? std::string()
                                            : std::format("{}({},{}): ", diagnostic.File, diagnostic.Line, diagnostic.Column);
                    NOX_CORE_ERROR("[C# build] {}{} {}", place, diagnostic.Code, diagnostic.Message);
                }
                if (errors == 0) // (failed without a message we can read: the output's last lines)
                    NOX_CORE_ERROR("[C# build] the build failed without a compiler error; see {}", m_Log.generic_string());
                m_Status = "Script build failed (" + std::to_string(errors) + " errors)";
                NOX_CORE_ERROR("Scripts: build failed, the scripts that were running stay; full output in {}", m_Log.generic_string());
            }
            m_LastResult = std::move(built);
        }

        bool due = m_BuildRequested;
        if (!due && AutoBuild && m_Changed)
        {
            const auto changedAt = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(m_ChangedAt.load()));
            due = std::chrono::steady_clock::now() - changedAt >= kDebounce;
        }
        if (!due)
            return;
        m_BuildRequested = false;
        m_Changed = false;
        StartBuild();
    }
}
