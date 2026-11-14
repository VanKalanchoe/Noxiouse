// Custom Lit (CustomLitShaders.h; docs/Stylized_Rendering_And_Material_Graph_Plan_2026-11.md step 3): the project's Material Shaders as the
// renderer's pipelines -- the Custom Lit pass (DeferredLighting.slang with NOX_CUSTOM_LIT, added by addDeferredLightingPass after the
// deferred lighting) and the forward shader's Custom Lit variant (forwardLitPipeline: transparent surfaces, the editor's other viewports,
// the Material Editor's preview). The slots materials are packed with (GpuMaterial::customLitSlot) come from here.
#include "NoxCore/Renderer/Renderer.h"

#include <algorithm>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/VirtualFileSystem.h"
#include "NoxCore/Renderer/CustomLitShaders.h"

namespace Nox
{
    std::unique_ptr<NRI::Pipeline> Renderer::buildCustomLitDeferredPipeline(bool forceCompile)
    {
        // Built on the calling thread, a compile error thrown at once (NRI::PipelineDesc::buildNow): a Material Shader is the project's
        // code and may not compile, and nothing may bind a pipeline that failed.
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.buildNow = true;
        desc.colorFormats = { NRI::ImageFormat::R16G16B16A16_SFLOAT };
        const std::string source = CustomLitShaders::DeferredSourcePath().generic_string();
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Task, .entryPoint = "taskMain", .sourcePath = source });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Mesh, .entryPoint = "meshMain", .sourcePath = source });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Fragment, .entryPoint = "fragMain", .sourcePath = source });
        return m_device->createPipeline(desc, *m_shaderCompiler);
    }

    std::unique_ptr<NRI::Pipeline> Renderer::buildCustomLitForwardPipeline(bool forceCompile)
    {
        // The forward shader's variant: as createTransparentLitPipeline's plain one (the meshlet stages are the same files).
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.buildNow = true;
        desc.colorFormats = { NRI::ImageFormat::R16G16B16A16_SFLOAT, NRI::ImageFormat::R32SINT };
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Task, .entryPoint = "taskMain", .sourcePath = "assets/shaders/Material_PBR_MeshTask.slang" });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Mesh, .entryPoint = "meshMain", .sourcePath = "assets/shaders/Material_PBR_Mesh.slang" });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Fragment, .entryPoint = "fragMain", .sourcePath = CustomLitShaders::ForwardSourcePath().generic_string() });
        return m_device->createPipeline(desc, *m_shaderCompiler);
    }

    bool Renderer::createCustomLitPipelines(bool forceCompile)
    {
        // Both here and now (a packaged game's start from the shipped binaries, an engine shader's hot reload). A hot reload's failure
        // keeps the last working binary (PipelineVK); a first compile's failure leaves the pipelines null and every Custom Lit material
        // packed as Default Lit.
        try
        {
            std::unique_ptr<NRI::Pipeline> deferred = buildCustomLitDeferredPipeline(forceCompile);
            std::unique_ptr<NRI::Pipeline> forward = buildCustomLitForwardPipeline(forceCompile);
            m_customLitPipeline = std::move(deferred);
            m_transparentLitCustomPipeline = std::move(forward);
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
        m_customLitStatus.Ready = true;
        m_customLitStatus.Message.clear();
        return true;
    }

    void Renderer::SetCustomLitShaders(const std::vector<CustomLitSource>& sources)
    {
        // One build at a time (it reads the generated files, which a new request writes): a request while one runs waits for it, the
        // newest replacing an older waiting one.
        if (m_customLitBuild)
        {
            m_customLitPending = sources;
            m_customLitStatus.Building = true;
            return;
        }
        startCustomLitBuild(sources);
    }

    void Renderer::startCustomLitBuild(const std::vector<CustomLitSource>& sources)
    {
        const std::vector<std::string> paths = CustomLitShaders::AssignSlots(m_customLitRequestedPaths, sources);
        const size_t shaderCount = static_cast<size_t>(std::count_if(paths.begin(), paths.end(), [](const std::string& path) { return !path.empty(); }));
        const bool slotsChanged = paths != m_customLitRequestedPaths;
        m_customLitRequestedPaths = paths;
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
            m_customLitStatus = CustomLitStatus{ 0, false, false, {}, m_customLitStatus.Version + 1 };
            m_gpuMaterialsDirty = true;
            return;
        }

        const bool written = CustomLitShaders::WriteGenerated(paths, sources);
        if (!written && !slotsChanged && (m_customLitPipeline || !m_customLitStatus.Message.empty()))
            return; // nothing changed since the last build (or its failure)

        // On a worker thread (UE's background shader compiles): the editor keeps drawing, the materials lit by the last working build
        // (or Default Lit) until each pipeline is done (pollCustomLitBuild swaps it in). First the Material Shaders alone
        // (CustomLitShaders::CheckSourcePath, about a second: an error is reported as one, the pipelines stay as they were), then the
        // Custom Lit pass's pipeline (a few seconds), then the forward variant (the slowest: transparent surfaces and the previews).
        // Compiled when a source changed; at the start, the binaries cached from a previous run (keyed by the generated text's hash).
        const bool compile = written && m_shaderCompiler->canCompile();
        auto build = std::make_shared<CustomLitBuild>();
        build->Paths = paths;
        build->ShaderCount = shaderCount;
        build->Task = std::async(std::launch::async, [this, build, compile]()
        {
            try
            {
                if (compile)
                    m_shaderCompiler->compile(CustomLitShaders::CheckSourcePath().generic_string());
                std::unique_ptr<NRI::Pipeline> deferred = buildCustomLitDeferredPipeline(compile);
                {
                    std::lock_guard lock(build->Mutex);
                    build->Deferred = std::move(deferred);
                    build->DeferredDone = true;
                }
                std::unique_ptr<NRI::Pipeline> forward = buildCustomLitForwardPipeline(compile);
                std::lock_guard lock(build->Mutex);
                build->Forward = std::move(forward);
                build->ForwardDone = true;
            }
            catch (const std::exception& error)
            {
                std::lock_guard lock(build->Mutex);
                build->Error = error.what();
                build->Failed = true;
            }
        });
        m_customLitBuild = std::move(build);
        m_customLitStatus.Building = true;
        ++m_customLitStatus.Version;
    }

    void Renderer::pollCustomLitBuild()
    {
        if (!m_customLitBuild)
            return;
        CustomLitBuild& build = *m_customLitBuild;
        std::unique_ptr<NRI::Pipeline> deferred, forward;
        bool failed, finished;
        std::string error;
        {
            std::lock_guard lock(build.Mutex);
            deferred = std::move(build.Deferred);
            forward = std::move(build.Forward);
            failed = build.Failed;
            finished = build.Failed || build.ForwardDone;
            error = build.Error;
        }
        if (deferred || forward)
        {
            m_device->waitIdle(); // (the pipelines replaced may still be in flight)
            if (deferred)
            {
                // The slots go with the pass's pipeline (the forward variant still running the old one lights an unknown slot as
                // Default Lit until its own build is done).
                m_customLitPipeline = std::move(deferred);
                m_customLitPaths = build.Paths;
                m_customLitStatus.ShaderCount = build.ShaderCount;
                m_customLitStatus.Ready = true;
                m_customLitStatus.Message.clear();
                m_gpuMaterialsDirty = true; // the slots (or whether they are usable) changed: every material packed again
                NOX_CORE_INFO("Custom Lit: {} Material Shader(s), the Custom Lit pass is ready (the forward variant follows)", build.ShaderCount);
            }
            if (forward)
            {
                m_transparentLitCustomPipeline = std::move(forward);
                NOX_CORE_INFO("Custom Lit: the forward variant is ready (translucent surfaces, previews)");
            }
            ++m_customLitStatus.Version;
        }
        if (!finished)
            return;
        if (failed)
        {
            m_customLitStatus.Message = std::string("A Material Shader does not compile (") + error +
                                        "): the Console / Nox.log has the compiler's message with the file and line.";
            NOX_CORE_ERROR("Custom Lit: {} Custom Lit materials keep the last build that compiled (or Default Lit).", m_customLitStatus.Message);
            ++m_customLitStatus.Version;
        }
        m_customLitBuild.reset(); // (its task has returned: the future's wait is immediate)
        m_customLitStatus.Building = false;
        if (m_customLitPending)
        {
            const std::vector<CustomLitSource> pending = std::move(*m_customLitPending);
            m_customLitPending.reset();
            startCustomLitBuild(pending);
        }
    }

    void Renderer::waitForCustomLitBuild()
    {
        // Until no build runs or waits (a game's start: its first frame lit as it should be; the renderer's shutdown).
        while (m_customLitBuild)
        {
            m_customLitBuild->Task.wait();
            pollCustomLitBuild();
        }
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

    void Renderer::InitializeCustomLitShaders(bool wait)
    {
        bool complete = true;
        std::vector<CustomLitSource> sources = CustomLitShaders::GatherProjectSources(complete);
        // A packaged game (its assets in a mounted pack, VirtualFileSystem.h) uses what it shipped: the manifest and the binaries -- its
        // pack holds no Material Shader to compile, and its registry may list none. Else (the editor, a development run with the
        // project's files) every Material Shader readable on disk and a compiler here: generated from the sources.
        const bool packaged = VFS::HasMounts();
        if (!packaged && complete && m_shaderCompiler->canCompile())
        {
            waitForCustomLitBuild(); // (another project's build)
            m_customLitRequestedPaths.clear(); // (a project opened: its own slots)
            m_customLitPending.reset();
            SetCustomLitShaders(sources);
            if (wait)
                waitForCustomLitBuild();
        }
        else
            LoadPackagedCustomLitShaders();
    }

    uint32_t Renderer::GetCustomLitSlot(const std::string& path) const
    {
        if (path.empty() || !m_customLitPipeline)
            return 0xFFFFFFFFu;
        const auto found = std::find(m_customLitPaths.begin(), m_customLitPaths.end(), path);
        if (found != m_customLitPaths.end())
            return static_cast<uint32_t>(found - m_customLitPaths.begin());
        // A renamed or moved Material Shader: the material still names its old path, which the registry keeps as an alias (FormerPaths).
        const AssetHandle shader = AssetManager::FindHandleByPath(path, AssetType::MaterialShader);
        if (shader == 0)
            return 0xFFFFFFFFu;
        for (size_t slot = 0; slot < m_customLitPaths.size(); ++slot)
        {
            if (!m_customLitPaths[slot].empty() && AssetManager::FindHandleByPath(m_customLitPaths[slot], AssetType::MaterialShader) == shader)
                return static_cast<uint32_t>(slot);
        }
        return 0xFFFFFFFFu;
    }
}
