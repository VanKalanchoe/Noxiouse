#pragma once
#include "xxhash.h"
#include <functional>
#include <string>
#include <vector>

namespace Nox
{
    class Utility
    {
    public:
        // These return empty string if cancelled
        static std::string OpenFile(const char* filter);
        static std::string SaveFile(const char* filter);
        // A folder picked in the system's folder dialog, starting in `defaultLocation` when given.
        static std::string OpenFolder(const char* defaultLocation = nullptr);

        // The same without waiting: the dialog opens and the program keeps running (drawing, streaming, previews); `done` runs on the
        // main thread at the start of a later frame (Application's loop) with the path, empty when cancelled. The blocking ones above
        // spin the main loop until the dialog closes.
        static void OpenFileAsync(const char* filter, std::function<void(std::string)> done);
        static void SaveFileAsync(const char* filter, std::function<void(std::string)> done);
        static void OpenFolderAsync(const char* defaultLocation, std::function<void(std::string)> done);
        // Main thread, once a frame (Application::Run): the answers that came in are given to their callers.
        static void DeliverDialogResults();
        
        static std::string LoadFileFromPath(const std::string& path);
        static std::vector<uint32_t> LoadSpvFromPath(const std::string& path);
        static void SaveToFile(const char* filename, const void* data, size_t size);
        static std::string GetCachePath();
        static std::string GetCachePath(std::string name);

        // Content hash of a file, memoized per path while its size and last-write time are unchanged.
        static XXH128_hash_t calcul_hash_streaming(const std::string& path);
        static void saveHashToFile(const std::string& hashFile, const XXH128_hash_t& hash);
        static bool loadHashFromFile(const std::string& hashFile, XXH128_hash_t& hash);
    private:
        static XXH128_hash_t HashFileContents(const std::string& path);
    };
}