#include "SlangCompiler.h"

#include <array>
#include <fstream>
#include <memory>
#include <unordered_set>


#include "NoxCore/Core/core.h"
#include "NoxCore/Core/Hash.h"
#include "NoxCore/Core/Log.h"

namespace NRI
{
    // assets/shaders (+ ../ variants) added when RTXDI-related .slang files were reorganized into
    // assets/shaders/RTXDI/{DI,GI,Presampling,PT}/ subfolders: Slang implicitly searches an
    // included file's OWN directory first, but a bare #include "shaderIO.h" from one of those
    // subfolders no longer finds it that way (shaderIO.h lives in the flat assets/shaders root) --
    // these explicit entries are the fallback that makes it resolve regardless of subfolder depth.
    static constexpr std::array<const char*, 16> g_searchPaths =
    {
        "../NoxCore/src/NoxCore/Renderer",
        "shaders",
        "../shaders",
        "../../shaders",
        "assets/shaders",
        "../assets/shaders",
        "../../assets/shaders",
        "../NoxCore/vendors/RTXDI/Include",
        "../../NoxCore/vendors/RTXDI/Include",
        "../../../NoxCore/vendors/RTXDI/Include",
        "../NoxCore/vendors/RTXDI/Include/Rtxdi",
        "../../NoxCore/vendors/RTXDI/Include/Rtxdi",
        "../../../NoxCore/vendors/RTXDI/Include/Rtxdi",
        "../NoxCore/vendors/RTXDI/shaders",
        "../../NoxCore/vendors/RTXDI/shaders",
        "../../../NoxCore/vendors/RTXDI/shaders"
    };

    std::unique_ptr<ShaderCompiler> CreateSlangCompiler()
    {
        return std::make_unique<SlangCompiler>();
    }

    SlangCompiler::SlangCompiler()
    {
        slang::createGlobalSession(m_globalSession.writeRef());
    }
    
    // Mirrors how Slang resolves a quoted include: the including file's own directory first, then the search paths.
    static uint64_t hashSourceAndIncludes(const std::filesystem::path& source, std::unordered_set<std::string>& visited)
    {
        if (!visited.insert(source.lexically_normal().string()).second)
            return 0;

        uint64_t hash = Nox::Hash::computeFile(source.string());

        std::ifstream file(source);
        std::string line;
        while (std::getline(file, line))
        {
            const size_t include = line.find("#include");
            if (include == std::string::npos)
                continue;

            const size_t begin = line.find('"', include);
            const size_t end = begin == std::string::npos ? std::string::npos : line.find('"', begin + 1);
            if (end == std::string::npos)
                continue;

            const std::string included = line.substr(begin + 1, end - begin - 1);
            std::filesystem::path resolved = source.parent_path() / included;
            for (const char* searchPath : g_searchPaths)
            {
                if (std::filesystem::exists(resolved))
                    break;
                resolved = std::filesystem::path(searchPath) / included;
            }

            if (std::filesystem::exists(resolved))
                hash = Nox::Hash::compute(&hash, sizeof(hash), hashSourceAndIncludes(resolved, visited));
        }

        return hash;
    }

    uint64_t SlangCompiler::sourceHash(const std::string& path)
    {
        std::unordered_set<std::string> visited;
        return hashSourceAndIncludes(path, visited);
    }

    std::vector<char> SlangCompiler::compile(const std::string& path)
    {
        if (!std::filesystem::exists(path)) NOX_CORE_ERROR("SlangCompiler::compile file not found: {}", path);
        
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
                { slang::CompilerOptionName::Capability, { slang::CompilerOptionValueKind::Int, m_globalSession->findCapability("spvRayQueryKHR") } },
            })
        };
        
        /*NOX_CORE_INFO("Slang current working directory: {}", std::filesystem::current_path().string());
        NOX_CORE_INFO("Slang search paths being checked:");
        for (const char* path : searchPaths)
        {
            std::filesystem::path p(path);
            std::filesystem::path absPath = std::filesystem::absolute(p);
            bool exists = std::filesystem::exists(absPath);
    
            NOX_CORE_INFO("  - [{}] {}", exists ? "EXISTS" : "NOT FOUND", absPath.string());
        }*/
        slang::SessionDesc slangSessionDesc
        {
            .targets{slangTargets.data()},
            .targetCount{SlangInt(slangTargets.size())},
            .defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR,
            .searchPaths = g_searchPaths.data(),
            .searchPathCount = SlangInt(g_searchPaths.size()),
            .compilerOptionEntries{slangOptions.data()},
            .compilerOptionEntryCount{uint32_t(slangOptions.size())},
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
