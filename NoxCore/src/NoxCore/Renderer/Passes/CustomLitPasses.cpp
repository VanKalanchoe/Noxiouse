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

    nri::Pipeline Renderer::buildCustomLitDeferredPipeline(bool forceCompile)
    {
        // Built on the calling thread, a compile error thrown at once (nri::PipelineDesc::buildNow): a Material Shader is the project's
        // code and may not compile, and nothing may bind a pipeline that failed.
        nri::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.buildNow = true;
        desc.colorFormats = { NRI_FORMAT_R16G16B16A16_SFLOAT };
        const std::string source = CustomLitShaders::DeferredSourcePath().generic_string();
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_TASK, .entryPoint = "taskMain", .sourcePath = source });
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_MESH, .entryPoint = "meshMain", .sourcePath = source });
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_FRAGMENT, .entryPoint = "fragMain", .sourcePath = source });
        return m_device.createPipeline(desc, m_shaderCompiler.handle());
    }

    nri::Pipeline Renderer::buildCustomLitForwardPipeline(bool forceCompile, bool withGraphs)
    {
        // The forward shader's variant: as createTransparentLitPipeline's plain one (the meshlet stages are the same files).
        nri::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.buildNow = true;
        desc.colorFormats = { NRI_FORMAT_R16G16B16A16_SFLOAT, NRI_FORMAT_R32_SINT };
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_TASK, .entryPoint = "taskMain", .sourcePath = "assets/shaders/Material_PBR_MeshTask.slang" });
        // With graphs, the mesh stage moves the vertices by a graph's World Position Offset (a game packaged before it has no such file: the
        // plain mesh stage).
        const std::filesystem::path graphMesh = CustomLitShaders::ForwardMeshSourcePath();
        const std::string meshSource = withGraphs && std::filesystem::exists(graphMesh) ? graphMesh.generic_string() : std::string("assets/shaders/Material_PBR_Mesh.slang");
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_MESH, .entryPoint = "meshMain", .sourcePath = meshSource });
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_FRAGMENT, .entryPoint = "fragMain", .sourcePath = CustomLitShaders::ForwardSourcePath().generic_string() });
        return m_device.createPipeline(desc, m_shaderCompiler.handle());
    }

    nri::Pipeline Renderer::buildMaterialGraphGBufferPipeline(bool forceCompile, bool writeVelocity)
    {
        nri::PipelineDesc desc = gbufferPipelineDesc(CustomLitShaders::GBufferSourcePath().generic_string(), writeVelocity);
        desc.forceCompile = forceCompile;
        desc.buildNow = true;
        return m_device.createPipeline(desc, m_shaderCompiler.handle());
    }

    nri::Pipeline Renderer::buildMaterialGraphVisibilityPipeline(bool forceCompile, bool shadowDepth)
    {
        // As createVisibilityPipeline / createShadowMapPipelines' mask pipeline, from the generated source with the graphs.
        const std::string source = CustomLitShaders::VisibilitySourcePath().generic_string();
        nri::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.buildNow = true;
        if (!shadowDepth)
            desc.colorFormats = { NRI_FORMAT_R32G32_UINT };
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_TASK, .entryPoint = "taskMain", .sourcePath = "assets/shaders/Material_PBR_MeshTask.slang" });
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_MESH, .entryPoint = "meshMain", .sourcePath = source });
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_FRAGMENT, .entryPoint = shadowDepth ? "depthMaskFragMain" : "fragMain", .sourcePath = source });
        return m_device.createPipeline(desc, m_shaderCompiler.handle());
    }

    nri::Pipeline Renderer::buildMaterialGraphRayTracingOffsetPipeline(bool forceCompile)
    {
        // The ray tracing scene's World Position Offset (RayTracingOffset.slang with the graphs): built now, as the others.
        nri::PipelineDesc desc{};
        desc.type = NRI_PIPELINE_COMPUTE;
        desc.forceCompile = forceCompile;
        desc.buildNow = true;
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_COMPUTE, .entryPoint = "computeMain",
                                 .sourcePath = CustomLitShaders::RayTracingOffsetSourcePath().generic_string() });
        return m_device.createPipeline(desc, m_shaderCompiler.handle());
    }

    nri::Pipeline Renderer::buildMaterialGraphEditorPipeline(bool selection)
    {
        // As createStableEntityDepthPipeline / createSelectionMaskPipeline, from the generated sources with the graphs.
        const std::string source = (selection ? CustomLitShaders::SelectionMaskSourcePath() : CustomLitShaders::StableDepthSourcePath()).generic_string();
        nri::PipelineDesc desc{};
        desc.buildNow = true;
        desc.colorFormats = { selection ? NRI_FORMAT_R16_SFLOAT : NRI_FORMAT_R32_SINT };
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_TASK, .entryPoint = "taskMain", .sourcePath = "assets/shaders/Material_PBR_MeshTask.slang" });
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_MESH, .entryPoint = "meshMain", .sourcePath = source });
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_FRAGMENT, .entryPoint = "fragMain", .sourcePath = source });
        return m_device.createPipeline(desc, m_shaderCompiler.handle());
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
            nri::Pipeline deferred = shaders ? buildCustomLitDeferredPipeline(forceCompile) : nri::Pipeline{};
            nri::Pipeline gbuffer = graphs ? buildMaterialGraphGBufferPipeline(forceCompile, true) : nri::Pipeline{};
            nri::Pipeline gbufferNoVelocity = graphs ? buildMaterialGraphGBufferPipeline(forceCompile, false) : nri::Pipeline{};
            nri::Pipeline visibility = graphs ? buildMaterialGraphVisibilityPipeline(forceCompile, false) : nri::Pipeline{};
            nri::Pipeline shadowDepthMask = graphs ? buildMaterialGraphVisibilityPipeline(forceCompile, true) : nri::Pipeline{};
            nri::Pipeline stableDepth = graphs && m_isEditor ? buildMaterialGraphEditorPipeline(false) : nri::Pipeline{};
            nri::Pipeline selectionMask = graphs && m_isEditor ? buildMaterialGraphEditorPipeline(true) : nri::Pipeline{};
            nri::Pipeline rayTracingOffset = graphs ? buildMaterialGraphRayTracingOffsetPipeline(forceCompile) : nri::Pipeline{};
            nri::Pipeline forward = shaders || graphs ? buildCustomLitForwardPipeline(forceCompile, graphs) : nri::Pipeline{};
            m_customLitPipeline = std::move(deferred);
            m_gbufferGraphPipeline = std::move(gbuffer);
            m_gbufferGraphPipelineNoVelocity = std::move(gbufferNoVelocity);
            m_visibilityGraphPipeline = std::move(visibility);
            m_shadowDepthMaskGraphPipeline = std::move(shadowDepthMask);
            m_stableEntityDepthGraphPipeline = std::move(stableDepth);
            m_selectionMaskGraphPipeline = std::move(selectionMask);
            m_rayTracingOffsetGraphPipeline = std::move(rayTracingOffset);
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
            m_rayTracingOffsetGraphPipeline.reset();
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

    void Renderer::startCustomLitBuild(const std::vector<CustomLitSource>& shaderSources, const std::vector<ProjectMaterialGraph>& graphs)
    {
        // The Material Shaders (those defining light()/ambient()), and the material graphs whose Custom Light / Custom Ambient light a
        // Custom Lit material (step 4c part 5): each takes a slot of the same dispatcher.
        std::vector<CustomLitSource> sources;
        sources.reserve(shaderSources.size());
        for (const CustomLitSource& shader : shaderSources)
            if (shader.Graph || CustomLitShaders::HasLightFunctions(shader.Code) || !CustomLitShaders::HasSurfaceFunction(shader.Code))
                sources.push_back(shader);
        for (const ProjectMaterialGraph& graph : graphs)
            if (graph.Program.Valid && !graph.Program.LightCode.empty())
                sources.push_back({ graph.Path, graph.Program.LightCode, std::string(), true });
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
            if (graph.Program.Valid && graph.Program.Outputs == 0 && graph.Program.Outputs2 == 0)
                continue;
            if (graph.Program.Valid)
            {
                graphSources.push_back({ graph.Path, graph.Program.Code, graph.Program.MaskCode, graph.Program.SurfaceCode, graph.Program.VertexCode });
                programs.emplace(graph.Path, graph.Program);
            }
            else
                graphErrors.emplace(graph.Path, graph.Program.Error);
        }
        // Step 5: hand-written surface() in Material Shaders (.slang) takes a slot in the same surface dispatcher.
        for (const CustomLitSource& shader : shaderSources)
        {
            if (shader.Graph || !CustomLitShaders::HasSurfaceFunction(shader.Code))
                continue;
            uint32_t out1 = 0, out2 = 0;
            CustomLitShaders::DetectSurfaceOutputs(shader.Code, out1, out2);
            graphNames.push_back(shader.Path);
            graphSources.push_back(CustomLitShaders::BuildShaderSurfaceSource(shader, out1));
            MaterialGraphProgram prog;
            prog.Valid = true;
            prog.Outputs = out1;
            prog.Outputs2 = out2;
            programs.emplace(shader.Path, std::move(prog));
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
                m_device.waitIdle();
                m_customLitPipeline.reset();
                m_transparentLitCustomPipeline.reset();
                m_gbufferGraphPipeline.reset();
                m_gbufferGraphPipelineNoVelocity.reset();
                m_visibilityGraphPipeline.reset();
                m_shadowDepthMaskGraphPipeline.reset();
                m_stableEntityDepthGraphPipeline.reset();
                m_selectionMaskGraphPipeline.reset();
                m_rayTracingOffsetGraphPipeline.reset();
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
            NOX_CORE_INFO("Custom Lit / material graphs: no shader code changed (a parameter's default, or a layout change): nothing to compile");
            updateRayTracingGraphMasks(m_gbufferGraphPipeline && GraphsUseRayTracing(m_materialGraphPrograms));
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
        const bool compile = written && m_shaderCompiler.canCompile();
        auto build = std::make_shared<CustomLitBuild>();
        build->Paths = paths;
        build->ShaderCount = shaderCount;
        build->GraphPaths = graphPaths;
        build->GraphPrograms = std::move(programs);
        build->GraphCount = graphCount;
        build->RayMasksOn = GraphsUseRayTracing(build->GraphPrograms);
        if (build->RayMasksOn)
        {
            // (the generated files are written: the masks' text this build compiles)
            build->RayMasksHash = rayTracingGraphsHash();
            build->RayPipelinesNeeded = !m_rayTracingGraphMasks || build->RayMasksHash != m_rayTracingGraphMasksHash;
        }
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
            std::future<bool> rayMaskTask;
            auto phase = [&]() -> bool
            {
                std::future<nri::Pipeline> forwardTask = std::async(std::launch::async, [this, graphCount]()
                {
                    const BuildThreadPriority priority;
                    return buildCustomLitForwardPipeline(false, graphCount > 0);
                });
                // The graph variants' shaders at once, one thread each (they were compiled one after another as their pipelines were
                // built: ~4 s before the G-buffer resolve's could be swapped in); the pipelines below then come from them.
                if (graphCount > 0 && !cancelled())
                {
                    std::vector<std::string> graphVariants = { CustomLitShaders::GBufferSourcePath().generic_string(),
                                                               CustomLitShaders::VisibilitySourcePath().generic_string() };
                    if (m_isEditor)
                    {
                        graphVariants.push_back(CustomLitShaders::StableDepthSourcePath().generic_string());
                        graphVariants.push_back(CustomLitShaders::SelectionMaskSourcePath().generic_string());
                    }
                    graphVariants.push_back(CustomLitShaders::RayTracingOffsetSourcePath().generic_string());
                    std::vector<std::string_view> graphViews(graphVariants.begin(), graphVariants.end());
                    m_device.compileShaderModules(graphViews, m_shaderCompiler.handle()); // (an error is thrown again by the pipeline below)
                }
                // The ray tracing passes' variants with the graphs' masks: compiled beside the forward variant once the graph variants
                // (what makes the graph visible) are done (those whose binary is not cached: only after a mask changed), then their
                // pipelines built here, GPU driver compile included, and handed over ready (pollCustomLitBuild swaps them in).
                if (build->RayPipelinesNeeded && !cancelled())
                    rayMaskTask = std::async(std::launch::async, [this, build]()
                    {
                        const BuildThreadPriority priority;
                        std::vector<std::string> variants;
                        for (const CustomLitShaders::RayTracingMaskVariant& variant : CustomLitShaders::RayTracingMaskVariants())
                            variants.push_back(variant.Variant);
                        std::vector<std::string_view> variantViews(variants.begin(), variants.end());
                        if (!m_device.compileShaderModules(variantViews, m_shaderCompiler.handle()))
                            return false;
                        try
                        {
                            RayTracingPassPipelines pipelines = buildRayTracingPassPipelines(true);
                            std::lock_guard lock(build->Mutex);
                            build->RayPipelines = std::move(pipelines);
                            build->RayPipelinesDone = true;
                            return true;
                        }
                        catch (const std::exception& error)
                        {
                            NOX_CORE_ERROR("Material graphs: a ray tracing pass's Opacity Mask variant does not build: {}", error.what());
                            return false;
                        }
                    });
                if (graphCount > 0 && !cancelled())
                {
                    nri::Pipeline gbuffer = buildMaterialGraphGBufferPipeline(false, true);
                    nri::Pipeline gbufferNoVelocity = buildMaterialGraphGBufferPipeline(false, false);
                    // The Masked pipelines with the graphs' Opacity Mask, swapped in with the G-buffer resolve's (one set of slots).
                    nri::Pipeline visibility = buildMaterialGraphVisibilityPipeline(false, false);
                    nri::Pipeline shadowDepthMask = buildMaterialGraphVisibilityPipeline(false, true);
                    // The editor's overlays (entity IDs / depth, selection outline) cut the mask too.
                    nri::Pipeline stableDepth = m_isEditor ? buildMaterialGraphEditorPipeline(false) : nri::Pipeline{};
                    nri::Pipeline selectionMask = m_isEditor ? buildMaterialGraphEditorPipeline(true) : nri::Pipeline{};
                    // The ray tracing scene's World Position Offset (its moved vertices), with the same slots.
                    nri::Pipeline rayTracingOffset = buildMaterialGraphRayTracingOffsetPipeline(false);
                    std::lock_guard lock(build->Mutex);
                    build->RayTracingOffset = std::move(rayTracingOffset);
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
                    nri::Pipeline deferred = buildCustomLitDeferredPipeline(false);
                    std::lock_guard lock(build->Mutex);
                    build->Deferred = std::move(deferred);
                    build->DeferredDone = true;
                }
                nri::Pipeline forward = forwardTask.get(); // (rethrows its compile error)
                if (cancelled())
                    return false;
                std::lock_guard lock(build->Mutex);
                build->Forward = std::move(forward);
                return true;
            };
            try
            {
                if (compile)
                    m_shaderCompiler.compile(CustomLitShaders::CheckSourcePath().generic_string());
                const bool phaseDone = phase();
                bool rayMasks = false;
                if (rayMaskTask.valid())
                {
                    rayMasks = rayMaskTask.get(); // (also when the phase stopped: the task uses this build's compiler)
                    if (!rayMasks)
                        NOX_CORE_ERROR("Material graphs: the ray tracing passes' Opacity Mask variants do not build (they test the material's own alpha)");
                }
                if (!phaseDone || cancelled())
                    return;
                std::lock_guard lock(build->Mutex);
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
        m_customLitStatus.GraphsLive = false;
        ++m_customLitStatus.Version;
    }

    void Renderer::pollCustomLitBuild()
    {
        if (!m_customLitBuild)
            return;
        CustomLitBuild& build = *m_customLitBuild;
        nri::Pipeline deferred, forward, gbuffer, gbufferNoVelocity, visibility, shadowDepthMask, stableDepth, selectionMask, rayTracingOffset;
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
            rayTracingOffset = std::move(build.RayTracingOffset);
            failed = build.Failed;
            finished = build.Failed || build.Cancelled || build.ForwardDone;
            error = build.Error;
        }
        if (deferred || forward || gbuffer)
        {
            m_device.waitIdle(); // (the pipelines replaced may still be in flight)
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
                m_rayTracingOffsetGraphPipeline = std::move(rayTracingOffset);
                m_materialGraphPaths = build.GraphPaths;
                m_materialGraphPrograms = build.GraphPrograms;
                m_customLitStatus.GraphCount = build.GraphCount;
                m_customLitStatus.GraphsReady = true;
                m_customLitStatus.GraphsLive = true;
                m_customLitStatus.Message.clear();
                m_gpuMaterialsDirty = true;
                build.GBufferSwapped = true;
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
        // The ray tracing passes' graph variants, as soon as they are ready (not after the forward variant), after the G-buffer
        // resolve's variant: the masks index the graphs by its slots.
        if (build.GBufferSwapped && !build.RayPipelinesInstalled && !build.Cancelled)
        {
            RayTracingPassPipelines rayPipelines;
            bool ready = false;
            {
                std::lock_guard lock(build.Mutex);
                if (build.RayPipelinesDone)
                {
                    rayPipelines = std::move(build.RayPipelines);
                    ready = true;
                }
            }
            if (ready)
            {
                installRayTracingPassPipelines(std::move(rayPipelines), true, build.RayMasksHash);
                build.RayPipelinesInstalled = true;
                ++m_customLitStatus.Version;
            }
        }
        if (!finished)
            return;
        if (!failed && !build.Cancelled)
        {
            // What this build has none of: its pipelines dropped (a project whose last Material Shader or graph was removed).
            if (build.ShaderCount == 0 && m_customLitPipeline)
            {
                m_device.waitIdle();
                m_customLitPipeline.reset();
                m_customLitPaths = build.Paths;
                m_customLitStatus.ShaderCount = 0;
                m_customLitStatus.Ready = false;
                m_gpuMaterialsDirty = true;
            }
            // The ray tracing passes: their graph variants while a graph connects Opacity Mask (swapped in above when a mask changed);
            // the material's own alpha again when no graph has one, or when the variants did not build (the masks they had index the
            // previous slots).
            if (!build.RayMasksOn || (build.RayPipelinesNeeded && !build.RayPipelinesInstalled))
                updateRayTracingGraphMasks(false);
            if (build.GraphCount == 0 && m_gbufferGraphPipeline)
            {
                m_device.waitIdle();
                m_gbufferGraphPipeline.reset();
                m_gbufferGraphPipelineNoVelocity.reset();
                m_visibilityGraphPipeline.reset();
                m_shadowDepthMaskGraphPipeline.reset();
                m_stableEntityDepthGraphPipeline.reset();
                m_selectionMaskGraphPipeline.reset();
                m_rayTracingOffsetGraphPipeline.reset();
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

    bool Renderer::GraphsUseRayTracing(const std::unordered_map<std::string, MaterialGraphProgram>& programs)
    {
        // A graph's mask cuts ray hits, its surface colours them (reflections, the path tracer, GI): any graph built.
        return std::any_of(programs.begin(), programs.end(), [](const auto& entry)
        {
            return !entry.second.MaskCode.empty() || (entry.second.Outputs & ~(MaterialGraphOutputOpacityMask | MaterialGraphOutputWorldPositionOffset)) != 0u;
        });
    }

    uint64_t Renderer::rayTracingGraphsHash() const
    {
        // What the variants compile from: the masks (every variant) and the surfaces (those that shade their hits) with what they include.
        const uint64_t masks = m_shaderCompiler.sourceHash(CustomLitShaders::GraphMasksSourcePath().generic_string());
        const uint64_t surfaces = m_shaderCompiler.sourceHash(CustomLitShaders::GraphSurfacesSourcePath().generic_string());
        return masks ^ (surfaces + 0x9E3779B97F4A7C15ull + (masks << 6) + (masks >> 2));
    }

    nri::PipelineDesc Renderer::rayTracingPassDesc(RayTracingPass pass, bool graphMasks, bool forceCompile)
    {
        auto shader = [graphMasks](const char* source) -> std::string
        {
            if (graphMasks)
                for (const CustomLitShaders::RayTracingMaskVariant& variant : CustomLitShaders::RayTracingMaskVariants())
                    if (variant.Source == source)
                        return variant.Variant;
            return source;
        };
        nri::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        // A full-screen pass: task, mesh and fragment stages of one file, its targets' formats.
        auto screenPass = [&](const char* source, std::initializer_list<nri::ImageFormat> formats)
        {
            desc.colorFormats = formats;
            const std::string path = shader(source);
            desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_TASK, .entryPoint = "taskMain", .sourcePath = path });
            desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_MESH, .entryPoint = "meshMain", .sourcePath = path });
            desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_FRAGMENT, .entryPoint = "fragMain", .sourcePath = path });
        };
        constexpr nri::ImageFormat RGBA16F = NRI_FORMAT_R16G16B16A16_SFLOAT;
        switch (pass)
        {
        case RayTracingPass::ShadowMask:
            screenPass("assets/shaders/ShadowMask.slang", { NRI_FORMAT_R16G16_SFLOAT });
            break;
        case RayTracingPass::DeferredLighting:
            screenPass("assets/shaders/DeferredLighting.slang", { RGBA16F });
            break;
        case RayTracingPass::RTAO:
            screenPass("assets/shaders/RTAO.slang", { NRI_FORMAT_R16_SFLOAT });
            break;
        case RayTracingPass::Reflection:
            screenPass("assets/shaders/Reflection.slang", { RGBA16F }); // raw reflection: radiance RGB + hit distance A
            break;
        case RayTracingPass::ScreenProbeTrace:
            // (compute over its own targets: ScreenProbeCommon.slang, so it can run on the async compute queue)
            desc.type = NRI_PIPELINE_COMPUTE;
            desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_COMPUTE, .entryPoint = "computeMain", .sourcePath = shader("assets/shaders/ScreenProbeTrace.slang") });
            break;
        case RayTracingPass::DDGIRadiance:
            screenPass("assets/shaders/DDGIRadiance.slang", { RGBA16F });
            break;
        case RayTracingPass::ReSTIRDIFinalShading:
            screenPass("assets/shaders/RTXDI/DI/ReSTIRDIFinalShading.slang", { RGBA16F, RGBA16F }); // diffuse, specular
            break;
        case RayTracingPass::ReSTIRGIInitial:
            screenPass("assets/shaders/RTXDI/GI/ReSTIRGIInitial.slang", { RGBA16F });
            break;
        case RayTracingPass::ReSTIRPTInitial:
            // SV_Target0 the debug-only combined radiance preview, SV_Target1 the primary surface's direct lighting for the Final Shading
            // pass (the ReSTIR PT Primary Direct graph texture).
            screenPass("assets/shaders/RTXDI/PT/ReSTIRPTInitial.slang", { RGBA16F, RGBA16F });
            break;
        case RayTracingPass::ReSTIRPTTemporal:
            screenPass("assets/shaders/RTXDI/PT/ReSTIRPTTemporal.slang", { RGBA16F });
            break;
        case RayTracingPass::PathTracer:
            desc.type = NRI_PIPELINE_RAY_TRACING;
            desc.maxRecursionDepth = 1;
            desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_RAY_GEN, .entryPoint = "rayGenMain", .sourcePath = "assets/shaders/PathTracer/PathTracerRayGen.slang" });   // 0
            desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_MISS, .entryPoint = "missMain", .sourcePath = "assets/shaders/PathTracer/PathTracerMiss.slang" });         // 1
            desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_CLOSEST_HIT, .entryPoint = "closestHitMain", .sourcePath = shader("assets/shaders/PathTracer/PathTracerHit.slang") }); // 2
            desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_ANY_HIT, .entryPoint = "anyHitMain", .sourcePath = shader("assets/shaders/PathTracer/PathTracerAnyHit.slang") });     // 3
            desc.hitGroups.push_back({ .closestHitShaderIndex = 2, .anyHitShaderIndex = ~0u, .intersectionShaderIndex = ~0u }); // hit group 0: opaque
            desc.hitGroups.push_back({ .closestHitShaderIndex = 2, .anyHitShaderIndex = 3, .intersectionShaderIndex = ~0u });   // hit group 1: alpha tested
            break;
        case RayTracingPass::Count:
            break;
        }
        return desc;
    }

    nri::Pipeline& Renderer::rayTracingPassPipeline(RayTracingPass pass)
    {
        switch (pass)
        {
        case RayTracingPass::ShadowMask: return m_shadowMaskPipeline;
        case RayTracingPass::DeferredLighting: return m_deferredLightingPipeline;
        case RayTracingPass::RTAO: return m_rtaoPipeline;
        case RayTracingPass::Reflection: return m_reflectionPipeline;
        case RayTracingPass::ScreenProbeTrace: return m_screenProbeTracePipeline;
        case RayTracingPass::DDGIRadiance: return m_ddgiRadiancePipeline;
        case RayTracingPass::ReSTIRDIFinalShading: return m_restirDIFinalShadingPipeline;
        case RayTracingPass::ReSTIRGIInitial: return m_restirGIInitialPipeline;
        case RayTracingPass::ReSTIRPTInitial: return m_restirPTInitialPipeline;
        case RayTracingPass::ReSTIRPTTemporal: return m_restirPTTemporalPipeline;
        case RayTracingPass::PathTracer:
        case RayTracingPass::Count: break;
        }
        return m_pathTracerPipeline;
    }

    Renderer::RayTracingPassPipelines Renderer::buildRayTracingPassPipelines(bool graphMasks)
    {
        // One thread each (the GPU driver compiles each variant for 0.5 - 2.5 s: the user's log), built now (nri::PipelineDesc::buildNow:
        // a pipeline built later on the pool would make the first frame that binds it wait).
        std::array<std::future<nri::Pipeline>, static_cast<size_t>(RayTracingPass::Count)> builds;
        for (size_t i = 0; i < builds.size(); ++i)
            builds[i] = std::async(std::launch::async, [this, i, graphMasks]()
            {
                const BuildThreadPriority priority;
                nri::PipelineDesc desc = rayTracingPassDesc(static_cast<RayTracingPass>(i), graphMasks, false);
                desc.buildNow = true;
                return m_device.createPipeline(desc, m_shaderCompiler.handle());
            });
        RayTracingPassPipelines pipelines;
        std::exception_ptr error;
        for (size_t i = 0; i < builds.size(); ++i)
        {
            try
            {
                pipelines[i] = builds[i].get();
            }
            catch (...)
            {
                if (!error)
                    error = std::current_exception();
            }
        }
        if (error)
            std::rethrow_exception(error);
        return pipelines;
    }

    void Renderer::installRayTracingPassPipelines(RayTracingPassPipelines pipelines, bool graphMasks, uint64_t masksHash)
    {
        m_device.waitIdle(); // (the pipelines replaced may still be in flight)
        for (size_t i = 0; i < pipelines.size(); ++i)
            rayTracingPassPipeline(static_cast<RayTracingPass>(i)) = std::move(pipelines[i]);
        bakePathTracerShaderTable();
        m_rayTracingGraphMasks = graphMasks;
        m_rayTracingGraphMasksHash = masksHash;
        NOX_CORE_INFO(graphMasks ? "Material graphs: ray tracing uses the graphs (Opacity Mask in shadows, AO, reflections, GI and the path tracer; the surface at reflection, GI and path tracer hits)"
                                 : "Material graphs: the ray tracing passes use the materials' own values again (no graph built)");
    }

    void Renderer::updateRayTracingGraphMasks(bool wanted)
    {
        // Created again only when something they compile from changed: whether the masks are on, or the masks' text.
        const uint64_t hash = wanted ? rayTracingGraphsHash() : 0;
        if (wanted == m_rayTracingGraphMasks && hash == m_rayTracingGraphMasksHash)
            return;
        installRayTracingPassPipelines(buildRayTracingPassPipelines(wanted), wanted, hash);
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
            m_device.waitIdle();
            createCustomLitPipelines(false);
            // (the ray tracing passes' mask variants shipped with their binaries when the editor had built them)
            const bool variantsShipped = std::all_of(CustomLitShaders::RayTracingMaskVariants().begin(), CustomLitShaders::RayTracingMaskVariants().end(),
                                                     [](const CustomLitShaders::RayTracingMaskVariant& variant) { return std::filesystem::exists(variant.Variant); });
            updateRayTracingGraphMasks(m_gbufferGraphPipeline && variantsShipped && GraphsUseRayTracing(m_materialGraphPrograms));
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
        if (!packaged && complete && graphsComplete && m_shaderCompiler.canCompile())
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
