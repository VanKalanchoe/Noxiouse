#pragma once

#include <condition_variable>
#include <mutex>
#include <vector>

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
        void setTargetAPI(GraphicsAPI api) override { m_targetAPI = api; }
        GraphicsAPI getTargetAPI() const override { return m_targetAPI; }
        
    private:
        // Pipelines compile on several threads at once (PipelineVK's pool); a Slang global session is used by one thread at a time
        // (Slang's rule), so each compile borrows one of a few, made as needed -- on the first compile: slang.dll is delay-loaded
        // (CMake), so a game whose shader cache is complete never loads it. At most kMaxSessions compile at once (each takes a few
        // hundred MB for the big shaders).
        Slang::ComPtr<slang::IGlobalSession> acquireSession();
        void releaseSession(Slang::ComPtr<slang::IGlobalSession> session);
        std::mutex m_sessionMutex;
        std::condition_variable m_sessionFree;
        std::vector<Slang::ComPtr<slang::IGlobalSession>> m_freeSessions;
        uint32_t m_sessionCount = 0;
        uint32_t m_maxSessions = 1;
        std::once_flag m_availableOnce;
        int m_available = -1; // -1 not checked yet, 0 / 1
        GraphicsAPI m_targetAPI = static_cast<GraphicsAPI>(0);
    };
}
