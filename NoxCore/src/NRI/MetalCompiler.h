#pragma once

#include "ShaderCompiler.h"

namespace NRI
{
    class MetalCompiler : public ShaderCompiler
    {
    public:
        MetalCompiler() = default;
        ~MetalCompiler() override = default;

        std::vector<char> compile(
            const std::string& path
        ) override;
    };

}

