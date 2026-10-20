#pragma once

#include <filesystem>
#include <format>
#include <memory>
#include <string>

#include "Ref.h"
#include "NoxCore/Utils/PlatformUtils.h"

//---------Event def------
#define BIT(x) (1 << (x))
#define Nox_BIND_EVENT_FN(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }
//------------------------

#if defined(_WIN32)
        #define NOX_DEBUGBREAK() __debugbreak()
    #elif defined(__linux__)
        #include <signal.h>
        #define NOX_DEBUGBREAK() raise(SIGTRAP)
#else
    #error "Platform doesn't support debugbreak yet!"
#endif

// A failed assert or verify stops in the debugger only when one is attached. Without one the breakpoint instruction
// ends the process (an unhandled breakpoint exception on Windows, SIGTRAP on Linux), so a Debug build started on its own
// logs the failure and keeps running instead of dying on the first assert.
#define NOX_DEBUGBREAK_IF_ATTACHED() { if (::Nox::Platform::IsDebuggerAttached()) { NOX_DEBUGBREAK(); } }

// NOX_DEBUG comes from CMake for Debug builds (NoxCore/CMakeLists.txt).
#ifdef NOX_DEBUG
    #define NOX_ENABLE_ASSERTS
#endif

#ifndef NOX_DIST
    #define NOX_ENABLE_VERIFY
#endif

#define NOX_EXPAND_MACRO(x) x
#define NOX_STRINGIFY_MACRO(x) #x

namespace Nox::Detail
{
    // Logs a failed assert or verify to the engine's (core) or the application's log (Log.cpp): the macros below expand in
    // headers that do not include the logger.
    void ReportAssertFailure(bool core, const std::string& message);
}
#define NOX_INTERNAL_REPORT_(message) ::Nox::Detail::ReportAssertFailure(false, message)
#define NOX_INTERNAL_REPORT_CORE_(message) ::Nox::Detail::ReportAssertFailure(true, message)

//---------Assert def-----
#ifdef NOX_ENABLE_ASSERTS

    // The message is a std::format string with its arguments, e.g. NOX_CORE_ASSERT(file, "Cannot open {}", path):
    // it is only formatted when the check fails.
    #define NOX_INTERNAL_ASSERT_IMPL(type, check, msg, ...) { if(!(check)) { NOX_INTERNAL_REPORT##type(std::format(msg, __VA_ARGS__)); NOX_DEBUGBREAK_IF_ATTACHED(); } }
    #define NOX_INTERNAL_ASSERT_WITH_MSG(type, check, ...) NOX_INTERNAL_ASSERT_IMPL(type, check, "Assertion '{0}' failed at {1}:{2}: {3}", NOX_STRINGIFY_MACRO(check), std::filesystem::path(__FILE__).filename().string(), __LINE__, std::format(__VA_ARGS__))
    #define NOX_INTERNAL_ASSERT_NO_MSG(type, check) NOX_INTERNAL_ASSERT_IMPL(type, check, "Assertion '{0}' failed at {1}:{2}", NOX_STRINGIFY_MACRO(check), std::filesystem::path(__FILE__).filename().string(), __LINE__)

    // Picks NO_MSG for the condition alone and WITH_MSG for a condition, a message and up to four format arguments.
    #define NOX_INTERNAL_ASSERT_GET_MACRO_NAME(arg1, arg2, arg3, arg4, arg5, arg6, macro, ...) macro
    #define NOX_INTERNAL_ASSERT_GET_MACRO(...) NOX_EXPAND_MACRO( NOX_INTERNAL_ASSERT_GET_MACRO_NAME(__VA_ARGS__, NOX_INTERNAL_ASSERT_WITH_MSG, NOX_INTERNAL_ASSERT_WITH_MSG, NOX_INTERNAL_ASSERT_WITH_MSG, NOX_INTERNAL_ASSERT_WITH_MSG, NOX_INTERNAL_ASSERT_WITH_MSG, NOX_INTERNAL_ASSERT_NO_MSG) )

    // The condition first, then optionally the message and its format arguments
    #define NOX_ASSERT(...) NOX_EXPAND_MACRO( NOX_INTERNAL_ASSERT_GET_MACRO(__VA_ARGS__)(_, __VA_ARGS__) )
    #define NOX_CORE_ASSERT(...) NOX_EXPAND_MACRO( NOX_INTERNAL_ASSERT_GET_MACRO(__VA_ARGS__)(_CORE_, __VA_ARGS__) )
#else
    #define NOX_ASSERT(...)
    #define NOX_CORE_ASSERT(...)
#endif

//------------------------

//---------Verify def-----
#ifdef NOX_ENABLE_VERIFY

    // Same forms as NOX_ASSERT, but kept in every build except Dist.
    #define NOX_INTERNAL_VERIFY_IMPL(type, check, msg, ...) { if(!(check)) { NOX_INTERNAL_REPORT##type(std::format(msg, __VA_ARGS__)); NOX_DEBUGBREAK_IF_ATTACHED(); } }
    #define NOX_INTERNAL_VERIFY_WITH_MSG(type, check, ...) NOX_INTERNAL_VERIFY_IMPL(type, check, "Verification '{0}' failed at {1}:{2}: {3}", NOX_STRINGIFY_MACRO(check), std::filesystem::path(__FILE__).filename().string(), __LINE__, std::format(__VA_ARGS__))
    #define NOX_INTERNAL_VERIFY_NO_MSG(type, check) NOX_INTERNAL_VERIFY_IMPL(type, check, "Verification '{0}' failed at {1}:{2}", NOX_STRINGIFY_MACRO(check), std::filesystem::path(__FILE__).filename().string(), __LINE__)

    #define NOX_INTERNAL_VERIFY_GET_MACRO_NAME(arg1, arg2, arg3, arg4, arg5, arg6, macro, ...) macro
    #define NOX_INTERNAL_VERIFY_GET_MACRO(...) NOX_EXPAND_MACRO( NOX_INTERNAL_VERIFY_GET_MACRO_NAME(__VA_ARGS__, NOX_INTERNAL_VERIFY_WITH_MSG, NOX_INTERNAL_VERIFY_WITH_MSG, NOX_INTERNAL_VERIFY_WITH_MSG, NOX_INTERNAL_VERIFY_WITH_MSG, NOX_INTERNAL_VERIFY_WITH_MSG, NOX_INTERNAL_VERIFY_NO_MSG) )

    // The condition first, then optionally the message and its format arguments
    #define NOX_VERIFY(...) NOX_EXPAND_MACRO( NOX_INTERNAL_VERIFY_GET_MACRO(__VA_ARGS__)(_, __VA_ARGS__) )
    #define NOX_CORE_VERIFY(...) NOX_EXPAND_MACRO( NOX_INTERNAL_VERIFY_GET_MACRO(__VA_ARGS__)(_CORE_, __VA_ARGS__) )
#else
    #define NOX_VERIFY(...)
    #define NOX_CORE_VERIFY(...)
#endif

namespace Nox
{
    inline bool IsEngineShuttingDown = false;
    
    template <typename T>
    using Scope = std::unique_ptr<T>;
    template <typename T, typename... Args>
    constexpr Scope<T> CreateScope(Args&&... args)
    {
        return std::make_unique<T>(std::forward<Args>(args)...);
    }

    template <typename T, typename... Args>
    Ref<T> CreateRef(Args&&... args)
    {
        // Simply call constructor instead of T::Create()
        T* obj = new T(std::forward<Args>(args)...);
        return Ref<T>(obj);
    }
}
