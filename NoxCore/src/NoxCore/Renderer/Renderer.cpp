#include "Renderer.h"
#include <Rtxdi/GI/ReSTIRGI.h>
#include <Rtxdi/PT/ReSTIRPT.h>
#include <Rtxdi/RtxdiUtils.h>

#include <iostream>
#include <algorithm> // Necessary for std::clamp
#include <unordered_set>
#include <chrono>
#include <fstream>
#include <cctype>
#include <unordered_map>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Asset/MeshImporter.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/Hash.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Utils/PlatformUtils.h"
#include "Passes/FrameGraphResources.h"

namespace Nox
{
    static AssetHandle FindTextureAsset(const std::string& texturePath)
    {
        if (texturePath.empty())
            return 0;

        static std::unordered_map<std::string, AssetHandle> textureCache;

        // Keyed on the raw material path first: the path conversion below can hit the disk, and this
        // runs for every textured draw whose descriptor slot isn't cached.
        static std::unordered_map<std::string, AssetHandle> rawPathCache;
        if (auto rawCached = rawPathCache.find(texturePath); rawCached != rawPathCache.end())
            return rawCached->second;

        std::filesystem::path sourcePath(texturePath);
        if (sourcePath.is_absolute())
        {
            // Material paths are built from the asset directory string, so the lexical form almost
            // always works; std::filesystem::relative (disk access) is only the fallback.
            std::filesystem::path lexical = sourcePath.lexically_normal().lexically_relative(
                Project::GetActiveAssetDirectory().lexically_normal());
            if (!lexical.empty() && *lexical.begin() != "..")
            {
                sourcePath = lexical;
            }
            else
            {
                std::error_code ec;
                sourcePath = std::filesystem::relative(
                    sourcePath,
                    Project::GetActiveAssetDirectory(),
                    ec
                );
                if (ec)
                    return 0;
            }
        }

        std::string parentFolder = sourcePath.parent_path().filename().string();
        std::transform(parentFolder.begin(), parentFolder.end(), parentFolder.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });

        std::filesystem::path cookedPath = sourcePath;
        if (parentFolder == "textures")
            cookedPath.replace_extension(".ntex");
        else
            cookedPath = sourcePath.parent_path() / "Textures" /
                (sourcePath.stem().string() + ".ntex");

        const std::string cacheKey = cookedPath.generic_string();
        auto cached = textureCache.find(cacheKey);
        if (cached != textureCache.end())
        {
            rawPathCache.emplace(texturePath, cached->second);
            return cached->second;
        }

