#include "NOXWatcher.h"

#include "NoxCore/Core/Log.h"

namespace Utils
{
    void NOXWatcher::watch(std::filesystem::path path, std::function<void(const std::filesystem::path&)> onModified)
    {
        if (!std::filesystem::exists(path))
        {
            NOX_CORE_ERROR("NOXWatcher::watch Cannot watch path because it does not exist: {}", path.string());

            return;
        }

        std::filesystem::path absPath = std::filesystem::absolute(path);
        std::string absolutePathStr = absPath.string();
        bool isDir = std::filesystem::is_directory(absPath);

        // Files not written after this point have unchanged content, whatever events arrive for them.
        const std::filesystem::file_time_type watchStart = std::filesystem::file_time_type::clock::now();

        {
            std::scoped_lock lock(m_mutex);
            m_fileTimestamps[absolutePathStr] = std::chrono::steady_clock::now();
        }

        m_activeWatches.push_back(std::make_unique<filewatch::FileWatch<std::string>>(
            absolutePathStr,
            [this, onModified, absPath, isDir, watchStart](const std::string& pathStr, const filewatch::Event change_type)
            {
                if (change_type != filewatch::Event::modified)
                    return;

                // If directory: pathStr is relative to it.
                // If single file: the modified path is the file itself!
                std::filesystem::path modifiedPath = isDir ? (absPath / pathStr) : absPath;
                std::string key = modifiedPath.string();

                // If the file was deleted or removed, do not trigger reimport!
                std::error_code ec;
                if (!std::filesystem::is_regular_file(modifiedPath, ec))
                    return;
                const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(modifiedPath, ec);
                if (ec)
                    return;

                {
                    std::scoped_lock lock(m_mutex);

                    // On Windows FileWatch listens for last-access, attribute and security changes too, and
                    // reports all of them as "modified" - so merely READING a file (an IDE, git, the search
                    // indexer, antivirus, grep) queued a hot-reload/reimport. Only a newer write time means
                    // the content actually changed.
                    auto writeIt = m_lastWriteTimes.find(key);
                    const std::filesystem::file_time_type lastWrite = writeIt != m_lastWriteTimes.end() ? writeIt->second : watchStart;
                    if (writeTime <= lastWrite)
                        return;

                    auto currentTime = std::chrono::steady_clock::now();
                    auto& lastTime = m_fileTimestamps[key];
                    auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastTime).count();
                    if (elapsedTime < 300)
                        return;

                    lastTime = currentTime;
                    m_lastWriteTimes[key] = writeTime;
                }

                NOX_CORE_INFO("NOXWatcher: {}", modifiedPath.string());

                if (onModified)
                {
                    onModified(modifiedPath);
                }
            }
        ));
    }
}
