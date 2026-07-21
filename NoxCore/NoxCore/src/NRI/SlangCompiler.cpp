#include "SlangCompiler.h"

#include <array>
#include <memory>
#include <filesystem>

#include "NoxCore/Core/Log.h"

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
                    .profile{m_globalSession->findProfile("spirv_1_6")}
                }
            })
        };
        auto slangOptions{
            std::to_array<slang::CompilerOptionEntry>({
                { slang::CompilerOptionName::EmitSpirvDirectly, { slang::CompilerOptionValueKind::Int, 1 } },
                { slang::CompilerOptionName::VulkanUseEntryPointName, { slang::CompilerOptionValueKind::Int, 1 } },
                { slang::CompilerOptionName::GLSLForceScalarLayout,  { slang::CompilerOptionValueKind::Int, 1  } },
                { slang::CompilerOptionName::Capability, { slang::CompilerOptionValueKind::Int, m_globalSession->findCapability("spvDescriptorHeapEXT") } },
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
        
        Slang::ComPtr<slang::IBlob> diagnostics;
        Slang::ComPtr<slang::IModule> slangModule
        {
            session->loadModuleFromSource(moduleName.c_str(), path.c_str(), nullptr, diagnostics.writeRef())
        };
        
        if (diagnostics)
        {
            NOX_CORE_ERROR("Slang compile diagnostics ({}): {}", path,
                static_cast<const char*>(diagnostics->getBufferPointer()));
        }
        
        if (!slangModule)
            throw std::runtime_error("Slang failed to load module: " + path);
        
        Slang::ComPtr<ISlangBlob> spirv;
        Slang::ComPtr<slang::IBlob> targetDiagnostics;
        SlangResult result = slangModule->getTargetCode(0, spirv.writeRef(), targetDiagnostics.writeRef());
        
        if (targetDiagnostics)
        {
            NOX_CORE_ERROR("Slang target-code diagnostics ({}): {}", path,
                static_cast<const char*>(targetDiagnostics->getBufferPointer()));
        }

        if (SLANG_FAILED(result) || !spirv)
            throw std::runtime_error("Slang failed to generate target code: " + path);
        
        const char* dataPtr = static_cast<const char*>(spirv->getBufferPointer());
        size_t dataSize = spirv->getBufferSize();
        
        return std::vector<char>(dataPtr, dataPtr + dataSize);
    }
}
