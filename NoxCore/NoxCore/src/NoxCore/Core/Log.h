#pragma once
#include <memory>

#include <spdlog/spdlog.h>

#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/string_cast.hpp"

// This ignores all warnings raised inside External headers
#pragma warning(push, 0)
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/fmt/std.h>
#pragma warning(pop)

namespace Nox
{
    class Log
    {
    public:
        static void Init();
        
        static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }
        static std::shared_ptr<spdlog::logger>& GetClientLogger() { return s_ClientLogger; }
        
    private:
        static std::shared_ptr<spdlog::logger> s_CoreLogger;
        static std::shared_ptr<spdlog::logger> s_ClientLogger;
    };
}

template<typename OStream, glm::length_t L, typename T, glm::qualifier Q>
inline OStream& operator<<(OStream& os, const glm::vec<L, T, Q>& vector)
{
    return os << glm::to_string(vector);
}

template<typename OStream, glm::length_t C, glm::length_t R, typename T, glm::qualifier Q>
inline OStream& operator<<(OStream& os, const glm::mat<C, R, T, Q>& matrix)
{
    return os << glm::to_string(matrix);
}

template<typename OStream, typename T, glm::qualifier Q>
inline OStream& operator<<(OStream& os, glm::qua<T, Q> quaternion)
{
    return os << glm::to_string(quaternion);
}

// Core log macros
#define NOX_CORE_TRACE(...) ::Nox::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define NOX_CORE_INFO(...)  ::Nox::Log::GetCoreLogger()->info(__VA_ARGS__)
#define NOX_CORE_WARN(...)  ::Nox::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define NOX_CORE_ERROR(...) ::Nox::Log::GetCoreLogger()->error(__VA_ARGS__)
#define NOX_CORE_FATAL(...) ::Nox::Log::GetCoreLogger()->fatal(__VA_ARGS__)

// Client log macros
#define NOX_TRACE(...)      ::Nox::Log::GetClientLogger()->trace(__VA_ARGS__)
#define NOX_INFO(...)       ::Nox::Log::GetClientLogger()->info(__VA_ARGS__)
#define NOX_WARN(...)       ::Nox::Log::GetClientLogger()->warn(__VA_ARGS__)
#define NOX_ERROR(...)      ::Nox::Log::GetClientLogger()->error(__VA_ARGS__)
#define NOX_FATAL(...)      ::Nox::Log::GetClientLogger()->fatal(__VA_ARGS__)