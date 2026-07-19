#pragma once
#include "xxhash.h"
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
        
        static std::string LoadFileFromPath(const std::string& path);
        static std::vector<uint32_t> LoadSpvFromPath(const std::string& path);
        static void SaveToFile(const char* filename, const void* data, size_t size);
        static std::string GetCachePath();
        static std::string GetCachePath(std::string name);

        static XXH128_hash_t calcul_hash_streaming(const std::string& path);
        static void saveHashToFile(const std::string& hashFile, const XXH128_hash_t& hash);
        static bool loadHashFromFile(const std::string& hashFile, XXH128_hash_t& hash);
    };
}