        for (const auto& [handle, metadata] : Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry())
        {
            if (metadata.Type != AssetType::Texture2D)
                continue;
            if (metadata.FilePath == cookedPath || metadata.SourceFilePath == sourcePath)
            {
                textureCache.emplace(cacheKey, handle);
                rawPathCache.emplace(texturePath, handle);
                return handle;
            }
        }
        return 0;
    }

    // MaterialData stores source paths, while the renderer needs the bindless descriptor slot.
    // Resolved once per path. When a texture is unloaded its slot is recycled for the next texture,
    // so entries pointing at that slot must be dropped or they'd draw the wrong image.
    static std::unordered_map<std::string, int> s_TextureDescriptorCache;

    // Paths with no texture asset. Without this, every such path re-ran the whole lookup every
    // frame. Only valid while the asset registry hasn't changed size (a new import may resolve it).
    static std::unordered_set<std::string> s_TextureDescriptorMisses;
    static size_t s_TextureDescriptorMissesRegistrySize = 0;

    void Renderer::InvalidateTextureDescriptorSlots(const std::vector<uint32_t>& slots)
    {
        if (slots.empty())
            return;

        std::erase_if(s_TextureDescriptorCache, [&](const auto& entry)
        {
            return std::find(slots.begin(), slots.end(), static_cast<uint32_t>(entry.second)) != slots.end();
        });

        // GPU materials may still point at the freed slots.
        if (s_Instance)
            s_Instance->m_gpuMaterialsDirty = true;
    }

    // Main thread. Returns true when the misses were dropped (the registry changed, a missing texture may exist now).
    static bool RefreshTextureDescriptorMisses()
    {
        const size_t registrySize = Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry().size();
        if (registrySize == s_TextureDescriptorMissesRegistrySize)
            return false;

        s_TextureDescriptorMisses.clear();
        s_TextureDescriptorMissesRegistrySize = registrySize;
        return true;
    }

    // Main thread: resolves the texture (requesting its load), filling the caches.
    static int GetTextureIndex(const std::string& path)
    {
        if (path.empty())
            return -1;

        auto& descriptorCache = s_TextureDescriptorCache;
        auto cached = descriptorCache.find(path);
        if (cached != descriptorCache.end())
            return cached->second;

        // Misses are refreshed once per frame (Renderer::BindGpuScene), so re-packing follows a registry change.
        if (s_TextureDescriptorMisses.contains(path))
            return -1;

        AssetHandle handle = FindTextureAsset(path);
        if (handle == 0)
        {
            s_TextureDescriptorMisses.insert(path);
            return -1;
        }

        // Loads in the background: the material draws without it until Renderer::MarkTexturesLoaded re-packs it.
        const AssetState state = AssetManager::RequestAsset(handle);
        if (state == AssetState::Loading)
            return -1;
        const Texture2D* texture = state == AssetState::Ready ? AssetManager::FindLoadedAsset<Texture2D>(handle) : nullptr;
        if (!texture)
        {
            s_TextureDescriptorMisses.insert(path);
            return -1;
        }

        const int descriptorIndex = static_cast<int>(texture->GetDescriptorIndexSlot());
        descriptorCache.emplace(path, descriptorIndex);
        return descriptorIndex;
    }

    // Main thread (resolves textures and requests their loads).
    static shaderio::GpuMaterial PackMaterial(const MaterialData& material)
    {
        shaderio::GpuMaterial packed{};
        packed.workflow = material.Workflow;
        packed.diffuseFactor = material.DiffuseFactor;
        packed.specularFactor = material.SpecularFactor;
        packed.baseColorFactor = (material.Workflow == 1.0f) ? material.DiffuseFactor : material.BaseColorFactor;
        packed.baseColorTextureIndex = GetTextureIndex(material.BaseColorTexturePath);
        packed.baseColorTextureSet = material.BaseColorTextureSet;
        packed.metallicFactor = material.MetallicFactor;
        packed.roughnessFactor = material.RoughnessFactor;
        packed.metallicRoughnessTextureIndex = GetTextureIndex(material.MetallicRoughnessTexturePath);
        packed.physicalDescriptorTextureSet = material.PhysicalDescriptorTextureSet;
        packed.normalTextureIndex = GetTextureIndex(material.NormalTexturePath);
        packed.normalTextureSet = material.NormalTextureSet;
        packed.occlusionTextureIndex = GetTextureIndex(material.OcclusionTexturePath);
        packed.occlusionTextureSet = material.OcclusionTextureSet;
        packed.emissiveFactor = material.EmissiveFactor;
        packed.emissiveTextureIndex = GetTextureIndex(material.EmissiveTexturePath);
        packed.emissiveTextureSet = material.EmissiveTextureSet;
        packed.emissiveStrength = material.emissiveStrength;
        packed.transmissionFactor = material.TransmissionFactor;
        packed.transmissionTextureIndex = GetTextureIndex(material.TransmissionTexturePath);
        packed.transmissionTextureSet = material.TransmissionTextureSet;
        packed.ior = material.IOR;
        packed.thickness = material.Thickness;
        packed.alphaMode = static_cast<uint32_t>(material.Mode);
        packed.alphaMaskCutoff = material.AlphaMaskCutoff;
        packed.doubleSided = material.DoubleSided ? 1u : 0u;
        packed.unlit = material.Unlit ? 1u : 0u;
        return packed;
    }

    Renderer::Renderer(std::shared_ptr<Window> window, bool isEditor) : m_window(std::move(window)), m_isEditor(isEditor)
    {
        NOX_CORE_INFO("Renderer Start");

        s_Instance = this;

        m_device = NRI::Device::create(NRI::GraphicsAPI::Vulkan, *m_window);
        if (!m_device) NOX_CORE_ASSERT("Failed to create NRI device");

        // Staging for every upload, right after the device: startup already uploads (textures, IBL inputs). A ring on
        // the transfer queue that streamed loads fill over several frames; oversized uploads get their own buffer.
        m_uploads.Init(*m_device, 128ull * 1024 * 1024);

#if NOX_PROFILING_ENABLED
        m_gpuProfiler = m_device->createGpuProfiler(MAX_FRAMES_IN_FLIGHT);
        Profiler::Get().SetGpuProfiler(m_gpuProfiler.get());
        if (!m_gpuProfiler->isSupported())
            NOX_CORE_WARN("GPU timestamp queries are not supported on the render queue; GPU timings are unavailable");
#endif

        initRenderer();

        /*
        bunnyMesh = MeshImporter::LoadMesh(MODEL_PATH_GLTF_STANDFORD);
        foxMesh = MeshImporter::LoadMesh(MODEL_PATH_FOX_GLTF);
        */

        // Outline
        watchShader("assets/shaders/Outline.slang", "Skybox", [this]() { createOutlinePipeline(true); });
        // Skybox
        watchShader("assets/shaders/Skybox.slang", "Skybox", [this]() { createSkyboxPipeline(true); });
        // Unlit
        watchShader("assets/shaders/Material_Unlit_Mesh.slang", "Unlit", [this]() { createUnlitPipeline(true); });
        watchShader("assets/shaders/Material_TransparentLit_Mesh.slang", "TransparentLit", [this]() { createTransparentLitPipeline(true); });
        watchShader("assets/shaders/PBRLighting.slang", "TransparentLit", [this]() { createTransparentLitPipeline(true); });
        watchShader("assets/shaders/PBRLighting.slang", "DeferredLighting", [this]() { createDeferredLightingPipeline(true); });
        // Visibility Buffer
        watchShader("assets/shaders/VisibilityBuffer.slang", "VisBuffer", [this]() { createVisibilityPipeline(true); });
        // G-Buffer (Fix watcher to recompile m_gbufferPipeline)
        watchShader("assets/shaders/GBufferMaterial.slang", "GBuffer", [this]() { createGBufferPipeline(true); });
        // Deferred PBR Lighting
        watchShader("assets/shaders/DeferredLighting.slang", "DeferredLighting", [this]() { createDeferredLightingPipeline(true); });
        // Post Process
        watchShader("assets/shaders/PostProcess.slang", "PostProcess", [this]() { createPostProcessPipeline(true); });
        watchShader("assets/shaders/TextureInspect.slang", "TextureInspect", [this]() { createTextureInspectPipeline(true); });
        watchShader("assets/shaders/InstanceCulling.slang", "InstanceCulling", [this]() { createInstanceCullingPipelines(true); });
        watchShader("assets/shaders/HiZBuild.slang", "HiZBuild", [this]() { createHiZBuildPipeline(true); });
        // NRD
        watchShader("assets/shaders/ShadowMask.slang", "ShadowMask", [this]() { createShadowMaskPipeline(true); });
        watchShader("assets/shaders/Reflection.slang", "Reflection", [this]() { createReflectionPipeline(true); });
        // Path Tracer
        watchShader("assets/shaders/PathTracer.slang", "PathTracer", [this]() { createPathTracerPipeline(true); });
        watchShader("assets/shaders/YCoCgDecodeInPlace.slang", "YCoCgDecodeInPlace", [this]() { createPathTracerPipeline(true); });
        // DDGI
        watchShader("assets/shaders/DDGIRadiance.slang", "DDGIRadiance", [this]() { createDDGIPipelines(true); });
        watchShader("assets/shaders/DDGIBlendIrradiance.slang", "DDGIBlendIrradiance", [this]() { createDDGIPipelines(true); });
        watchShader("assets/shaders/DDGIBlendDistance.slang", "DDGIBlendDistance", [this]() { createDDGIPipelines(true); });
        watchShader("assets/shaders/DDGIProbeSpheres.slang", "DDGIProbeSpheres", [this]() { createDDGIPipelines(true); });
        // ReSTIR GI
        watchShader("assets/shaders/RTXDI/GI/ReSTIRGIInitial.slang", "ReSTIRGIInitial", [this]() { createReSTIRGIPipelines(true); });
        watchShader("assets/shaders/RTXDI/GI/ReSTIRGITemporal.slang", "ReSTIRGITemporal", [this]() { createReSTIRGIPipelines(true); });
        watchShader("assets/shaders/RTXDI/GI/ReSTIRGISpatial.slang", "ReSTIRGISpatial", [this]() { createReSTIRGIPipelines(true); });
        watchShader("assets/shaders/RTXDI/Presampling/ReSTIRDIWriteLightPDF.slang", "ReSTIRDIWriteLightPDF", [this]() { createReSTIRDIPipelines(true); });
        watchShader("assets/shaders/RTXDI/Presampling/ReSTIRDIReduceLightPDFMip.slang", "ReSTIRDIReduceLightPDFMip", [this]() { createReSTIRDIPipelines(true); });
        watchShader("assets/shaders/RTXDI/Presampling/ReSTIRDIPresample.slang", "ReSTIRDIPresample", [this]() { createReSTIRDIPipelines(true); });
        watchShader("assets/shaders/RTXDI/Presampling/ReSTIRDIPresampleReGIR.slang", "ReSTIRDIPresampleReGIR", [this]() { createReSTIRDIPipelines(true); });
        watchShader("assets/shaders/RTXDI/DI/ReSTIRDIInitial.slang", "ReSTIRDIInitial", [this]() { createReSTIRDIPipelines(true); });
        watchShader("assets/shaders/RTXDI/DI/ReSTIRDITemporal.slang", "ReSTIRDITemporal", [this]() { createReSTIRDIPipelines(true); });
        watchShader("assets/shaders/RTXDI/DI/ReSTIRDISpatial.slang", "ReSTIRDISpatial", [this]() { createReSTIRDIPipelines(true); });
        watchShader("assets/shaders/RTXDI/DI/ReSTIRDIFinalShading.slang", "ReSTIRDIFinalShading", [this]() { createReSTIRDIPipelines(true); });
        watchShader("assets/shaders/RTXDI/PT/ReSTIRPTInitial.slang", "ReSTIRPTInitial", [this]() { createReSTIRPTPipelines(true); });
        watchShader("assets/shaders/RTXDI/PT/ReSTIRPTTemporal.slang", "ReSTIRPTTemporal", [this]() { createReSTIRPTPipelines(true); });
        watchShader("assets/shaders/RTXDI/PT/ReSTIRPTFinalShading.slang", "ReSTIRPTFinalShading", [this]() { createReSTIRPTPipelines(true); });

        m_whiteTexture = createSolidColorTexture(255, 255, 255, 255);

        // Allow buffer to hold up to 4K resolution entity pixels (3840 x 2160 * 4 bytes ≈ 33 MB)
        const uint32_t maxPickerBufferSize = 3840 * 2160 * sizeof(int32_t);

        m_pickerStagingBuffers.reserve(MAX_FRAMES_IN_FLIGHT);
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            m_pickerStagingBuffers.emplace_back(m_device->createBuffer(NRI::BufferDesc{
                .size = maxPickerBufferSize,
                .usage = NRI::BufferUsage::Staging
            }));
        }
        m_pickerReadbackRequests.resize(MAX_FRAMES_IN_FLIGHT);

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            m_inspectionProbeBuffers.emplace_back(m_device->createBuffer(NRI::BufferDesc{
                .size = sizeof(float) * 4,
                .usage = NRI::BufferUsage::Staging
            }));
        }
        m_inspectionProbePending.resize(MAX_FRAMES_IN_FLIGHT, 0);

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            m_cullViewBuffers.emplace_back(m_device->createBuffer(NRI::BufferDesc{
                .size = sizeof(shaderio::CullView),
                .usage = NRI::BufferUsage::Storage
            }));
            m_cullStatsBuffers.emplace_back(m_device->createBuffer(NRI::BufferDesc{
                .size = sizeof(shaderio::CullCounts),
                .usage = NRI::BufferUsage::Staging
            }));
            m_mipFeedbackReadback.emplace_back(m_device->createBuffer(NRI::BufferDesc{
                .size = sizeof(uint32_t) * shaderio::MipFeedbackSlots,
                .usage = NRI::BufferUsage::Staging
            }));
            m_clusterStatsReadback.emplace_back(m_device->createBuffer(NRI::BufferDesc{
                .size = sizeof(shaderio::ClusterStats),
                .usage = NRI::BufferUsage::Staging
            }));
            m_blasCompactionQueryPools.emplace_back(m_device->createQueryPool(NRI::QueryPoolDesc{
                .type = NRI::QueryType::AccelerationStructureCompactedSize,
                .capacity = BlasCompactionQueriesPerFrame
            }));
        }
        m_mipFeedbackBuffer = m_device->createBuffer(NRI::BufferDesc{
            .size = sizeof(uint32_t) * shaderio::MipFeedbackSlots,
            .usage = NRI::BufferUsage::StorageStatic
        });
        m_clusterStatsBuffer = m_device->createBuffer(NRI::BufferDesc{
            .size = sizeof(shaderio::ClusterStats),
            .usage = NRI::BufferUsage::StorageStatic
        });

        m_renderer2D = std::make_unique<Renderer2D>(isEditor, RendererContext
                                                    {
                                                        *m_device,
                                                        *m_shaderCompiler,
                                                        m_fileWatcher,
                                                        m_whiteTexture,
                                                        [this]()
                                                        {
                                                            return beginSingleTimeCommands();
                                                        },

                                                        [this](std::unique_ptr<NRI::CommandBuffer>&& commandBuffer)
                                                        {
                                                            endSingleTimeCommands(std::move(commandBuffer));
                                                        }
                                                    }
        );
    }

    Renderer::~Renderer()
    {
        NOX_CORE_INFO("Renderer Shutdown");

        // Release queued assets while the instance is still reachable: a Mesh destructor calls Renderer::UnloadMesh,
        // which asserts on s_Instance and queues its geometry, so this drains in rounds -- clearing the vector in place
        // would push into it while its elements are being destroyed.
        m_device->waitIdle();
        while (!m_deferredReleases.empty())
        {
            std::vector<DeferredRelease> releasing;
            {
                std::scoped_lock lock(m_deferredReleasesMutex);
                releasing.swap(m_deferredReleases);
            }
        }

#if NOX_PROFILING_ENABLED
        Profiler::Get().SetGpuProfiler(nullptr);
#endif

        if (s_Instance == this) s_Instance = nullptr;

        m_device->waitIdle();
        // Before the descriptor heap and device go away.
        m_renderGraph.ReleaseResources();
        m_device->shutdown(); // needed for texture to not remove imguitexture
        m_renderer2D.reset();
        // Cleanup Vulkan resources here
    }

    void Renderer::resizeWindow()
    {
        framebufferResized = true;
        m_lastWindowResizeRequestTime = std::chrono::steady_clock::now();
    }

    void Renderer::initRenderer()
    {
        createSwapChain();
        createCompiler();
        createUnlitPipeline(false);
        createTransparentLitPipeline(false);
        if (!m_isEditor) createPresentPipeline(false);
        createComputePipeline();
        createSkyboxPipeline(false);
        createOutlinePipeline(false);

        // Visability
        createVisibilityPipeline(false); // <--- ADD THIS
        // G-Buffer
        createGBufferPipeline();
        // Post Process
        createPostProcessPipeline(false);
        createTextureInspectPipeline(false);
        createInstanceCullingPipelines(false);
        createHiZBuildPipeline(false);
        //NRD
        createShadowMaskPipeline();
        createReflectionPipeline();
        // Path Tracer
        createPathTracerPipeline(false);
        // DDGI
        createDDGIPipelines(false);
        // ReSTIR GI
        createReSTIRGIPipelines(false);
        // ReSTIR DI
        createReSTIRDIPipelines(false);
        // ReSTIR PT
        createReSTIRPTPipelines(false);

        createCommandPool();
        createUniformBuffers();
        createSelectedEntityIDBuffers();
        createDescriptorHeaps();
        m_renderGraph.Initialize(*m_device, *m_resourceHeap, MAX_FRAMES_IN_FLIGHT);
        m_renderGraph.SetCommandBufferSetup([this](NRI::CommandBuffer& cmd) { applyCommandBufferBaseline(cmd); });
        createTextureImage();
        createSceneResources();
        // PBR
        createDeferredLightingPipeline(false);
        // NRD keeps size-dependent internal state; its textures are render graph resources.
        if (m_renderSize.width > 0 && m_renderSize.height > 0)
            m_device->initNRD(m_renderSize.width, m_renderSize.height);
        // ReSTIR GI/DI static data, ReSTIR PT context
        m_lightPDFMipLevels = static_cast<uint32_t>(log2(static_cast<double>(m_lightPDFTextureSize)));
        createRTXDINeighborOffsets();
        createReSTIRPTContext();


        // Allocate baseline capacities for dynamic GPU buffers so vectors are NEVER empty
        m_gpuScene.Initialize(*m_device, MAX_FRAMES_IN_FLIGHT);

        m_DrawListBufferCapacity = sizeof(uint32_t) * 1024;
        createDrawListBuffers(m_DrawListBufferCapacity);

        m_BoneBufferCapacity = sizeof(glm::mat4) * 64;
        createBoneBuffer(m_BoneBufferCapacity);

        initGeometryBuffers();

        // Create every resource needed for PBR
        initPBR();
    }

    void Renderer::cleanupSwapChain()
    {
        m_swapChain.reset();
    }

    void Renderer::recreateSwapChain()
    {
        /*
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(m_window, &width, &height);
        while (width == 0 || height == 0)
        {
            SDL_Event event;
            SDL_WaitEvent(&event);
    
            SDL_GetWindowSizeInPixels(m_window, &width, &height);
        }
        */

        m_device->waitIdle();

        if (!m_isEditor)
        {
            m_resourceHeap->unregisterTexture(m_sceneResource->GetDescriptorIndexSlot());
        }

        cleanupSwapChain();
        createSwapChain();
        createSceneResources();

        // Output-resolution graph targets follow the new swapchain size by themselves.
        if (m_renderSize.width > 0 && m_renderSize.height > 0)
            m_device->initNRD(m_renderSize.width, m_renderSize.height);
        m_renderGraph.ResetHistory(NRDHistoryKey);
        m_pathTracerSampleCount = 0;
    }

    void Renderer::createSwapChain()
    {
        int width = 0, height = 0;
        m_window->getSizeInPixels(width, height);
        m_swapChain = m_device->createSwapchain(NRI::SwapchainDesc{static_cast<uint32_t>(width), static_cast<uint32_t>(height), m_vSync});
        m_swapChainExtent = m_swapChain->getExtent();
    }

    void Renderer::setVSync(bool enabled)
    {
        if (m_vSync != enabled)
        {
            m_vSync = enabled;
            recreateSwapChain(); // Rebuild swapchain with the new Present Mode
        }
    }

    void Renderer::onViewportSizeChange(NRI::Extent2D size)
    {
        // Debounced -- see m_viewportResizePending's declaration in Renderer.h for why this no longer
        // calls applyRenderResolution() synchronously. EditorLayer.cpp calls this every frame the ImGui
        // viewport panel's size differs from ours, which during a live drag is every frame; the actual
        // (expensive) rebuild is deferred to applyPendingRenderResolutionIfNeeded() and only happens once
        // the size has been stable for a short settle window.
        //
        // EditorLayer compares against the APPLIED size (getViewPortSize), so it keeps calling this
        // every frame with the same value until the apply happens - restarting the timer on those
        // repeats meant the settle window never elapsed and the render targets never resized, leaving
        // them at the startup size and offsetting mouse picking toward the top-left.
        if (m_viewportResizePending &&
            size.width == m_pendingViewportSize.width && size.height == m_pendingViewportSize.height)
            return;

        m_pendingViewportSize = size;
        m_viewportResizePending = true;
        m_lastViewportResizeRequestTime = std::chrono::steady_clock::now();
    }

    void Renderer::setDLSSEnabled(bool enabled)
    {
        if (m_dlssEnabled == enabled) return;
        m_dlssEnabled = enabled;
        m_pendingRenderResolutionUpdate = true;
    }

    void Renderer::setUpscaleMode(NRI::UpscaleMode mode)
    {
        if (m_dlssMode == mode) return;
        m_dlssMode = mode;
        if (m_dlssEnabled)
            m_pendingRenderResolutionUpdate = true;
    }

    void Renderer::setDLSSRayReconstructionEnabled(bool enabled)
    {
        if (m_dlssRayReconstructionEnabled != enabled)
        {
            m_dlssRayReconstructionEnabled = enabled;
            m_renderGraph.ResetHistory(DLSSHistoryKey); // Streamline flushes its history without destroying contexts

            // Mutual Exclusion: DLSS-RR replaces every downstream NRD denoiser (reflections/GI/DI in the
            // hybrid path, or the whole image in the path tracer) -- running both would double-filter.
            if (enabled)
            {
                bool anyDisabled = false;
                if (m_nrdReflectionDenoiser != NRI::NRDReflectionDenoiser::Off)
                {
                    m_nrdReflectionDenoiser = NRI::NRDReflectionDenoiser::Off;
                    anyDisabled = true;
                }
                if (m_nrdGIDenoiser != NRI::NRDDiffuseDenoiser::Off)
                {
                    m_nrdGIDenoiser = NRI::NRDDiffuseDenoiser::Off;
                    anyDisabled = true;
                }
                if (m_nrdDIDenoiser != NRI::NRDDiffuseDenoiser::Off)
                {
                    m_nrdDIDenoiser = NRI::NRDDiffuseDenoiser::Off;
                    anyDisabled = true;
                }
                if (m_nrdPTDenoiser != NRI::NRDDiffuseDenoiser::Off)
                {
                    m_nrdPTDenoiser = NRI::NRDDiffuseDenoiser::Off;
                    anyDisabled = true;
                }
                if (anyDisabled)
                {
                    m_renderGraph.ResetHistory(NRDHistoryKey);
                    NOX_CORE_INFO("[Denoising] DLSS Ray Reconstruction activated: NRD REBLUR/RELAX automatically disabled (mutually exclusive).");
                }
            }
        }
    }

    void Renderer::setNRDReflectionDenoiser(NRI::NRDReflectionDenoiser mode)
    {
        if (m_nrdReflectionDenoiser != mode)
        {
            m_nrdReflectionDenoiser = mode;
            m_renderGraph.ResetHistory(NRDHistoryKey);

            // Mutual Exclusion: NRD REBLUR/RELAX conflicts with DLSS Ray Reconstruction
            if (mode != NRI::NRDReflectionDenoiser::Off && m_dlssRayReconstructionEnabled)
            {
                m_dlssRayReconstructionEnabled = false;
                m_renderGraph.ResetHistory(DLSSHistoryKey);
                NOX_CORE_INFO("[Denoising] NRD Reflection Denoiser activated: DLSS Ray Reconstruction automatically disabled (mutually exclusive).");
            }
        }
    }

    void Renderer::setNRDGIDenoiser(NRI::NRDDiffuseDenoiser mode)
    {
        if (m_nrdGIDenoiser != mode)
        {
            m_nrdGIDenoiser = mode;
            m_renderGraph.ResetHistory(NRDHistoryKey);

            if (mode != NRI::NRDDiffuseDenoiser::Off && m_dlssRayReconstructionEnabled)
            {
                m_dlssRayReconstructionEnabled = false;
                m_renderGraph.ResetHistory(DLSSHistoryKey);
                NOX_CORE_INFO("[Denoising] NRD GI Denoiser activated: DLSS Ray Reconstruction automatically disabled (mutually exclusive).");
            }
        }
    }

    void Renderer::setNRDDIDenoiser(NRI::NRDDiffuseDenoiser mode)
    {
        if (m_nrdDIDenoiser != mode)
        {
            m_nrdDIDenoiser = mode;
            m_renderGraph.ResetHistory(NRDHistoryKey);

            if (mode != NRI::NRDDiffuseDenoiser::Off && m_dlssRayReconstructionEnabled)
            {
                m_dlssRayReconstructionEnabled = false;
                m_renderGraph.ResetHistory(DLSSHistoryKey);
                NOX_CORE_INFO("[Denoising] NRD DI Denoiser activated: DLSS Ray Reconstruction automatically disabled (mutually exclusive).");
            }
        }
    }

    void Renderer::setNRDPTDenoiser(NRI::NRDDiffuseDenoiser mode)
    {
        if (m_nrdPTDenoiser != mode)
        {
            m_nrdPTDenoiser = mode;
            m_renderGraph.ResetHistory(NRDHistoryKey);

            if (mode != NRI::NRDDiffuseDenoiser::Off && m_dlssRayReconstructionEnabled)
            {
                m_dlssRayReconstructionEnabled = false;
                m_renderGraph.ResetHistory(DLSSHistoryKey);
                NOX_CORE_INFO("[Denoising] NRD Path Tracer Denoiser activated: DLSS Ray Reconstruction automatically disabled (mutually exclusive).");
            }
        }
    }

    void Renderer::applyRenderResolution()
    {
        NRI::Extent2D outputSize = m_isEditor ? m_viewportSize : m_swapChainExtent;
        if (outputSize.width == 0 || outputSize.height == 0)
            return;

        // While DLSS is disabled we always render 1:1 regardless of the remembered mode, so turning
        // it back on later doesn't require the user to also re-pick a mode.
        NRI::UpscaleMode effectiveMode = m_dlssEnabled ? m_dlssMode : NRI::UpscaleMode::Off;
        auto optimal = m_device->getDLSSOptimalRenderSize(effectiveMode, outputSize);
        m_renderSize = (optimal.size.width > 0 && optimal.size.height > 0) ? optimal.size : outputSize;

#if NOX_PROFILE_STATS
        // Timings measured at the previous resolution / upscale mode are not comparable anymore.
        Profiler::Get().ResetStats();
#endif

        m_device->waitIdle();
        createSceneResources();

        // Graph-owned targets and histories follow the new sizes; with the GPU idle, drop the old-size ones right away
        // instead of letting them age out, and invalidate every history (reservoirs, accumulation, NRD, DLSS).
        m_renderGraph.ReleaseResources();
        m_renderGraph.ResetAllHistory();
        m_device->initNRD(m_renderSize.width, m_renderSize.height);
        createReSTIRPTContext();
        m_pathTracerSampleCount = 0;

        // NGX's internal DLSS feature is fixed-size once created; it must be explicitly freed here
        // so it gets recreated at the new resolution on the next evaluate, otherwise evaluate silently
        // no-ops forever once our tagged resources no longer match the size it was created with.
        m_device->resetDLSSViewport();
    }

    void Renderer::applyPendingRenderResolutionIfNeeded()
    {
        // Settle window for live viewport-panel resize drags -- see m_viewportResizePending in Renderer.h.
        // 120ms is short enough to feel responsive once you stop dragging, but long enough that a
        // continuous drag (which re-fires onViewportSizeChange every frame) never triggers a rebuild
        // mid-drag, only once after it stops.
        if (m_viewportResizePending)
        {
            constexpr auto settleDelay = std::chrono::milliseconds(120);
            if (std::chrono::steady_clock::now() - m_lastViewportResizeRequestTime >= settleDelay)
            {
                m_viewportResizePending = false;
                m_viewportSize = m_pendingViewportSize;
                applyRenderResolution();
            }
        }

        if (!m_pendingRenderResolutionUpdate)
            return;
        m_pendingRenderResolutionUpdate = false;
        applyRenderResolution();
    }

    void Renderer::createShadowMaskPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats = {
            NRI::ImageFormat::R16G16_SFLOAT,
            NRI::ImageFormat::R16_SFLOAT,
            NRI::ImageFormat::R10G10B10A2_UNORM
        };

        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/ShadowMask.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/ShadowMask.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/ShadowMask.slang"
        });

        m_shadowMaskPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::createReflectionPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats = {
            NRI::ImageFormat::R16G16B16A16_SFLOAT // Raw Reflection (Radiance RGB + HitDist A)
        };

        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/Reflection.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/Reflection.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/Reflection.slang"
        });

        m_reflectionPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::createPathTracerPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats = {NRI::ImageFormat::R16G16B16A16_SFLOAT};

        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/PathTracer.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/PathTracer.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/PathTracer.slang"
        });

        m_pathTracerPipeline = m_device->createPipeline(desc, *m_shaderCompiler);

        NRI::PipelineDesc decodeDesc{};
        decodeDesc.type = NRI::PipelineType::Compute;
        decodeDesc.forceCompile = forceCompile;
        decodeDesc.shaders.push_back({
            .stage = NRI::ShaderStage::Compute,
            .entryPoint = "compMain",
            .sourcePath = "assets/shaders/YCoCgDecodeInPlace.slang"
        });
        m_ycocgDecodePipeline = m_device->createPipeline(decodeDesc, *m_shaderCompiler);
    }

    void Renderer::resetDDGIGridToDefaults()
    {
        m_ddgiGridOrigin = glm::vec3(-20.0f, -0.5f, -12.0f);
        m_ddgiGridSpacing = glm::vec3(1.8f, 1.4f, 1.7f);
        m_ddgiHysteresis = 0.97f;
        m_ddgiNormalBias = 0.2f;
        m_ddgiDebugSphereRadius = 0.15f;
        m_renderGraph.ResetHistory(DDGIHistoryKey);
    }

    void Renderer::createRTXDINeighborOffsets()
    {
        // 128 offsets within a unit disk ([-1, 1]) for the ReSTIR GI/DI spatial passes. Static data.
        constexpr uint32_t neighborOffsetCount = 128;
        glm::vec2 floatOffsets[neighborOffsetCount];
        const float phi2 = 1.0f / 1.3247179572447f;
        float u = 0.5f;
        float v = 0.5f;
        uint32_t count = 0;
        while (count < neighborOffsetCount)
        {
            u += phi2;
            v += phi2 * phi2;
            if (u >= 1.0f) u -= 1.0f;
            if (v >= 1.0f) v -= 1.0f;

            float du = (u - 0.5f) * 2.0f;
            float dv = (v - 0.5f) * 2.0f;
            if (du * du + dv * dv <= 1.0f)
            {
                floatOffsets[count++] = glm::vec2(du, dv);
            }
        }

        uint64_t offsetsBufferSize = sizeof(floatOffsets);
        m_restirGINeighborOffsetsBuffer = m_device->createBuffer(NRI::BufferDesc{
            .size = offsetsBufferSize,
            .usage = NRI::BufferUsage::Storage
        });

        void* mapped = m_restirGINeighborOffsetsBuffer->map(0, offsetsBufferSize);
        memcpy(mapped, floatOffsets, offsetsBufferSize);
        m_restirGINeighborOffsetsBuffer->unmap();
    }

    void Renderer::setReSTIRPTTemporalEnabled(bool enabled)
    {
        m_restirPTTemporalEnabled = enabled;
        if (m_restirPTContext)
            m_restirPTContext->SetResamplingMode(enabled ? rtxdi::ReSTIRPT_ResamplingMode::Temporal : rtxdi::ReSTIRPT_ResamplingMode::None);
    }

    void Renderer::createReSTIRPTContext()
    {
        const uint32_t width = m_renderSize.width;
        const uint32_t height = m_renderSize.height;

        if (width == 0 || height == 0)
            return;

        // The real SDK context (Source/ReSTIRPT.cpp) owns buffer-index rotation and default parameters, recreated whenever
        // the render size changes. Slots 0/1 ping-pong for temporal resampling, slot 2 preserves the unresampled
        // initial-sampling reservoir for final shading's decorrelation fallback (rtxdi::ReSTIRPTContext::UpdateBufferIndices).
        rtxdi::ReSTIRPTStaticParameters staticParams{};
        staticParams.RenderWidth = width;
        staticParams.RenderHeight = height;
        staticParams.CheckerboardSamplingMode = rtxdi::CheckerboardMode::Off;
        m_restirPTContext = std::make_unique<rtxdi::ReSTIRPTContext>(staticParams);
        // Temporal resampling currently produces a visible lighting-rotation artifact under investigation: defaults to
        // None (the known-good state), toggled via setReSTIRPTTemporalEnabled().
        m_restirPTContext->SetResamplingMode(m_restirPTTemporalEnabled ? rtxdi::ReSTIRPT_ResamplingMode::Temporal : rtxdi::ReSTIRPT_ResamplingMode::None);
    }

    void Renderer::createReSTIRPTPipelines(bool forceCompile)
    {
        // 1. Initial Sampling Pipeline (ResamplingMode::None -- generates + RIS-combines
        // numInitialSamples full paths per pixel, no temporal/spatial reuse yet)
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            // 2 targets: SV_Target0 is the debug-only combined-radiance preview, SV_Target1 is the
            // primary-surface direct lighting consumed by the Final Shading pass (the ReSTIR PT Primary Direct graph texture).
            desc.colorFormats = {NRI::ImageFormat::R16G16B16A16_SFLOAT, NRI::ImageFormat::R16G16B16A16_SFLOAT};
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Task,
                .entryPoint = "taskMain",
                .sourcePath = "assets/shaders/RTXDI/PT/ReSTIRPTInitial.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Mesh,
                .entryPoint = "meshMain",
                .sourcePath = "assets/shaders/RTXDI/PT/ReSTIRPTInitial.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Fragment,
                .entryPoint = "fragMain",
                .sourcePath = "assets/shaders/RTXDI/PT/ReSTIRPTInitial.slang"
            });
            m_restirPTInitialPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }

        // 2. Final Shading Pipeline
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            desc.colorFormats = {NRI::ImageFormat::R16G16B16A16_SFLOAT};
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Task,
                .entryPoint = "taskMain",
                .sourcePath = "assets/shaders/RTXDI/PT/ReSTIRPTFinalShading.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Mesh,
                .entryPoint = "meshMain",
                .sourcePath = "assets/shaders/RTXDI/PT/ReSTIRPTFinalShading.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Fragment,
                .entryPoint = "fragMain",
                .sourcePath = "assets/shaders/RTXDI/PT/ReSTIRPTFinalShading.slang"
            });
            m_restirPTFinalShadingPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }

        // 3. Temporal Resampling Pipeline (RandomReplay/hybrid-shift reconnection against last frame's
        // finalized reservoir) -- runs between Initial Sampling and Final Shading whenever the context's
        // resampling mode is Temporal (or TemporalAndSpatial once Spatial exists).
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            desc.colorFormats = {NRI::ImageFormat::R16G16B16A16_SFLOAT};
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Task,
                .entryPoint = "taskMain",
                .sourcePath = "assets/shaders/RTXDI/PT/ReSTIRPTTemporal.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Mesh,
                .entryPoint = "meshMain",
                .sourcePath = "assets/shaders/RTXDI/PT/ReSTIRPTTemporal.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Fragment,
                .entryPoint = "fragMain",
                .sourcePath = "assets/shaders/RTXDI/PT/ReSTIRPTTemporal.slang"
            });
            m_restirPTTemporalPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }
    }

    void Renderer::createDDGIPipelines(bool forceCompile)
    {
        // 1. Radiance Pipeline
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            desc.colorFormats = {NRI::ImageFormat::R16G16B16A16_SFLOAT};
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Task,
                .entryPoint = "taskMain",
                .sourcePath = "assets/shaders/DDGIRadiance.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Mesh,
                .entryPoint = "meshMain",
                .sourcePath = "assets/shaders/DDGIRadiance.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Fragment,
                .entryPoint = "fragMain",
                .sourcePath = "assets/shaders/DDGIRadiance.slang"
            });
            m_ddgiRadiancePipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }

        // 2. Blend Irradiance Pipeline
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            desc.colorFormats = {NRI::ImageFormat::R16G16B16A16_SFLOAT};
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Task,
                .entryPoint = "taskMain",
                .sourcePath = "assets/shaders/DDGIBlendIrradiance.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Mesh,
                .entryPoint = "meshMain",
                .sourcePath = "assets/shaders/DDGIBlendIrradiance.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Fragment,
                .entryPoint = "fragMain",
                .sourcePath = "assets/shaders/DDGIBlendIrradiance.slang"
            });
            m_ddgiBlendIrradiancePipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }

        // 3. Blend Distance Pipeline
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            desc.colorFormats = {NRI::ImageFormat::R16G16_SFLOAT};
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Task,
                .entryPoint = "taskMain",
                .sourcePath = "assets/shaders/DDGIBlendDistance.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Mesh,
                .entryPoint = "meshMain",
                .sourcePath = "assets/shaders/DDGIBlendDistance.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Fragment,
                .entryPoint = "fragMain",
                .sourcePath = "assets/shaders/DDGIBlendDistance.slang"
            });
            m_ddgiBlendDistancePipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }

        // 4. Debug Spheres Pipeline (Rendered in Forward 3D pass into the HDR scene + entity ID targets)
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = true;
            desc.colorFormats = {
                NRI::ImageFormat::R16G16B16A16_SFLOAT,
                NRI::ImageFormat::R32SINT
            };
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Mesh,
                .entryPoint = "meshMain",
                .sourcePath = "assets/shaders/DDGIProbeSpheres.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Fragment,
                .entryPoint = "fragMain",
                .sourcePath = "assets/shaders/DDGIProbeSpheres.slang"
            });
            m_ddgiDebugSpheresPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }
    }

    void Renderer::createReSTIRGIPipelines(bool forceCompile)
    {
        // 1. Initial Candidate Generation Pipeline
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            desc.colorFormats = {NRI::ImageFormat::R16G16B16A16_SFLOAT};
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Task,
                .entryPoint = "taskMain",
                .sourcePath = "assets/shaders/RTXDI/GI/ReSTIRGIInitial.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Mesh,
                .entryPoint = "meshMain",
                .sourcePath = "assets/shaders/RTXDI/GI/ReSTIRGIInitial.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Fragment,
                .entryPoint = "fragMain",
                .sourcePath = "assets/shaders/RTXDI/GI/ReSTIRGIInitial.slang"
            });
            m_restirGIInitialPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }

        // 2. Temporal Resampling Pipeline
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            desc.colorFormats = {NRI::ImageFormat::R16G16B16A16_SFLOAT};
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Task,
                .entryPoint = "taskMain",
                .sourcePath = "assets/shaders/RTXDI/GI/ReSTIRGITemporal.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Mesh,
                .entryPoint = "meshMain",
                .sourcePath = "assets/shaders/RTXDI/GI/ReSTIRGITemporal.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Fragment,
                .entryPoint = "fragMain",
                .sourcePath = "assets/shaders/RTXDI/GI/ReSTIRGITemporal.slang"
            });
            m_restirGITemporalPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }

        // 3. Spatial Resampling Pipeline
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            desc.colorFormats = {NRI::ImageFormat::R16G16B16A16_SFLOAT};
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Task,
                .entryPoint = "taskMain",
                .sourcePath = "assets/shaders/RTXDI/GI/ReSTIRGISpatial.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Mesh,
                .entryPoint = "meshMain",
                .sourcePath = "assets/shaders/RTXDI/GI/ReSTIRGISpatial.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Fragment,
                .entryPoint = "fragMain",
                .sourcePath = "assets/shaders/RTXDI/GI/ReSTIRGISpatial.slang"
            });
            m_restirGISpatialPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }
    }

    void Renderer::createReSTIRDIPipelines(bool forceCompile)
    {
        // -1a. Light PDF Mip 0 Write Pipeline (feeds RTXDI_PresampleLocalLights)
        {
            NRI::PipelineDesc desc{};
            desc.type = NRI::PipelineType::Compute;
            desc.forceCompile = forceCompile;
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Compute,
                .entryPoint = "compMain",
                .sourcePath = "assets/shaders/RTXDI/Presampling/ReSTIRDIWriteLightPDF.slang"
            });
            m_restirDIWriteLightPDFPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }

        // -1b. Light PDF Mip Chain Reduce Pipeline (sum-reduction, dispatched once per mip level)
        {
            NRI::PipelineDesc desc{};
            desc.type = NRI::PipelineType::Compute;
            desc.forceCompile = forceCompile;
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Compute,
                .entryPoint = "compMain",
                .sourcePath = "assets/shaders/RTXDI/Presampling/ReSTIRDIReduceLightPDFMip.slang"
            });
            m_restirDIReduceLightPDFMipPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }

        // 0a. RIS Presample Pipeline (plain 1D compute, no G-buffer -- see shaderIO.h comment)
        {
            NRI::PipelineDesc desc{};
            desc.type = NRI::PipelineType::Compute;
            desc.forceCompile = forceCompile;
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Compute,
                .entryPoint = "compMain",
                .sourcePath = "assets/shaders/RTXDI/Presampling/ReSTIRDIPresample.slang"
            });
            m_restirDIPresamplePipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }

        // 0b. ReGIR Presample Pipeline (plain 1D compute)
        {
            NRI::PipelineDesc desc{};
            desc.type = NRI::PipelineType::Compute;
            desc.forceCompile = forceCompile;
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Compute,
                .entryPoint = "compMain",
                .sourcePath = "assets/shaders/RTXDI/Presampling/ReSTIRDIPresampleReGIR.slang"
            });
            m_restirDIPresampleReGIRPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }

        // 1. Initial Sampling Pipeline
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            desc.colorFormats = {NRI::ImageFormat::R16G16B16A16_SFLOAT};
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Task,
                .entryPoint = "taskMain",
                .sourcePath = "assets/shaders/RTXDI/DI/ReSTIRDIInitial.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Mesh,
                .entryPoint = "meshMain",
                .sourcePath = "assets/shaders/RTXDI/DI/ReSTIRDIInitial.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Fragment,
                .entryPoint = "fragMain",
                .sourcePath = "assets/shaders/RTXDI/DI/ReSTIRDIInitial.slang"
            });
            m_restirDIInitialPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }

        // 2. Temporal Resampling Pipeline
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            desc.colorFormats = {NRI::ImageFormat::R16G16B16A16_SFLOAT};
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Task,
                .entryPoint = "taskMain",
                .sourcePath = "assets/shaders/RTXDI/DI/ReSTIRDITemporal.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Mesh,
                .entryPoint = "meshMain",
                .sourcePath = "assets/shaders/RTXDI/DI/ReSTIRDITemporal.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Fragment,
                .entryPoint = "fragMain",
                .sourcePath = "assets/shaders/RTXDI/DI/ReSTIRDITemporal.slang"
            });
            m_restirDITemporalPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }

        // 3. Spatial Resampling Pipeline
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            desc.colorFormats = {NRI::ImageFormat::R16G16B16A16_SFLOAT};
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Task,
                .entryPoint = "taskMain",
                .sourcePath = "assets/shaders/RTXDI/DI/ReSTIRDISpatial.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Mesh,
                .entryPoint = "meshMain",
                .sourcePath = "assets/shaders/RTXDI/DI/ReSTIRDISpatial.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Fragment,
                .entryPoint = "fragMain",
                .sourcePath = "assets/shaders/RTXDI/DI/ReSTIRDISpatial.slang"
            });
            m_restirDISpatialPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }

        // 4. Final Shading Pipeline
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            desc.colorFormats = {NRI::ImageFormat::R16G16B16A16_SFLOAT};
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Task,
                .entryPoint = "taskMain",
                .sourcePath = "assets/shaders/RTXDI/DI/ReSTIRDIFinalShading.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Mesh,
                .entryPoint = "meshMain",
                .sourcePath = "assets/shaders/RTXDI/DI/ReSTIRDIFinalShading.slang"
            });
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Fragment,
                .entryPoint = "fragMain",
                .sourcePath = "assets/shaders/RTXDI/DI/ReSTIRDIFinalShading.slang"
            });
            m_restirDIFinalShadingPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        }
    }

    void Renderer::createCompiler()
    {
        m_shaderCompiler = NRI::CreateSlangCompiler();
    }

    void Renderer::watchShader(const std::filesystem::path& path, const std::string& pipelineKey, std::function<void()> reloadFn)
    {
        m_fileWatcher.watch(path, [this, pipelineKey, reloadFn](const auto& modifiedPath)
        {
            std::scoped_lock lock(m_reloadMutex);
            m_pendingReloads[pipelineKey] = reloadFn;
        });
    }

    void Renderer::createUnlitPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;

        desc.colorFormats =
        {
            NRI::ImageFormat::R16G16B16A16_SFLOAT,
            NRI::ImageFormat::R32SINT
        };
        // notes i had to split task from mesh because of i think drawid otherwise weird flickering and not showing up correctly
        // might be a slang issue could change in the future fuck nvidia not testing slang
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/Material_PBR_MeshTask.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/Material_PBR_Mesh.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/Material_Unlit_Mesh.slang"
        });
        m_unlitPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::createTransparentLitPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;

        desc.colorFormats =
        {
            NRI::ImageFormat::R16G16B16A16_SFLOAT,
            NRI::ImageFormat::R32SINT
        };
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/Material_PBR_MeshTask.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/Material_PBR_Mesh.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/Material_TransparentLit_Mesh.slang"
        });
        m_transparentLitPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::createPresentPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats = {
            NRI::ImageFormat::Surface
        };
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Vertex,
            .entryPoint = "vertMain",
            .sourcePath = "assets/shaders/present.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/present.slang"
        });
        m_presentPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::createComputePipeline()
    {
        /*NRI::PipelineDesc computeDesc
        {
            .type = NRI::PipelineType::Compute,
            .shaders = {
                {
                    .stage = NRI::ShaderStage::Compute,
                    .entryPoint = "compMain",
                    .bytecode = readFile("../../shaders/slang.spv")
                }
            }
        };
    
        m_computePipeline = m_device->createPipeline(computeDesc);*/
    }

    void Renderer::createSkyboxPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;

        desc.colorFormats =
        {
            NRI::ImageFormat::R16G16B16A16_SFLOAT,
            NRI::ImageFormat::R32SINT
        };

        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/Skybox.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/Skybox.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/Skybox.slang"
        });

        m_skyboxPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::createOutlinePipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats = {
            NRI::ImageFormat::Surface
        };
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/Outline.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/Outline.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/Outline.slang"
        });
        m_outlinePipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::createCommandPool()
    {
        m_commandAllocator = m_device->createCommandAllocator(NRI::CommandBufferReset::PerCommandBuffer);
    }

    void Renderer::createSceneResources()
    {
        // The final LDR image outlives the frame (the editor's ImGui viewport shows it), so it stays renderer-owned and
        // is imported into the render graph; every other render target is a graph resource.
        m_sceneResource = m_device->createTexture(NRI::TextureDesc{
            .width = m_isEditor ? m_viewportSize.width : m_swapChainExtent.width,
            .height = m_isEditor ? m_viewportSize.height : m_swapChainExtent.height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::Surface,
            .directFormat = UINT32_MAX
        });

        if (!m_isEditor)
        {
            m_resourceHeap->registerTexture(*m_sceneResource);
            uniformData.imageHeapIndexOffset = m_resourceHeap->getImageHeapIndexOffset();
            uniformData.finalImageIndex = m_sceneResource->GetDescriptorIndexSlot();
        }
    }

    void Renderer::createVisibilityPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats = {NRI::ImageFormat::R32G32_UINT};

        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/Material_PBR_MeshTask.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/VisibilityBuffer.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/VisibilityBuffer.slang"
        });

        m_visibilityPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::createGBufferPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats = {
            NRI::ImageFormat::RGBA8, // 0: SV_Target0 (Albedo)
            NRI::ImageFormat::R16G16B16A16_SFLOAT, // 1: SV_Target1 (Normal)
            NRI::ImageFormat::RGBA8, // 2: SV_Target2 (Material)
            NRI::ImageFormat::R16G16B16A16_SFLOAT, // 3: SV_Target3 (Emission)
            NRI::ImageFormat::R32SINT, // 4: SV_Target4 (Entity ID)
            NRI::ImageFormat::R32G32_SFLOAT, // 5: SV_Target5 (Velocity)
            NRI::ImageFormat::RGBA8 // 6: SV_Target6 (Specular Albedo)
        };

        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/GBufferMaterial.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/GBufferMaterial.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/GBufferMaterial.slang"
        });

        m_gbufferPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::createDeferredLightingPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats = {NRI::ImageFormat::R16G16B16A16_SFLOAT};

        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/DeferredLighting.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/DeferredLighting.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/DeferredLighting.slang"
        });

        m_deferredLightingPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::createTextureInspectPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.type = NRI::PipelineType::Compute;
        desc.forceCompile = forceCompile;
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Compute,
            .entryPoint = "compMain",
            .sourcePath = "assets/shaders/TextureInspect.slang"
        });
        m_textureInspectPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::createInstanceCullingPipelines(bool forceCompile)
    {
        constexpr std::array<const char*, 5> EntryPoints = { "cullMain", "countMain", "offsetMain", "writeMain", "lateMain" };
        for (size_t index = 0; index < EntryPoints.size(); ++index)
        {
            NRI::PipelineDesc desc{};
            desc.type = NRI::PipelineType::Compute;
            desc.forceCompile = forceCompile;
            desc.shaders.push_back({
                .stage = NRI::ShaderStage::Compute,
                .entryPoint = EntryPoints[index],
                .sourcePath = "assets/shaders/InstanceCulling.slang"
            });
            m_instanceCullingPipelines[index] = m_device->createPipeline(desc, *m_shaderCompiler);
        }
    }

    void Renderer::createHiZBuildPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.type = NRI::PipelineType::Compute;
        desc.forceCompile = forceCompile;
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Compute,
            .entryPoint = "buildMain",
            .sourcePath = "assets/shaders/HiZBuild.slang"
        });
        m_hiZBuildPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::createPostProcessPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats = {NRI::ImageFormat::Surface};

        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/PostProcess.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/PostProcess.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/PostProcess.slang"
        });

        m_postProcessPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::createTextureImage()
    {
        m_textureResource = TextureImporter::LoadTexture2D(TEXTURE_PATH_FOX, {}, this);
    }

    Ref<Texture2D> Renderer::UploadTexture(const TextureData& cpuData)
    {
        // A complete mip chain of one 2D layer (every cooked texture) is plain copies and goes to the transfer queue.
        // Mips generated here need blits, and arrays/cubes a different staging layout: those keep the graphics path.
        const bool copyOnly = cpuData.ArrayLayers == 1 && !cpuData.IsCubeMap &&
                              (cpuData.MipLevels <= 1 || cpuData.MipOffsets.size() == cpuData.MipLevels);
        if (copyOnly)
        {
            const TextureUpload upload = *BeginTextureUpload(cpuData, 0, std::max(cpuData.MipLevels, 1u), cpuData.Data.Size, true);
            memcpy(upload.Staging.data, cpuData.Data.Data, cpuData.Data.Size);
            EndTextureUpload(upload, true);
            PublishTexture(*upload.Texture);
            return upload.Texture;
        }

        Ref<Texture2D> textureResource = m_device->createTexture(NRI::TextureDesc
            {
                .width = static_cast<uint32_t>(cpuData.Width),
                .height = static_cast<uint32_t>(cpuData.Height),
                .arrayLayers = cpuData.ArrayLayers,
                .isCubeMap = cpuData.IsCubeMap,
                .mipLevels = cpuData.MipLevels,
                .sampleCount = 1,
                .usage = cpuData.Usage,
                .format = cpuData.Format,
                .directFormat = cpuData.DirectFormat
            });

        std::unique_ptr<NRI::Buffer> stagingBuffer = m_device->createBuffer(NRI::BufferDesc{
            .size = cpuData.Data.Size,
            .usage = NRI::BufferUsage::Staging
        });
        void* data = stagingBuffer->map(0, cpuData.Data.Size);
        memcpy(data, cpuData.Data.Data, cpuData.Data.Size);
        stagingBuffer->unmap();

        std::unique_ptr<NRI::CommandBuffer> commandBuffer = beginSingleTimeCommands();
        textureResource->uploadFromBuffer(*commandBuffer, *stagingBuffer, cpuData.Width, cpuData.Height, cpuData.MipLevels, cpuData.MipOffsets);
        endSingleTimeCommands(std::move(commandBuffer));

        PublishTexture(*textureResource);
        return textureResource;
    }

    std::optional<TextureUpload> Renderer::BeginTextureUpload(const TextureData& texture, uint32_t firstMip, uint32_t uploadMipCount, uint64_t dataSize, bool wait)
    {
        NOX_PROFILE_SCOPE("Begin Texture Upload");
        const uint32_t mipLevels = std::max(texture.MipLevels, 1u);
        const std::vector<size_t> mipOffsets = texture.MipOffsets.empty() ? std::vector<size_t>{ 0 } : texture.MipOffsets;

        TextureUpload upload;
        upload.FirstMip = firstMip;
        if (uploadMipCount > 0)
        {
            // All mips: the whole block, in whatever order it holds them. A run of mips (streaming): one range, as cooked
            // textures store mips largest first.
            const uint32_t endMip = firstMip + uploadMipCount;
            const bool allMips = firstMip == 0 && endMip >= mipLevels;
            const uint64_t begin = allMips ? 0 : mipOffsets[firstMip];
            const uint64_t size = (allMips || endMip >= mipLevels ? dataSize : mipOffsets[endMip]) - begin;
            std::optional<StagingSpan> staging = wait ? m_uploads.ReserveStaging(size) : m_uploads.TryReserveStaging(size);
            if (!staging)
                return std::nullopt;
            upload.Staging = *staging;
            for (uint32_t mip = firstMip; mip < endMip; ++mip)
                upload.MipOffsets.push_back(mipOffsets[mip] - begin);
        }

        upload.Texture = m_device->createTexture(NRI::TextureDesc
            {
                .width = std::max(texture.Width >> firstMip, 1u),
                .height = std::max(texture.Height >> firstMip, 1u),
                .arrayLayers = 1,
                .mipLevels = mipLevels - firstMip,
                .sampleCount = 1,
                .usage = texture.Usage,
                .format = texture.Format,
                .directFormat = texture.DirectFormat,
                .sharedAcrossQueues = true
            });
        return upload;
    }

    uint64_t Renderer::EndTextureUpload(const TextureUpload& upload, bool nextFrameReads, NRI::Texture2D* keptFrom, uint32_t keptFromMip, uint32_t keptMipCount)
    {
        if (upload.MipOffsets.empty())
            m_uploads.InitializeTexture(*upload.Texture);
        else
            m_uploads.CopyToTexture(upload.Staging, *upload.Texture, upload.MipOffsets);
        if (keptFrom)
            m_uploads.CopyTextureMips(*keptFrom, keptFromMip, *upload.Texture, static_cast<uint32_t>(upload.MipOffsets.size()), keptMipCount);
        return upload.MipOffsets.empty() ? m_uploads.CommitCopies(nextFrameReads) : m_uploads.Commit(upload.Staging, nextFrameReads);
    }

    void Renderer::PublishTexture(Texture2D& texture)
    {
        m_resourceHeap->registerTexture(texture);
        uniformData.imageHeapIndexOffset = m_resourceHeap->getImageHeapIndexOffset();
    }

    void Renderer::MarkTexturesLoaded()
    {
        if (s_Instance)
            s_Instance->m_gpuMaterialsDirty = true;
    }

    Ref<Texture2D> Renderer::createSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        std::array<uint8_t, 4> pixel = {r, g, b, a};

        TextureData cpuData{};
        cpuData.Width = 1;
        cpuData.Height = 1;
        cpuData.MipLevels = 1;
        cpuData.Format = NRI::ImageFormat::SRGBA8;
        cpuData.DirectFormat = UINT32_MAX;
        cpuData.Data.Data = pixel.data();
        cpuData.Data.Size = pixel.size();
        cpuData.MipOffsets = {0};

        return UploadTexture(cpuData);
    }

    void Renderer::initPBR()
    {
        // 1. Load temporary 2D HDR panorama
        Ref<Texture2D> enviromentHDR = TextureImporter::LoadTexture2D("assets/enviroments/papermill/khronos_papermill.hdr", {}, this);

        // 2. Create the permanent Cubemap Texture (512x512 per face, 6 array layers)
        constexpr uint32_t cubemapSize = 512;
        uint32_t cubemapNumMips = static_cast<uint32_t>(floor(log2(cubemapSize))) + 1;

        m_environmentCubemap = m_device->createTexture(NRI::TextureDesc{
            .width = cubemapSize,
            .height = cubemapSize,
            .arrayLayers = 6,
            .isCubeMap = true,
            .mipLevels = cubemapNumMips, // Can be increased if generating specular mips later
            .sampleCount = 1,
            .usage = NRI::TextureUsage::Storage, // Allows compute shader writing
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT
        });

        // 3. Register slot X as eStorageImage (writes Storage Descriptor to memory)
        m_resourceHeap->registerTexture(*m_environmentCubemap, NRI::TextureUsage::Storage);

        // 4. Create local Equirect-to-Cubemap compute pipeline
        NRI::PipelineDesc computeDesc{};
        computeDesc.type = NRI::PipelineType::Compute;
        computeDesc.shaders.push_back({
            .stage = NRI::ShaderStage::Compute,
            .entryPoint = "compMain",
            .sourcePath = "assets/shaders/EquirectToCubemap.slang"
        });
        std::unique_ptr<NRI::Pipeline> equirectPipeline = m_device->createPipeline(computeDesc, *m_shaderCompiler);

        // 5. Structure for Push Constants to pass descriptor slots to Slang
        struct EquirectPushConstants
        {
            uint32_t hdrTextureIndex;
            uint32_t cubemapStorageIndex;
            uint32_t cubemapSize;
        } pushData;

        pushData.hdrTextureIndex = enviromentHDR->GetDescriptorIndexSlot();
        pushData.cubemapStorageIndex = m_environmentCubemap->GetDescriptorIndexSlot();
        pushData.cubemapSize = cubemapSize;

        // 6. Record and dispatch the compute work

        std::unique_ptr<NRI::CommandBuffer> cmd = beginSingleTimeCommands();

        // Bind global descriptor heaps
        cmd->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());

        // Bind compute pipeline & push constant parameters
        cmd->bindPipeline(NRI::PipelineBindPoint::Compute, *equirectPipeline);
        cmd->pushData(&pushData, sizeof(EquirectPushConstants));

        // Calculate thread group counts (16x16 threads per group in compute shader)
        uint32_t groupCountX = (cubemapSize + 15) / 16;
        uint32_t groupCountY = (cubemapSize + 15) / 16;

        // Dispatch work: X and Y cover the resolution, Z=6 covers all 6 cubemap faces
        cmd->dispatch(groupCountX, groupCountY, 6);

        // Submit command buffer and wait for execution to complete
        endSingleTimeCommands(std::move(cmd));

        {
            std::unique_ptr<NRI::CommandBuffer> mipCmd = beginSingleTimeCommands();
            m_environmentCubemap->generateMipmaps(*mipCmd);
            endSingleTimeCommands(std::move(mipCmd));
        }

        // 7. Overwrite descriptor slot in heap with Sampled Image Descriptor
        m_resourceHeap->registerTexture(*m_environmentCubemap, NRI::TextureUsage::ShaderResource);

        // equiRectPipeline and enviromentHDR cleanly go out of scope and release temporary resources

        // ==========================================
        //  DIFFUSE IRRADIANCE CONVOLUTION
        // ==========================================

        constexpr uint32_t irradianceSize = 64; // dont forget to change sampler to hardcoded maxlod
        uint32_t irradianceNumMips = static_cast<uint32_t>(floor(log2(irradianceSize))) + 1;
        m_irradianceCubemap = m_device->createTexture(NRI::TextureDesc{
            .width = irradianceSize,
            .height = irradianceSize,
            .arrayLayers = 6,
            .isCubeMap = true,
            .mipLevels = irradianceNumMips,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::Storage,
            .format = NRI::ImageFormat::R32G32B32A32_SFLOAT
        });

        m_resourceHeap->registerTexture(*m_irradianceCubemap, NRI::TextureUsage::Storage);

        NRI::PipelineDesc convComputeDesc{};
        convComputeDesc.type = NRI::PipelineType::Compute;
        convComputeDesc.shaders.push_back({
            .stage = NRI::ShaderStage::Compute,
            .entryPoint = "compMain",
            .sourcePath = "assets/shaders/IrradianceConvolution.slang"
        });
        std::unique_ptr<NRI::Pipeline> irradiancePipeline = m_device->createPipeline(convComputeDesc, *m_shaderCompiler);

        pushData.hdrTextureIndex = m_environmentCubemap->GetDescriptorIndexSlot();
        pushData.cubemapStorageIndex = m_irradianceCubemap->GetDescriptorIndexSlot();
        pushData.cubemapSize = irradianceSize;

        std::unique_ptr<NRI::CommandBuffer> irradCmd = beginSingleTimeCommands();
        irradCmd->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());

        irradCmd->bindPipeline(NRI::PipelineBindPoint::Compute, *irradiancePipeline);

        std::vector<uint32_t> tempIrradMipSlots;
        // Loop through each mip level to generate all mips (just like Sascha!)
        for (uint32_t mip = 0; mip < irradianceNumMips; ++mip)
        {
            uint32_t mipSize = irradianceSize >> mip;

            // Target specifically this mip level with a spec-compliant storage descriptor
            uint32_t mipStorageSlot = m_resourceHeap->registerStorageTextureMip(*m_irradianceCubemap, mip);
            tempIrradMipSlots.push_back(mipStorageSlot);

            pushData.hdrTextureIndex = m_environmentCubemap->GetDescriptorIndexSlot();
            pushData.cubemapStorageIndex = mipStorageSlot;
            pushData.cubemapSize = mipSize;

            irradCmd->pushData(&pushData, sizeof(EquirectPushConstants));

            uint32_t irradGroupCountX = (mipSize + 15) / 16;
            uint32_t irradGroupCountY = (mipSize + 15) / 16;
            irradCmd->dispatch(irradGroupCountX, irradGroupCountY, 6);
        }

        endSingleTimeCommands(std::move(irradCmd));

        // Free the temporary per-mip storage descriptor slots
        for (uint32_t slot : tempIrradMipSlots)
        {
            m_resourceHeap->unregisterTexture(slot);
        }

        m_resourceHeap->registerTexture(*m_irradianceCubemap, NRI::TextureUsage::ShaderResource);

        // ==========================================
        //  SPECULAR IBL: PRE-FILTERED ENVIRONMENT MAP
        // ==========================================
        constexpr uint32_t prefilteredSize = 512; // dont forget to change sampler to hardcoded maxlod
        prefilterCubeMipLevels = static_cast<uint32_t>(floor(log2(prefilteredSize))) + 1;

        m_prefilteredEnvMap = m_device->createTexture(NRI::TextureDesc{
            .width = prefilteredSize,
            .height = prefilteredSize,
            .arrayLayers = 6,
            .isCubeMap = true,
            .mipLevels = prefilterCubeMipLevels,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::Storage,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT
        });

        m_resourceHeap->registerTexture(*m_prefilteredEnvMap, NRI::TextureUsage::Storage);

        NRI::PipelineDesc prefilterComputeDesc{};
        prefilterComputeDesc.type = NRI::PipelineType::Compute;
        prefilterComputeDesc.shaders.push_back({
            .stage = NRI::ShaderStage::Compute,
            .entryPoint = "compMain",
            .sourcePath = "assets/shaders/PrefilterEnv.slang"
        });
        std::unique_ptr<NRI::Pipeline> prefilterPipeline = m_device->createPipeline(prefilterComputeDesc, *m_shaderCompiler);

        struct PrefilterPushConstants
        {
            uint32_t envTextureIndex;
            uint32_t cubemapStorageIndex;
            uint32_t cubemapSize;
            float roughness;
        } prefilterData;

        std::vector<uint32_t> tempMipSlots;
        // Loop through each roughness mip level, one submission each: all of them in one ran for seconds under a capture
        // tool, past the 2 s GPU timeout (device lost).
        for (uint32_t mip = 0; mip < prefilterCubeMipLevels; ++mip)
        {
            std::unique_ptr<NRI::CommandBuffer> prefCmd = beginSingleTimeCommands();
            prefCmd->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
            prefCmd->bindPipeline(NRI::PipelineBindPoint::Compute, *prefilterPipeline);

            uint32_t mipWidth = prefilteredSize >> mip;
            uint32_t mipHeight = prefilteredSize >> mip;
            float roughness = (float)mip / (float)(prefilterCubeMipLevels - 1);

            // Register a descriptor pointing directly to this mip level
            uint32_t mipStorageSlot = m_resourceHeap->registerStorageTextureMip(*m_prefilteredEnvMap, mip);
            tempMipSlots.push_back(mipStorageSlot);

            prefilterData.envTextureIndex = m_environmentCubemap->GetDescriptorIndexSlot();
            prefilterData.cubemapStorageIndex = mipStorageSlot; // Note: Needs slice/mip targeting in shader or descriptor binding if multi-mip storage
            prefilterData.cubemapSize = mipWidth;
            prefilterData.roughness = roughness;

            prefCmd->pushData(&prefilterData, sizeof(PrefilterPushConstants));

            uint32_t groupX = (mipWidth + 15) / 16;
            uint32_t groupY = (mipHeight + 15) / 16;
            prefCmd->dispatch(groupX, groupY, 6);
            endSingleTimeCommands(std::move(prefCmd));
        }

        // Free the temporary per-mip storage descriptor slots
        for (uint32_t slot : tempMipSlots)
        {
            m_resourceHeap->unregisterTexture(slot);
        }
        m_resourceHeap->registerTexture(*m_prefilteredEnvMap, NRI::TextureUsage::ShaderResource);

        // ==========================================
        //  SPECULAR IBL: BRDF INTEGRATION MAP (2D LUT)
        // ==========================================
        constexpr uint32_t brdfLUTSize = 512;

        m_brdfLUT = m_device->createTexture(NRI::TextureDesc{
            .width = brdfLUTSize,
            .height = brdfLUTSize,
            .arrayLayers = 1,
            .isCubeMap = false,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::Storage,
            .format = NRI::ImageFormat::R16G16_SFLOAT
        });

        m_resourceHeap->registerTexture(*m_brdfLUT, NRI::TextureUsage::Storage);

        NRI::PipelineDesc brdfComputeDesc{};
        brdfComputeDesc.type = NRI::PipelineType::Compute;
        brdfComputeDesc.shaders.push_back({
            .stage = NRI::ShaderStage::Compute,
            .entryPoint = "compMain",
            .sourcePath = "assets/shaders/BRDFIntegration.slang"
        });
        std::unique_ptr<NRI::Pipeline> brdfPipeline = m_device->createPipeline(brdfComputeDesc, *m_shaderCompiler);

        struct BRDFPushConstants
        {
            uint32_t lutStorageIndex;
            uint32_t lutSize;
        } brdfData;

        brdfData.lutStorageIndex = m_brdfLUT->GetDescriptorIndexSlot();
        brdfData.lutSize = brdfLUTSize;

        std::unique_ptr<NRI::CommandBuffer> brdfCmd = beginSingleTimeCommands();
        brdfCmd->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());

        brdfCmd->bindPipeline(NRI::PipelineBindPoint::Compute, *brdfPipeline);
        brdfCmd->pushData(&brdfData, sizeof(BRDFPushConstants));

        uint32_t brdfGroupX = (brdfLUTSize + 15) / 16;
        uint32_t brdfGroupY = (brdfLUTSize + 15) / 16;
        brdfCmd->dispatch(brdfGroupX, brdfGroupY, 1);

        endSingleTimeCommands(std::move(brdfCmd));
        m_resourceHeap->registerTexture(*m_brdfLUT, NRI::TextureUsage::ShaderResource);
    }

    template <typename T>
    void Renderer::UploadBufferSlice(NRI::Buffer& dstBuffer, const T* data, uint32_t elementOffset, uint32_t elementCount)
    {
        if (elementCount == 0) return;

        const uint64_t bytes = sizeof(T) * uint64_t(elementCount);
        const StagingSpan staging = m_uploads.ReserveStaging(bytes);
        memcpy(staging.data, data, bytes);
        m_uploads.CopyToBuffer(staging, 0, bytes, dstBuffer, sizeof(T) * uint64_t(elementOffset));
        m_uploads.Commit(staging, true);
    }

    void Renderer::initGeometryBuffers()
    {
        constexpr uint32_t INITIAL_VERTICES = 100'000;
        constexpr uint32_t INITIAL_DRAWS = 50'000;
        constexpr uint32_t INITIAL_VERTS = 500'000;
        constexpr uint32_t INITIAL_TRIS = 1'500'000;

        // Each stream starts at the size a page used to have and grows by copying itself; the ceiling is the element
        // space its allocator hands offsets out of, so it is set well above what a scene is expected to need.
        constexpr uint32_t MAX_ELEMENTS = 1u << 28;
        m_vertexStream.Init(*m_device, sizeof(shaderio::Vertex), INITIAL_VERTICES, MAX_ELEMENTS);
        m_meshletDrawStream.Init(*m_device, sizeof(shaderio::MeshletDraw), INITIAL_DRAWS, MAX_ELEMENTS);
        m_meshletBoundsStream.Init(*m_device, sizeof(shaderio::MeshletBounds), INITIAL_DRAWS, MAX_ELEMENTS);
        m_meshletVertexStream.Init(*m_device, sizeof(uint32_t), INITIAL_VERTS, MAX_ELEMENTS);
        m_meshletTriangleStream.Init(*m_device, sizeof(uint8_t), INITIAL_TRIS, MAX_ELEMENTS);
        m_rtIndexStream.Init(*m_device, sizeof(uint32_t), INITIAL_TRIS, MAX_ELEMENTS, NRI::BufferUsage::Index);
    }

    GeometryRange Renderer::allocateGeometry(GeometryArena& stream, uint32_t count)
    {
        std::unique_ptr<NRI::Buffer> grownFrom;
        const GeometryRange range = stream.Allocate(count, grownFrom);

        // The stream grew: its content moves to the larger buffer unchanged (offsets stay valid, the address changes).
        if (grownFrom)
        {
            // Ordered after every write already queued to the old buffer and before the new range's writes (same transfer
            // submission). Frames in flight keep reading the old buffer, and the next frame to be submitted waits for
            // this copy, which reads it too: one frame longer than a release made during recording, as a load grows
            // the stream before that frame starts (and counts down) -- without it the buffer died under the copy.
            m_uploads.CopyBuffer(*grownFrom, stream.GetBuffer(), grownFrom->getSize());

            {
                std::scoped_lock lock(m_deferredReleasesMutex);
                m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT + 1, std::move(grownFrom) });
            }
        }
        return range;
    }

    namespace
    {
        // Where each stream's data sits in a submesh's staging (16-byte aligned blocks, in stream order).
        struct MeshStagingLayout
        {
            uint64_t vertices = 0;
            uint64_t draws = 0;
            uint64_t bounds = 0;
            uint64_t meshletVertices = 0;
            uint64_t meshletTriangles = 0;
            uint64_t rtIndices = 0;
            uint64_t size = 0;
        };

        MeshStagingLayout meshStagingLayout(uint64_t vertices, uint64_t draws, uint64_t meshletVertices, uint64_t meshletTriangles, uint64_t rtIndices)
        {
            MeshStagingLayout layout;
            uint64_t cursor = 0;
            auto place = [&cursor](uint64_t bytes)
            {
                const uint64_t offset = cursor;
                cursor = (cursor + bytes + 15) / 16 * 16;
                return offset;
            };
            layout.vertices = place(sizeof(shaderio::Vertex) * vertices);
            layout.draws = place(sizeof(shaderio::MeshletDraw) * draws);
            layout.bounds = place(sizeof(shaderio::MeshletBounds) * draws);
            layout.meshletVertices = place(sizeof(uint32_t) * meshletVertices);
            layout.meshletTriangles = place(sizeof(uint8_t) * meshletTriangles);
            layout.rtIndices = place(sizeof(uint32_t) * rtIndices);
            layout.size = cursor;
            return layout;
        }

        MeshStagingLayout meshStagingLayout(const MeshHandle& handle)
        {
            return meshStagingLayout(handle.vertices.count, handle.meshletDraws.count, handle.meshletVertices.count,
                                     handle.meshletTriangles.count, handle.rtIndices.count);
        }
    }

    MeshHandle Renderer::UploadMeshGeometry(const MeshData& data, bool isOpaque)
    {
        MeshUpload upload = *BeginMeshUpload(data, isOpaque, true);
        WriteMeshUpload(data, upload);
        EndMeshUpload(upload, true);
        return PublishMesh(upload);
    }

    std::optional<MeshUpload> Renderer::BeginMeshUpload(const MeshData& data, bool isOpaque, bool wait)
    {
        NOX_PROFILE_SCOPE("Begin Mesh Upload");
        const uint32_t vertCount = static_cast<uint32_t>(data.Vertices.size());
        const uint32_t drawCount = static_cast<uint32_t>(data.Draws.size());
        const uint32_t meshVertCount = static_cast<uint32_t>(data.MeshletVertices.size());
        const uint32_t meshTriCount = static_cast<uint32_t>(data.MeshletTriangles.size());
        // The flat triangle list rebuilt from the meshlets: BLAS build input and hit shading.
        uint32_t rtIndexCount = 0;
        if (vertCount > 0)
        {
            for (size_t index = 0; index < data.Draws.size(); ++index)
            {
                if (data.Bounds[index].lodLevel == 0)
                    rtIndexCount += data.Draws[index].triangleCount * 3;
            }
        }

        MeshUpload upload;
        upload.IsOpaque = isOpaque;
        const MeshStagingLayout layout = meshStagingLayout(vertCount, drawCount, meshVertCount, meshTriCount, rtIndexCount);
        if (layout.size > 0)
        {
            std::optional<StagingSpan> staging = wait ? m_uploads.ReserveStaging(layout.size) : m_uploads.TryReserveStaging(layout.size);
            if (!staging)
                return std::nullopt;
            upload.Staging = *staging;
        }

        // One range per stream (a stream grows first when the allocation no longer fits)
        upload.Handle.vertices = allocateGeometry(m_vertexStream, vertCount);
        upload.Handle.meshletDraws = allocateGeometry(m_meshletDrawStream, drawCount);
        upload.Handle.meshletBounds = allocateGeometry(m_meshletBoundsStream, drawCount);
        upload.Handle.meshletVertices = allocateGeometry(m_meshletVertexStream, meshVertCount);
        upload.Handle.meshletTriangles = allocateGeometry(m_meshletTriangleStream, meshTriCount);
        upload.Handle.rtIndices = allocateGeometry(m_rtIndexStream, rtIndexCount);
        return upload;
    }

    void Renderer::WriteMeshUpload(const MeshData& data, MeshUpload& upload)
    {
        const MeshHandle& handle = upload.Handle;
        const MeshStagingLayout layout = meshStagingLayout(handle);
        uint8_t* staging = upload.Staging.data;

        // Staging memory is written front to back and never read (it may be write-combined). Exactly the ranges'
        // counts: a block holds no more, and the bounds are one per draw.
        memcpy(staging + layout.vertices, data.Vertices.data(), sizeof(shaderio::Vertex) * handle.vertices.count);
        for (size_t index = 0; index < handle.meshletDraws.count; ++index)
        {
            // Meshlet offsets become stream offsets.
            shaderio::MeshletDraw draw = data.Draws[index];
            draw.vertexOffset += handle.meshletVertices.offset;
            draw.triangleOffset += handle.meshletTriangles.offset;
            draw.globalVertexOffset += handle.vertices.offset;
            memcpy(staging + layout.draws + index * sizeof(draw), &draw, sizeof(draw));
        }
        memcpy(staging + layout.bounds, data.Bounds.data(), sizeof(shaderio::MeshletBounds) * std::min<size_t>(data.Bounds.size(), handle.meshletBounds.count));
        memcpy(staging + layout.meshletVertices, data.MeshletVertices.data(), sizeof(uint32_t) * handle.meshletVertices.count);
        memcpy(staging + layout.meshletTriangles, data.MeshletTriangles.data(), handle.meshletTriangles.count);

        shaderio::GpuMesh& gpuMesh = upload.GpuMesh;
        gpuMesh = {};
        if (handle.rtIndices.IsValid())
        {
            // The original clusters: the other LOD levels cover the same surface.
            uint8_t* indices = staging + layout.rtIndices;
            for (size_t index = 0; index < data.Draws.size(); ++index)
            {
                if (data.Bounds[index].lodLevel != 0)
                    continue;
                const shaderio::MeshletDraw& draw = data.Draws[index];
                for (uint32_t triangle = 0; triangle < draw.triangleCount; ++triangle)
                {
                    const uint32_t triBase = draw.triangleOffset + triangle * 3;
                    const uint32_t corners[3] = {
                        data.MeshletVertices[draw.vertexOffset + data.MeshletTriangles[triBase + 0]],
                        data.MeshletVertices[draw.vertexOffset + data.MeshletTriangles[triBase + 1]],
                        data.MeshletVertices[draw.vertexOffset + data.MeshletTriangles[triBase + 2]]
                    };
                    memcpy(indices, corners, sizeof(corners));
                    indices += sizeof(corners);
                }
            }
        }

        // GPU scene mesh record: instances reference it by slot.
        gpuMesh.drawsOffset = handle.meshletDraws.offset;
        gpuMesh.boundsOffset = handle.meshletBounds.offset;
        gpuMesh.verticesOffset = handle.vertices.offset;
        gpuMesh.indicesOffset = handle.rtIndices.IsValid() ? handle.rtIndices.offset : shaderio::NoGeometryRange;
        gpuMesh.meshletCount = handle.meshletDraws.count;
        for (size_t index = 0; index < data.Draws.size(); ++index)
        {
            if (data.Bounds[index].lodLevel == 0)
                gpuMesh.triangleCount += data.Draws[index].triangleCount;
        }

        // Local bounds of the (bind pose) vertices.
        glm::vec3 boundsMin(std::numeric_limits<float>::max());
        glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
        for (const shaderio::Vertex& vertex : data.Vertices)
        {
            boundsMin = glm::min(boundsMin, vertex.pos);
            boundsMax = glm::max(boundsMax, vertex.pos);
        }
        const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
        float radius = 0.0f;
        for (const shaderio::Vertex& vertex : data.Vertices)
            radius = std::max(radius, glm::distance(center, vertex.pos));
        gpuMesh.boundsSphere = glm::vec4(center, radius);
        gpuMesh.boundsMin = boundsMin;
        gpuMesh.boundsMax = boundsMax;
    }

    uint64_t Renderer::EndMeshUpload(const MeshUpload& upload, bool nextFrameReads)
    {
        if (upload.Staging.size == 0)
            return 0;

        // Into each stream's current buffer: it may have grown since the ranges were allocated.
        const MeshHandle& handle = upload.Handle;
        const MeshStagingLayout layout = meshStagingLayout(handle);
        auto copy = [&](GeometryArena& stream, const GeometryRange& range, uint64_t elementSize, uint64_t stagingOffset)
        {
            if (range.count > 0)
                m_uploads.CopyToBuffer(upload.Staging, stagingOffset, elementSize * range.count, stream.GetBuffer(), elementSize * range.offset);
        };
        copy(m_vertexStream, handle.vertices, sizeof(shaderio::Vertex), layout.vertices);
        copy(m_meshletDrawStream, handle.meshletDraws, sizeof(shaderio::MeshletDraw), layout.draws);
        copy(m_meshletBoundsStream, handle.meshletBounds, sizeof(shaderio::MeshletBounds), layout.bounds);
        copy(m_meshletVertexStream, handle.meshletVertices, sizeof(uint32_t), layout.meshletVertices);
        copy(m_meshletTriangleStream, handle.meshletTriangles, sizeof(uint8_t), layout.meshletTriangles);
        copy(m_rtIndexStream, handle.rtIndices, sizeof(uint32_t), layout.rtIndices);
        return m_uploads.Commit(upload.Staging, nextFrameReads);
    }

    MeshHandle Renderer::PublishMesh(const MeshUpload& upload)
    {
        NOX_PROFILE_SCOPE("Publish Mesh");
        MeshHandle handle = upload.Handle;
        if (!handle.IsValid())
            return handle;

        if (handle.rtIndices.IsValid())
        {
            // The BLAS object now, its build in a frame (addBLASBuildPass).
            const NRI::AccelerationStructureBuildDesc buildDesc = blasBuildDesc({ .vertices = handle.vertices, .indices = handle.rtIndices, .isOpaque = upload.IsOpaque });
            const NRI::AccelerationStructureBuildSizes buildSizes = m_device->getAccelerationStructureBuildSizes(buildDesc);

            std::unique_ptr<NRI::Buffer> asBuffer = m_device->createBuffer(NRI::BufferDesc{
                .size = buildSizes.accelerationStructureSize,
                .usage = NRI::BufferUsage::AccelerationStructure
            });
            std::unique_ptr<NRI::AccelerationStructure> blas = m_device->createAccelerationStructure(NRI::AccelerationStructureDesc{
                .type = NRI::AccelerationStructureType::BottomLevel,
                .storageBuffer = asBuffer.get(),
                .bufferOffset = 0,
                .size = buildSizes.accelerationStructureSize
            });

            // Reusing ids released by unloaded meshes
            MeshBLAS meshBLAS{
                .storageBuffer = std::move(asBuffer),
                .as = std::move(blas),
                .serial = ++m_blasSerial
            };
            if (!m_freeBLASIds.empty())
            {
                handle.blasId = m_freeBLASIds.back();
                m_freeBLASIds.pop_back();
                m_meshBLASes[handle.blasId] = std::move(meshBLAS);
            }
            else
            {
                handle.blasId = static_cast<uint32_t>(m_meshBLASes.size());
                m_meshBLASes.push_back(std::move(meshBLAS));
            }
        }

        // Not ray traced until its BLAS is built in a frame (SetMeshBlas).
        handle.gpuSceneMesh = m_gpuScene.AddMesh(upload.GpuMesh, 0);
        if (handle.blasId != UINT32_MAX)
            m_blasBuilds.push_back({ handle.blasId, handle.gpuSceneMesh, handle.vertices, handle.rtIndices, upload.IsOpaque });
        return handle;
    }

    void Renderer::deferAssetRelease(Ref<Asset> asset)
    {
        std::scoped_lock lock(m_deferredReleasesMutex);
        m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(asset) });
    }

    NRI::AccelerationStructureBuildDesc Renderer::blasBuildDesc(const BlasBuild& build) const
    {
        return NRI::AccelerationStructureBuildDesc{
            .type = NRI::AccelerationStructureType::BottomLevel,
            .flags = NRI::AccelerationStructureBuildFlags::PreferFastTrace | NRI::AccelerationStructureBuildFlags::AllowCompaction,
            .triangles = {
                NRI::AccelerationStructureTrianglesDesc{
                    .vertexBufferAddress = m_vertexStream.GetDeviceAddress() + sizeof(shaderio::Vertex) * uint64_t(build.vertices.offset),
                    .vertexStride = sizeof(shaderio::Vertex),
                    .maxVertex = build.vertices.count - 1,
                    .indexBufferAddress = m_rtIndexStream.GetDeviceAddress() + sizeof(uint32_t) * uint64_t(build.indices.offset),
                    .primitiveCount = build.indices.count / 3,
                    .primitiveOffset = 0,
                    .firstVertex = 0,
                    .isOpaque = build.isOpaque
                }
            }
        };
    }

    void Renderer::UnloadMeshGeometry(const MeshHandle& handle)
    {
        if (!handle.IsValid()) return;

        std::erase_if(m_blasBuilds, [&](const BlasBuild& build) { return build.blasId == handle.blasId; });

        // Instances of this mesh stop drawing now; their entities register again (with the reloaded mesh).
        if (handle.gpuSceneMesh != GpuScene::InvalidSlot)
        {
            std::vector<uint32_t> deactivated;
            m_gpuScene.RemoveMesh(handle.gpuSceneMesh, deactivated);
            for (uint32_t instance : deactivated)
                m_invalidatedMeshEntities.push_back(m_gpuScene.GetInstanceEntity(instance));
        }

        // Defer returning the ranges so frames in flight finish reading them
        {
            std::scoped_lock lock(m_deferredReleasesMutex);
            m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, handle });
        }

    }

    void Renderer::updateBoneBuffer(uint32_t currentImage)
    {
        if (m_boneMatrices.empty())
            return;

        uint64_t requiredBoneSize = sizeof(glm::mat4) * m_boneMatrices.size();
        if (requiredBoneSize > m_BoneBufferCapacity)
        {
            m_BoneBufferCapacity = requiredBoneSize * 2;

            for (auto& oldBuffer : m_boneBuffers)
            {
                if (oldBuffer)
                {
                    {
                        std::scoped_lock lock(m_deferredReleasesMutex);
                        m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(oldBuffer) });
                    }
                }
            }

            createBoneBuffer(m_BoneBufferCapacity);
        }

        memcpy(m_boneBuffersMapped[currentImage], m_boneMatrices.data(), requiredBoneSize);
    }

    void Renderer::createBoneBuffer(uint64_t size)
    {
        m_boneBuffers.clear();
        m_boneBuffersMapped.clear();

        // Reserve memory in vectors to prevent reallocation overhead
        m_boneBuffers.reserve(MAX_FRAMES_IN_FLIGHT);
        m_boneBuffersMapped.reserve(MAX_FRAMES_IN_FLIGHT);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            std::unique_ptr<NRI::Buffer> uboBuffer = m_device->createBuffer(NRI::BufferDesc
                {
                    .size = size,
                    .usage = NRI::BufferUsage::Storage
                });

            void* mappedMemory = uboBuffer->map(0, size);

            m_boneBuffers.emplace_back(std::move(uboBuffer));
            m_boneBuffersMapped.emplace_back(mappedMemory);
        }
    }

    void Renderer::createUniformBuffers()
    {
        uint64_t bufferSize = sizeof(shaderio::UniformBufferObject);
        // Reserve memory in vectors to prevent reallocation overhead
        m_uniformBuffers.reserve(MAX_FRAMES_IN_FLIGHT);
        m_uniformBuffersMapped.reserve(MAX_FRAMES_IN_FLIGHT);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            std::unique_ptr<NRI::Buffer> uboBuffer = m_device->createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::Uniform
                });

            void* mappedMemory = uboBuffer->map(0, bufferSize);

            m_uniformBuffers.emplace_back(std::move(uboBuffer));
            m_uniformBuffersMapped.emplace_back(mappedMemory);
        }
    }

    void Renderer::createDrawListBuffers(uint64_t bufferSize)
    {
        m_drawListBuffers.clear();
        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            m_drawListBuffers.emplace_back(m_device->createBuffer(NRI::BufferDesc{
                .size = bufferSize,
                .usage = NRI::BufferUsage::Storage
            }));
        }
        m_drawListStale.fill(true);
    }

    void Renderer::createSelectedEntityIDBuffers()
    {
        constexpr uint32_t maxSelectedEntities = 4096;

        uint64_t bufferSize =
            maxSelectedEntities * sizeof(int32_t);

        m_selectedEntityIDBuffers.reserve(MAX_FRAMES_IN_FLIGHT);
        m_selectedEntityIDBuffersMapped.reserve(MAX_FRAMES_IN_FLIGHT);

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            auto buffer = m_device->createBuffer(
                NRI::BufferDesc{
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::Storage
                }
            );

            void* mapped = buffer->map(0, bufferSize);

            m_selectedEntityIDBuffers.emplace_back(std::move(buffer));
            m_selectedEntityIDBuffersMapped.emplace_back(mapped);
        }
    }

    void Renderer::createLightBuffer(uint64_t bufferSize)
    {
        m_lightBuffers.clear();
        m_lightBuffersMapped.clear();
        m_lightBuffers.reserve(MAX_FRAMES_IN_FLIGHT);
        m_lightBuffersMapped.reserve(MAX_FRAMES_IN_FLIGHT);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            std::unique_ptr<NRI::Buffer> lightBuffer = m_device->createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::Storage
                });

            void* mappedMemory = lightBuffer->map(0, bufferSize);
            m_lightBuffers.emplace_back(std::move(lightBuffer));
            m_lightBuffersMapped.emplace_back(mappedMemory);
        }
    }

    void Renderer::createDescriptorHeaps()
    {
        /*// Create sampler skybox sampelr from sascha williams ?????????
                VkSamplerCreateInfo samplerCreateInfo{};
                samplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                samplerCreateInfo.magFilter = VK_FILTER_LINEAR;
                samplerCreateInfo.minFilter = VK_FILTER_LINEAR;
                samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                samplerCreateInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                samplerCreateInfo.addressModeV = samplerCreateInfo.addressModeU;
                samplerCreateInfo.addressModeW = samplerCreateInfo.addressModeU;
                samplerCreateInfo.mipLodBias = 0.0f;
                samplerCreateInfo.maxAnisotropy = device->enabledFeatures.samplerAnisotropy ? device->properties.limits.maxSamplerAnisotropy : 1.0f;
                samplerCreateInfo.anisotropyEnable = device->enabledFeatures.samplerAnisotropy;
                samplerCreateInfo.compareOp = VK_COMPARE_OP_NEVER;
                samplerCreateInfo.minLod = 0.0f;
                samplerCreateInfo.maxLod = (float)mipLevels;
                samplerCreateInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
                VK_CHECK_RESULT(vkCreateSampler(device->logicalDevice, &samplerCreateInfo, nullptr, &sampler));*/
        std::vector<NRI::SamplerDesc> samplers(shaderio::SamplerCount);

        // SAMPLER_LINEAR_REPEAT = 0
        samplers[shaderio::SAMPLER_LINEAR_REPEAT] = NRI::SamplerDesc
        {
            .magFilter = NRI::Filter::Linear,
            .minFilter = NRI::Filter::Linear,
            .mipmapMode = NRI::SamplerMipmapMode::Linear,
            .addressModeU = NRI::SamplerAddressMode::Repeat,
            .addressModeV = NRI::SamplerAddressMode::Repeat,
            .addressModeW = NRI::SamplerAddressMode::Repeat,
            .mipLodBias = 0.0f,
            .anisotropyEnable = true,
            .maxAnisotropy = 16.0f,
            .compareEnable = false,
            .compareOp = NRI::CompareOp::Never,
            .minLod = 0.0f,
            .maxLod = 1000.0f
        };

        samplers[shaderio::SAMPLER_NEAREST_REPEAT] = NRI::SamplerDesc
        {
            .magFilter = NRI::Filter::Nearest,
            .minFilter = NRI::Filter::Nearest,
            .mipmapMode = NRI::SamplerMipmapMode::Nearest,
            .addressModeU = NRI::SamplerAddressMode::Repeat,
            .addressModeV = NRI::SamplerAddressMode::Repeat,
            .addressModeW = NRI::SamplerAddressMode::Repeat,
            .mipLodBias = 0.0f,
            .anisotropyEnable = true,
            .maxAnisotropy = 1.0f,
            .compareEnable = true,
            .compareOp = NRI::CompareOp::Always,
            .minLod = 0.0f,
            .maxLod = 1000.0f
        };

        samplers[shaderio::SAMPLER_BRDFLUT] = NRI::SamplerDesc
        {
            .magFilter = NRI::Filter::Linear,
            .minFilter = NRI::Filter::Linear,
            .mipmapMode = NRI::SamplerMipmapMode::Nearest,
            .addressModeU = NRI::SamplerAddressMode::ClampToEdge,
            .addressModeV = NRI::SamplerAddressMode::ClampToEdge,
            .addressModeW = NRI::SamplerAddressMode::ClampToEdge,
            .mipLodBias = 0.0f,
            .maxAnisotropy = 1.0f,
            .minLod = 0.0f,
            .maxLod = 1.0f,
            .borderColor = NRI::BorderColor::FloatOpaqueWhite
        };

        samplers[shaderio::SAMPLER_IRRADIANCE] = NRI::SamplerDesc
        {
            .magFilter = NRI::Filter::Linear,
            .minFilter = NRI::Filter::Linear,
            .mipmapMode = NRI::SamplerMipmapMode::Linear,
            .addressModeU = NRI::SamplerAddressMode::ClampToEdge,
            .addressModeV = NRI::SamplerAddressMode::ClampToEdge,
            .addressModeW = NRI::SamplerAddressMode::ClampToEdge,
            .mipLodBias = 0.0f,
            .maxAnisotropy = 1.0f,
            .minLod = 0.0f,
            .maxLod = static_cast<float>(static_cast<uint32_t>(floor(log2(64))) + 1),
            .borderColor = NRI::BorderColor::FloatOpaqueWhite
        };

        samplers[shaderio::SAMPLER_PREFILTER] = NRI::SamplerDesc
        {
            .magFilter = NRI::Filter::Linear,
            .minFilter = NRI::Filter::Linear,
            .mipmapMode = NRI::SamplerMipmapMode::Linear,
            .addressModeU = NRI::SamplerAddressMode::ClampToEdge,
            .addressModeV = NRI::SamplerAddressMode::ClampToEdge,
            .addressModeW = NRI::SamplerAddressMode::ClampToEdge,
            .mipLodBias = 0.0f,
            .maxAnisotropy = 1.0f,
            .minLod = 0.0f,
            .maxLod = static_cast<float>(static_cast<uint32_t>(floor(log2(512))) + 1),
            .borderColor = NRI::BorderColor::FloatOpaqueWhite
        };

        samplers[shaderio::SAMPLER_LINEAR_CLAMP] = NRI::SamplerDesc
        {
            .magFilter = NRI::Filter::Linear,
            .minFilter = NRI::Filter::Linear,
            .mipmapMode = NRI::SamplerMipmapMode::Linear,
            .addressModeU = NRI::SamplerAddressMode::ClampToEdge,
            .addressModeV = NRI::SamplerAddressMode::ClampToEdge,
            .addressModeW = NRI::SamplerAddressMode::ClampToEdge,
            .mipLodBias = 0.0f,
            .maxAnisotropy = 1.0f,
            .compareEnable = false,
            .compareOp = NRI::CompareOp::Never,
            .minLod = 0.0f,
            .maxLod = 1000.0f,
            .borderColor = NRI::BorderColor::FloatOpaqueWhite
        };

        // todo: imgui needs more space to if you want to register it
        // currently 1000 in initimgui devicevk.cpp

        //hardcoded samplerinfos inside descriptorheapvk cosntructor
        m_samplerHeap = m_device->createDescriptorHeap(NRI::DescriptorHeapDesc{
            .type = NRI::DescriptorHeapType::Sampler,
            .maxSamplerDescriptors = shaderio::SamplerCount,
            .samplers = std::move(samplers)
        });

        m_resourceHeap = m_device->createDescriptorHeap(NRI::DescriptorHeapDesc{
            .type = NRI::DescriptorHeapType::Resource,
            .maxBufferDescriptors = 128,
            .maxImageDescriptors = 4096 // a streamed texture's old and new image coexist until the old one is released
        });

        /*
        Abstract storage tracking vectors
        std::vector<std::unique_ptr<NRI::Buffer>> m_modelDataBuffers;
        
        m_modelDataBuffers.reserve(2);
        for (uint32_t i = 0; i < 2; i++)
        {
            // Create an abstract Storage Buffer
            std::unique_ptr<NRI::Buffer> modelBuffer = m_device->createBuffer(NRI::BufferDesc{
                .size = sizeof(ModelData),
                .usage = NRI::BufferUsage::Storage
            });
            
            // Map and fill initial CPU data
            void* mappedData = modelBuffer->map(0, sizeof(ModelData));
            const glm::vec4 positions[2] = {glm::vec4(-1.5f, 0.0f, 0.0f, 0.0f), glm::vec4(1.5f, 0.0f, 0.0f, 0.0f)};
            const glm::vec4 colors[2] = {glm::vec4(0.5f, 1.0f, 0.5f, 0.0f), glm::vec4(0.5f, 0.5f, 1.0f, 0.0f)};
            ModelData mdata{.pos = positions[i], .color = colors[i]};
            std::memcpy(mappedData, &mdata, sizeof(ModelData));
            modelBuffer->unmap();
    
            /#1#/ 4. Register the buffer directly to the Resource Heap at runtime!
            uint32_t bufferBindlessIndex = m_resourceHeap->registerBuffer(*modelBuffer, sizeof(ModelData));#1#
            
            // Cache your model buffer in the renderer
            m_modelDataBuffers.emplace_back(std::move(modelBuffer));
        }*/
    }

    std::unique_ptr<NRI::CommandBuffer> Renderer::beginSingleTimeCommands()
    {
        std::unique_ptr<NRI::CommandBuffer> commandBuffer = m_commandAllocator->allocateCommandBuffer(1);
        commandBuffer->begin(0, true);

        return std::move(commandBuffer);
    }

    void Renderer::endSingleTimeCommands(std::unique_ptr<NRI::CommandBuffer>&& commandBuffer)
    {
        commandBuffer->end(0);

        // Startup and tool work that may read what was just uploaded on the transfer queue (IBL precompute reads the
        // environment texture): it already blocks, so it waits for pending uploads first.
        m_uploads.GetTimeline().wait(m_uploads.Flush());
        m_device->submitAndWait(*commandBuffer, 0);
    }

    std::span<NRI::CommandBuffer* const> Renderer::recordFrame(uint32_t imageIndex)
    {
        {
            NOX_PROFILE_SCOPE("Build Frame Graph");
            prepareFrameGraph(imageIndex);

            // Frame order (§5.4.8). Each add* contributes its feature's passes only when the feature runs.
            addGpuSceneUpdatePass();
            addBLASBuildPass();
            addTLASBuildPass();
            addInstanceCullingPass();
            addVisibilityPass();
            addHiZBuildPass();
            addInstanceCullingLatePass();
            addVisibilityLatePass();
            addClusterStatsReadbackPass();
            addGBufferPass();
            addRTShadowPasses();
            addRTReflectionPasses();
            addDDGIPasses();
            addReSTIRGIPasses();
            addPreviousFrameCopyPass();
            addReSTIRDIPasses();
            addReSTIRPTPasses();
            addPathTracerPasses();
            addDeferredLightingPass();
            addForward3DPass();
            addMipFeedbackReadbackPass();
            addDLSSPass();
            addEntityDepthBlitPass();
            addPostProcessPass();
            addOverlay2DPass();
            addOutlinePass();
            addPresentPass();
            addPickReadbackPass();
            addTextureInspection();
        }

        {
            NOX_PROFILE_SCOPE("Compile Frame Graph");
            m_renderGraph.Compile();
            resolveFrameUniforms();
            // Read back only what this frame's inspector actually probes.
            m_inspectionProbePending[frameIndex] = m_frame.inspectionProbe && m_renderGraph.IsInspecting() ? 1 : 0;
        }

        // Every value the GPU reads from the uniforms is final once the passes are set up; the GPU reads the buffer only
        // after submission.
        memcpy(m_uniformBuffersMapped[frameIndex], &uniformData, sizeof(shaderio::UniformBufferObject));

        std::span<NRI::CommandBuffer* const> commandBuffers;
        {
            NOX_PROFILE_SCOPE("Execute Frame Graph");
            commandBuffers = m_renderGraph.Execute(frameIndex);
        }
        NOX_PROFILE_COUNTER("Command Buffers", commandBuffers.size());
        return commandBuffers;
    }

    void Renderer::applyCommandBufferBaseline(NRI::CommandBuffer& cmd) const
    {
        cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());

        // Rasterization (most of these come from VK_EXT_extended_dynamic_state_3).
        cmd.setRasterizerDiscardEnable(false);
        cmd.setPolygonMode(NRI::PolygonMode::Fill);
        cmd.setCullMode(NRI::CullMode::Back);
        cmd.setFrontFace(NRI::FrontFace::CounterClockWise);
        cmd.setDepthBiasEnable(false);
        cmd.setDepthClampEnable(false);
        cmd.setLineWidth(1.0f);

        // Multisampling.
        const uint32_t sampleCount = 1;
        cmd.setRasterizationSamples(sampleCount);
        const uint32_t sampleMask = 0xFFFFFFFF;
        cmd.setSampleMask(sampleCount, sampleMask);
        cmd.setAlphaToCoverageEnable(false);
        // alphaToOne is required by the spec when its device feature is enabled and a
        // shader object is bound, even if we don't actually use it.
        cmd.setAlphaToOneEnableEXT(false);

        // Depth / stencil.
        cmd.setDepthTestEnable(true);
        cmd.setDepthWriteEnable(true);
        cmd.setDepthCompareOp(NRI::CompareOp::Greater);
        cmd.setDepthBoundsTestEnable(false);
        cmd.setStencilTestEnable(false);

        // Color blend (for one color attachment); passes with more targets set theirs.
        const NRI::ColorBlendEquation blendEquation
        {
            .srcColorBlendFactor = NRI::BlendFactor::SrcAlpha,
            .dstColorBlendFactor = NRI::BlendFactor::OneMinusSrcAlpha,
            .colorBlendOp = NRI::BlendOp::Add,
            .srcAlphaBlendFactor = NRI::BlendFactor::SrcAlpha,
            .dstAlphaBlendFactor = NRI::BlendFactor::OneMinusSrcAlpha,
            .alphaBlendOp = NRI::BlendOp::Add,
        };
        uint32_t colorWriteMask = NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A;
        cmd.setColorBlendEnable(0, false);
        cmd.setColorBlendEquation(0, blendEquation);
        cmd.setColorWriteMask(0, colorWriteMask);

        cmd.setLogicOpEnable(false);
    }

    void Renderer::prepareFrameGraph(uint32_t imageIndex)
    {
        RenderGraph& graph = m_renderGraph;

        FrameGraphState& frame = m_frame;
        frame = {};
        frame.imageIndex = imageIndex;
        frame.renderExtent = m_renderSize;
        frame.outputExtent = m_isEditor ? m_viewportSize : m_swapChainExtent;

        graph.Reset({ m_sceneFrameCounter, frame.renderExtent, frame.outputExtent });

        // Shared indirect meshlet drawing state (visibility and forward passes).
        if (!m_drawList.empty())
        {
            // drawInstancesReference: the view's visible instances, a graph buffer (beginMeshletDraws).
            shaderio::PushConstantMeshlets& references = frame.meshletReferences;
            references.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
            bool hasBoneBuffers = frameIndex < m_boneBuffers.size() && m_boneBuffers[frameIndex] != nullptr;
            bool hasBones = !m_boneMatrices.empty();
            references.boneMatrixReference = (hasBones && hasBoneBuffers) ? m_boneBuffers[frameIndex]->getDeviceAddress() : 0;

        }

        // Every hybrid-only feature skips itself while path tracing. ReSTIR DI/GI are the exception when explicitly
        // requested: the plain path tracer can consume their primary-surface lighting buffers (RTXPT-style hybrid).
        frame.runPathTracer = (m_pathTracingEnabled || m_debugMode == 18 || m_debugMode == 19);
        frame.pathTracerUsesRTXDI = frame.runPathTracer && m_pathTracerUsesRTXDI;
        frame.totalDDGIProbes = m_ddgiProbeCountX * m_ddgiProbeCountY * m_ddgiProbeCountZ;

        // "A denoiser was toggled / the resolution changed": every NRD pass of this frame flushes its history.
        frame.resetNRD = graph.WasHistoryReset(NRDHistoryKey);

        // NRD's integration layer requires its internal frame counter to advance by exactly 1 every real frame (see
        // Device::tickNRD); the denoise passes only run when their feature does, so tick unconditionally here.
        if (m_device->isNRDInitialized())
        {
            m_device->tickNRD(static_cast<uint32_t>(m_sceneFrameCounter), m_isFirstFrame || frame.resetNRD,
                uniformData.view, uniformData.nonJitteredProj, uniformData.prevView, uniformData.prevProj);
        }

        auto renderTarget = [](NRI::ImageFormat format)
        {
            RGTextureDesc desc;
            desc.Format = format;
            return desc;
        };
        auto outputTarget = [](NRI::ImageFormat format)
        {
            RGTextureDesc desc;
            desc.Size = RGSize::OutputResolution;
            desc.Format = format;
            return desc;
        };
        RGTextureDesc renderDepth;
        renderDepth.Usage = NRI::TextureUsage::DepthStencilAttachment;
        RGTextureDesc outputDepth = renderDepth;
        outputDepth.Size = RGSize::OutputResolution;

        // Frame-lifetime targets (allocated only when an executed pass uses them). Features that add resources of their
        // own (DDGI, RTXDI reservoirs, path tracer accumulation) create them in their add* functions.
        FrameGraphResources resources;
        resources.Visibility = graph.CreateTexture("Visibility", renderTarget(NRI::ImageFormat::R32G32_UINT));
        resources.Depth = graph.CreateTexture("Depth", renderDepth);
        resources.DepthHi = graph.CreateTexture("Depth (Display)", outputDepth);
        resources.Entity = graph.CreateTexture("Entity IDs", renderTarget(NRI::ImageFormat::R32SINT));
        resources.EntityHi = graph.CreateTexture("Entity IDs (Display)", outputTarget(NRI::ImageFormat::R32SINT));
        resources.GBufferAlbedo = graph.CreateTexture("GBuffer Albedo", renderTarget(NRI::ImageFormat::RGBA8));
        resources.GBufferSpecular = graph.CreateTexture("GBuffer Specular", renderTarget(NRI::ImageFormat::RGBA8));
        resources.GBufferNormal = graph.CreateTexture("GBuffer Normal", renderTarget(NRI::ImageFormat::R16G16B16A16_SFLOAT));
        resources.GBufferMaterial = graph.CreateTexture("GBuffer Material", renderTarget(NRI::ImageFormat::RGBA8));
        resources.GBufferEmission = graph.CreateTexture("GBuffer Emission", renderTarget(NRI::ImageFormat::R16G16B16A16_SFLOAT));
        resources.GBufferVelocity = graph.CreateTexture("GBuffer Velocity", renderTarget(NRI::ImageFormat::R32G32_SFLOAT));
        resources.HDRScene = graph.CreateTexture("HDR Scene", renderTarget(NRI::ImageFormat::R16G16B16A16_SFLOAT));
        resources.DLSSOutput = graph.CreateTexture("DLSS Output", outputTarget(NRI::ImageFormat::R16G16B16A16_SFLOAT));

        resources.RawShadowMask = graph.CreateTexture("Raw Shadow Mask", renderTarget(NRI::ImageFormat::R16G16_SFLOAT));
        resources.DenoisedShadowMask = graph.CreateTexture("Denoised Shadow Mask", renderTarget(NRI::ImageFormat::R16G16_SFLOAT));
        resources.RawReflection = graph.CreateTexture("Raw Reflection", renderTarget(NRI::ImageFormat::R16G16B16A16_SFLOAT));
        resources.DenoisedReflection = graph.CreateTexture("Denoised Reflection", renderTarget(NRI::ImageFormat::R16G16B16A16_SFLOAT));
        // NRD's view-Z and packed normal/roughness guides are written by the RT shadow pass but read by every NRD
        // denoiser, which can run while RT shadows are off. They persist across frames (histories) so those denoisers
        // keep reading the last guides instead of an unwritten texture.
        resources.ViewZ = graph.GetHistoryTexture("NRD View Z", renderTarget(NRI::ImageFormat::R16_SFLOAT), 1).Textures[0];
        resources.NRDNormalRoughness = graph.GetHistoryTexture("NRD Normal Roughness", renderTarget(NRI::ImageFormat::R10G10B10A2_UNORM), 1).Textures[0];

        resources.ReSTIRGIRaw = graph.CreateTexture("ReSTIR GI Diffuse", renderTarget(NRI::ImageFormat::R16G16B16A16_SFLOAT));
        resources.ReSTIRGIDenoised = graph.CreateTexture("ReSTIR GI Denoised", renderTarget(NRI::ImageFormat::R16G16B16A16_SFLOAT));
        resources.ReSTIRDIDirect = graph.CreateTexture("ReSTIR DI Direct", renderTarget(NRI::ImageFormat::R16G16B16A16_SFLOAT));
        resources.ReSTIRDIDenoised = graph.CreateTexture("ReSTIR DI Denoised", renderTarget(NRI::ImageFormat::R16G16B16A16_SFLOAT));
        {
            RGTextureDesc lightPDF;
            lightPDF.Size = RGSize::Absolute;
            lightPDF.Width = m_lightPDFTextureSize;
            lightPDF.Height = m_lightPDFTextureSize;
            lightPDF.Format = NRI::ImageFormat::R16_SFLOAT;
            lightPDF.Usage = NRI::TextureUsage::Storage;
            lightPDF.MipLevels = m_lightPDFMipLevels;
            resources.LightPDF = graph.CreateTexture("Light PDF", lightPDF);
        }

        resources.ReSTIRPTOutput = graph.CreateTexture("ReSTIR PT Output", renderTarget(NRI::ImageFormat::R16G16B16A16_SFLOAT));
        resources.ReSTIRPTPrimaryDirect = graph.CreateTexture("ReSTIR PT Primary Direct", renderTarget(NRI::ImageFormat::R16G16B16A16_SFLOAT));
        {
            // Storage: NRD writes it, and the in-place YCoCg decode (REBLUR) needs a storage slot on it.
            RGTextureDesc denoised = renderTarget(NRI::ImageFormat::R16G16B16A16_SFLOAT);
            denoised.Usage = NRI::TextureUsage::Storage;
            resources.PathTracerDenoised = graph.CreateTexture("Path Tracer Denoised", denoised);
        }

        // Previous-frame G-buffer snapshots (depth/normal for ReSTIR GI/DI temporal validity, albedo/material for ReSTIR
        // PT's RandomReplay / RAB_AreMaterialsSimilar), copied every frame by the Previous Frame Copy pass.
        resources.PrevDepth = graph.GetHistoryTexture("Previous Depth", renderDepth, 1).Textures[0];
        resources.PrevNormal = graph.GetHistoryTexture("Previous Normal", renderTarget(NRI::ImageFormat::R16G16B16A16_SFLOAT), 1).Textures[0];
        resources.PrevAlbedo = graph.GetHistoryTexture("Previous Albedo", renderTarget(NRI::ImageFormat::RGBA8), 1).Textures[0];
        resources.PrevMaterial = graph.GetHistoryTexture("Previous Material", renderTarget(NRI::ImageFormat::RGBA8), 1).Textures[0];

        // Renderer-owned resources.
        resources.Scene = graph.ImportTexture("Scene", m_sceneResource.get());
        if (m_environmentCubemap)
            resources.EnvironmentCubemap = graph.ImportTexture("Environment", m_environmentCubemap.get(), RGImportAccess::ReadOnly);
        if (m_restirGINeighborOffsetsBuffer)
            resources.NeighborOffsets = graph.ImportBuffer("RTXDI Neighbor Offsets", m_restirGINeighborOffsetsBuffer.get(), RGImportAccess::ReadOnly);
        // Depth pyramid of the camera view: written from this frame's depth, read by the next frame's phase 1 (§5.6.4).
        {
            RGTextureDesc hiZ;
            hiZ.Size = RGSize::Absolute;
            hiZ.Width = std::max(frame.renderExtent.width / 2, 1u);
            hiZ.Height = std::max(frame.renderExtent.height / 2, 1u);
            hiZ.Format = NRI::ImageFormat::R32_SFLOAT;
            hiZ.Usage = NRI::TextureUsage::Storage;
            hiZ.MipLevels = 1;
            for (uint32_t size = std::max(hiZ.Width, hiZ.Height); size > 1; size >>= 1)
                ++hiZ.MipLevels;

            const RGTextureHistory history = graph.GetHistoryTexture("Camera Hi-Z", hiZ, 1);
            resources.CameraHiZ = history.Textures[0];
            frame.hiZReset = history.WasReset;
        }

        if (m_tlasBuffer)
            resources.TLAS = graph.ImportBuffer("TLAS", m_tlasBuffer.get());
        // GPU scene tables: written by GPU Scene Update, read through the uniforms by the passes that draw or trace the scene.
        if (NRI::Buffer* buffer = m_gpuScene.GetInstances().GetBuffer())
            resources.SceneInstances = graph.ImportBuffer("Scene Instances", buffer);
        if (NRI::Buffer* buffer = m_gpuScene.GetTransforms().GetBuffer())
            resources.SceneTransforms = graph.ImportBuffer("Scene Transforms", buffer);
        if (NRI::Buffer* buffer = m_gpuScene.GetMaterials().GetBuffer())
            resources.SceneMaterials = graph.ImportBuffer("Scene Materials", buffer);
        if (NRI::Buffer* buffer = m_gpuScene.GetMeshes().GetBuffer())
            resources.SceneMeshes = graph.ImportBuffer("Scene Meshes", buffer);
        if (NRI::Buffer* buffer = m_gpuScene.GetRayTracingInstances().GetBuffer())
            resources.SceneRayTracingInstances = graph.ImportBuffer("Scene Ray Tracing Instances", buffer);
        if (frameIndex < m_pickerStagingBuffers.size() && m_pickerStagingBuffers[frameIndex])
            resources.PickerStaging = graph.ImportBuffer("Picker Staging", m_pickerStagingBuffers[frameIndex].get());
        resources.MipFeedback = graph.ImportBuffer("Mip Feedback", m_mipFeedbackBuffer.get());
        resources.ClusterStats = graph.ImportBuffer("Cluster Stats", m_clusterStatsBuffer.get());

        prepareDDGIFrame(resources);

        graph.GetBlackboard().Add(resources);
    }

    void Renderer::resolveFrameUniforms()
    {
        const RenderGraph& graph = m_renderGraph;
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const FrameGraphState& frame = m_frame;
        constexpr uint32_t NoTexture = 0xFFFFFFFF;

        uniformData.entityTextureIndex = graph.GetSlotOr(resources.EntityHi, 0);
        uniformData.entityGBufferTextureIndex = graph.GetSlotOr(resources.Entity, 0);

        // The atlases this frame's DDGI blend writes (the lighting pass reads them).
        uniformData.ddgiIrradianceTextureIndex = frame.ddgiAdded ? graph.GetSlotOr(resources.DDGIIrradiance[frame.ddgiWriteIndex], NoTexture) : NoTexture;
        uniformData.ddgiDistanceTextureIndex = frame.ddgiAdded ? graph.GetSlotOr(resources.DDGIDistance[frame.ddgiWriteIndex], NoTexture) : NoTexture;

        // ReSTIR GI: the denoised result when its NRD pass runs (NRD evaluation cannot fail once initialized), else raw.
        if (frame.restirGIAdded)
        {
            uniformData.diffuseGIMode = m_diffuseGIMode;
            uniformData.restirGIDiffuseTextureIndex = graph.GetSlotOr(frame.nrdGIAdded ? resources.ReSTIRGIDenoised : resources.ReSTIRGIRaw, NoTexture);
            uniformData.restirGIReservoirBufferIndex = 0;
            uniformData.restirGINeighborOffsetsBufferIndex = 0;
        }
        else if (m_diffuseGIMode == 2)
        {
            // Fallback to IBL ambient if ReSTIR GI cannot run yet (e.g. TLAS build pending)
            uniformData.diffuseGIMode = 0;
            uniformData.restirGIDiffuseTextureIndex = NoTexture;
        }
        else
        {
            uniformData.diffuseGIMode = m_diffuseGIMode;
            uniformData.restirGIDiffuseTextureIndex = NoTexture;
        }

        // ReSTIR DI: same pattern; the brute-force loop when it cannot run yet.
        if (frame.restirDIAdded)
        {
            uniformData.directLightingMode = m_directLightingMode;
            uniformData.restirDIDirectLightingTextureIndex = graph.GetSlotOr(frame.nrdDIAdded ? resources.ReSTIRDIDenoised : resources.ReSTIRDIDirect, NoTexture);
        }
        else
        {
            uniformData.directLightingMode = 0;
            uniformData.restirDIDirectLightingTextureIndex = NoTexture;
        }
    }

    Renderer::MeshletDrawCursor Renderer::beginMeshletDraws(const RGPassContext& context, const ViewDrawResources& draws, bool late) const
    {
        MeshletDrawCursor cursor;
        cursor.references = m_frame.meshletReferences;
        cursor.references.drawInstancesReference = context.Buffer(late ? draws.LateInstances : draws.VisibleInstances).getDeviceAddress();
        cursor.commands = &context.Buffer(late ? draws.LateCommands : draws.Commands);
        cursor.counts = &context.Buffer(draws.Counts);
        cursor.late = late;
        cursor.taskFlags = m_meshletCulling;
        return cursor;
    }

    void Renderer::drawMeshletBucket(NRI::CommandBuffer& cmd, MeshletDrawCursor& cursor, RenderBucket bucket, NRI::Pipeline& pipeline, NRI::CullMode cullMode, bool depthWrite, bool blendEnable) const
    {
        const uint32_t maxDrawCount = getBucketEntryCount(bucket);
        if (maxDrawCount == 0)
            return;

        // Visible instances of the bucket start at its draw list start.
        const uint32_t firstEntry = m_drawBucketStarts[static_cast<size_t>(bucket)];
        cursor.references.instanceBaseIndex = firstEntry;
        cursor.references.meshletCulling = cursor.taskFlags;
        cmd.pushData(&cursor.references, sizeof(shaderio::PushConstantMeshlets));

        if (cursor.boundPipeline != &pipeline)
        {
            cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, pipeline);
            cursor.boundPipeline = &pipeline;
        }

        cmd.setCullMode(cullMode);
        cmd.setDepthWriteEnable(depthWrite);
        cmd.setColorBlendEnable(0, blendEnable);

        constexpr uint32_t CommandStride = sizeof(shaderio::MeshTasksIndirectCommand);
        const size_t countArray = cursor.late ? offsetof(shaderio::CullCounts, lateDrawCount) : offsetof(shaderio::CullCounts, drawCount);
        const uint64_t countOffset = countArray + sizeof(uint32_t) * static_cast<size_t>(bucket);
        cmd.drawMeshTasksIndirectCount(*cursor.commands, static_cast<uint64_t>(firstEntry) * CommandStride, *cursor.counts, countOffset, maxDrawCount, CommandStride);
    }

    void Renderer::updateEntityIDBuffer(uint32_t currentImage)
    {
        uint32_t selectedCount =
            static_cast<uint32_t>(
                std::min<size_t>(
                    m_SelectedEntityIDs.size(),
                    4096
                )
            );

        if (selectedCount > 0)
        {
            memcpy(
                m_selectedEntityIDBuffersMapped[currentImage],
                m_SelectedEntityIDs.data(),
                selectedCount * sizeof(int32_t)
            );
        }
    }

    void Renderer::updateUniformBuffer(uint32_t currentImage)
    {
        static auto startTime = std::chrono::high_resolution_clock::now();

        auto currentTime = std::chrono::high_resolution_clock::now();
        float time = std::chrono::duration<float>(currentTime - startTime).count();

        /*UniformBufferObject ubo{};*/
        /*uniformData.model = rotate(glm::mat4(1.0f), /*time *#1# glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));*/
        /*uniformData.view = lookAt(glm::vec3(0.0f, 2.0f, 3.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        uniformData.proj =
            glm::perspective(glm::radians(90.0f),
                             static_cast<float>(m_isEditor ? m_viewportSize.width : m_swapChainExtent.width) / static_cast<float>(m_isEditor ? m_viewportSize.height : m_swapChainExtent.height), 0.1f,
                             100.0f);
        uniformData.proj[1][1] *= -1;*/

        uniformData.jitterOffset = m_currentJitter;
        uniformData.samplerIndex = selectedSampler;
        uniformData.imageHeapIndexOffset = m_resourceHeap->getImageHeapIndexOffset();
        uniformData.mipFeedbackReference = m_mipFeedbackBuffer->getDeviceAddress();
        uniformData.mipFeedbackFrame = static_cast<uint32_t>(m_sceneFrameCounter);
        uniformData.lodErrorThreshold = m_lodErrorPixels / static_cast<float>(std::max(m_renderSize.height, 1u));
        uniformData.lodCameraNear = m_cameraNear;
        uniformData.lodFullDetail = m_lodFullDetail ? 1u : 0u;
        uniformData.clusterStatsReference = m_clusterStatsBuffer->getDeviceAddress();

        // PBR IBL
        uniformData.irradianceMapIndex = m_irradianceCubemap->GetDescriptorIndexSlot();
        uniformData.prefilteredMapIndex = m_prefilteredEnvMap->GetDescriptorIndexSlot();
        uniformData.prefilteredCubeMipLevels = static_cast<float>(prefilterCubeMipLevels);
        uniformData.brdfLutIndex = m_brdfLUT->GetDescriptorIndexSlot();
        uniformData.exposure = m_exposure; // slider in the future in imgui
        uniformData.gamma = m_gamma; // slider in the future in imgui
        uniformData.scaleIBLAmbient = m_scaleIBLAmbient; // slider in the future in imgui
        uniformData.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);

        memcpy(m_uniformBuffersMapped[currentImage], &uniformData, sizeof(uniformData));
    }

    void Renderer::updateGpuScene(uint32_t currentImage)
    {
        refreshGpuMaterials();
        m_gpuScene.SetGeometryBases(m_vertexStream.GetDeviceAddress(), m_rtIndexStream.GetDeviceAddress());

        if (m_gpuScene.UpdateDrawList(glm::vec3(uniformData.cameraWorldPos), m_drawList, m_drawBucketStarts))
            m_drawListStale.fill(true);
        updateDrawListBuffers(currentImage);

        // The camera view: the frozen frustum while culling is frozen (BeginScene keeps it equal to the camera otherwise).
        // The occlusion test compares against the pyramid of the previous frame, so it uses that frame's view; while the
        // culling view is frozen it does not match the drawn depth, so occlusion is skipped.
        shaderio::CullView view{};
        view.frustum = uniformData.frozenFrustum;
        view.viewProj = uniformData.nonJitteredProj * uniformData.view;
        view.previousViewProj = uniformData.prevProj * uniformData.prevView;
        std::copy(m_drawBucketStarts.begin(), m_drawBucketStarts.end(), view.bucketStart);
        view.entryCount = static_cast<uint32_t>(m_drawList.size());
        view.blockCount = (view.entryCount + shaderio::CULL_BLOCK_SIZE - 1) / shaderio::CULL_BLOCK_SIZE;
        view.flags = (m_instanceCullingEnabled ? shaderio::CULL_INSTANCES : 0u) |
                     (m_occlusionCullingEnabled && !m_frozen ? shaderio::CULL_OCCLUSION : 0u);
        memcpy(m_cullViewBuffers[currentImage]->map(0, sizeof(view)), &view, sizeof(view));
        m_cullViewBuffers[currentImage]->unmap();

        std::vector<std::unique_ptr<NRI::Buffer>> releasedBuffers;
        m_gpuScene.PrepareUploads(currentImage, releasedBuffers);
        for (std::unique_ptr<NRI::Buffer>& buffer : releasedBuffers)
            {
                std::scoped_lock lock(m_deferredReleasesMutex);
                m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(buffer) });
            }

        uniformData.sceneInstancesReference = m_gpuScene.GetInstances().GetDeviceAddress();
        uniformData.sceneTransformsReference = m_gpuScene.GetTransforms().GetDeviceAddress();
        uniformData.sceneMaterialsReference = m_gpuScene.GetMaterials().GetDeviceAddress();
        uniformData.sceneMeshesReference = m_gpuScene.GetMeshes().GetDeviceAddress();
        uniformData.sceneRayTracingInstancesReference = m_gpuScene.GetRayTracingInstances().GetDeviceAddress();

        uniformData.geometryVerticesReference = m_vertexStream.GetDeviceAddress();
        uniformData.geometryMeshletDrawsReference = m_meshletDrawStream.GetDeviceAddress();
        uniformData.geometryMeshletBoundsReference = m_meshletBoundsStream.GetDeviceAddress();
        uniformData.geometryMeshletVerticesReference = m_meshletVertexStream.GetDeviceAddress();
        uniformData.geometryMeshletTrianglesReference = m_meshletTriangleStream.GetDeviceAddress();

    }

    void Renderer::updateDrawListBuffers(uint32_t currentImage)
    {
        const uint64_t byteCount = sizeof(uint32_t) * m_drawList.size();
        if (byteCount > m_DrawListBufferCapacity)
        {
            m_DrawListBufferCapacity = byteCount * 2;
            for (std::unique_ptr<NRI::Buffer>& oldBuffer : m_drawListBuffers)
            {
                if (oldBuffer)
                    {
                        std::scoped_lock lock(m_deferredReleasesMutex);
                        m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(oldBuffer) });
                    }
            }
            createDrawListBuffers(m_DrawListBufferCapacity);
        }

        if (!std::exchange(m_drawListStale[currentImage], false) || byteCount == 0)
            return;
        memcpy(m_drawListBuffers[currentImage]->map(0, byteCount), m_drawList.data(), byteCount);
        m_drawListBuffers[currentImage]->unmap();
    }

    void Renderer::updateLightBuffer(uint32_t currentImage)
    {
        if (m_lightBuffers.empty())
        {
            m_LightBufferCapacity = sizeof(shaderio::LightData) * 16;
            createLightBuffer(m_LightBufferCapacity);
        }

        // ReSTIR DI needs local (point/spot) and infinite (directional) lights in separate
        // contiguous regions (RTXDI_LightBufferRegion is just a {firstIndex, count} range) --
        // partition once here so callers don't need to insert lights in any particular order.
        auto infiniteStart = std::stable_partition(m_lightBufferObjects.begin(), m_lightBufferObjects.end(),
            [](const shaderio::LightData& light) { return light.position.w != 0.0f; }); // != Directional
        m_restirDIFirstLocalLight = 0;
        m_restirDINumLocalLights = static_cast<uint32_t>(std::distance(m_lightBufferObjects.begin(), infiniteStart));
        m_restirDIFirstInfiniteLight = m_restirDINumLocalLights;
        m_restirDINumInfiniteLights = static_cast<uint32_t>(m_lightBufferObjects.size()) - m_restirDINumLocalLights;

        uint64_t requiredLightSize = sizeof(shaderio::LightData) * m_lightBufferObjects.size();
        if (requiredLightSize > m_LightBufferCapacity)
        {
            m_LightBufferCapacity = requiredLightSize * 2;
            for (auto& oldBuffer : m_lightBuffers)
            {
                if (oldBuffer)
                {
                    {
                        std::scoped_lock lock(m_deferredReleasesMutex);
                        m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(oldBuffer) });
                    }
                }
            }
            createLightBuffer(m_LightBufferCapacity);
        }

        if (requiredLightSize > 0)
        {
            memcpy(m_lightBuffersMapped[currentImage], m_lightBufferObjects.data(), requiredLightSize);
        }

        uniformData.lightDataReference = m_lightBuffers[currentImage]->getDeviceAddress();
        uniformData.lightCount = static_cast<uint32_t>(m_lightBufferObjects.size());
    }

    void Renderer::processDeferredReleases()
    {
        // Releasing one entry can queue another: dropping the last reference to a mesh asset runs its destructor, which
        // unloads its geometry and pushes that onto this queue. So the due entries are moved out first and released
        // afterwards, with nothing iterating the queue.
        std::vector<DeferredRelease> due;
        {
            std::scoped_lock lock(m_deferredReleasesMutex);
            for (DeferredRelease& release : m_deferredReleases)
            {
                if (release.framesRemaining > 0)
                    --release.framesRemaining;

                // Checked after the decrement, not as its else: an entry reaching 0 in this pass must be moved out too,
                // or the erase below destroys it under the lock (a Mesh destructor queuing its geometry would then
                // lock this mutex again).
                if (release.framesRemaining == 0)
                    due.push_back(std::move(release));
            }
            std::erase_if(m_deferredReleases, [](const DeferredRelease& release) { return release.framesRemaining == 0; });
        }

        // A mesh hands its ranges back to the streams (the next load reuses them) and frees its BLAS id; buffers and
        // asset references are released when `due` goes out of scope.
        for (DeferredRelease& release : due)
        {
            const MeshHandle* mesh = std::get_if<MeshHandle>(&release.payload);
            if (!mesh)
                continue;

            m_vertexStream.Free(mesh->vertices);
            m_meshletDrawStream.Free(mesh->meshletDraws);
            m_meshletBoundsStream.Free(mesh->meshletBounds);
            m_meshletVertexStream.Free(mesh->meshletVertices);
            m_meshletTriangleStream.Free(mesh->meshletTriangles);
            m_rtIndexStream.Free(mesh->rtIndices);

            // The GPU scene stopped tracing this mesh when it was unloaded.
            if (mesh->blasId != UINT32_MAX && mesh->blasId < m_meshBLASes.size() && m_meshBLASes[mesh->blasId].as)
            {
                m_meshBLASes[mesh->blasId] = MeshBLAS{};
                m_freeBLASIds.push_back(mesh->blasId);
            }
        }
    }

    // with compute there might be a snyc issue idk
    // according to gpt no async since compute and graphics same commandbuffer and executed in order
    void Renderer::updateMemoryBudget()
    {
        m_memoryBudget.Update(m_memoryHeapStats);

        // Geometry: the streams hold their capacity, of which the live ranges are in use.
        uint64_t geometryCommitted = 0;
        uint64_t geometryUsed = 0;
        for (const GeometryArena* stream : { &m_vertexStream, &m_meshletDrawStream, &m_meshletBoundsStream,
                                             &m_meshletVertexStream, &m_meshletTriangleStream, &m_rtIndexStream })
        {
            geometryCommitted += stream->GetCapacityBytes();
            geometryUsed += stream->GetUsedBytes();
        }
        m_memoryBudget.SetCommitted(MemoryCategory::Geometry, geometryCommitted, geometryUsed);

        // Ray tracing: the acceleration structures and what builds them.
        uint64_t rayTracing = 0;
        for (const MeshBLAS& blas : m_meshBLASes)
        {
            if (blas.storageBuffer)
                rayTracing += blas.storageBuffer->getSize();
        }
        if (m_tlasBuffer)
            rayTracing += m_tlasBuffer->getSize();
        if (m_tlasScratchBuffer)
            rayTracing += m_tlasScratchBuffer->getSize();
        for (const std::unique_ptr<NRI::Buffer>& buffer : m_rtInstanceBuffers)
        {
            if (buffer)
                rayTracing += buffer->getSize();
        }
        m_memoryBudget.SetCommitted(MemoryCategory::RayTracing, rayTracing, rayTracing);

        // Scene: the GPU scene tables.
        const uint64_t scene = m_gpuScene.GetDeviceBytes();
        m_memoryBudget.SetCommitted(MemoryCategory::Scene, scene, scene);

        // Transient: the render graph pool.
        const RGResourcePool& pool = m_renderGraph.GetResourcePool();
        const uint64_t transient = pool.GetTextureMemory() + pool.GetBufferMemory();
        m_memoryBudget.SetCommitted(MemoryCategory::Transient, transient, transient);

        // Textures: every image the device holds, minus the render graph's share of them.
        const uint64_t textures = m_device->getTextureBytes();
        m_memoryBudget.SetCommitted(MemoryCategory::Textures,
                                    textures > pool.GetTextureMemory() ? textures - pool.GetTextureMemory() : 0,
                                    textures > pool.GetTextureMemory() ? textures - pool.GetTextureMemory() : 0);
    }

    void Renderer::sampleMemoryStats()
    {
        // Memory changes slowly compared to frame time; a few samples per second are enough for the overlay/plots.
        constexpr auto SampleInterval = std::chrono::milliseconds(250);
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastMemoryStatsSample < SampleInterval)
            return;
        m_lastMemoryStatsSample = now;

        m_device->getMemoryStats(m_memoryHeapStats);
        updateMemoryBudget();
