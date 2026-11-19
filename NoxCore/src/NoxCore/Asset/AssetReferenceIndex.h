#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    // What one file names: numbers that can be asset handles, and path-like strings (normalized: forward slashes, lower case),
    // each with every tail that starts after a '/'.
    struct AssetReferenceTokens
    {
        std::unordered_set<AssetHandle> Handles;
        std::unordered_set<std::string> Paths;
    };

    // The project's reference cache (the role of Unreal's asset registry dependency data): for every file that can name assets
    // (scenes, prefabs, graphs, materials, meshes, world cells, scripts, the project file), what it names -- so "who uses this
    // asset?" (Delete, Find References, unused assets, packaging) is a lookup instead of reading the project.
    //
    //  - Built on a background thread when the project opens; kept in <Project>/Cache/AssetReferences.nrefs, so the next start
    //    reads only the files that changed (same size and write time: the cached answer stands).
    //  - Always current: Get() compares the file's size and write time first and reads it again when they changed.
    //  - Thread-safe: the editor asks from a worker thread while the build runs.
    class AssetReferenceIndex
    {
    public:
        using Reader = std::function<AssetReferenceTokens(const std::filesystem::path& absoluteFile)>;

        AssetReferenceIndex(std::filesystem::path assetDirectory, std::filesystem::path cacheFile, Reader reader);
        ~AssetReferenceIndex(); // stops the build and saves

        // Reads (on a background thread) the files of `relativeFiles` the cache does not know or that changed, then saves the
        // cache; files that are not in the list any more leave it.
        void StartBuild(std::vector<std::filesystem::path> relativeFiles);

        // What `relativeFile` names (empty when it does not exist), read again first if it changed on disk.
        std::shared_ptr<const AssetReferenceTokens> Get(const std::filesystem::path& relativeFile);

        struct Progress
        {
            size_t Done = 0;
            size_t Total = 0;
            bool Building = false;
        };
        Progress GetProgress() const;

        void Save();

    private:
        struct Entry
        {
            uint64_t Size = 0;
            int64_t Time = 0;
            std::shared_ptr<const AssetReferenceTokens> Tokens;
        };

        void Load();
        void StopBuild();

        std::filesystem::path m_AssetDirectory;
        std::filesystem::path m_CacheFile;
        Reader m_Reader;

        mutable std::mutex m_Mutex;
        std::unordered_map<std::string, Entry> m_Entries; // key: relative path, generic form
        bool m_Dirty = false;

        std::thread m_Builder;
        std::atomic<bool> m_Stop{ false };
        std::atomic<bool> m_Building{ false };
        std::atomic<size_t> m_Done{ 0 };
        std::atomic<size_t> m_Total{ 0 };
    };
}
