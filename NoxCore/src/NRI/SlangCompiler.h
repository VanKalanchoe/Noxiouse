#pragma once

#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

#include "ShaderCompiler.h"

namespace NRI
{
    class SlangCompiler : public ShaderCompiler
    {
    public:
        SlangCompiler();
        ~SlangCompiler() override = default;
        
        std::vector<char> compile(const std::string& path) override;
        uint64_t sourceHash(const std::string& path) override;
        bool canCompile() override;
        
    private:
        // Created on the first compile: slang.dll is delay-loaded (CMake), so a game whose shader cache is complete never loads it.
        bool ensureSession();
        Slang::ComPtr<slang::IGlobalSession> m_globalSession;
        int m_available = -1; // -1 not checked yet, 0 / 1
    };
}
