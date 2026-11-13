#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Nox
{
    // Starts `executable` with `arguments` as a process of its own that outlives this one (the editor relaunching itself into the
    // Project Manager). On Windows it gets its own console: the editor is a console program, and a child sharing the parent's console
    // is killed with it when that console closes (Visual Studio closes the debugged program's console as soon as it exits). It starts in
    // this process's working directory. False with `error` set when it could not be started.
    bool LaunchDetachedProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments, std::string& error);

    // A process the editor started and may end (multiplayer Play's other players and its dedicated server, closed with Stop -- Unreal
    // closes its play-in-new-process windows the same way). Destroying it ends the process.
    class ChildProcess
    {
    public:
        ChildProcess() = default;
        ~ChildProcess() { Kill(); }
        ChildProcess(const ChildProcess&) = delete;
        ChildProcess& operator=(const ChildProcess&) = delete;

        // `console`: a console window of its own (a dedicated server's, to type its commands); else none (its log goes to its file).
        bool Launch(const std::filesystem::path& executable, const std::vector<std::string>& arguments, bool console, std::string& error);
        bool IsRunning() const;
        void Kill();

    private:
        void* m_Handle = nullptr; // HANDLE on Windows, SDL_Process* elsewhere
    };
}
