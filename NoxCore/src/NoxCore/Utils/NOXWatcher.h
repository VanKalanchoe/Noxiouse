#pragma once

#include <FileWatch.hpp>

namespace Utils
{
    class NOXWatcher
    {
    public:
    void watch(std::filesystem::path path, std::function<void()> onModified);
        
    private:
        std::vector<std::unique_ptr<filewatch::FileWatch<std::string>>> m_activeWatches;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_fileTimestamps;
    };
}
