#include "MetalCompiler.h"

#include <fstream>
#include <stdexcept>

namespace NRI
{
    std::vector<char> MetalCompiler::compile(
        const std::string& path
    )
    {
        std::ifstream file(
            path,
            std::ios::binary | std::ios::ate
        );

        if (!file)
        {
            throw std::runtime_error(
                "Failed to open Metal shader: " + path
            );
        }

        const auto size = file.tellg();

        std::vector<char> source(
            static_cast<size_t>(size)
        );

        file.seekg(0);
        file.read(
            source.data(),
            size
        );

        return source;
    }

    std::unique_ptr<ShaderCompiler> CreateMetalCompiler()
    {
        return std::make_unique<MetalCompiler>();
    }
}

