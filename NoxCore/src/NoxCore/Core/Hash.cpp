#include "Hash.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <fstream>
#include <vector>

#include "core.h"

namespace Nox
{
    uint64_t Hash::compute(const void* data, size_t size, uint64_t seed)
    {
        return XXH3_64bits_withSeed(data, size, seed);
    }

    uint64_t Hash::compute(const std::string& str, uint64_t seed)
    {
        return compute(str.data(), str.size(), seed);
    }

    uint64_t Hash::computeFile(const std::string& path, uint64_t seed)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);

        if (!file) NOX_CORE_ASSERT("Hash::computeFile failed to open file: {}", path);

        size_t size = file.tellg();
        file.seekg(0);

        std::vector<char> buffer(size);
        file.read(buffer.data(), static_cast<std::streamsize>(size));

        return compute(buffer.data(), buffer.size(), seed);
    }
}
