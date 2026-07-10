#include "SlangCompiler.h"

#include <array>
#include <memory>
#include <filesystem>

namespace NRI
{
    std::unique_ptr<ShaderCompiler> CreateSlangCompiler()
    {
        return std::make_unique<SlangCompiler>();
    }

    SlangCompiler::SlangCompiler()
    {
        slang::createGlobalSession(m_globalSession.writeRef());
    }
    
    std::vector<char> SlangCompiler::compile(const std::string& path)
    {
        Slang::ComPtr<slang::ISession> session;
        
        auto slangTargets{
            std::to_array<slang::TargetDesc>({
                {
                    .format{SLANG_SPIRV},
                    .profile{m_globalSession->findProfile("spirv_1_4")}
                }
            })
        };
        auto slangOptions{
            std::to_array<slang::CompilerOptionEntry>({
                { slang::CompilerOptionName::EmitSpirvDirectly, { slang::CompilerOptionValueKind::Int, 1 } },
                { slang::CompilerOptionName::VulkanUseEntryPointName, { slang::CompilerOptionValueKind::Int, 1 } },
                { slang::CompilerOptionName::GLSLForceScalarLayout,  { slang::CompilerOptionValueKind::Int, 1  } },
                { slang::CompilerOptionName::Capability, { slang::CompilerOptionValueKind::Int, m_globalSession->findCapability("spvDescriptorHeapEXT") } }
            })
        };
        slang::SessionDesc slangSessionDesc{
            .targets{slangTargets.data()},
            .targetCount{SlangInt(slangTargets.size())},
            .defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR,
            .compilerOptionEntries{slangOptions.data()},
            .compilerOptionEntryCount{uint32_t(slangOptions.size())}
        };
        m_globalSession->createSession(slangSessionDesc, session.writeRef());
        
        std::string moduleName = std::filesystem::path(path).stem().string();
        
        Slang::ComPtr<slang::IModule> slangModule
        {
            session->loadModuleFromSource(moduleName.c_str(), path.c_str(), nullptr, nullptr)
        };
        
        Slang::ComPtr<ISlangBlob> spirv;
        slangModule->getTargetCode(0, spirv.writeRef());
        
        const char* dataPtr = static_cast<const char*>(spirv->getBufferPointer());
        size_t dataSize = spirv->getBufferSize();
        
        return std::vector<char>(dataPtr, dataPtr + dataSize);
    }
}
