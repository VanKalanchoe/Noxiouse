#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <memory>

namespace NRI
{
    class ShaderCompiler
    {
    public:
        virtual ~ShaderCompiler() = default;
        
        virtual std::vector<char> compile(const std::string& path) = 0;
        // Hash of the shader source and every file it includes, so a cached binary is only reused while all of them match.
        virtual uint64_t sourceHash(const std::string& path) = 0;
    };
    
    std::unique_ptr<ShaderCompiler> CreateSlangCompiler();
}
