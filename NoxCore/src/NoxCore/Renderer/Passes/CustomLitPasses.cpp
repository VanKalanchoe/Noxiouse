// Custom Lit (CustomLitShaders.h; docs/Stylized_Rendering_And_Material_Graph_Plan_2026-11.md step 3): the project's Material Shaders as the
// renderer's pipelines -- the Custom Lit pass (DeferredLighting.slang with NOX_CUSTOM_LIT, added by addDeferredLightingPass after the
// deferred lighting) and the forward shader's Custom Lit variant (forwardLitPipeline: transparent surfaces, the editor's other viewports,
// the Material Editor's preview). The slots materials are packed with (GpuMaterial::customLitSlot) come from here.
#include "NoxCore/Renderer/Renderer.h"

#include <algorithm>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/VirtualFileSystem.h"
#include "NoxCore/Renderer/CustomLitShaders.h"

namespace Nox
{
    bool Renderer::createCustomLitPipelines(bool forceCompile)
    {
        // Built here, on this thread, a compile error thrown at once (NRI::PipelineDesc::buildNow): a Material Shader is the project's
        // code and may not compile, and nothing may bind a pipeline that failed. A hot reload's failure keeps the last working binary
        // (PipelineVK); a first compile's failure leaves the pipelines null and every Custom Lit material packed as Default Lit.
        std::unique_ptr<NRI::Pipeline> deferred;
        std::unique_ptr<NRI::Pipeline> forward;
        try
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            desc.buildNow = true;
            desc.colorFormats = { NRI::ImageFormat::R16G16B16A16_SFLOAT };
            const std::string deferredSource = CustomLitShaders::DeferredSourcePath().generic_string();
            desc.shaders.push_back({ .stage = NRI::ShaderStage::Task, .entryPoint = "taskMain", .sourcePath = deferredSource });
            desc.shaders.push_back({ .stage = NRI::ShaderStage::Mesh, .entryPoint = "meshMain", .sourcePath = deferredSource });
            desc.shaders.push_back({ .stage = NRI::ShaderStage::Fragment, .entryPoint = "fragMain", .sourcePath = deferredSource });
            deferred = m_device->createPipeline(desc, *m_shaderCompiler);

            // The forward shader's variant: as createTransparentLitPipeline's plain one (the meshlet stages are the same files).
            NRI::PipelineDesc forwardDesc{};
            forwardDesc.forceCompile = forceCompile;
            forwardDesc.buildNow = true;
            forwardDesc.colorFormats = { NRI::ImageFormat::R16G16B16A16_SFLOAT, NRI::ImageFormat::R32SINT };
            forwardDesc.shaders.push_back({ .stage = NRI::ShaderStage::Task, .entryPoint = "taskMain", .sourcePath = "assets/shaders/Material_PBR_MeshTask.slang" });
            forwardDesc.shaders.push_back({ .stage = NRI::ShaderStage::Mesh, .entryPoint = "meshMain", .sourcePath = "assets/shaders/Material_PBR_Mesh.slang" });
            forwardDesc.shaders.push_back({ .stage = NRI::ShaderStage::Fragment, .entryPoint = "fragMain",
                                            .sourcePath = CustomLitShaders::ForwardSourcePath().generic_string() });
            forward = m_device->createPipeline(forwardDesc, *m_shaderCompiler);
        }
        catch (const std::exception& error)
        {
            NOX_CORE_ERROR("Custom Lit: the Material Shaders did not compile ({}); the log above has the compiler's message with the file and line. "
                           "Custom Lit materials are lit as Default Lit until they do.", error.what());
            m_customLitStatus.Message = std::string("The Material Shaders did not compile: ") + error.what() +
                                        " (the Console / Nox.log has the compiler's message with the file and line).";
            m_customLitPipeline.reset();
            m_transparentLitCustomPipeline.reset();
            m_customLitStatus.Ready = false;
            return false;
        }
        m_customLitPipeline = std::move(deferred);
        m_transparentLitCustomPipeline = std::move(forward);
        m_customLitStatus.Ready = true;
        m_customLitStatus.Message.clear();
        return true;
    }

    void Renderer::SetCustomLitShaders(const std::vector<CustomLitSource>& sources)
    {
        const std::vector<std::string> paths = CustomLitShaders::AssignSlots(m_customLitPaths, sources);
        const size_t shaderCount = static_cast<size_t>(std::count_if(paths.begin(), paths.end(), [](const std::string& path) { return !path.empty(); }));
        const bool slotsChanged = paths != m_customLitPaths;
        if (shaderCount == 0)
        {
            // No Material Shader: no Custom Lit pipeline (nothing to compile, no pass); materials naming one are Default Lit.
            if (m_customLitPipeline || m_transparentLitCustomPipeline)
            {
                m_device->waitIdle();
                m_customLitPipeline.reset();
                m_transparentLitCustomPipeline.reset();
            }
            m_customLitPaths = paths;
            m_customLitStatus = CustomLitStatus{ 0, false, {}, m_customLitStatus.Version + 1 };
            m_gpuMaterialsDirty = true;
            return;
        }

        const bool written = CustomLitShaders::WriteGenerated(paths, sources);
        if (!written && !slotsChanged && (m_customLitPipeline || !m_customLitStatus.Message.empty()))
            return; // nothing changed since the last build (or its failure)

        // The Material Shaders alone first (CustomLitShaders::CheckSourcePath, a fraction of the pipelines' compile): an error is reported
        // as one and the pipelines stay as they were (the materials keep the last working build, or Default Lit without one). The
        // compiler's message (file and line) is in the log.
        if (written && m_shaderCompiler->canCompile())
        {
            try
            {
                m_shaderCompiler->compile(CustomLitShaders::CheckSourcePath().generic_string());
            }
            catch (const std::exception& error)
            {
                m_customLitStatus.Message = std::string("A Material Shader does not compile (") + error.what() +
                                            "): the Console / Nox.log has the compiler's message with the file and line.";
                ++m_customLitStatus.Version;
                NOX_CORE_ERROR("Custom Lit: {}", m_customLitStatus.Message);
                return;
            }
        }

        m_device->waitIdle(); // (the old pipelines may still be in flight)
        m_customLitPaths = paths;
        m_customLitStatus.ShaderCount = shaderCount;
        // Compiled when a source changed; at the start, the binaries cached from a previous run (keyed by the generated text's hash).
        createCustomLitPipelines(written && m_shaderCompiler->canCompile());
        ++m_customLitStatus.Version;
        m_gpuMaterialsDirty = true; // the slots (or whether they are usable) changed: every material packed again
        NOX_CORE_INFO("Custom Lit: {} Material Shader(s), pipelines {}", shaderCount, m_customLitStatus.Ready ? "ready" : "failed");
    }

    void Renderer::LoadPackagedCustomLitShaders()
    {
        // A packaged game: the generated sources and their binaries came with it (GamePackager copies shaders/ beside the executable),
        // the manifest says which Material Shader each slot is.
        std::vector<std::string> paths = CustomLitShaders::ReadManifest();
        const size_t shaderCount = static_cast<size_t>(std::count_if(paths.begin(), paths.end(), [](const std::string& path) { return !path.empty(); }));
        m_customLitPaths = std::move(paths);
        m_customLitStatus.ShaderCount = shaderCount;
        if (shaderCount > 0 && std::filesystem::exists(CustomLitShaders::DeferredSourcePath()) && std::filesystem::exists(CustomLitShaders::ForwardSourcePath()))
        {
            m_device->waitIdle();
            createCustomLitPipelines(false);
        }
        ++m_customLitStatus.Version;
        m_gpuMaterialsDirty = true;
    }

    void Renderer::InitializeCustomLitShaders()
    {
        bool complete = true;
        std::vector<CustomLitSource> sources = CustomLitShaders::GatherProjectSources(complete);
        // A packaged game (its assets in a mounted pack, VirtualFileSystem.h) uses what it shipped: the manifest and the binaries -- its
        // pack holds no Material Shader to compile, and its registry may list none. Else (the editor, a development run with the
        // project's files) every Material Shader readable on disk and a compiler here: generated from the sources.
        const bool packaged = VFS::HasMounts();
        if (!packaged && complete && m_shaderCompiler->canCompile())
        {
            m_customLitPaths.clear(); // (a project opened: its own slots)
            SetCustomLitShaders(sources);
        }
        else
            LoadPackagedCustomLitShaders();
    }

    uint32_t Renderer::GetCustomLitSlot(const std::string& path) const
    {
        if (path.empty() || !m_customLitPipeline)
            return 0xFFFFFFFFu;
        const auto found = std::find(m_customLitPaths.begin(), m_customLitPaths.end(), path);
        return found == m_customLitPaths.end() ? 0xFFFFFFFFu : static_cast<uint32_t>(found - m_customLitPaths.begin());
    }
}
