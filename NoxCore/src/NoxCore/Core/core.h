#pragma once

#include <memory>

#include "Ref.h"

//---------Event def------
#define BIT(x) (1 << (x))
#define Nox_BIND_EVENT_FN(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }
//------------------------

#if defined(_WIN32)
        #define NOX_DEBUGBREAK() __debugbreak()
    #elif defined(__linux__) || defined(__APPLE__)
        #include <signal.h>
        #define NOX_DEBUGBREAK() raise(SIGTRAP)
#else
    #error "Platform doesn't support debugbreak yet!"
#endif

#ifdef NOX_DEBUG
    #define NOX_ENABLE_ASSERTS
#endif

#ifndef NOX_DIST
    #define NOX_ENABLE_VERIFY
#endif

#define NOX_EXPAND_MACRO(x) x
#define NOX_STRINGIFY_MACRO(x) #x

//---------Assert def-----
#ifdef NOX_ENABLE_ASSERTS

    // Alteratively we could use the same "default" message for both "WITH_MSG" and "NO_MSG" and
    // provide support for custom formatting by concatenating the formatting string instead of having the format inside the default message
    #define NOX_INTERNAL_ASSERT_IMPL(type, check, msg, ...) { if(!(check)) { NOX##type##ERROR(msg, __VA_ARGS__); NOX_DEBUGBREAK(); } }
    #define NOX_INTERNAL_ASSERT_WITH_MSG(type, check, ...) NOX_INTERNAL_ASSERT_IMPL(type, check, "Assertion failed: {0}", __VA_ARGS__)
    #define NOX_INTERNAL_ASSERT_NO_MSG(type, check) NOX_INTERNAL_ASSERT_IMPL(type, check, "Assertion '{0}' failed at {1}:{2}", NOX_STRINGIFY_MACRO(check), std::filesystem::path(__FILE__).filename().string(), __LINE__)

    #define NOX_INTERNAL_ASSERT_GET_MACRO_NAME(arg1, arg2, macro, ...) macro
    #define NOX_INTERNAL_ASSERT_GET_MACRO(...) NOX_EXPAND_MACRO( NOX_INTERNAL_ASSERT_GET_MACRO_NAME(__VA_ARGS__, NOX_INTERNAL_ASSERT_WITH_MSG, NOX_INTERNAL_ASSERT_NO_MSG) )

    // Currently accepts at least the condition and one additional parameter (the message) being optional
    #define NOX_ASSERT(...) NOX_EXPAND_MACRO( NOX_INTERNAL_ASSERT_GET_MACRO(__VA_ARGS__)(_, __VA_ARGS__) )
    #define NOX_CORE_ASSERT(...) NOX_EXPAND_MACRO( NOX_INTERNAL_ASSERT_GET_MACRO(__VA_ARGS__)(_CORE_, __VA_ARGS__) )
#else
    #define NOX_ASSERT(...)
    #define NOX_CORE_ASSERT(...)
#endif

//------------------------

//---------Verify def-----
#ifdef NOX_ENABLE_VERIFY

    // Alteratively we could use the same "default" message for both "WITH_MSG" and "NO_MSG" and
    // provide support for custom formatting by concatenating the formatting string instead of having the format inside the default message
    #define NOX_INTERNAL_VERIFY_IMPL(type, check, msg, ...) { if(!(check)) { NOX##type##ERROR(msg, __VA_ARGS__); NOX_DEBUGBREAK(); } }
    #define NOX_INTERNAL_VERIFY_WITH_MSG(type, check, ...) NOX_INTERNAL_VERIFY_IMPL(type, check, "Verification failed: {0}", __VA_ARGS__)
    #define NOX_INTERNAL_VERIFY_NO_MSG(type, check) NOX_INTERNAL_VERIFY_IMPL(type, check, "Verification '{0}' failed at {1}:{2}", NOX_STRINGIFY_MACRO(check), std::filesystem::path(__FILE__).filename().string(), __LINE__)

    #define NOX_INTERNAL_VERIFY_GET_MACRO_NAME(arg1, arg2, macro, ...) macro
    #define NOX_INTERNAL_VERIFY_GET_MACRO(...) NOX_EXPAND_MACRO( NOX_INTERNAL_VERIFY_GET_MACRO_NAME(__VA_ARGS__, NOX_INTERNAL_VERIFY_WITH_MSG, NOX_INTERNAL_VERIFY_NO_MSG) )

    // Currently accepts at least the condition and one additional parameter (the message) being optional
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
