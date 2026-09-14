#pragma once

#include <FileWatch.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Utils
{
    class NOXWatcher
    {
    public:
    void watch(std::filesystem::path path, std::function<void(const std::filesystem::path&)> onModified);

    private:
        std::vector<std::unique_ptr<filewatch::FileWatch<std::string>>> m_activeWatches;

        // Every FileWatch runs its callback on its own thread, all touching these.
        std::mutex m_mutex;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_fileTimestamps;
        std::unordered_map<std::string, std::filesystem::file_time_type> m_lastWriteTimes;
    };
}
