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
}
