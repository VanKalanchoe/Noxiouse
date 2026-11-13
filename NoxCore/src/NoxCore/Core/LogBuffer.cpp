#include "LogBuffer.h"

#include <chrono>
#include <ctime>

#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

namespace Nox
{
    namespace
    {
        // Every line into the LogBuffer (the buffer locks; the sink itself needs no lock of its own).
        class LogBufferSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex>
        {
        protected:
            void sink_it_(const spdlog::details::log_msg& message) override
            {
                LogEntry entry;
                entry.Level = message.level;
                entry.Logger.assign(message.logger_name.data(), message.logger_name.size());
                entry.Text.assign(message.payload.data(), message.payload.size());
                const std::time_t seconds = std::chrono::system_clock::to_time_t(message.time);
                std::tm local{};
#ifdef _WIN32
                localtime_s(&local, &seconds);
#else
                localtime_r(&seconds, &local);
#endif
                char clock[16];
                std::strftime(clock, sizeof(clock), "%H:%M:%S", &local);
                entry.Clock = clock;
                LogBuffer::Get().Add(std::move(entry));
            }
            void flush_() override {}
        };
    }

    LogBuffer& LogBuffer::Get()
    {
        static LogBuffer buffer;
        return buffer;
    }

    void LogBuffer::Add(LogEntry entry)
    {
        std::lock_guard lock(m_Mutex);
        entry.Sequence = m_NextSequence++;
        m_Entries.push_back(std::move(entry));
        while (m_Entries.size() > Capacity)
            m_Entries.pop_front();
    }

    uint64_t LogBuffer::CopySince(uint64_t after, std::vector<LogEntry>& out) const
    {
        std::lock_guard lock(m_Mutex);
        if (m_Entries.empty())
            return 0;
        // Sequences are consecutive: the first newer line is found by arithmetic, not a search.
        const uint64_t first = m_Entries.front().Sequence;
        const size_t start = after < first ? 0 : static_cast<size_t>(after - first + 1);
        for (size_t index = start; index < m_Entries.size(); ++index)
            out.push_back(m_Entries[index]);
        return m_Entries.back().Sequence;
    }

    uint64_t LogBuffer::GetLatestSequence() const
    {
        std::lock_guard lock(m_Mutex);
        return m_Entries.empty() ? 0 : m_Entries.back().Sequence;
    }

    std::shared_ptr<spdlog::sinks::sink> LogBuffer::CreateSink()
    {
        auto sink = std::make_shared<LogBufferSink>();
        sink->set_level(spdlog::level::trace);
        return sink;
    }
}
