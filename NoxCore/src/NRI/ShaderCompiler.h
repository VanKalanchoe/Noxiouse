#pragma once
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
    };
    
    std::unique_ptr<ShaderCompiler> CreateSlangCompiler();
    std::unique_ptr<ShaderCompiler> CreateMetalCompiler();
}