#if NOX_PROFILING_ENABLED
        Profiler::Get().SubmitMemoryStats(m_memoryHeapStats, m_device->isMemoryBudgetSupported(), Platform::QueryProcessMemory(),
                                          m_memoryBudget.GetCategories());
#endif
    }

    void Renderer::drawFrame()
    {
        NOX_PROFILE_SCOPE("Renderer::drawFrame");

        // Before any allocation of this frame: refreshes the VMA memory budget (VMA "Staying within budget").
        m_device->beginFrame(static_cast<uint32_t>(m_sceneFrameCounter));
        // Every build: the texture streaming pool is a share of the budget.
        sampleMemoryStats();

        // Process any queued shader hot-reloads
        if (!m_pendingReloads.empty())
        {
            NOX_PROFILE_SCOPE("Shader Hot Reload");
            m_device->waitIdle(); // Ensure GPU is idle before destroying and recreating pipelines!

            std::unordered_map<std::string, std::function<void()>> reloads;
            {
                std::scoped_lock lock(m_reloadMutex);
                reloads = std::move(m_pendingReloads);
            }

            for (auto& [pipelineName, reloadFn] : reloads)
            {
                NOX_CORE_INFO("Hot-reloading pipeline: {}", pipelineName);
                reloadFn();
            }
            return;
        }

        if (m_renderer2D->BeginFrame())
        {
            return;
        }

        uint32_t imageIndex = 0;

        NRI::FrameResult acquireResult;
        {
            // Includes waiting for this frame slot's previous submission (in-flight fence).
            NOX_PROFILE_WAIT_SCOPE("Wait GPU + Acquire");
            acquireResult = m_swapChain->acquireNextImage(frameIndex, imageIndex);
        }
        if (acquireResult == NRI::FrameResult::ResizeRequired)
        {
            recreateSwapChain();
            return;
        }

        {
            // After the in-flight wait, so only the previous frame can still be on the GPU: a release that waits
            // MAX_FRAMES_IN_FLIGHT frames outlives every frame that used it. (Before the wait, two frames could still run
            // and a release came one frame early -- in Release builds the GPU was still reading freed stream buffers.)
            NOX_PROFILE_SCOPE("Deferred Deletions");
            processDeferredReleases();
            m_uploads.Poll();
        }

        {
            // acquireNextImage waited for this slot's previous submission, which recorded the slot's pick copy.
            NOX_PROFILE_SCOPE("Pick Readback");
            readPickResult(frameIndex);
            readInspectionProbe(frameIndex);
            readCullStats(frameIndex);
            readBlasCompactedSizes(frameIndex);
            readMipFeedback(frameIndex);
            readClusterStats(frameIndex);
        }

        {
            NOX_PROFILE_SCOPE("Entity IDs + Lights");
            updateEntityIDBuffer(frameIndex);
            updateLightBuffer(frameIndex);
        }

        {
            NOX_PROFILE_SCOPE("GPU Scene");
            updateGpuScene(frameIndex);
        }
        NOX_PROFILE_COUNTER("Instances", m_gpuScene.GetInstanceCount());
        NOX_PROFILE_COUNTER("Draw List", m_drawList.size());
        NOX_PROFILE_COUNTER("Visible Instances", m_visibleInstanceCount);
        NOX_PROFILE_COUNTER("Occluded Candidates", m_lateCandidateCount);
        NOX_PROFILE_COUNTER("Phase 2 Drawn", m_lateDrawnCount);
        NOX_PROFILE_COUNTER("Visible Triangles", m_visibleTriangleCount);
        NOX_PROFILE_COUNTER("Drawn Clusters", m_clusterStats.drawnClusters);
        NOX_PROFILE_COUNTER("Drawn Triangles", m_clusterStats.drawnTriangles);
        NOX_PROFILE_COUNTER("Lights", m_lightBufferObjects.size());

        {
            NOX_PROFILE_SCOPE("Bones Upload");
            updateBoneBuffer(frameIndex);
        }

        {
            // Hardware Ray Tracing: CPU gathering, instance buffer upload, TLAS heap registration
            NOX_PROFILE_SCOPE("TLAS Prepare");
            updateSceneAccelerationStructure(frameIndex);
        }
        NOX_PROFILE_COUNTER("BLAS", m_meshBLASes.size() - m_freeBLASIds.size());

        {
            NOX_PROFILE_SCOPE("Uniforms");
            updateUniformBuffer(frameIndex);
        }

        {
            NOX_PROFILE_SCOPE("Renderer2D Update");
            m_renderer2D->Update(frameIndex);
        }

        std::span<NRI::CommandBuffer* const> commandBuffers;
        {
            NOX_PROFILE_SCOPE("Record Commands");
            commandBuffers = recordFrame(imageIndex);
        }

        {
            NOX_PROFILE_SCOPE("Submit");
            // What the frame reads is copied before it runs: uploads published right away and stream growth wait on
            // the GPU; streamed loads were published after their copies completed, so their wait costs nothing and
            // only orders the transfer writes. Streamed copies still in flight are not waited for.
            m_uploads.Flush();
            const NRI::TimelinePoint uploads{ &m_uploads.GetTimeline(), m_uploads.GetFrameWaitValue() };
            m_device->submitCommandBuffers(commandBuffers, *m_swapChain, frameIndex, imageIndex,
                                           std::span<const NRI::TimelinePoint>(&uploads, 1));
        }

        // Debounced -- see m_lastWindowResizeRequestTime's declaration in Renderer.h. A hard
        // ResizeRequired from present() (swapchain genuinely out of date) always recreates immediately;
        // the passive framebufferResized flag (set by every SDL resize event during a live window-border
        // drag) only triggers a recreate once no new resize event has arrived for a short settle window.
        NRI::FrameResult presentResult;
        {
            NOX_PROFILE_WAIT_SCOPE("Present");
            presentResult = m_swapChain->present(frameIndex, imageIndex);
        }
        bool windowResizeSettled = framebufferResized &&
            (std::chrono::steady_clock::now() - m_lastWindowResizeRequestTime >= std::chrono::milliseconds(120));
        if (presentResult == NRI::FrameResult::ResizeRequired || windowResizeSettled)
        {
            framebufferResized = false;
            recreateSwapChain();
        }

        m_renderer2D->EndFrame();

        frameIndex = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
        m_sceneFrameCounter++;

        m_lightBufferObjects.clear();
    }

    void Renderer::updateSceneAccelerationStructure(uint32_t currentFrameIndex)
    {
        bool isPathTracing = m_pathTracingEnabled || (m_debugMode == 18 || m_debugMode == 19);
        // DDGI and ReSTIR GI both ray-trace against this TLAS, so neither means anything without ray
        // tracing hardware access -- require the master toggle here too, matching runDDGI/runReSTIRGI,
        // so disabling "Enable Hybrid Ray Tracing" actually stops the TLAS rebuild cost as well.
        bool isDDGI = m_rayTracingEnabled && (m_ddgiEnabled || m_debugMode == 16 || m_debugMode == 17);
        bool isReSTIRGI = m_rayTracingEnabled && (m_diffuseGIMode == 2 || (m_debugMode == 16 && m_diffuseGIMode != 1));
        bool isHybridRT = m_rayTracingEnabled && (m_rayTracingShadows || m_rayTracingReflections);

        if (!isPathTracing && !isDDGI && !isReSTIRGI && !isHybridRT)
        {
            // Built once ray tracing is used again.
            m_hasTLASBuild = false;
            m_tlasNeedBuild = true;
            uniformData.tlasDeviceAddress = 0;
            uniformData.enableRTShadows = 0;
            uniformData.enableRTReflections = 0;
            return;
        }

        // Instance records live in the GPU scene (instanceCustomIndex = instance slot).
        const uint32_t instanceCount = m_gpuScene.GetTlasInstanceCount();
        if (instanceCount == 0)
        {
            m_hasTLASBuild = false;
            m_tlasNeedBuild = true;
            uniformData.enableRTShadows = 0;
            uniformData.enableRTReflections = 0;
            return;
        }

        // Rebuilt in frames where ray traced instances changed. NVIDIA RT best practices: rebuild the TLAS instead of
        // refitting it; refits keep the tree of the old placement, so moving instances (Bistro's fans) degrade traversal.
        m_tlasNeedBuild |= m_gpuScene.ConsumeTlasChanged();

        if (m_tlasNeedBuild)
        {
            if (m_rtInstanceBuffers.size() < MAX_FRAMES_IN_FLIGHT)
                m_rtInstanceBuffers.resize(MAX_FRAMES_IN_FLIGHT);

            const uint64_t instanceBufferSize = sizeof(NRI::AccelerationStructureInstance) * instanceCount;

            // 1. This frame slot's instance buffer (the build reads it on the GPU)
            if (!m_rtInstanceBuffers[currentFrameIndex] || m_rtInstanceBuffers[currentFrameIndex]->getSize() < instanceBufferSize)
            {
                m_rtInstanceBuffers[currentFrameIndex] = m_device->createBuffer(NRI::BufferDesc{
                    .size = std::max(instanceBufferSize, static_cast<uint64_t>(64 * 1024)),
                    .usage = NRI::BufferUsage::AccelerationStructureInstance
                });
            }
            auto* mapped = static_cast<NRI::AccelerationStructureInstance*>(m_rtInstanceBuffers[currentFrameIndex]->map(0, instanceBufferSize));
            m_gpuScene.WriteTlasInstances({ mapped, instanceCount });
            m_rtInstanceBuffers[currentFrameIndex]->unmap();

            // 2. Query TLAS build sizes
            m_tlasBuildDesc = NRI::AccelerationStructureBuildDesc{
                .type = NRI::AccelerationStructureType::TopLevel,
                .flags = NRI::AccelerationStructureBuildFlags::PreferFastTrace,
                .instances = {
                    .instanceBufferAddress = m_rtInstanceBuffers[currentFrameIndex]->getDeviceAddress(),
                    .instanceCount = instanceCount
                }
            };

            NRI::AccelerationStructureBuildSizes tlasSizes = m_device->getAccelerationStructureBuildSizes(m_tlasBuildDesc);

            // 3. Allocate / resize TLAS storage and scratch buffers
            if (!m_sceneTLAS || m_sceneTLASCapacity < instanceCount || !m_tlasBuffer || m_tlasBuffer->getSize() < tlasSizes.accelerationStructureSize)
            {
                m_device->waitIdle();

                // CRITICAL ORDER: Destroy VkAccelerationStructureKHR FIRST, then the backing VkBuffer!
                m_sceneTLAS.reset();
                m_tlasBuffer.reset();
                m_tlasScratchBuffer.reset();

                m_sceneTLASCapacity = std::max(instanceCount * 2, 64u);

                m_tlasBuffer = m_device->createBuffer(NRI::BufferDesc{
                    .size = tlasSizes.accelerationStructureSize,
                    .usage = NRI::BufferUsage::AccelerationStructure
                });

                m_sceneTLAS = m_device->createAccelerationStructure(NRI::AccelerationStructureDesc{
                    .type = NRI::AccelerationStructureType::TopLevel,
                    .storageBuffer = m_tlasBuffer.get(),
                    .bufferOffset = 0,
                    .size = tlasSizes.accelerationStructureSize
                });

                m_tlasScratchBuffer = m_device->createBuffer(NRI::BufferDesc{
                    .size = tlasSizes.buildScratchSize,
                    .usage = NRI::BufferUsage::AccelerationStructureScratch
                });

                // Register TLAS into Descriptor Heap ONLY when newly created or resized
                m_tlasHeapSlot = m_resourceHeap->registerAccelerationStructure(*m_sceneTLAS, m_tlasHeapSlot);
                m_tlasHeapIndex = m_tlasHeapSlot;
            }
        }

        // 4. Update UBO flags (ready for updateUniformBuffer!)
        uniformData.tlasDeviceAddress = m_sceneTLAS ? m_sceneTLAS->getDeviceAddress() : 0;
        uniformData.tlasHeapIndex = m_tlasHeapIndex;
        uniformData.enableRTShadows = (m_rayTracingEnabled && m_rayTracingShadows) ? 1 : 0;
        uniformData.enableRTReflections = (m_rayTracingEnabled && m_rayTracingReflections) ? 1 : 0;

        m_hasTLASBuild = true;
    }

    void Renderer::BuildSceneAccelerationStructure(NRI::CommandBuffer& cmd)
    {
        // Host instance writes -> AS build read, build, AS build write -> shader read.
        cmd.accelerationStructureBarrier(NRI::AccelerationStructureBarrierType::TransferToBuild);
        cmd.buildAccelerationStructure(m_tlasBuildDesc, m_tlasScratchBuffer->getDeviceAddress(), *m_sceneTLAS);
        cmd.accelerationStructureBarrier(NRI::AccelerationStructureBarrierType::BuildToShaderRead);
    }

    void Renderer::readBlasCompactedSizes(uint32_t frameSlot)
    {
        std::vector<BlasCompactionQuery> queries = std::exchange(m_blasCompactionQueries[frameSlot], {});
        if (queries.empty())
            return;

        std::vector<uint64_t> sizes(queries.size());
        if (!m_blasCompactionQueryPools[frameSlot]->getResults(0, sizes))
            return;

        for (size_t index = 0; index < queries.size(); ++index)
        {
            const BlasCompactionQuery& query = queries[index];
            // Unloaded (or its id reused) meanwhile, or nothing to gain.
            if (query.blasId >= m_meshBLASes.size() || m_meshBLASes[query.blasId].serial != query.serial || !m_meshBLASes[query.blasId].as)
                continue;
            if (sizes[index] == 0 || sizes[index] >= m_meshBLASes[query.blasId].as->getSize())
                continue;

            BlasCompaction compaction{ .blasId = query.blasId, .meshSlot = query.meshSlot };
            compaction.compacted.storageBuffer = m_device->createBuffer(NRI::BufferDesc{
                .size = sizes[index],
                .usage = NRI::BufferUsage::AccelerationStructure
            });
            compaction.compacted.as = m_device->createAccelerationStructure(NRI::AccelerationStructureDesc{
                .type = NRI::AccelerationStructureType::BottomLevel,
                .storageBuffer = compaction.compacted.storageBuffer.get(),
                .bufferOffset = 0,
                .size = sizes[index]
            });
            compaction.compacted.serial = query.serial;
            m_blasCompactions.push_back(std::move(compaction));
        }
    }

    void Renderer::readCullStats(uint32_t frameSlot)
    {
        if (!std::exchange(m_cullStatsPending[frameSlot], false))
            return;

        const auto* counts = static_cast<const shaderio::CullCounts*>(m_cullStatsBuffers[frameSlot]->map(0, sizeof(shaderio::CullCounts)));
        m_visibleInstanceCount = 0;
        m_lateCandidateCount = 0;
        for (uint32_t drawCount : counts->drawCount)
            m_visibleInstanceCount += drawCount;
        for (uint32_t drawCount : counts->lateDrawCount)
            m_lateCandidateCount += drawCount;
        m_lateDrawnCount = counts->lateDrawn;
        m_visibleTriangleCount = counts->visibleTriangles + counts->lateTriangles;
        m_cullStatsBuffers[frameSlot]->unmap();
    }

    void Renderer::readClusterStats(uint32_t frameSlot)
    {
        if (!std::exchange(m_clusterStatsPending[frameSlot], false))
            return;

        m_clusterStats = *static_cast<const shaderio::ClusterStats*>(m_clusterStatsReadback[frameSlot]->map(0, sizeof(shaderio::ClusterStats)));
        m_clusterStatsReadback[frameSlot]->unmap();
    }

    void Renderer::readMipFeedback(uint32_t frameSlot)
    {
        if (!std::exchange(m_mipFeedbackPending[frameSlot], false))
            return;

        const auto* feedback = static_cast<const uint32_t*>(m_mipFeedbackReadback[frameSlot]->map(0, sizeof(uint32_t) * shaderio::MipFeedbackSlots));
        m_mipFeedback.assign(feedback, feedback + shaderio::MipFeedbackSlots);
        m_mipFeedbackReadback[frameSlot]->unmap();
        ++m_mipFeedbackSerial;
    }

    void Renderer::readInspectionProbe(uint32_t frameSlot)
    {
        if (!m_inspectionProbePending[frameSlot])
            return;
        m_inspectionProbePending[frameSlot] = 0;

        void* mappedMemory = m_inspectionProbeBuffers[frameSlot]->map(0, sizeof(float) * 4);
        if (!mappedMemory)
            return;
        memcpy(&m_inspectionProbeValue, mappedMemory, sizeof(float) * 4);
        m_inspectionProbeBuffers[frameSlot]->unmap();
        m_inspectionProbeValid = true;
    }

    void Renderer::readPickResult(uint32_t frameSlot)
    {
        PickRequest& request = m_pickerReadbackRequests[frameSlot];
        if (!request.active)
            return;
        request.active = false;

        const size_t pixelCount = static_cast<size_t>(request.width) * request.height;
        if (pixelCount == 0)
            return;

        void* mappedMemory = m_pickerStagingBuffers[frameSlot]->map(0, pixelCount * sizeof(int32_t));
        if (!mappedMemory)
            return;

        const int32_t* pixels = static_cast<const int32_t*>(mappedMemory);
        m_pickResult.request = request;
        m_pickResult.pixels.assign(pixels, pixels + pixelCount);
        m_pickerStagingBuffers[frameSlot]->unmap();
    }

    int32_t Renderer::getPickedEntityID() const
    {
        return m_pickResult.pixels.empty() ? -1 : m_pickResult.pixels.front();
    }

    std::vector<int32_t> Renderer::getPickedEntityIDs() const
    {
        std::vector<int32_t> uniqueIDs;
        std::unordered_set<int32_t> seen;
        for (int32_t id : m_pickResult.pixels)
        {
            if (id >= 0 && seen.insert(id).second)
                uniqueIDs.push_back(id);
        }
        return uniqueIDs;
    }

    std::vector<char> Renderer::readFile(const std::string& filename)
    {
        std::ifstream file(filename, std::ios::ate | std::ios::binary);
        if (!file.is_open())
        {
            throw std::runtime_error("failed to open file!");
        }
        std::vector<char> buffer(file.tellg());
        file.seekg(0, std::ios::beg);
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        file.close();
        return buffer;
    }

    void Renderer::initImGui()
    {
        m_device->initImGui(*m_window);
    }

    void Renderer::shutdownImGui()
    {
        m_device->waitIdle();
        m_device->shutdownImGui();
    }

    void Renderer::beginImGui()
    {
        m_device->beginImGui();
    }

    void Renderer::endImGui()
    {
        m_device->endImGui();
    }

    // 8-Phase Halton(2, 3) sequence for subpixel jitter
    static constexpr glm::vec2 s_Halton8[8] = {
        {0.5000f, 0.3333f},
        {0.2500f, 0.6667f},
        {0.7500f, 0.1111f},
        {0.1250f, 0.4444f},
        {0.6250f, 0.7778f},
        {0.3750f, 0.2222f},
        {0.8750f, 0.5556f},
        {0.0625f, 0.8889f}
    };

    void Renderer::BeginScene(const Camera& camera, const glm::mat4& cameraWorldMatrix)
    {
        // The view must not inherit scale from the entity hierarchy: rigid camera transform only.
        glm::mat4 transform(1.0f);
        transform[0] = glm::vec4(glm::normalize(glm::vec3(cameraWorldMatrix[0])), 0.0f);
        transform[1] = glm::vec4(glm::normalize(glm::vec3(cameraWorldMatrix[1])), 0.0f);
        transform[2] = glm::vec4(glm::normalize(glm::vec3(cameraWorldMatrix[2])), 0.0f);
        transform[3] = glm::vec4(glm::vec3(cameraWorldMatrix[3]), 1.0f);

        // DLSS/DLAA cannot function without jitter, so it's forced on whenever DLSS will actually
        // evaluate this frame - matching the exact gate in RecordCommandBuffer's evaluateDLSS() call,
        // not just "DLSS enabled" (mode == Off means evaluateDLSS never runs, so jitter shouldn't
        // either - the "Camera Subpixel Jitter" toggle only has an effect while DLSS is genuinely off).
        const bool enableJitter =
            (m_cameraJitterEnabled || (m_dlssEnabled && m_dlssMode != NRI::UpscaleMode::Off && m_device->isDLSSSupported()))
            && m_viewportSize.width > 0
            && m_viewportSize.height > 0;

        // Must run before anything this frame touches a render-resolution-dependent texture (G-buffer,
        // depth, entity, scene targets) - BeginScene runs during OnUpdate(), strictly before
        // OnImGuiRender() draws the Viewport window's ImGui::Image(), so applying a pending DLSS
        // enable/mode change here (rather than synchronously inside the Settings checkbox callback)
        // guarantees resources are never destroyed mid-ImGui-frame after already being referenced.
        applyPendingRenderResolutionIfNeeded();

        // Only advance temporal state once per frame (ignores secondary calls like OnOverlayRender)
        if (m_lastSceneFrameCounter != m_sceneFrameCounter)
        {
            m_lastSceneFrameCounter = m_sceneFrameCounter;
            const glm::vec3 newCameraWorldPos = glm::vec3(transform[3]);

            if (m_isFirstFrame)
            {
                m_prevView = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) * glm::inverse(transform);
                m_prevNonJitteredProj = camera.GetProjection();
                m_prevPrevCameraWorldPos = newCameraWorldPos;
                m_prevCameraWorldPos = newCameraWorldPos;
                m_currentCameraWorldPos = newCameraWorldPos;
                m_isFirstFrame = false;
            }
            else
            {
                m_prevView = m_currentView;
                m_prevNonJitteredProj = m_currentNonJitteredProj;
                m_prevPrevCameraWorldPos = m_prevCameraWorldPos;
                m_prevCameraWorldPos = m_currentCameraWorldPos;
            }

            m_currentView = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) * glm::inverse(transform);
            m_currentNonJitteredProj = camera.GetProjection();
            m_currentCameraWorldPos = newCameraWorldPos;

            // Same camera cache as the EditorCamera overload (DLSS / Ray Reconstruction inputs). Camera looks down -Z.
            m_cameraPosition = newCameraWorldPos;
            m_cameraRight = glm::vec3(transform[0]);
            m_cameraUp = glm::vec3(transform[1]);
            m_cameraForward = -glm::vec3(transform[2]);
            if (m_currentNonJitteredProj[2][3] != 0.0f)
            {
                // Reverse-Z infinite perspective: [1][1] = 1 / tan(fovY / 2), [3][2] = near.
                m_cameraFOV = glm::degrees(2.0f * std::atan(1.0f / m_currentNonJitteredProj[1][1]));
                m_cameraNear = m_currentNonJitteredProj[3][2];
            }

            if (enableJitter)
            {
                m_jitterPhase = (m_jitterPhase + 1) % 8;
                glm::vec2 halton = s_Halton8[m_jitterPhase];
                m_currentJitter = halton - 0.5f;
            }
            else
            {
                m_currentJitter = glm::vec2(0.0f);
            }
        }

        // Apply subpixel jitter to the rasterization projection matrix
        glm::mat4 rasterProj = m_currentNonJitteredProj;
        if (enableJitter)
        {
            float deltaNdcX = (2.0f * m_currentJitter.x) / static_cast<float>(m_viewportSize.width);
            float deltaNdcY = (2.0f * m_currentJitter.y) / static_cast<float>(m_viewportSize.height);
            rasterProj[2][0] += deltaNdcX;
            rasterProj[2][1] += deltaNdcY;
        }

        uniformData.proj = rasterProj;
        uniformData.view = m_currentView;
        uniformData.nonJitteredProj = m_currentNonJitteredProj;
        uniformData.prevProj = m_prevNonJitteredProj;
        uniformData.prevView = m_prevView;
        uniformData.invViewProj = glm::inverse(uniformData.proj * uniformData.view);
        uniformData.cameraWorldPos = glm::vec4(glm::vec3(transform[3]), 0.0f);
        uniformData.frustum = shaderio::Frustum{uniformData.proj * uniformData.view};

        // DDGI
        uniformData.enableDDGI = m_ddgiEnabled ? 1 : 0;
        uniformData.ddgiGridOrigin = glm::vec4(m_ddgiGridOrigin, static_cast<float>(m_ddgiProbeCountX));
        uniformData.ddgiGridSpacing = glm::vec4(m_ddgiGridSpacing, static_cast<float>(m_ddgiProbeCountY));
        uniformData.ddgiGridParams = glm::vec4(
            static_cast<float>(m_ddgiProbeCountZ),
            static_cast<float>(m_ddgiRaysPerProbe),
            m_ddgiHysteresis,
            m_ddgiNormalBias
        );
        uint32_t ddgiTotalProbes = m_ddgiProbeCountX * m_ddgiProbeCountY * m_ddgiProbeCountZ;
        uint32_t ddgiProbeRows = (ddgiTotalProbes + DDGIProbesPerRow - 1) / DDGIProbesPerRow;
        uniformData.ddgiAtlasParams = glm::vec4(
            static_cast<float>(DDGIProbesPerRow * 10),
            static_cast<float>(ddgiProbeRows * 10),
            static_cast<float>(DDGIProbesPerRow * 18),
            static_cast<float>(ddgiProbeRows * 18)
        );

        // ReSTIR GI
        uniformData.diffuseGIMode = m_diffuseGIMode;
        uniformData.restirGIReservoirBufferIndex = 0;
        uniformData.restirGINeighborOffsetsBufferIndex = 0;

        if (m_frozen)
        {
            if (!m_frozenDone)
            {
                frozenUniformData = uniformData;
                m_frozenDone = true;
            }

            uniformData.frozenProj = frozenUniformData.proj;
            uniformData.frozenView = frozenUniformData.view;
            uniformData.frozenCameraWorldPos = frozenUniformData.cameraWorldPos;
            uniformData.frozenFrustum = frozenUniformData.frustum;
        }
        else
        {
            uniformData.frozenProj = uniformData.proj;
            uniformData.frozenView = uniformData.view;
            uniformData.frozenCameraWorldPos = uniformData.cameraWorldPos;
            uniformData.frozenFrustum = uniformData.frustum;
        }

        m_renderer2D->BeginScene(camera, transform);
    }

    void Renderer::BeginScene(const EditorCamera& camera)
    {
        // See comment in the other BeginScene overload - must run before this frame's Viewport
        // ImGui::Image() call, which happens during OnUpdate() -> BeginScene(), before OnImGuiRender().
        applyPendingRenderResolutionIfNeeded();

        // Camera Subpixel Jitter checkbox only matters while DLSS is genuinely off (disabled, or mode
        // == Off) - DLSS/DLAA cannot function without jitter, so it's forced on whenever evaluateDLSS()
        // will actually run this frame, matching that exact gate in RecordCommandBuffer.
        const bool enableJitter =
            (m_cameraJitterEnabled ||
                (m_dlssEnabled && m_dlssMode != NRI::UpscaleMode::Off && m_device->isDLSSSupported()))
            && m_viewportSize.width > 0
            && m_viewportSize.height > 0;
        // Only advance temporal state once per frame (ignores secondary calls like OnOverlayRender)
        if (m_lastSceneFrameCounter != m_sceneFrameCounter)
        {
            m_lastSceneFrameCounter = m_sceneFrameCounter;
            const glm::vec3 newCameraWorldPos = camera.GetPosition();

            if (m_isFirstFrame)
            {
                m_prevView = camera.GetViewMatrix();
                m_prevNonJitteredProj = camera.GetProjection();
                m_prevPrevCameraWorldPos = newCameraWorldPos;
                m_prevCameraWorldPos = newCameraWorldPos;
                m_currentCameraWorldPos = newCameraWorldPos;
                m_isFirstFrame = false;
            }
            else
            {
                m_prevView = m_currentView;
                m_prevNonJitteredProj = m_currentNonJitteredProj;
                m_prevPrevCameraWorldPos = m_prevCameraWorldPos;
                m_prevCameraWorldPos = m_currentCameraWorldPos;
            }

            m_currentView = camera.GetViewMatrix();
            m_currentNonJitteredProj = camera.GetProjection();
            m_currentCameraWorldPos = newCameraWorldPos;

            m_cameraPosition = camera.GetPosition();
            m_cameraUp = camera.GetUpDirection();
            m_cameraRight = camera.GetRightDirection();
            m_cameraForward = camera.GetForwardDirection();
            m_cameraNear = camera.GetNearClip();
            m_cameraFOV = camera.GetFOV();

            // Apply subpixel jitter to the rasterization projection matrix
            if (enableJitter)
            {
                m_jitterPhase = (m_jitterPhase + 1) % 8;
                glm::vec2 halton = s_Halton8[m_jitterPhase];
                m_currentJitter = halton - 0.5f;
            }
            else
            {
                m_currentJitter = glm::vec2(0.0f);
            }
        }

        // Apply subpixel jitter to the rasterization projection matrix
        glm::mat4 rasterProj = m_currentNonJitteredProj;
        if (enableJitter)
        {
            float deltaNdcX = (2.0f * m_currentJitter.x) / static_cast<float>(m_viewportSize.width);
            float deltaNdcY = (2.0f * m_currentJitter.y) / static_cast<float>(m_viewportSize.height);
            rasterProj[2][0] += deltaNdcX;
            rasterProj[2][1] += deltaNdcY;
        }

        uniformData.proj = rasterProj;
        uniformData.view = m_currentView;
        uniformData.nonJitteredProj = m_currentNonJitteredProj;
        uniformData.prevProj = m_prevNonJitteredProj;
        uniformData.prevView = m_prevView;

        uniformData.invViewProj = glm::inverse(uniformData.proj * uniformData.view);
        uniformData.cameraWorldPos = {camera.GetPosition(), 0.0f};
        uniformData.frustum = shaderio::Frustum{uniformData.proj * uniformData.view};
        uniformData.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);

        // DDGI
        uniformData.enableDDGI = m_ddgiEnabled ? 1 : 0;
        uniformData.ddgiGridOrigin = glm::vec4(m_ddgiGridOrigin, static_cast<float>(m_ddgiProbeCountX));
        uniformData.ddgiGridSpacing = glm::vec4(m_ddgiGridSpacing, static_cast<float>(m_ddgiProbeCountY));
        uniformData.ddgiGridParams = glm::vec4(
            static_cast<float>(m_ddgiProbeCountZ),
            static_cast<float>(m_ddgiRaysPerProbe),
            m_ddgiHysteresis,
            m_ddgiNormalBias
        );
        uint32_t ddgiTotalProbes = m_ddgiProbeCountX * m_ddgiProbeCountY * m_ddgiProbeCountZ;
        uint32_t ddgiProbeRows = (ddgiTotalProbes + DDGIProbesPerRow - 1) / DDGIProbesPerRow;
        uniformData.ddgiAtlasParams = glm::vec4(
            static_cast<float>(DDGIProbesPerRow * 10),
            static_cast<float>(ddgiProbeRows * 10),
            static_cast<float>(DDGIProbesPerRow * 18),
            static_cast<float>(ddgiProbeRows * 18)
        );

        // ReSTIR GI
        uniformData.diffuseGIMode = m_diffuseGIMode;
        uniformData.restirGIReservoirBufferIndex = 0;
        uniformData.restirGINeighborOffsetsBufferIndex = 0;

        if (m_frozen)
        {
            if (!m_frozenDone)
            {
                frozenUniformData = uniformData;
                m_frozenDone = true;
            }

            uniformData.frozenProj = frozenUniformData.proj;
            uniformData.frozenView = frozenUniformData.view;
            uniformData.frozenCameraWorldPos = frozenUniformData.cameraWorldPos;
            uniformData.frozenFrustum = frozenUniformData.frustum;
        }
        else
        {
            uniformData.frozenProj = uniformData.proj;
            uniformData.frozenView = uniformData.view;
            uniformData.frozenCameraWorldPos = uniformData.cameraWorldPos;
            uniformData.frozenFrustum = uniformData.frustum;
        }

        m_renderer2D->BeginScene(camera);
    }

    void Renderer::EndScene()
    {
        m_renderer2D->EndScene();
    }

    bool Renderer::BindGpuScene(uint64_t sceneID)
    {
        // Joint matrices are collected again every frame.
        m_boneMatrices.clear();
        // A registry change may resolve texture paths that were missing: re-pack the materials.
        if (RefreshTextureDescriptorMisses())
            m_gpuMaterialsDirty = true;

        if (m_gpuSceneOwner == sceneID)
            return false;

        m_gpuScene.RemoveAllInstances();
        m_invalidatedMeshEntities.clear();
        m_gpuSceneOwner = sceneID;
        return true;
    }

    bool Renderer::AddMeshInstances(const glm::mat4& world, const MeshComponent& component, const MaterialComponent* material, int32_t entityID,
                                    std::vector<uint32_t>& outInstances, std::vector<AssetHandle>& outMissingAssets)
    {
        if (component.Mesh == 0)
            return true;

        const AssetType type = AssetManager::GetAssetType(component.Mesh);
        if (type == AssetType::Mesh)
        {
            if (const Mesh* mesh = AssetManager::FindLoadedAsset<Mesh>(component.Mesh))
                return addSubmeshInstances(world, *mesh, component, material, entityID, outInstances, outMissingAssets);
        }
        else if (type == AssetType::StaticMesh)
        {
            if (const StaticMesh* mesh = AssetManager::FindLoadedAsset<StaticMesh>(component.Mesh))
                return addSubmeshInstances(world, *mesh, component, material, entityID, outInstances, outMissingAssets);
        }
        else
        {
            return true; // not a drawable mesh asset
        }

        outMissingAssets.push_back(component.Mesh);
        return false;
    }

    void Renderer::RemoveMeshInstances(std::span<const uint32_t> instances)
    {
        for (uint32_t instance : instances)
            m_gpuScene.RemoveInstance(instance);
    }

    void Renderer::SetMeshInstancesTransform(std::span<const uint32_t> instances, const glm::mat4& world)
    {
        for (uint32_t instance : instances)
            m_gpuScene.SetTransform(instance, world);
    }

    void Renderer::SetMeshInstancesBones(std::span<const uint32_t> instances, std::span<const glm::mat4> bones)
    {
        // The entity's submeshes share its joint matrices.
        uint32_t boneMatrixOffset = GpuScene::NoBoneMatrices;
        if (!bones.empty())
        {
            boneMatrixOffset = static_cast<uint32_t>(m_boneMatrices.size());
            m_boneMatrices.insert(m_boneMatrices.end(), bones.begin(), bones.end());
        }

        for (uint32_t instance : instances)
            m_gpuScene.SetBoneMatrixOffset(instance, boneMatrixOffset);
    }

    void Renderer::MarkMaterialChanged(AssetHandle material)
    {
        if (!s_Instance)
            return;

        GpuScene& gpuScene = s_Instance->m_gpuScene;
        const uint32_t slot = gpuScene.FindMaterial({ .Asset = static_cast<uint64_t>(material) });
        if (slot == GpuScene::InvalidSlot)
            return;
        if (const Material* loaded = AssetManager::FindLoadedAsset<Material>(material))
            gpuScene.UpdateMaterial(slot, PackMaterial(loaded->GetData()));
    }

    template <typename MeshAsset>
    bool Renderer::addSubmeshInstances(const glm::mat4& world, const MeshAsset& mesh, const MeshComponent& component, const MaterialComponent* material,
                                       int32_t entityID, std::vector<uint32_t>& outInstances, std::vector<AssetHandle>& outMissingAssets)
    {
        // Per slot: the entity's override when it has one, the mesh's imported .nmat otherwise.
        const std::vector<AssetHandle>& meshMaterials = mesh.GetMaterialAssets();

        const uint64_t submeshTotal = mesh.GetSubMeshCount();
        const uint64_t first = std::min<uint64_t>(component.SubmeshIndex, submeshTotal);
        const uint64_t count = component.SubmeshCount == UINT32_MAX ? submeshTotal : std::max(component.SubmeshCount, 1u);
        const uint64_t last = std::min(first + count, submeshTotal);

        bool complete = true;
        for (uint64_t submesh = first; submesh < last; ++submesh)
        {
            const MeshHandle& handle = mesh.GetSubMesh(submesh);
            if (handle.gpuSceneMesh == GpuScene::InvalidSlot)
                continue;

            AssetHandle materialAsset = material && submesh < material->MaterialAssets.size() ? material->MaterialAssets[submesh] : AssetHandle(0);
            if (materialAsset == 0 && submesh < meshMaterials.size())
                materialAsset = meshMaterials[submesh];
            const uint32_t materialSlot = acquireGpuMaterial(static_cast<uint64_t>(component.Mesh), static_cast<uint32_t>(submesh), mesh.GetMaterial(submesh),
                                                             materialAsset, complete, outMissingAssets);
            outInstances.push_back(m_gpuScene.AddInstance(handle.gpuSceneMesh, materialSlot, world, entityID));
        }
        return complete;
    }

    uint32_t Renderer::acquireGpuMaterial(uint64_t meshAsset, uint32_t submesh, const MaterialData& meshMaterial, AssetHandle materialAsset,
                                          bool& outComplete, std::vector<AssetHandle>& outMissingAssets)
    {
        // The .nmat asset replaces the mesh's embedded material, which stands in until the asset is loaded.
        if (materialAsset != 0 && AssetManager::IsAssetHandleValid(materialAsset))
        {
            const GpuMaterialKey key{ .Asset = static_cast<uint64_t>(materialAsset) };
            if (const uint32_t slot = m_gpuScene.FindMaterial(key); slot != GpuScene::InvalidSlot)
                return slot;
            if (const Material* loaded = AssetManager::FindLoadedAsset<Material>(materialAsset))
                return m_gpuScene.AddMaterial(key, PackMaterial(loaded->GetData()));

            outMissingAssets.push_back(materialAsset);
            outComplete = false;
        }

        const GpuMaterialKey key{ .Asset = meshAsset, .Submesh = submesh };
        if (const uint32_t slot = m_gpuScene.FindMaterial(key); slot != GpuScene::InvalidSlot)
            return slot;
        return m_gpuScene.AddMaterial(key, PackMaterial(meshMaterial));
    }

    void Renderer::refreshGpuMaterials()
    {
        if (!m_gpuMaterialsDirty)
            return;
        m_gpuMaterialsDirty = false;

        NOX_PROFILE_SCOPE("Refresh GPU Materials");
        m_gpuScene.ForEachMaterial([this](uint32_t slot, const GpuMaterialKey& key)
        {
            const AssetHandle asset(key.Asset);
            const MaterialData* data = nullptr;
            if (key.Submesh == GpuMaterialKey::AssetMaterial)
            {
                if (const Material* material = AssetManager::FindLoadedAsset<Material>(asset))
                    data = &material->GetData();
            }
            else if (AssetManager::GetAssetType(asset) == AssetType::Mesh)
            {
                if (const Mesh* mesh = AssetManager::FindLoadedAsset<Mesh>(asset); mesh && key.Submesh < mesh->GetSubMeshCount())
                    data = &mesh->GetMaterial(key.Submesh);
            }
            else if (const StaticMesh* mesh = AssetManager::FindLoadedAsset<StaticMesh>(asset); mesh && key.Submesh < mesh->GetSubMeshCount())
            {
                data = &mesh->GetMaterial(key.Submesh);
            }

            if (data)
                m_gpuScene.UpdateMaterial(slot, PackMaterial(*data));
        });
    }

    void Renderer::SubmitLight(const glm::mat4& transform, const DirectionalLightComponent& light)
    {
        // glTF's KHR_lights_punctual convention defines a light's direction as its local -Z axis
        // transformed to world space - the direction the light EMITS/travels (matches the Khronos
        // spec and this node's rotation directly). But every shader in this project (DeferredLighting,
        // PathTracer, ShadowMask, ReSTIR DI/GI/PT, ...) consumes light.direction.xyz directly as the
        // surface-to-light vector for NdotL and for the direction of the shadow ray toward the light -
        // exactly what the Khronos reference shader itself does via `normalize(-light.direction)`.
        // Negate here, once, at the source, so every consumer's existing (correct) usage lines up
        // instead of patching light.direction.xyz in a dozen shader files.
        glm::vec3 forward = glm::normalize(glm::vec3(transform * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        glm::vec3 towardLight = -forward;

        shaderio::LightData l{};
        l.position = glm::vec4(0.0f, 0.0f, 0.0f, (float)shaderio::LightType::Directional);
        l.direction = glm::vec4(towardLight, 0.0f);
        l.color = glm::vec4(light.Color, light.Intensity);
        // spotParams.z: half-angle angular radius in radians; spotParams.w: shadow samples
        l.spotParams = glm::vec4(0.0f, 0.0f, glm::radians(glm::max(0.0f, light.AngularDiameter) * 0.5f), (float)glm::max(1u, light.ShadowSamples));

        m_lightBufferObjects.push_back(l);
    }

    void Renderer::SubmitLight(const glm::mat4& transform, const PointLightComponent& light)
    {
        glm::vec3 worldPos = glm::vec3(transform[3]);

        shaderio::LightData l{};
        l.position = glm::vec4(worldPos, (float)shaderio::LightType::Point);
        l.direction = glm::vec4(0.0f, 0.0f, 0.0f, light.Range);
        l.color = glm::vec4(light.Color, light.Intensity);
        // spotParams.z: light source radius in meters; spotParams.w: shadow samples
        l.spotParams = glm::vec4(0.0f, 0.0f, glm::max(0.0f, light.Radius), (float)glm::max(1u, light.ShadowSamples));

        m_lightBufferObjects.push_back(l);
    }

    void Renderer::SubmitLight(const glm::mat4& transform, const SpotLightComponent& light)
    {
        glm::vec3 worldPos = glm::vec3(transform[3]);
        glm::vec3 forward = glm::normalize(glm::vec3(transform * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

        // glTF KHR_lights_punctual specification:
        float innerConeAngle = glm::radians(light.InnerAngle);
        float outerConeAngle = glm::radians(light.OuterAngle);

        float cosInner = glm::cos(innerConeAngle);
        float cosOuter = glm::cos(outerConeAngle);

        float lightAngleScale = 1.0f / glm::max(0.001f, cosInner - cosOuter);
        float lightAngleOffset = -cosOuter * lightAngleScale;

        shaderio::LightData l{};
        l.position = glm::vec4(worldPos, (float)shaderio::LightType::Spot);
        l.direction = glm::vec4(forward, light.Range);
        l.color = glm::vec4(light.Color, light.Intensity);
        l.spotParams = glm::vec4(lightAngleScale, lightAngleOffset, glm::max(0.0f, light.Radius), (float)glm::max(1u, light.ShadowSamples));

        m_lightBufferObjects.push_back(l);
    }
}
