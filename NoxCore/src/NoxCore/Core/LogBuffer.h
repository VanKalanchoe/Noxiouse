#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/common.h>

namespace spdlog::sinks
{
    class sink;
}

namespace Nox
{
    // One line of the log as it was written (any thread).
    struct LogEntry
    {
        uint64_t Sequence = 0;                                // 1, 2, 3 ... in the order written
        spdlog::level::level_enum Level = spdlog::level::info;
        std::string Clock;                                    // "hh:mm:ss" local time
        std::string Logger;                                   // "NOX" (engine), "APP" (client)
        std::string Text;
    };

    // The most recent log lines in memory (Unreal's output log history), filled by a spdlog sink every logger writes to: what the
    // editor's Output Log shows, and what a tool can read back without parsing Nox.log. Thread-safe; the oldest lines are dropped
    // past Capacity.
    class LogBuffer
    {
    public:
        static constexpr size_t Capacity = 20000;

        static LogBuffer& Get();

        void Add(LogEntry entry);
        // Appends the lines written after sequence `after` to `out`, oldest first; returns the newest sequence (0 when empty).
        uint64_t CopySince(uint64_t after, std::vector<LogEntry>& out) const;
        uint64_t GetLatestSequence() const;

        // The sink to add to a logger (Log::Init adds it to the engine's and the client's).
        static std::shared_ptr<spdlog::sinks::sink> CreateSink();

    private:
        mutable std::mutex m_Mutex;
        std::deque<LogEntry> m_Entries;
        uint64_t m_NextSequence = 1;
    };
}
