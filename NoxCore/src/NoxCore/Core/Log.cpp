#include "Log.h"

#include <cstdlib>
#include <exception>

#include "spdlog/sinks/stdout_color_sinks.h"
#include <spdlog/sinks/basic_file_sink.h>

namespace Nox
{
    std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
    std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

    void Log::Init()
    {
        std::vector<spdlog::sink_ptr> logSinks;
        logSinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        
        logSinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("Nox.log", true));
        
        logSinks[0]->set_pattern("%^[%T] %n: %v%$");
        logSinks[1]->set_pattern("%^[%T] %n: %v%$");
        
        s_CoreLogger = std::make_shared<spdlog::logger>("NOX", logSinks.begin(), logSinks.end());
        spdlog::register_logger(s_CoreLogger);
        s_CoreLogger->set_level(spdlog::level::trace);

        s_ClientLogger = std::make_shared<spdlog::logger>("APP", logSinks.begin(), logSinks.end());
        spdlog::register_logger(s_ClientLogger);
        s_ClientLogger->set_level(spdlog::level::trace);

        // Warnings and errors are written through (a buffered tail is lost in a crash); flushing every info line made
        // asset loads crawl.
        s_CoreLogger->flush_on(spdlog::level::warn);
        s_ClientLogger->flush_on(spdlog::level::warn);

        // An exception nothing caught (e.g. vk::DeviceLostError) otherwise ends the process without a word, taking the
        // buffered tail of the log with it: log what it was and flush, then terminate as before.
        std::set_terminate([]
        {
            if (const std::exception_ptr exception = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(exception);
                }
                catch (const std::exception& error)
                {
                    s_CoreLogger->critical("Unhandled exception: {}", error.what());
                }
                catch (...)
                {
                    s_CoreLogger->critical("Unhandled exception of unknown type");
                }
            }
            s_CoreLogger->flush();
            std::abort();
        });
    }
}
