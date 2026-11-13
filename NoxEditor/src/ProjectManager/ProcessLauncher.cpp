#include "ProcessLauncher.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
    bool ChildProcess::Launch(const std::filesystem::path& executable, const std::vector<std::string>& arguments, bool console, std::string& error)
    {
        Kill();
        std::wstring commandLine = QuoteArgument(executable.wstring());
        for (const std::string& argument : arguments)
            commandLine += L" " + QuoteArgument(Widen(argument));
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const DWORD flags = (console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW) | CREATE_NEW_PROCESS_GROUP;
        if (!CreateProcessW(executable.wstring().c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup, &process))
        {
            error = "CreateProcess failed (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        CloseHandle(process.hThread);
        m_Handle = process.hProcess;
        return true;
    }

    bool ChildProcess::IsRunning() const
    {
        return m_Handle && WaitForSingleObject(static_cast<HANDLE>(m_Handle), 0) == WAIT_TIMEOUT;
    }

    void ChildProcess::Kill()
    {
        if (!m_Handle)
            return;
        if (WaitForSingleObject(static_cast<HANDLE>(m_Handle), 0) == WAIT_TIMEOUT)
            TerminateProcess(static_cast<HANDLE>(m_Handle), 0);
        CloseHandle(static_cast<HANDLE>(m_Handle));
        m_Handle = nullptr;
    }
#else
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#endif

namespace Nox
{
#ifdef _WIN32
    namespace
    {
        // One argument quoted the way CommandLineToArgvW / the C runtime split it back (backslashes doubled before quotes).
        std::wstring QuoteArgument(const std::wstring& argument)
        {
            if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos)
                return argument;
            std::wstring quoted = L"\"";
            size_t backslashes = 0;
            for (wchar_t c : argument)
            {
                if (c == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (c == L'"')
                    quoted.append(backslashes * 2 + 1, L'\\');
                else
                    quoted.append(backslashes, L'\\');
                backslashes = 0;
                quoted.push_back(c);
            }
            quoted.append(backslashes * 2, L'\\');
            quoted.push_back(L'"');
            return quoted;
        }

        std::wstring Widen(const std::string& text)
        {
            if (text.empty())
                return {};
            const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring wide(static_cast<size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), length);
            return wide;
        }
    }

    bool LaunchDetachedProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments, std::string& error)
    {
        std::wstring commandLine = QuoteArgument(executable.wstring());
        for (const std::string& argument : arguments)
            commandLine += L" " + QuoteArgument(Widen(argument));

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        // No handles inherited, a console of its own, this process's working directory (where the editor's assets are found).
        if (!CreateProcessW(executable.wstring().c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP,
                            nullptr, nullptr, &startup, &process))
        {
            error = "CreateProcess failed (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }
#else
    bool LaunchDetachedProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments, std::string& error)
    {
        std::vector<std::string> all{ executable.string() };
        all.insert(all.end(), arguments.begin(), arguments.end());
        std::vector<const char*> argv;
        for (const std::string& argument : all)
            argv.push_back(argument.c_str());
        argv.push_back(nullptr);

        // In the background (its own session: it is not stopped with this process's terminal), standard output still shown.
        const SDL_PropertiesID properties = SDL_CreateProperties();
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
        SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_INHERITED);
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED);
        SDL_Process* process = SDL_CreateProcessWithProperties(properties);
        SDL_DestroyProperties(properties);
        if (!process)
        {
            error = SDL_GetError();
            return false;
        }
        SDL_DestroyProcess(process); // (only the handle: the process keeps running)
        return true;
    }
    bool ChildProcess::Launch(const std::filesystem::path& executable, const std::vector<std::string>& arguments, bool console, std::string& error)
    {
        (void)console; // (a terminal program's output goes where this process's goes)
        Kill();
        std::vector<std::string> all{ executable.string() };
        all.insert(all.end(), arguments.begin(), arguments.end());
        std::vector<const char*> argv;
        for (const std::string& argument : all)
            argv.push_back(argument.c_str());
        argv.push_back(nullptr);
        const SDL_PropertiesID properties = SDL_CreateProperties();
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_INHERITED);
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED);
        SDL_Process* process = SDL_CreateProcessWithProperties(properties);
        SDL_DestroyProperties(properties);
        if (!process)
        {
            error = SDL_GetError();
            return false;
        }
        m_Handle = process;
        return true;
    }

    bool ChildProcess::IsRunning() const
    {
        int exitCode = 0;
        return m_Handle && !SDL_WaitProcess(static_cast<SDL_Process*>(m_Handle), false, &exitCode);
    }

    void ChildProcess::Kill()
    {
        if (!m_Handle)
            return;
        SDL_Process* process = static_cast<SDL_Process*>(m_Handle);
        int exitCode = 0;
        if (!SDL_WaitProcess(process, false, &exitCode))
            SDL_KillProcess(process, true);
        SDL_DestroyProcess(process);
        m_Handle = nullptr;
    }
#endif
}
