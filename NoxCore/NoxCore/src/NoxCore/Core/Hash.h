#pragma once
#include <cstdint>
#include <string>

namespace Nox
{
    class Hash
    {
    public:
        static uint64_t compute(const void* data, size_t size, uint64_t seed = 0);
        static uint64_t compute(const std::string& str, uint64_t seed = 0);
        static uint64_t computeFile(const std::string& path, uint64_t seed = 0);
    };
}