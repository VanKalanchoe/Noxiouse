// Custom Lit (CustomLitShaders.h; docs/Stylized_Rendering_And_Material_Graph_Plan_2026-11.md step 3): the project's Material Shaders as the
// renderer's pipelines -- the Custom Lit pass (DeferredLighting.slang with NOX_CUSTOM_LIT, added by addDeferredLightingPass after the
// deferred lighting) and the forward shader's Custom Lit variant (forwardLitPipeline: transparent surfaces, the editor's other viewports,
// the Material Editor's preview). The slots materials are packed with (GpuMaterial::customLitSlot) come from here. So do the material
// graphs' (MaterialGraph.h; GpuMaterial::graphSlot): the same generated file, with the G-buffer resolve's variant (GBufferMaterial.slang
// with NOX_MATERIAL_GRAPH, used by addGBufferPass while a material has a graph) and the same forward variant.
#include "NoxCore/Renderer/Renderer.h"

#include <algorithm>

#include <SDL3/SDL_thread.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/NodeGraph/NodeGraphAsset.h"
#include "NoxCore/Core/VirtualFileSystem.h"
#include "NoxCore/Renderer/CustomLitShaders.h"

namespace Nox
{
    namespace
    {
        // A build thread above the editor's own (its render and job threads run flat out): the user's log had the forward variant compile
        // in 6 s at the start but 35 s while the editor drew -- the compile thread was starved, not slow.
        // For the task's duration only (MSVC's std::async runs tasks on pooled threads: the priority must not stay with the thread).
        struct BuildThreadPriority
        {
            BuildThreadPriority() { SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH); }
            ~BuildThreadPriority() { SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_NORMAL); }
        };
    }

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

    std::unique_ptr<NRI::Pipeline> Renderer::buildCustomLitForwardPipeline(bool forceCompile, bool withGraphs)
    {
        // The forward shader's variant: as createTransparentLitPipeline's plain one (the meshlet stages are the same files).
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.buildNow = true;
        desc.colorFormats = { NRI::ImageFormat::R16G16B16A16_SFLOAT, NRI::ImageFormat::R32SINT };
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Task, .entryPoint = "taskMain", .sourcePath = "assets/shaders/Material_PBR_MeshTask.slang" });
        // With graphs, the mesh stage moves the vertices by a graph's World Position Offset (a game packaged before it has no such file: the
        // plain mesh stage).
        const std::filesystem::path graphMesh = CustomLitShaders::ForwardMeshSourcePath();
        const std::string meshSource = withGraphs && std::filesystem::exists(graphMesh) ? graphMesh.generic_string() : std::string("assets/shaders/Material_PBR_Mesh.slang");
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Mesh, .entryPoint = "meshMain", .sourcePath = meshSource });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Fragment, .entryPoint = "fragMain", .sourcePath = CustomLitShaders::ForwardSourcePath().generic_string() });
        return m_device->createPipeline(desc, *m_shaderCompiler);
    }

    std::unique_ptr<NRI::Pipeline> Renderer::buildMaterialGraphGBufferPipeline(bool forceCompile, bool writeVelocity)
    {
        NRI::PipelineDesc desc = gbufferPipelineDesc(CustomLitShaders::GBufferSourcePath().generic_string(), writeVelocity);
        desc.forceCompile = forceCompile;
        desc.buildNow = true;
        return m_device->createPipeline(desc, *m_shaderCompiler);
    }

    std::unique_ptr<NRI::Pipeline> Renderer::buildMaterialGraphVisibilityPipeline(bool forceCompile, bool shadowDepth)
    {
        // As createVisibilityPipeline / createShadowMapPipelines' mask pipeline, from the generated source with the graphs.
        const std::string source = CustomLitShaders::VisibilitySourcePath().generic_string();
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.buildNow = true;
        if (!shadowDepth)
            desc.colorFormats = { NRI::ImageFormat::R32G32_UINT };
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Task, .entryPoint = "taskMain", .sourcePath = "assets/shaders/Material_PBR_MeshTask.slang" });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Mesh, .entryPoint = "meshMain", .sourcePath = source });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Fragment, .entryPoint = shadowDepth ? "depthMaskFragMain" : "fragMain", .sourcePath = source });
        return m_device->createPipeline(desc, *m_shaderCompiler);
    }

    std::unique_ptr<NRI::Pipeline> Renderer::buildMaterialGraphEditorPipeline(bool selection)
    {
        // As createStableEntityDepthPipeline / createSelectionMaskPipeline, from the generated sources with the graphs.
        const std::string source = (selection ? CustomLitShaders::SelectionMaskSourcePath() : CustomLitShaders::StableDepthSourcePath()).generic_string();
        NRI::PipelineDesc desc{};
        desc.buildNow = true;
        desc.colorFormats = { selection ? NRI::ImageFormat::R16_SFLOAT : NRI::ImageFormat::R32SINT };
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Task, .entryPoint = "taskMain", .sourcePath = "assets/shaders/Material_PBR_MeshTask.slang" });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Mesh, .entryPoint = "meshMain", .sourcePath = source });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Fragment, .entryPoint = "fragMain", .sourcePath = source });
        return m_device->createPipeline(desc, *m_shaderCompiler);
    }

    bool Renderer::createCustomLitPipelines(bool forceCompile)
    {
        // All here and now (a packaged game's start from the shipped binaries, an engine shader's hot reload), those the project needs:
        // the Custom Lit pass with Material Shaders, the G-buffer resolve's variant with material graphs, the forward variant with
        // either. A hot reload's failure keeps the last working binary (PipelineVK); a first compile's failure leaves the pipelines null,
        // every Custom Lit material packed as Default Lit and every graph material with its own values.
        const bool shaders = m_customLitStatus.ShaderCount > 0;
        const bool graphs = m_customLitStatus.GraphCount > 0;
        try
        {
            std::unique_ptr<NRI::Pipeline> deferred = shaders ? buildCustomLitDeferredPipeline(forceCompile) : nullptr;
            std::unique_ptr<NRI::Pipeline> gbuffer = graphs ? buildMaterialGraphGBufferPipeline(forceCompile, true) : nullptr;
            std::unique_ptr<NRI::Pipeline> gbufferNoVelocity = graphs ? buildMaterialGraphGBufferPipeline(forceCompile, false) : nullptr;
            std::unique_ptr<NRI::Pipeline> visibility = graphs ? buildMaterialGraphVisibilityPipeline(forceCompile, false) : nullptr;
            std::unique_ptr<NRI::Pipeline> shadowDepthMask = graphs ? buildMaterialGraphVisibilityPipeline(forceCompile, true) : nullptr;
            std::unique_ptr<NRI::Pipeline> stableDepth = graphs && m_isEditor ? buildMaterialGraphEditorPipeline(false) : nullptr;
            std::unique_ptr<NRI::Pipeline> selectionMask = graphs && m_isEditor ? buildMaterialGraphEditorPipeline(true) : nullptr;
            std::unique_ptr<NRI::Pipeline> forward = shaders || graphs ? buildCustomLitForwardPipeline(forceCompile, graphs) : nullptr;
            m_customLitPipeline = std::move(deferred);
            m_gbufferGraphPipeline = std::move(gbuffer);
            m_gbufferGraphPipelineNoVelocity = std::move(gbufferNoVelocity);
            m_visibilityGraphPipeline = std::move(visibility);
            m_shadowDepthMaskGraphPipeline = std::move(shadowDepthMask);
            m_stableEntityDepthGraphPipeline = std::move(stableDepth);
            m_selectionMaskGraphPipeline = std::move(selectionMask);
            m_transparentLitCustomPipeline = std::move(forward);
        }
        catch (const std::exception& error)
        {
            NOX_CORE_ERROR("Custom Lit: the Material Shaders did not compile ({}); the log above has the compiler's message with the file and line. "
                           "Custom Lit materials are lit as Default Lit until they do.", error.what());
            m_customLitStatus.Message = std::string("The Material Shaders did not compile: ") + error.what() +
                                        " (the Console / Nox.log has the compiler's message with the file and line).";
            m_customLitPipeline.reset();
            m_gbufferGraphPipeline.reset();
            m_gbufferGraphPipelineNoVelocity.reset();
            m_visibilityGraphPipeline.reset();
            m_shadowDepthMaskGraphPipeline.reset();
            m_stableEntityDepthGraphPipeline.reset();
            m_selectionMaskGraphPipeline.reset();
            m_transparentLitCustomPipeline.reset();
            m_customLitStatus.Ready = false;
            m_customLitStatus.GraphsReady = false;
            return false;
        }
        m_customLitStatus.Ready = shaders;
        m_customLitStatus.GraphsReady = graphs;
        m_customLitStatus.Message.clear();
        return true;
    }

    void Renderer::SetCustomLitShaders(const std::vector<CustomLitSource>& sources, const std::vector<ProjectMaterialGraph>& graphs)
    {
        // One build at a time (it reads the generated files, which a new request writes): a request while one runs waits for it, the
        // newest replacing an older waiting one.
        if (m_customLitBuild)
        {
            // The running build is for older text: told to stop at its next stage (it finishes the compile it is in), then this one runs.
            m_customLitPending = CustomLitRequest{ sources, graphs };
            m_customLitBuild->Cancel = true;
            m_customLitStatus.Building = true;
            return;
        }
        startCustomLitBuild(sources, graphs);
    }

    void Renderer::startCustomLitBuild(const std::vector<CustomLitSource>& sources, const std::vector<ProjectMaterialGraph>& graphs)
    {
        const std::vector<std::string> paths = CustomLitShaders::AssignSlots(m_customLitRequestedPaths, sources);
        const size_t shaderCount = static_cast<size_t>(std::count_if(paths.begin(), paths.end(), [](const std::string& path) { return !path.empty(); }));
        // Every graph has a slot, compiling or not (a graph fixed later keeps it); one that does not compile has no case in the shader.
        std::vector<std::string> graphNames;
        std::vector<MaterialGraphSource> graphSources;
        std::unordered_map<std::string, MaterialGraphProgram> programs;
        std::map<std::string, std::string> graphErrors;
        for (const ProjectMaterialGraph& graph : graphs)
        {
            graphNames.push_back(graph.Path);
            // A graph that connects no Material Output pin changes nothing: no case, no build (a new graph is one -- creating it
            // compiled every variant again, the user's test).
            if (graph.Program.Valid && graph.Program.Outputs == 0)
                continue;
            if (graph.Program.Valid)
            {
                graphSources.push_back({ graph.Path, graph.Program.Code, graph.Program.MaskCode });
                programs.emplace(graph.Path, graph.Program);
            }
            else
                graphErrors.emplace(graph.Path, graph.Program.Error);
        }
        const std::vector<std::string> graphPaths = CustomLitShaders::AssignPathSlots(m_materialGraphRequestedPaths, graphNames);
        const size_t graphCount = programs.size();
        const bool slotsChanged = paths != m_customLitRequestedPaths || graphPaths != m_materialGraphRequestedPaths;
        m_customLitRequestedPaths = paths;
        m_materialGraphRequestedPaths = graphPaths;
        m_customLitStatus.GraphErrors = std::move(graphErrors);
        if (shaderCount == 0 && graphCount == 0)
        {
            // No Material Shader and no material graph that compiles: no pipeline of theirs (nothing to compile, no pass); materials
            // naming one are Default Lit / keep their own values.
            if (m_customLitPipeline || m_transparentLitCustomPipeline || m_gbufferGraphPipeline)
            {
                m_device->waitIdle();
                m_customLitPipeline.reset();
                m_transparentLitCustomPipeline.reset();
                m_gbufferGraphPipeline.reset();
                m_gbufferGraphPipelineNoVelocity.reset();
                m_visibilityGraphPipeline.reset();
                m_shadowDepthMaskGraphPipeline.reset();
                m_stableEntityDepthGraphPipeline.reset();
                m_selectionMaskGraphPipeline.reset();
            }
            updateRayTracingGraphMasks(false);
            m_customLitPaths = paths;
            m_materialGraphPaths = graphPaths;
            m_materialGraphPrograms.clear();
            CustomLitStatus empty;
            empty.GraphErrors = std::move(m_customLitStatus.GraphErrors);
            empty.Version = m_customLitStatus.Version + 1;
            m_customLitStatus = std::move(empty);
            m_gpuMaterialsDirty = true;
            return;
        }

        const bool written = CustomLitShaders::WriteGenerated(paths, sources, graphPaths, graphSources);
        if (!written && !slotsChanged && (m_customLitPipeline || m_gbufferGraphPipeline || !m_customLitStatus.Message.empty()))
        {
            // The same shader text as the last build (or its failure): nothing to compile. A graph's parameter defaults may have changed
            // (they are not in its code, so its layout is the same): the materials packed again with them.
            if (m_materialGraphPaths == graphPaths)
                m_materialGraphPrograms = std::move(programs);
            updateRayTracingGraphMasks(m_gbufferGraphPipeline && AnyGraphMask(m_materialGraphPrograms));
            ++m_customLitStatus.Version;
            m_gpuMaterialsDirty = true;
            return;
        }

        // On a worker thread (UE's background shader compiles): the editor keeps drawing, the materials lit by the last working build
        // (or Default Lit) until each pipeline is done (pollCustomLitBuild swaps it in). First the Material Shaders alone
        // (CustomLitShaders::CheckSourcePath, about a second: an error is reported as one, the pipelines stay as they were), then the
        // Custom Lit pass's pipeline (a few seconds), then the forward variant (the slowest: transparent surfaces and the previews).
        // Each pipeline is compiled only when its sources changed: the binaries are cached by the hash of the source and everything it
        // includes, so a graph's edit compiles the G-buffer resolve's variant and the forward variant and loads the Custom Lit pass's
        // binary, a Material Shader's edit the other way round (every pipeline used to be compiled again: the user's test). With
        // material graphs, the G-buffer resolve's variant comes first (opaque surfaces: most of what a graph is on).
        const bool compile = written && m_shaderCompiler->canCompile();
        auto build = std::make_shared<CustomLitBuild>();
        build->Paths = paths;
        build->ShaderCount = shaderCount;
        build->GraphPaths = graphPaths;
        build->GraphPrograms = std::move(programs);
        build->GraphCount = graphCount;
        build->RayMasksWanted = AnyGraphMask(build->GraphPrograms);
        // The forward variant (the slowest) compiles on a thread of its own beside the others; both above the editor's priority.
        build->Task = std::async(std::launch::async, [this, build, compile, shaderCount, graphCount]()
        {
            // The renderer's shutdown (or a newer request) stops the build between its stages: it waits for one compile, not all of them.
            auto cancelled = [&]()
            {
                if (!build->Cancel.load())
                    return false;
                std::lock_guard lock(build->Mutex);
                build->Cancelled = true;
                return true;
            };
            const BuildThreadPriority priority;
            auto phase = [&]() -> bool
            {
                std::future<std::unique_ptr<NRI::Pipeline>> forwardTask = std::async(std::launch::async, [this, graphCount]()
                {
                    const BuildThreadPriority priority;
                    return buildCustomLitForwardPipeline(false, graphCount > 0);
                });
                if (graphCount > 0 && !cancelled())
                {
                    std::unique_ptr<NRI::Pipeline> gbuffer = buildMaterialGraphGBufferPipeline(false, true);
                    std::unique_ptr<NRI::Pipeline> gbufferNoVelocity = buildMaterialGraphGBufferPipeline(false, false);
                    // The Masked pipelines with the graphs' Opacity Mask, swapped in with the G-buffer resolve's (one set of slots).
                    std::unique_ptr<NRI::Pipeline> visibility = buildMaterialGraphVisibilityPipeline(false, false);
                    std::unique_ptr<NRI::Pipeline> shadowDepthMask = buildMaterialGraphVisibilityPipeline(false, true);
                    // The editor's overlays (entity IDs / depth, selection outline) cut the mask too.
                    std::unique_ptr<NRI::Pipeline> stableDepth = m_isEditor ? buildMaterialGraphEditorPipeline(false) : nullptr;
                    std::unique_ptr<NRI::Pipeline> selectionMask = m_isEditor ? buildMaterialGraphEditorPipeline(true) : nullptr;
                    std::lock_guard lock(build->Mutex);
                    build->StableDepth = std::move(stableDepth);
                    build->SelectionMask = std::move(selectionMask);
                    build->GBuffer = std::move(gbuffer);
                    build->GBufferNoVelocity = std::move(gbufferNoVelocity);
                    build->Visibility = std::move(visibility);
                    build->ShadowDepthMask = std::move(shadowDepthMask);
                    build->GBufferDone = true;
                }
                if (shaderCount > 0 && !cancelled())
                {
                    std::unique_ptr<NRI::Pipeline> deferred = buildCustomLitDeferredPipeline(false);
                    std::lock_guard lock(build->Mutex);
                    build->Deferred = std::move(deferred);
                    build->DeferredDone = true;
                }
                std::unique_ptr<NRI::Pipeline> forward = forwardTask.get(); // (rethrows its compile error)
                if (cancelled())
                    return false;
                std::lock_guard lock(build->Mutex);
                build->Forward = std::move(forward);
                return true;
            };
            try
            {
                if (compile)
                    m_shaderCompiler->compile(CustomLitShaders::CheckSourcePath().generic_string());
                if (!phase())
                    return;
                // The ray tracing passes' variants with the graphs' masks: their shaders compiled here (those whose binary is not cached:
                // only after a mask changed), the main thread then creates the pipelines from them (updateRayTracingGraphMasks).
                bool rayMasks = false;
                if (build->RayMasksWanted && !cancelled())
                {
                    std::vector<std::string> variants;
                    for (const CustomLitShaders::RayTracingMaskVariant& variant : CustomLitShaders::RayTracingMaskVariants())
                        variants.push_back(variant.Variant);
                    rayMasks = m_device->compileShaderModules(variants, *m_shaderCompiler);
                    if (!rayMasks)
                        NOX_CORE_ERROR("Material graphs: the ray tracing passes' Opacity Mask variants do not compile (they keep the material's own alpha)");
                }
                if (cancelled())
                    return;
                std::lock_guard lock(build->Mutex);
                build->RayMasks = rayMasks;
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
        std::unique_ptr<NRI::Pipeline> deferred, forward, gbuffer, gbufferNoVelocity, visibility, shadowDepthMask, stableDepth, selectionMask;
        bool failed, finished;
        std::string error;
        {
            std::lock_guard lock(build.Mutex);
            deferred = std::move(build.Deferred);
            forward = std::move(build.Forward);
            gbuffer = std::move(build.GBuffer);
            gbufferNoVelocity = std::move(build.GBufferNoVelocity);
            visibility = std::move(build.Visibility);
            shadowDepthMask = std::move(build.ShadowDepthMask);
            stableDepth = std::move(build.StableDepth);
            selectionMask = std::move(build.SelectionMask);
            failed = build.Failed;
            finished = build.Failed || build.Cancelled || build.ForwardDone;
            error = build.Error;
        }
        if (deferred || forward || gbuffer)
        {
            m_device->waitIdle(); // (the pipelines replaced may still be in flight)
            if (gbuffer)
            {
                // The slots and parameter layouts go with the G-buffer resolve's variant (the forward variant still running the old one
                // leaves an unknown slot's material with its own values until its own build is done).
                m_gbufferGraphPipeline = std::move(gbuffer);
                m_gbufferGraphPipelineNoVelocity = std::move(gbufferNoVelocity);
                m_visibilityGraphPipeline = std::move(visibility);
                m_shadowDepthMaskGraphPipeline = std::move(shadowDepthMask);
                m_stableEntityDepthGraphPipeline = std::move(stableDepth);
                m_selectionMaskGraphPipeline = std::move(selectionMask);
                m_materialGraphPaths = build.GraphPaths;
                m_materialGraphPrograms = build.GraphPrograms;
                m_customLitStatus.GraphCount = build.GraphCount;
                m_customLitStatus.GraphsReady = true;
                m_customLitStatus.Message.clear();
                m_gpuMaterialsDirty = true;
                NOX_CORE_INFO("Material graphs: {} graph(s), the G-buffer resolve is ready (the forward variant follows)", build.GraphCount);
            }
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
        if (!failed && !build.Cancelled)
        {
            // What this build has none of: its pipelines dropped (a project whose last Material Shader or graph was removed).
            if (build.ShaderCount == 0 && m_customLitPipeline)
            {
                m_device->waitIdle();
                m_customLitPipeline.reset();
                m_customLitPaths = build.Paths;
                m_customLitStatus.ShaderCount = 0;
                m_customLitStatus.Ready = false;
                m_gpuMaterialsDirty = true;
            }
            // The ray tracing passes: their graph variants while a graph connects Opacity Mask (created again when a mask changed).
            updateRayTracingGraphMasks(build.RayMasks);
            if (build.GraphCount == 0 && m_gbufferGraphPipeline)
            {
                m_device->waitIdle();
                m_gbufferGraphPipeline.reset();
                m_gbufferGraphPipelineNoVelocity.reset();
                m_visibilityGraphPipeline.reset();
                m_shadowDepthMaskGraphPipeline.reset();
                m_stableEntityDepthGraphPipeline.reset();
                m_selectionMaskGraphPipeline.reset();
                m_materialGraphPaths = build.GraphPaths;
                m_materialGraphPrograms.clear();
                m_customLitStatus.GraphCount = 0;
                m_customLitStatus.GraphsReady = false;
                m_gpuMaterialsDirty = true;
            }
        }
        if (failed)
        {
            // The compiler's first error (SlangCompiler puts it after the module's path) is what the Material Editor shows.
            const size_t newline = error.find('\n');
            const std::string detail = newline != std::string::npos ? error.substr(newline + 1) : error;
            m_customLitStatus.Message = "A Material Shader or material graph does not compile: " + detail +
                                        " (the Output Log / Nox.log has the whole message).";
            NOX_CORE_ERROR("Custom Lit: {} Custom Lit and graph materials keep the last build that compiled (or Default Lit / their own values).",
                           m_customLitStatus.Message);
            ++m_customLitStatus.Version;
        }
        if (build.Cancelled)
            NOX_CORE_INFO("Custom Lit / material graphs: a build stopped for a newer change (or the shutdown)");
        m_customLitBuild.reset(); // (its task has returned: the future's wait is immediate)
        m_customLitStatus.Building = false;
        ++m_customLitStatus.Version;
        if (m_customLitPending)
        {
            const CustomLitRequest pending = std::move(*m_customLitPending);
            m_customLitPending.reset();
            startCustomLitBuild(pending.Sources, pending.Graphs);
        }
    }

    bool Renderer::AnyGraphMask(const std::unordered_map<std::string, MaterialGraphProgram>& programs)
    {
        return std::any_of(programs.begin(), programs.end(), [](const auto& entry) { return !entry.second.MaskCode.empty(); });
    }

    std::string Renderer::rayTracingShader(const std::string& source) const
    {
        if (m_rayTracingGraphMasks)
            for (const CustomLitShaders::RayTracingMaskVariant& variant : CustomLitShaders::RayTracingMaskVariants())
                if (variant.Source == source)
                    return variant.Variant;
        return source;
    }

    void Renderer::updateRayTracingGraphMasks(bool wanted)
    {
        // Created again only when something they compile from changed: whether the masks are on, or the masks' text.
        const uint64_t hash = wanted ? m_shaderCompiler->sourceHash(CustomLitShaders::GraphMasksSourcePath().generic_string()) : 0;
        if (wanted == m_rayTracingGraphMasks && hash == m_rayTracingGraphMasksHash)
            return;
        m_device->waitIdle(); // (the pipelines replaced may still be in flight)
        m_rayTracingGraphMasks = wanted;
        m_rayTracingGraphMasksHash = hash;
        // Every pass that alpha tests ray hits (their shaders compiled on the build's thread, or cached: built on the pool from them).
        createShadowMaskPipeline(false);
        createDeferredLightingPipeline(false);
        createRTAOPipeline(false);
        createReflectionPipeline(false);
        createScreenProbePipelines(false);
        createDDGIPipelines(false);
        createReSTIRDIPipelines(false);
        createReSTIRGIPipelines(false);
        createReSTIRPTPipelines(false);
        createPathTracerPipeline(false);
        NOX_CORE_INFO(wanted ? "Material graphs: ray-traced shadows, reflections, GI, AO and the path tracer cut the graphs' Opacity Mask"
                             : "Material graphs: the ray tracing passes test the materials' own alpha again (no graph connects Opacity Mask)");
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
        // The material graphs: their slots from the manifest, their parameter layouts compiled again from the graphs in the pack
        // (CompileMaterialGraph is deterministic: the layout the shipped shader was generated with).
        m_materialGraphPaths = CustomLitShaders::ReadGraphManifest();
        m_materialGraphPrograms.clear();
        for (const std::string& path : m_materialGraphPaths)
        {
            if (path.empty())
                continue;
            const AssetHandle handle = AssetManager::FindHandleByPath(path, AssetType::MaterialGraph);
            const Ref<NodeGraphAsset> asset = handle != 0 ? AssetManager::GetAsset<NodeGraphAsset>(handle) : nullptr;
            if (!asset)
            {
                NOX_CORE_ERROR("Material graph {}: not in the game's assets (its materials keep their own values)", path);
                continue;
            }
            MaterialGraphProgram program = CompileMaterialGraph(asset->Graph);
            if (program.Valid)
                m_materialGraphPrograms.emplace(path, std::move(program));
        }
        m_customLitStatus.GraphCount = m_materialGraphPrograms.size();
        const bool sourcesShipped = std::filesystem::exists(CustomLitShaders::ForwardSourcePath()) &&
                                    (shaderCount == 0 || std::filesystem::exists(CustomLitShaders::DeferredSourcePath())) &&
                                    (m_customLitStatus.GraphCount == 0 || (std::filesystem::exists(CustomLitShaders::GBufferSourcePath()) &&
                                                                           std::filesystem::exists(CustomLitShaders::VisibilitySourcePath())));
        if ((shaderCount > 0 || m_customLitStatus.GraphCount > 0) && sourcesShipped)
        {
            m_device->waitIdle();
            createCustomLitPipelines(false);
            // (the ray tracing passes' mask variants shipped with their binaries when the editor had built them)
            const bool variantsShipped = std::all_of(CustomLitShaders::RayTracingMaskVariants().begin(), CustomLitShaders::RayTracingMaskVariants().end(),
                                                     [](const CustomLitShaders::RayTracingMaskVariant& variant) { return std::filesystem::exists(variant.Variant); });
            updateRayTracingGraphMasks(m_gbufferGraphPipeline && variantsShipped && AnyGraphMask(m_materialGraphPrograms));
        }
        ++m_customLitStatus.Version;
        m_gpuMaterialsDirty = true;
    }

    void Renderer::InitializeCustomLitShaders(bool wait)
    {
        bool complete = true;
        std::vector<CustomLitSource> sources = CustomLitShaders::GatherProjectSources(complete);
        bool graphsComplete = true;
        std::vector<ProjectMaterialGraph> graphs = GatherProjectMaterialGraphs(graphsComplete);
        // A packaged game (its assets in a mounted pack, VirtualFileSystem.h) uses what it shipped: the manifest and the binaries -- its
        // pack holds no Material Shader to compile, and its registry may list none. Else (the editor, a development run with the
        // project's files) every Material Shader readable on disk and a compiler here: generated from the sources.
        const bool packaged = VFS::HasMounts();
        if (!packaged && complete && graphsComplete && m_shaderCompiler->canCompile())
        {
            waitForCustomLitBuild(); // (another project's build)
            m_customLitRequestedPaths.clear(); // (a project opened: its own slots)
            m_materialGraphRequestedPaths.clear();
            m_customLitPending.reset();
            SetCustomLitShaders(sources, graphs);
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

    uint32_t Renderer::GetMaterialGraphSlot(const std::string& path, const MaterialGraphProgram** program) const
    {
        if (program)
            *program = nullptr;
        if (path.empty() || !m_gbufferGraphPipeline)
            return 0xFFFFFFFFu;
        auto slotOf = [&](const std::string& graphPath) -> uint32_t
        {
            const auto compiled = m_materialGraphPrograms.find(graphPath);
            if (compiled == m_materialGraphPrograms.end())
                return 0xFFFFFFFFu; // (it does not compile: no case in the shader)
            const auto found = std::find(m_materialGraphPaths.begin(), m_materialGraphPaths.end(), graphPath);
            if (found == m_materialGraphPaths.end())
                return 0xFFFFFFFFu;
            if (program)
                *program = &compiled->second;
            return static_cast<uint32_t>(found - m_materialGraphPaths.begin());
        };
        const uint32_t slot = slotOf(path);
        if (slot != 0xFFFFFFFFu)
            return slot;
        // A renamed or moved graph: the material still names its old path, which the registry keeps as an alias (FormerPaths).
        const AssetHandle graph = AssetManager::FindHandleByPath(path, AssetType::MaterialGraph);
        if (graph == 0)
            return 0xFFFFFFFFu;
        for (const std::string& graphPath : m_materialGraphPaths)
        {
            if (!graphPath.empty() && AssetManager::FindHandleByPath(graphPath, AssetType::MaterialGraph) == graph)
                return slotOf(graphPath);
        }
        return 0xFFFFFFFFu;
    }
}
