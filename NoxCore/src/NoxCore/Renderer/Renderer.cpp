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
    }

    static int GetTextureIndex(const std::string& path)
    {
        if (path.empty())
            return -1;

        auto& descriptorCache = s_TextureDescriptorCache;
        auto cached = descriptorCache.find(path);
        if (cached != descriptorCache.end())
            return cached->second;

        const size_t registrySize = Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry().size();
        if (registrySize != s_TextureDescriptorMissesRegistrySize)
        {
            s_TextureDescriptorMisses.clear();
            s_TextureDescriptorMissesRegistrySize = registrySize;
        }
        if (s_TextureDescriptorMisses.contains(path))
            return -1;

        AssetHandle handle = FindTextureAsset(path);
        if (handle == 0)
        {
            s_TextureDescriptorMisses.insert(path);
            return -1;
        }

        Ref<Texture2D> texture = AssetManager::GetAsset<Texture2D>(handle);
        if (!texture)
        {
            s_TextureDescriptorMisses.insert(path);
            return -1;
        }

        const int descriptorIndex = static_cast<int>(texture->GetDescriptorIndexSlot());
        descriptorCache.emplace(path, descriptorIndex);
        return descriptorIndex;
    }

    static void PackMaterial(shaderio::InstanceData& instance, const MaterialData& material)
    {
        instance.workflow = material.Workflow;
        instance.diffuseFactor = material.DiffuseFactor;
        instance.specularFactor = material.SpecularFactor;
        instance.baseColorFactor = (material.Workflow == 1.0f) ? material.DiffuseFactor : material.BaseColorFactor;
        instance.baseColorTextureIndex = GetTextureIndex(material.BaseColorTexturePath);
        instance.baseColorTextureSet = material.BaseColorTextureSet;
        instance.metallicFactor = material.MetallicFactor;
        instance.roughnessFactor = material.RoughnessFactor;
        instance.metallicRoughnessTextureIndex = GetTextureIndex(material.MetallicRoughnessTexturePath);
        instance.physicalDescriptorTextureSet = material.PhysicalDescriptorTextureSet;
        instance.normalTextureIndex = GetTextureIndex(material.NormalTexturePath);
        instance.normalTextureSet = material.NormalTextureSet;
        instance.occlusionTextureIndex = GetTextureIndex(material.OcclusionTexturePath);
        instance.occlusionTextureSet = material.OcclusionTextureSet;
        instance.emissiveFactor = material.EmissiveFactor;
        instance.emissiveTextureIndex = GetTextureIndex(material.EmissiveTexturePath);
        instance.emissiveTextureSet = material.EmissiveTextureSet;
        instance.emissiveStrength = material.emissiveStrength;
        instance.transmissionFactor = material.TransmissionFactor;
        instance.transmissionTextureIndex = GetTextureIndex(material.TransmissionTexturePath);
        instance.transmissionTextureSet = material.TransmissionTextureSet;
        instance.alphaMode = static_cast<uint32_t>(material.Mode);
        instance.alphaMaskCutoff = material.AlphaMaskCutoff;
        instance.doubleSided = material.DoubleSided ? 1u : 0u;
        instance.unlit = material.Unlit ? 1u : 0u;
    }

    Renderer::Renderer(std::shared_ptr<Window> window, bool isEditor) : m_window(std::move(window)), m_isEditor(isEditor)
    {
        NOX_CORE_INFO("Renderer Start");

        s_Instance = this;

        m_device = NRI::Device::create(NRI::GraphicsAPI::Vulkan, *m_window);
        if (!m_device) NOX_CORE_ASSERT("Failed to create NRI device");

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

        // Release queued assets while the instance is still reachable: a Mesh destructor calls
        // Renderer::UnloadMesh, which asserts on s_Instance.
        m_device->waitIdle();
        m_deferredAssetReleases.clear();

#if NOX_PROFILING_ENABLED
        Profiler::Get().SetGpuProfiler(nullptr);
#endif

        if (s_Instance == this) s_Instance = nullptr;

        m_device->waitIdle();
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
        createTextureImage();
        createSceneResources();
        createEntityResources();
        createDepthResources();

        // Visability
        createVisibilityResources();
        // G-Buffer
        createGBufferResources();
        // PBR
        createDeferredLightingPipeline(false);
        // NRD
        createShadowMaskResources();
        // Path Tracer
        createPathTracerResources();
        // DDGI
        createDDGIResources();
        // ReSTIR GI
        createReSTIRGIResources();
        // ReSTIR DI
        createReSTIRDIResources();
        // ReSTIR PT
        createReSTIRPTResources();

        createCommandBuffers();

        // Allocate baseline capacities for dynamic GPU buffers so vectors are NEVER empty
        m_InstanceBufferCapacity = sizeof(shaderio::InstanceData) * 64;
        createInstanceBuffer(m_InstanceBufferCapacity);

        m_IndirectBufferCapacity = sizeof(DrawMeshTasksIndirectCommand) * 64;
        createIndirectBuffer(m_IndirectBufferCapacity);

        m_BoneBufferCapacity = sizeof(glm::mat4) * 64;
        createBoneBuffer(m_BoneBufferCapacity);

        m_PageTableCapacity = 64;
        createPageTableBuffers(m_PageTableCapacity);

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
        createEntityResources();
        createDepthResources();

        // Visability
        createVisibilityResources();

        // G-Buffer
        createGBufferResources();
        createShadowMaskResources();
        createPathTracerResources();
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
            m_resetDLSS = true; // Signals Streamline to flush history cleanly on the next frame without destroying contexts

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
                    m_resetNRD = true;
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
            m_resetNRD = true;

            // Mutual Exclusion: NRD REBLUR/RELAX conflicts with DLSS Ray Reconstruction
            if (mode != NRI::NRDReflectionDenoiser::Off && m_dlssRayReconstructionEnabled)
            {
                m_dlssRayReconstructionEnabled = false;
                m_resetDLSS = true;
                NOX_CORE_INFO("[Denoising] NRD Reflection Denoiser activated: DLSS Ray Reconstruction automatically disabled (mutually exclusive).");
            }
        }
    }

    void Renderer::setNRDGIDenoiser(NRI::NRDDiffuseDenoiser mode)
    {
        if (m_nrdGIDenoiser != mode)
        {
            m_nrdGIDenoiser = mode;
            m_resetNRD = true;

            if (mode != NRI::NRDDiffuseDenoiser::Off && m_dlssRayReconstructionEnabled)
            {
                m_dlssRayReconstructionEnabled = false;
                m_resetDLSS = true;
                NOX_CORE_INFO("[Denoising] NRD GI Denoiser activated: DLSS Ray Reconstruction automatically disabled (mutually exclusive).");
            }
        }
    }

    void Renderer::setNRDDIDenoiser(NRI::NRDDiffuseDenoiser mode)
    {
        if (m_nrdDIDenoiser != mode)
        {
            m_nrdDIDenoiser = mode;
            m_resetNRD = true;

            if (mode != NRI::NRDDiffuseDenoiser::Off && m_dlssRayReconstructionEnabled)
            {
                m_dlssRayReconstructionEnabled = false;
                m_resetDLSS = true;
                NOX_CORE_INFO("[Denoising] NRD DI Denoiser activated: DLSS Ray Reconstruction automatically disabled (mutually exclusive).");
            }
        }
    }

    void Renderer::setNRDPTDenoiser(NRI::NRDDiffuseDenoiser mode)
    {
        if (m_nrdPTDenoiser != mode)
        {
            m_nrdPTDenoiser = mode;
            m_resetNRD = true;

            if (mode != NRI::NRDDiffuseDenoiser::Off && m_dlssRayReconstructionEnabled)
            {
                m_dlssRayReconstructionEnabled = false;
                m_resetDLSS = true;
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
        createEntityResources();
        createDepthResources();

        //Visability
        createVisibilityResources();

        // G-Buffer
        createGBufferResources();
        createShadowMaskResources();
        createPathTracerResources();
        createReSTIRGIResources();
        createReSTIRDIResources();
        createReSTIRPTResources();
        m_pathTracerSampleCount = 0;

        // NGX's internal DLSS feature is fixed-size once created; it must be explicitly freed here
        // so it gets recreated at the new resolution on the next evaluate, otherwise evaluate silently
        // no-ops forever once our tagged resources no longer match the size it was created with.
        m_device->resetDLSSViewport();
        m_resetDLSS = true;
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

    void Renderer::createShadowMaskResources()
    {
        const uint32_t width = m_renderSize.width;
        const uint32_t height = m_renderSize.height;

        if (width == 0 || height == 0)
            return;

        m_rawShadowMask = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_rawShadowMask);

        m_denoisedShadowMask = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_denoisedShadowMask);

        m_viewZ = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_viewZ);

        m_nrdNormalRoughness = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R10G10B10A2_UNORM,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_nrdNormalRoughness);

        m_rawReflection = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_rawReflection);

        m_denoisedReflection = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_denoisedReflection);

        m_device->initNRD(width, height);
        m_resetNRD = true;
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
            NRI::ImageFormat::R16G16B16A16_SFLOAT // m_rawReflection (Radiance RGB + HitDist A)
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

    void Renderer::createPathTracerResources()
    {
        const uint32_t width = m_renderSize.width;
        const uint32_t height = m_renderSize.height;

        for (int i = 0; i < 2; i++)
        {
            m_pathTracerAccum[i] = m_device->createTexture(NRI::TextureDesc{
                .width = width,
                .height = height,
                .mipLevels = 1,
                .sampleCount = 1,
                .usage = NRI::TextureUsage::ColorAttachment,
                .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
                .directFormat = UINT32_MAX
            });
            m_resourceHeap->registerTexture(*m_pathTracerAccum[i]);
        }

        // Storage usage (not ColorAttachment): NRD's DenoiseVK call writes this via its own internal
        // barriers regardless, but the in-place YCoCg decode pass (YCoCgDecodeInPlace.slang, needed
        // only when REBLUR is selected) also needs a UAV write slot on it -- Storage usage already
        // includes eSampled (see TextureVK.cpp), so reading it as a normal bindless texture still works.
        m_denoisedPathTracer = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::Storage,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_denoisedPathTracer, NRI::TextureUsage::ShaderResource);
        m_denoisedPathTracerWriteSlot = m_resourceHeap->registerStorageTextureMip(*m_denoisedPathTracer, 0);
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

    void Renderer::createDDGIResources()
    {
        uint32_t totalProbes = m_ddgiProbeCountX * m_ddgiProbeCountY * m_ddgiProbeCountZ;
        if (totalProbes == 0)
            return;

        uint32_t probesPerRow = 64;
        uint32_t probeRows = (totalProbes + probesPerRow - 1) / probesPerRow;

        if (m_ddgiRayData)
        {
            m_resourceHeap->unregisterTexture(m_ddgiRayData->GetDescriptorIndexSlot());
        }
        for (int i = 0; i < 2; i++)
        {
            if (m_ddgiIrradiance[i])
                m_resourceHeap->unregisterTexture(m_ddgiIrradiance[i]->GetDescriptorIndexSlot());
            if (m_ddgiDistance[i])
                m_resourceHeap->unregisterTexture(m_ddgiDistance[i]->GetDescriptorIndexSlot());
        }

        // 1. Ray Data Buffer (Width = RaysPerProbe, Height = TotalProbes)
        m_ddgiRayData = m_device->createTexture(NRI::TextureDesc{
            .width = m_ddgiRaysPerProbe,
            .height = totalProbes,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_ddgiRayData);

        // 2. Irradiance Atlases (Ping-Pong: 8x8 interior + 2 border = 10x10 per probe)
        uint32_t irrWidth = probesPerRow * 10;
        uint32_t irrHeight = probeRows * 10;
        for (int i = 0; i < 2; i++)
        {
            m_ddgiIrradiance[i] = m_device->createTexture(NRI::TextureDesc{
                .width = irrWidth,
                .height = irrHeight,
                .mipLevels = 1,
                .sampleCount = 1,
                .usage = NRI::TextureUsage::ColorAttachment,
                .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
                .directFormat = UINT32_MAX
            });
            m_resourceHeap->registerTexture(*m_ddgiIrradiance[i]);
        }

        // 3. Distance Atlases (Ping-Pong: 16x16 interior + 2 border = 18x18 per probe)
        uint32_t distWidth = probesPerRow * 18;
        uint32_t distHeight = probeRows * 18;
        for (int i = 0; i < 2; i++)
        {
            m_ddgiDistance[i] = m_device->createTexture(NRI::TextureDesc{
                .width = distWidth,
                .height = distHeight,
                .mipLevels = 1,
                .sampleCount = 1,
                .usage = NRI::TextureUsage::ColorAttachment,
                .format = NRI::ImageFormat::R16G16_SFLOAT,
                .directFormat = UINT32_MAX
            });
            m_resourceHeap->registerTexture(*m_ddgiDistance[i]);
        }

        m_ddgiFirstFrame = true;
    }

    void Renderer::resetDDGIGridToDefaults()
    {
        m_ddgiGridOrigin = glm::vec3(-20.0f, -0.5f, -12.0f);
        m_ddgiGridSpacing = glm::vec3(1.8f, 1.4f, 1.7f);
        m_ddgiHysteresis = 0.97f;
        m_ddgiNormalBias = 0.2f;
        m_ddgiDebugSphereRadius = 0.15f;
        m_ddgiFirstFrame = true;
    }

    void Renderer::createReSTIRGIResources()
    {
        const uint32_t width = m_renderSize.width;
        const uint32_t height = m_renderSize.height;

        if (width == 0 || height == 0)
            return;

        // 1. Output Texture: Raw Resampled Diffuse Irradiance (R16G16B16A16_SFLOAT)
        if (m_restirGIRawDiffuse)
        {
            m_resourceHeap->unregisterTexture(m_restirGIRawDiffuse->GetDescriptorIndexSlot());
        }

        m_restirGIRawDiffuse = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_restirGIRawDiffuse);

        if (m_denoisedReSTIRGIDiffuse)
        {
            m_resourceHeap->unregisterTexture(m_denoisedReSTIRGIDiffuse->GetDescriptorIndexSlot());
        }

        m_denoisedReSTIRGIDiffuse = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_denoisedReSTIRGIDiffuse);

        // 2. Reservoir Buffers (Ping-Pong, Block-Linear 16x16 tiles per RTXDI SDK)
        RTXDI_ReservoirBufferParameters resParams = rtxdi::CalculateReservoirBufferParameters(
            width, height, rtxdi::CheckerboardMode::Off);
        uint64_t reservoirBufferSize = static_cast<uint64_t>(resParams.reservoirArrayPitch) * sizeof(RTXDI_PackedGIReservoir);

        for (int i = 0; i < 2; i++)
        {
            m_restirGIReservoirBuffers[i] = m_device->createBuffer(NRI::BufferDesc{
                .size = reservoirBufferSize,
                .usage = NRI::BufferUsage::Storage
            });
            void* mapped = m_restirGIReservoirBuffers[i]->map(0, reservoirBufferSize);
            memset(mapped, 0, reservoirBufferSize);
            m_restirGIReservoirBuffers[i]->unmap();
        }

        // 3. Neighbor Offsets Buffer (128 offsets within a unit disk: [-1.0, 1.0])
        if (!m_restirGINeighborOffsetsBuffer)
        {
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

    }

    void Renderer::createReSTIRDIResources()
    {
        const uint32_t width = m_renderSize.width;
        const uint32_t height = m_renderSize.height;

        if (width == 0 || height == 0)
            return;

        // 1. Output Texture: Final Resampled Direct Lighting (R16G16B16A16_SFLOAT)
        if (m_restirDIDirectLighting)
        {
            m_resourceHeap->unregisterTexture(m_restirDIDirectLighting->GetDescriptorIndexSlot());
        }

        m_restirDIDirectLighting = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_restirDIDirectLighting);

        if (m_denoisedReSTIRDIDirectLighting)
        {
            m_resourceHeap->unregisterTexture(m_denoisedReSTIRDIDirectLighting->GetDescriptorIndexSlot());
        }

        m_denoisedReSTIRDIDirectLighting = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_denoisedReSTIRDIDirectLighting);

        // 2. Reservoir Buffers: 3 physical buffers, rotated every frame (NOT GI's fixed 2-role
        // scheme -- see the rotation math documented on PushConstantReSTIRDIInitial in shaderIO.h).
        RTXDI_ReservoirBufferParameters resParams = rtxdi::CalculateReservoirBufferParameters(
            width, height, rtxdi::CheckerboardMode::Off);
        uint64_t reservoirBufferSize = static_cast<uint64_t>(resParams.reservoirArrayPitch) * sizeof(RTXDI_PackedDIReservoir);

        for (int i = 0; i < 3; i++)
        {
            m_restirDIReservoirBuffers[i] = m_device->createBuffer(NRI::BufferDesc{
                .size = reservoirBufferSize,
                .usage = NRI::BufferUsage::Storage
            });
            void* mapped = m_restirDIReservoirBuffers[i]->map(0, reservoirBufferSize);
            memset(mapped, 0, reservoirBufferSize);
            m_restirDIReservoirBuffers[i]->unmap();
        }

        m_restirDILastFrameOutputReservoir = 0;

        // 3. RIS Buffer (uint2 per element): [0, risBufferOffset) = plain RIS tiles, [risBufferOffset,
        // end) = ReGIR grid cells. Not render-resolution-dependent -- only (re)created once, like
        // m_restirGINeighborOffsetsBuffer, not on every resize.
        if (!m_restirDIRISBuffer)
        {
            uint32_t risTileElements = m_restirDIRISTileSize * m_restirDIRISTileCount;
            uint32_t regirCellCount = m_regirCellsX * m_regirCellsY * m_regirCellsZ;
            uint32_t regirElements = regirCellCount * m_regirLightsPerCell;
            uint64_t risBufferSize = static_cast<uint64_t>(risTileElements + regirElements) * sizeof(glm::uvec2);

            m_restirDIRISBuffer = m_device->createBuffer(NRI::BufferDesc{
                .size = risBufferSize,
                .usage = NRI::BufferUsage::Storage
            });
            void* mapped = m_restirDIRISBuffer->map(0, risBufferSize);
            memset(mapped, 0, risBufferSize);
            m_restirDIRISBuffer->unmap();
        }

        // 4. Local-light PDF mip chain (feeds RTXDI_PresampleLocalLights) -- also created once, not
        // render-resolution-dependent, exactly like the RIS buffer above.
        if (!m_lightPDFTexture)
        {
            m_lightPDFMipLevels = static_cast<uint32_t>(log2(static_cast<double>(m_lightPDFTextureSize)));

            m_lightPDFTexture = m_device->createTexture(NRI::TextureDesc{
                .width = m_lightPDFTextureSize,
                .height = m_lightPDFTextureSize,
                .mipLevels = m_lightPDFMipLevels,
                .sampleCount = 1,
                .usage = NRI::TextureUsage::Storage,
                .format = NRI::ImageFormat::R16_SFLOAT,
                .directFormat = UINT32_MAX
            });
            m_resourceHeap->registerTexture(*m_lightPDFTexture, NRI::TextureUsage::ShaderResource);

            m_lightPDFMipStorageSlots.clear();
            for (uint32_t mip = 0; mip < m_lightPDFMipLevels; mip++)
            {
                m_lightPDFMipStorageSlots.push_back(m_resourceHeap->registerStorageTextureMip(*m_lightPDFTexture, mip));
            }
        }
    }

    void Renderer::setReSTIRPTTemporalEnabled(bool enabled)
    {
        m_restirPTTemporalEnabled = enabled;
        if (m_restirPTContext)
            m_restirPTContext->SetResamplingMode(enabled ? rtxdi::ReSTIRPT_ResamplingMode::Temporal : rtxdi::ReSTIRPT_ResamplingMode::None);
    }

    void Renderer::createReSTIRPTResources()
    {
        const uint32_t width = m_renderSize.width;
        const uint32_t height = m_renderSize.height;

        if (width == 0 || height == 0)
            return;

        // The real SDK context (Source/ReSTIRPT.cpp, already compiled into the build) owns buffer-index
        // rotation and default parameters -- (re)created here whenever render size changes, mirroring
        // how DI/GI's resize-dependent state gets rebuilt. Slots 0/1 ping-pong for temporal resampling,
        // slot 2 preserves the unresampled initial-sampling reservoir for final shading's decorrelation
        // fallback (see rtxdi::ReSTIRPTContext::UpdateBufferIndices in ReSTIRPT.cpp).
        rtxdi::ReSTIRPTStaticParameters staticParams{};
        staticParams.RenderWidth = width;
        staticParams.RenderHeight = height;
        staticParams.CheckerboardSamplingMode = rtxdi::CheckerboardMode::Off;
        m_restirPTContext = std::make_unique<rtxdi::ReSTIRPTContext>(staticParams);
        // Temporal resampling (RandomReplay/hybrid-shift reconnection, ported into ReSTIRPTTemporal.slang)
        // currently produces a visible lighting-rotation artifact under investigation -- defaults to
        // None here (matching the known-good state) and is toggled via setReSTIRPTTemporalEnabled(),
        // e.g. for a resize-triggered recreation while the toggle was already on.
        m_restirPTContext->SetResamplingMode(m_restirPTTemporalEnabled ? rtxdi::ReSTIRPT_ResamplingMode::Temporal : rtxdi::ReSTIRPT_ResamplingMode::None);

        RTXDI_ReservoirBufferParameters ptResParams = m_restirPTContext->GetReservoirBufferParameters();
        uint64_t ptReservoirBufferSize = static_cast<uint64_t>(ptResParams.reservoirArrayPitch) * sizeof(RTXDI_PackedPTReservoir);

        for (int i = 0; i < 3; i++)
        {
            m_restirPTReservoirBuffers[i] = m_device->createBuffer(NRI::BufferDesc{
                .size = ptReservoirBufferSize,
                .usage = NRI::BufferUsage::Storage
            });
            void* mapped = m_restirPTReservoirBuffers[i]->map(0, ptReservoirBufferSize);
            memset(mapped, 0, ptReservoirBufferSize);
            m_restirPTReservoirBuffers[i]->unmap();
        }

        if (m_restirPTOutput)
        {
            m_resourceHeap->unregisterTexture(m_restirPTOutput->GetDescriptorIndexSlot());
        }

        m_restirPTOutput = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_restirPTOutput);

        if (m_restirPTPrimaryDirect)
        {
            m_resourceHeap->unregisterTexture(m_restirPTPrimaryDirect->GetDescriptorIndexSlot());
        }

        m_restirPTPrimaryDirect = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_restirPTPrimaryDirect);
    }

    void Renderer::createReSTIRPTPipelines(bool forceCompile)
    {
        // 1. Initial Sampling Pipeline (ResamplingMode::None -- generates + RIS-combines
        // numInitialSamples full paths per pixel, no temporal/spatial reuse yet)
        {
            NRI::PipelineDesc desc{};
            desc.forceCompile = forceCompile;
            // 2 targets: SV_Target0 is the debug-only combined-radiance preview, SV_Target1 is the
            // primary-surface direct lighting consumed by the Final Shading pass (see m_restirPTPrimaryDirect).
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

        // 4. Debug Spheres Pipeline (Rendered in Forward 3D pass into m_hdrSceneResource + m_entityResource)
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
        m_commandAllocator = m_device->createCommandAllocator();
    }

    void Renderer::createSceneResources()
    {
        //changed from m_swapChainExtent to m_viewportSize
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

        // HDR scene target for 3D deferred lighting, skybox, and unlit passes - render resolution,
        // this is what DLSS reads as its color input.
        m_hdrSceneResource = m_device->createTexture(NRI::TextureDesc{
            .width = m_renderSize.width,
            .height = m_renderSize.height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_hdrSceneResource);

        // DLSS Super Resolution output target (HDR unresolved before tonemapping)
        m_dlssOutputResource = m_device->createTexture(NRI::TextureDesc{
            .width = m_isEditor ? m_viewportSize.width : m_swapChainExtent.width,
            .height = m_isEditor ? m_viewportSize.height : m_swapChainExtent.height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment, // <-- Must be ColorAttachment so DescriptorHeap registers it as eSampledImage for PostProcess.slang
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_dlssOutputResource);
    }

    void Renderer::createEntityResources()
    {
        const uint32_t outputWidth = m_isEditor ? m_viewportSize.width : m_swapChainExtent.width;
        const uint32_t outputHeight = m_isEditor ? m_viewportSize.height : m_swapChainExtent.height;

        // Render-resolution entity IDs, written directly by the G-buffer/unlit/skybox 3D passes.
        m_entityResource = m_device->createTexture(NRI::TextureDesc{
            .width = m_renderSize.width,
            .height = m_renderSize.height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R32SINT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_entityResource);

        // Display-resolution copy (nearest-upsampled from m_entityResource each frame - see the blit
        // right after DLSS evaluate in RecordCommandBuffer) - what the 2D overlay pass, outline effect,
        // and mouse-pick readback all use so they line up with the final on-screen image.
        m_entityResourceHi = m_device->createTexture(NRI::TextureDesc{
            .width = outputWidth,
            .height = outputHeight,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R32SINT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_entityResourceHi);

        uniformData.imageHeapIndexOffset = m_resourceHeap->getImageHeapIndexOffset();
        uniformData.entityTextureIndex = m_entityResourceHi->GetDescriptorIndexSlot();
        uniformData.entityGBufferTextureIndex = m_entityResource->GetDescriptorIndexSlot();
    }

    void Renderer::createDepthResources()
    {
        const uint32_t outputWidth = m_isEditor ? m_viewportSize.width : m_swapChainExtent.width;
        const uint32_t outputHeight = m_isEditor ? m_viewportSize.height : m_swapChainExtent.height;

        // Render-resolution depth, written by the visibility/G-buffer/unlit/skybox 3D passes and
        // tagged directly to DLSS.
        m_depthResource = m_device->createTexture(NRI::TextureDesc{
            .width = m_renderSize.width,
            .height = m_renderSize.height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::DepthStencilAttachment
        });
        m_resourceHeap->registerTexture(*m_depthResource);

        // Previous-frame depth snapshot for ReSTIR GI temporal reprojection (see declaration comment).
        m_prevDepthResource = m_device->createTexture(NRI::TextureDesc{
            .width = m_renderSize.width,
            .height = m_renderSize.height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::DepthStencilAttachment
        });
        m_resourceHeap->registerTexture(*m_prevDepthResource);

        // Display-resolution copy (nearest-upsampled each frame) used only by the 2D overlay pass so
        // world-space 2D/text content (e.g. in-world signs) still depth-tests correctly against 3D
        // geometry at full display resolution even while DLSS is rendering the 3D scene smaller.
        m_depthResourceHi = m_device->createTexture(NRI::TextureDesc{
            .width = outputWidth,
            .height = outputHeight,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::DepthStencilAttachment
        });
        m_resourceHeap->registerTexture(*m_depthResourceHi);
    }

    void Renderer::createVisibilityResources()
    {
        m_visibilityResource = m_device->createTexture(NRI::TextureDesc{
            .width = m_renderSize.width,
            .height = m_renderSize.height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R32G32_UINT,
            .directFormat = UINT32_MAX
        });

        // Register with resource heap so fullscreen debug/compute passes can read it bindlessly
        m_resourceHeap->registerTexture(*m_visibilityResource);
        uniformData.imageHeapIndexOffset = m_resourceHeap->getImageHeapIndexOffset();
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

    void Renderer::createGBufferResources()
    {
        const uint32_t width = m_renderSize.width;
        const uint32_t height = m_renderSize.height;

        m_gbufferAlbedo = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::RGBA8,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_gbufferAlbedo);

        m_gbufferSpecular = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::RGBA8,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_gbufferSpecular);

        m_gbufferNormal = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_gbufferNormal);

        // Previous-frame normal snapshot for ReSTIR GI temporal reprojection (see declaration comment).
        m_prevGbufferNormal = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_prevGbufferNormal);

        m_gbufferMaterial = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::RGBA8,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_gbufferMaterial);

        // Previous-frame albedo/material snapshots for ReSTIR PT's temporal resampling (see declaration
        // comment) -- ReSTIR GI never needed these since its RAB_Surface has no material fields.
        m_prevGbufferAlbedo = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::RGBA8,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_prevGbufferAlbedo);

        m_prevGbufferMaterial = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::RGBA8,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_prevGbufferMaterial);

        m_gbufferEmission = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R16G16B16A16_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_gbufferEmission);

        m_gbufferVelocity = m_device->createTexture(NRI::TextureDesc{
            .width = width,
            .height = height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R32G32_SFLOAT,
            .directFormat = UINT32_MAX
        });
        m_resourceHeap->registerTexture(*m_gbufferVelocity);

        uniformData.imageHeapIndexOffset = m_resourceHeap->getImageHeapIndexOffset();
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
        std::unique_ptr<NRI::Buffer> stagingBuffer = m_device->createBuffer(NRI::BufferDesc{
            .size = cpuData.Data.Size,
            .usage = NRI::BufferUsage::Staging
        });

        void* data = stagingBuffer->map(0, cpuData.Data.Size);
        memcpy(data, cpuData.Data.Data, cpuData.Data.Size);
        stagingBuffer->unmap();

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

        std::unique_ptr<NRI::CommandBuffer> commandBuffer = beginSingleTimeCommands();
        textureResource->uploadFromBuffer(*commandBuffer, *stagingBuffer, cpuData.Width, cpuData.Height, cpuData.MipLevels, cpuData.MipOffsets);
        endSingleTimeCommands(std::move(commandBuffer));

        m_resourceHeap->registerTexture(*textureResource);
        uniformData.imageHeapIndexOffset = m_resourceHeap->getImageHeapIndexOffset();

        return textureResource;
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

        std::unique_ptr<NRI::CommandBuffer> prefCmd = beginSingleTimeCommands();
        prefCmd->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());

        prefCmd->bindPipeline(NRI::PipelineBindPoint::Compute, *prefilterPipeline);

        std::vector<uint32_t> tempMipSlots;
        // Loop through each roughness mip level
        for (uint32_t mip = 0; mip < prefilterCubeMipLevels; ++mip)
        {
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
        }

        endSingleTimeCommands(std::move(prefCmd));
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

        uint64_t bufferSize = sizeof(T) * elementCount;
        uint64_t dstByteOffset = sizeof(T) * elementOffset;

        if (m_uploadBatch.depth > 0)
        {
            uploadBatchCopy(dstBuffer, data, bufferSize, dstByteOffset);
            return;
        }

        std::unique_ptr<NRI::Buffer> stagingBuffer = m_device->createBuffer(NRI::BufferDesc{
            .size = bufferSize,
            .usage = NRI::BufferUsage::Staging
        });

        void* mappedMemory = stagingBuffer->map(0, bufferSize);
        memcpy(mappedMemory, data, bufferSize);
        stagingBuffer->unmap();

        std::unique_ptr<NRI::CommandBuffer> cmd = beginSingleTimeCommands();
        // Construct the copy region struct
        NRI::BufferCopyRegion copyRegion
        {
            .srcOffset = 0,
            .dstOffset = dstByteOffset,
            .size = bufferSize
        };
        cmd->copyBuffer(*stagingBuffer, dstBuffer, copyRegion);
        endSingleTimeCommands(std::move(cmd));
    }

    // --- Upload batching ---
    // Staging and work budgets: bound host memory held by a batch, and keep each submission's GPU
    // work short (a single submit building thousands of BLASes could exceed the driver timeout).
    static constexpr uint64_t UPLOAD_BATCH_STAGING_BYTES = 64ull * 1024 * 1024;
    static constexpr uint64_t UPLOAD_BATCH_RETAINED_BYTES = 256ull * 1024 * 1024;
    static constexpr uint64_t UPLOAD_BATCH_MAX_PRIMITIVES = 2'000'000;
    static constexpr uint32_t UPLOAD_BATCH_MAX_BUILDS = 512;

    NRI::CommandBuffer& Renderer::uploadBatchCommandBuffer()
    {
        if (!m_uploadBatch.commandBuffer)
            m_uploadBatch.commandBuffer = beginSingleTimeCommands();
        return *m_uploadBatch.commandBuffer;
    }

    void Renderer::uploadBatchCopy(NRI::Buffer& dstBuffer, const void* data, uint64_t byteSize, uint64_t dstByteOffset)
    {
        if (byteSize > UPLOAD_BATCH_STAGING_BYTES)
        {
            std::unique_ptr<NRI::Buffer> oversizedStaging = m_device->createBuffer(NRI::BufferDesc{
                .size = byteSize,
                .usage = NRI::BufferUsage::Staging
            });
            void* mapped = oversizedStaging->map(0, byteSize);
            memcpy(mapped, data, byteSize);
            oversizedStaging->unmap();

            uploadBatchCommandBuffer().copyBuffer(*oversizedStaging, dstBuffer, NRI::BufferCopyRegion{
                .srcOffset = 0,
                .dstOffset = dstByteOffset,
                .size = byteSize
            });
            m_uploadBatch.retainedBuffers.push_back(std::move(oversizedStaging));
            m_uploadBatch.retainedBytes += byteSize;
            if (m_uploadBatch.retainedBytes > UPLOAD_BATCH_RETAINED_BYTES)
                flushUploadBatch();
            return;
        }

        // Recorded copies read the staging buffer at submit time, so it can only be reused or
        // replaced after a flush. It starts at what's needed and grows toward the cap: allocating the
        // full 64MB up front made every single small mesh upload slower than before batching.
        if (m_uploadBatch.stagingUsed + byteSize > m_uploadBatch.stagingCapacity)
        {
            flushUploadBatch();

            if (byteSize > m_uploadBatch.stagingCapacity || m_uploadBatch.stagingCapacity < UPLOAD_BATCH_STAGING_BYTES)
            {
                const uint64_t newCapacity = std::min(UPLOAD_BATCH_STAGING_BYTES,
                    std::max({ byteSize, m_uploadBatch.stagingCapacity * 2, uint64_t(1024 * 1024) }));

                if (m_uploadBatch.staging)
                    m_uploadBatch.staging->unmap();
                m_uploadBatch.staging = m_device->createBuffer(NRI::BufferDesc{
                    .size = newCapacity,
                    .usage = NRI::BufferUsage::Staging
                });
                m_uploadBatch.stagingMapped = static_cast<uint8_t*>(m_uploadBatch.staging->map(0, newCapacity));
                m_uploadBatch.stagingCapacity = newCapacity;
            }
        }

        memcpy(m_uploadBatch.stagingMapped + m_uploadBatch.stagingUsed, data, byteSize);
        uploadBatchCommandBuffer().copyBuffer(*m_uploadBatch.staging, dstBuffer, NRI::BufferCopyRegion{
            .srcOffset = m_uploadBatch.stagingUsed,
            .dstOffset = dstByteOffset,
            .size = byteSize
        });
        m_uploadBatch.stagingUsed += byteSize;
    }

    void Renderer::flushUploadBatch()
    {
        if (!m_uploadBatch.commandBuffer)
            return;

        endSingleTimeCommands(std::move(m_uploadBatch.commandBuffer));
        m_uploadBatch.commandBuffer.reset();

        m_uploadBatch.stagingUsed = 0;
        m_uploadBatch.retainedBuffers.clear();
        m_uploadBatch.retainedBytes = 0;
        m_uploadBatch.pendingPrimitives = 0;
        m_uploadBatch.pendingBuilds = 0;
    }

    void Renderer::endUploadBatch()
    {
        if (m_uploadBatch.depth == 0 || --m_uploadBatch.depth > 0)
            return;

        flushUploadBatch();

        if (m_uploadBatch.staging)
            m_uploadBatch.staging->unmap();
        m_uploadBatch.staging.reset();
        m_uploadBatch.stagingMapped = nullptr;
        m_uploadBatch.stagingCapacity = 0;
        m_uploadBatch.scratch.reset();
        m_uploadBatch.scratchCapacity = 0;
    }

    void Renderer::initGeometryBuffers()
    {
        constexpr uint32_t INITIAL_VERTICES = 100'000;
        constexpr uint32_t INITIAL_DRAWS = 50'000;
        constexpr uint32_t INITIAL_VERTS = 500'000;
        constexpr uint32_t INITIAL_TRIS = 1'500'000;

        m_vertexPages.Init(m_device.get(), INITIAL_VERTICES);
        m_meshletDrawPages.Init(m_device.get(), INITIAL_DRAWS);
        m_meshletBoundsPages.Init(m_device.get(), INITIAL_DRAWS);
        m_meshletVertPages.Init(m_device.get(), INITIAL_VERTS);
        m_meshletTriPages.Init(m_device.get(), INITIAL_TRIS);
    }

    void Renderer::markPageTablesDirty()
    {
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            m_pageTablesDirty[i] = true;
        }
    }

    MeshHandle Renderer::UploadMeshGeometry(const MeshData& data, bool isOpaque)
    {
        MeshHandle handle{};

        uint32_t vertCount = static_cast<uint32_t>(data.Vertices.size());
        uint32_t drawCount = static_cast<uint32_t>(data.Draws.size());
        uint32_t meshVertCount = static_cast<uint32_t>(data.MeshletVertices.size());
        uint32_t meshTriCount = static_cast<uint32_t>(data.MeshletTriangles.size());

        // Allocate across pages (creates a new page if full or oversized)
        PageAllocation vertAlloc = m_vertexPages.Allocate(vertCount);
        PageAllocation drawAlloc = m_meshletDrawPages.Allocate(drawCount);
        PageAllocation boundAlloc = m_meshletBoundsPages.Allocate(drawCount);
        PageAllocation mvertAlloc = m_meshletVertPages.Allocate(meshVertCount);
        PageAllocation mtriAlloc = m_meshletTriPages.Allocate(meshTriCount);

        handle.vertices = {vertAlloc.pageIndex, vertAlloc.offset, vertAlloc.count};
        handle.meshletDraws = {drawAlloc.pageIndex, drawAlloc.offset, drawAlloc.count};
        handle.meshletVertices = {mvertAlloc.pageIndex, mvertAlloc.offset, mvertAlloc.count};
        handle.meshletTriangles = {mtriAlloc.pageIndex, mtriAlloc.offset, mtriAlloc.count};

        // Patch meshlet local offsets
        std::vector<shaderio::MeshletDraw> adjustedDraws = data.Draws;
        for (auto& draw : adjustedDraws)
        {
            draw.vertexOffset += handle.meshletVertices.offset;
            draw.triangleOffset += handle.meshletTriangles.offset;
            draw.globalVertexOffset += handle.vertices.offset;
        }

        // Upload slice directly to target page buffers
        UploadBufferSlice(*m_vertexPages.GetBuffer(vertAlloc.pageIndex), data.Vertices.data(), vertAlloc.offset, vertAlloc.count);
        UploadBufferSlice(*m_meshletDrawPages.GetBuffer(drawAlloc.pageIndex), adjustedDraws.data(), drawAlloc.offset, drawAlloc.count);
        UploadBufferSlice(*m_meshletBoundsPages.GetBuffer(boundAlloc.pageIndex), data.Bounds.data(), boundAlloc.offset, boundAlloc.count);
        UploadBufferSlice(*m_meshletVertPages.GetBuffer(mvertAlloc.pageIndex), data.MeshletVertices.data(), mvertAlloc.offset, mvertAlloc.count);
        UploadBufferSlice(*m_meshletTriPages.GetBuffer(mtriAlloc.pageIndex), data.MeshletTriangles.data(), mtriAlloc.offset, mtriAlloc.count);

        // --- Hardware Ray Tracing: Build BLAS ---
        if (vertCount > 0 && !data.Draws.empty())
        {
            // 1. Reconstruct flat index buffer from meshlets (compatible with glTF, OBJ, and .nmesh)
            std::vector<uint32_t> indices;
            for (const auto& draw : data.Draws)
            {
                for (uint32_t t = 0; t < draw.triangleCount; t++)
                {
                    uint32_t triBase = draw.triangleOffset + t * 3;
                    uint32_t i0 = data.MeshletVertices[draw.vertexOffset + data.MeshletTriangles[triBase + 0]];
                    uint32_t i1 = data.MeshletVertices[draw.vertexOffset + data.MeshletTriangles[triBase + 1]];
                    uint32_t i2 = data.MeshletVertices[draw.vertexOffset + data.MeshletTriangles[triBase + 2]];
                    indices.push_back(i0);
                    indices.push_back(i1);
                    indices.push_back(i2);
                }
            }

            if (!indices.empty())
            {
                // 2. Upload dedicated index buffer for AS build and shader lookup
                uint64_t indexBufferSize = sizeof(uint32_t) * indices.size();
                std::unique_ptr<NRI::Buffer> indexBuffer = m_device->createBuffer(NRI::BufferDesc{
                    .size = indexBufferSize,
                    .usage = NRI::BufferUsage::Index
                });
                UploadBufferSlice(*indexBuffer, indices.data(), 0, static_cast<uint32_t>(indices.size()));

                // 3. Query BLAS build sizes
                uint64_t vertexBufferBDA = m_vertexPages.GetBuffer(vertAlloc.pageIndex)->getDeviceAddress() + (sizeof(shaderio::Vertex) * vertAlloc.offset);

                NRI::AccelerationStructureBuildDesc buildDesc{
                    .type = NRI::AccelerationStructureType::BottomLevel,
                    .flags = NRI::AccelerationStructureBuildFlags::PreferFastTrace,
                    .triangles = {
                        NRI::AccelerationStructureTrianglesDesc{
                            .vertexBufferAddress = vertexBufferBDA,
                            .vertexStride = sizeof(shaderio::Vertex),
                            .maxVertex = vertCount - 1,
                            .indexBufferAddress = indexBuffer->getDeviceAddress(),
                            .primitiveCount = static_cast<uint32_t>(indices.size() / 3),
                            .primitiveOffset = 0,
                            .firstVertex = 0,
                            .isOpaque = isOpaque
                        }
                    }
                };

                NRI::AccelerationStructureBuildSizes buildSizes = m_device->getAccelerationStructureBuildSizes(buildDesc);

                // 4. Create storage buffer and BLAS
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

                // 5. Build on GPU
                if (m_uploadBatch.depth > 0)
                {
                    // Builds recorded one after another in the same command buffer can share one
                    // scratch buffer. Growing it must flush first: recorded builds use the current one.
                    if (buildSizes.buildScratchSize > m_uploadBatch.scratchCapacity)
                    {
                        flushUploadBatch();
                        m_uploadBatch.scratchCapacity = std::max<uint64_t>(buildSizes.buildScratchSize, m_uploadBatch.scratchCapacity * 2);
                        m_uploadBatch.scratch = m_device->createBuffer(NRI::BufferDesc{
                            .size = m_uploadBatch.scratchCapacity,
                            .usage = NRI::BufferUsage::AccelerationStructureScratch
                        });
                    }

                    // This mesh's vertex/index copies were recorded earlier in this same command buffer.
                    NRI::CommandBuffer& cmd = uploadBatchCommandBuffer();
                    cmd.accelerationStructureBarrier(NRI::AccelerationStructureBarrierType::TransferToBuild);
                    cmd.buildAccelerationStructure(buildDesc, m_uploadBatch.scratch->getDeviceAddress(), *blas);
                    cmd.accelerationStructureBarrier(NRI::AccelerationStructureBarrierType::BuildToBuild);

                    m_uploadBatch.pendingPrimitives += indices.size() / 3;
                    ++m_uploadBatch.pendingBuilds;
                }
                else
                {
                    std::unique_ptr<NRI::Buffer> scratchBuffer = m_device->createBuffer(NRI::BufferDesc{
                        .size = buildSizes.buildScratchSize,
                        .usage = NRI::BufferUsage::AccelerationStructureScratch
                    });

                    std::unique_ptr<NRI::CommandBuffer> cmd = beginSingleTimeCommands();
                    cmd->buildAccelerationStructure(buildDesc, scratchBuffer->getDeviceAddress(), *blas);
                    cmd->accelerationStructureBarrier(NRI::AccelerationStructureBarrierType::BuildToBuild);
                    endSingleTimeCommands(std::move(cmd));
                }

                // 6. Cache BLAS and store ID in handle (reusing ids released by unloaded meshes)
                MeshBLAS meshBLAS{
                    .storageBuffer = std::move(asBuffer),
                    .as = std::move(blas),
                    .indexBuffer = std::move(indexBuffer),
                    .vertexBufferAddress = vertexBufferBDA,
                    .indexCount = static_cast<uint32_t>(indices.size())
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
        }

        // Submit once this batch holds enough BLAS work (all resources above are owned by now).
        if (m_uploadBatch.depth > 0 &&
            (m_uploadBatch.pendingPrimitives >= UPLOAD_BATCH_MAX_PRIMITIVES || m_uploadBatch.pendingBuilds >= UPLOAD_BATCH_MAX_BUILDS))
        {
            flushUploadBatch();
        }

        markPageTablesDirty();

        return handle;
    }

    void Renderer::UnloadMeshGeometry(const MeshHandle& handle)
    {
        if (!handle.IsValid()) return;

        // Defer returning offsets so current frames in flight finish reading
        m_deferredMeshFrees.push_back({
            .handle = handle,
            .framesRemaining = MAX_FRAMES_IN_FLIGHT
        });

        markPageTablesDirty();
    }

    void Renderer::createPageTableBuffers(uint64_t elementCapacity)
    {
        uint64_t bufferSize = elementCapacity * sizeof(uint64_t);

        auto initMappedBuffer = [&](std::vector<std::unique_ptr<NRI::Buffer>>& buffers, std::vector<void*>& mapped)
        {
            buffers.clear();
            mapped.clear();
            buffers.reserve(MAX_FRAMES_IN_FLIGHT);
            mapped.reserve(MAX_FRAMES_IN_FLIGHT);

            for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
            {
                std::unique_ptr<NRI::Buffer> buf = m_device->createBuffer(NRI::BufferDesc{
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::Storage // Just like your instance buffers
                });
                mapped.push_back(buf->map(0, bufferSize));
                buffers.push_back(std::move(buf));
            }
        };

        initMappedBuffer(m_vertexPageTableBuffers, m_vertexPageTableBuffersMapped);
        initMappedBuffer(m_meshletDrawPageTableBuffers, m_meshletDrawPageTableBuffersMapped);
        initMappedBuffer(m_meshletBoundPageTableBuffers, m_meshletBoundPageTableBuffersMapped);
        initMappedBuffer(m_meshletVertPageTableBuffers, m_meshletVertPageTableBuffersMapped);
        initMappedBuffer(m_meshletTriPageTableBuffers, m_meshletTriPageTableBuffersMapped);
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
                    m_deferredBufferDeletions.push_back({
                        std::move(oldBuffer),
                        frameIndex + MAX_FRAMES_IN_FLIGHT
                    });
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

    void Renderer::updatePageTables(uint32_t currentImage)
    {
        // SKIP entirely if nothing has changed!
        if (!m_pageTablesDirty[currentImage])
        {
            return;
        }

        // Find the maximum page count across all allocators to ensure capacity
        uint64_t maxPagesRequired = std::max({
            m_vertexPages.GetPageCount(),
            m_meshletDrawPages.GetPageCount(),
            m_meshletBoundsPages.GetPageCount(),
            m_meshletVertPages.GetPageCount(),
            m_meshletTriPages.GetPageCount()
        });

        if (maxPagesRequired == 0)
        {
            m_pageTablesDirty[currentImage] = false;
            return;
        }

        // Resize if we don't have enough capacity
        if (maxPagesRequired > m_PageTableCapacity || m_vertexPageTableBuffers.empty())
        {
            m_PageTableCapacity = maxPagesRequired * 2; // double it to avoid frequent resizes

            // Note: Just like your instance buffers, you should add old buffers to m_deferredBufferDeletions here

            createPageTableBuffers(m_PageTableCapacity);
        }

        // Helper to gather BDAs and memcpy them directly into mapped memory
        auto uploadBDAs = [](const auto& pageAllocator, void* mappedPtr)
        {
            uint32_t count = pageAllocator.GetPageCount();
            if (count == 0) return;

            std::vector<uint64_t> bdas;
            bdas.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                NRI::Buffer* page = pageAllocator.GetBuffer(static_cast<uint32_t>(i));
                bdas.push_back(page ? page->getDeviceAddress() : 0);
            }

            // Instant memcpy, no command buffers, no blocking sync!
            memcpy(mappedPtr, bdas.data(), count * sizeof(uint64_t));
        };

        uploadBDAs(m_vertexPages, m_vertexPageTableBuffersMapped[currentImage]);
        uploadBDAs(m_meshletDrawPages, m_meshletDrawPageTableBuffersMapped[currentImage]);
        uploadBDAs(m_meshletBoundsPages, m_meshletBoundPageTableBuffersMapped[currentImage]);
        uploadBDAs(m_meshletVertPages, m_meshletVertPageTableBuffersMapped[currentImage]);
        uploadBDAs(m_meshletTriPages, m_meshletTriPageTableBuffersMapped[currentImage]);

        // Mark as clean for this frame
        m_pageTablesDirty[currentImage] = false;
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

    void Renderer::createInstanceBuffer(uint64_t bufferSize)
    {
        m_instanceBuffers.clear();
        m_instanceBuffersMapped.clear();

        // Reserve memory in vectors to prevent reallocation overhead
        m_instanceBuffers.reserve(MAX_FRAMES_IN_FLIGHT);
        m_instanceBuffersMapped.reserve(MAX_FRAMES_IN_FLIGHT);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            std::unique_ptr<NRI::Buffer> uboBuffer = m_device->createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::Storage
                });

            void* mappedMemory = uboBuffer->map(0, bufferSize);

            m_instanceBuffers.emplace_back(std::move(uboBuffer));
            m_instanceBuffersMapped.emplace_back(mappedMemory);
        }
    }

    void Renderer::createIndirectBuffer(uint64_t bufferSize)
    {
        m_indirectBuffers.clear();
        m_indirectBuffersMapped.clear();

        // Reserve memory in vectors to prevent reallocation overhead
        m_indirectBuffers.reserve(MAX_FRAMES_IN_FLIGHT);
        m_indirectBuffersMapped.reserve(MAX_FRAMES_IN_FLIGHT);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            std::unique_ptr<NRI::Buffer> uboBuffer = m_device->createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::Indirect
                });

            void* mappedMemory = uboBuffer->map(0, bufferSize);

            m_indirectBuffers.emplace_back(std::move(uboBuffer));
            m_indirectBuffersMapped.emplace_back(mappedMemory);
        }
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
            .maxImageDescriptors = 1000
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
        m_device->submitAndWait(*commandBuffer, 0);
    }

    void Renderer::createCommandBuffers()
    {
        m_commandBuffers = m_commandAllocator->allocateCommandBuffer(MAX_FRAMES_IN_FLIGHT);
    }

    void Renderer::recordCommandBuffer(uint32_t imageIndex)
    {
        m_commandBuffers->begin(frameIndex, false);

        // Reads this slot's previous GPU timings and resets its timestamp queries (must precede any rendering).
        NOX_PROFILE_GPU_FRAME_BEGIN(*m_commandBuffers, frameIndex);

        m_commandBuffers->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());

        {
            // Hardware Ray Tracing: Record TLAS build/update commands on GPU
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "TLAS Build");
            BuildSceneAccelerationStructure(frameIndex);
        }

        m_commandBuffers->transitionSwapchainLayout(*m_swapChain, imageIndex, NRI::TextureLayout::Undefined, NRI::TextureLayout::ColorAttachment);

        // -------------------------------------------------------------
        // Shared Indirect Meshlet Drawing State (Used by Pass 1 & Pass 4)
        // -------------------------------------------------------------
        const uint32_t cmdStride = sizeof(DrawMeshTasksIndirectCommand);
        const uint32_t instanceStride = sizeof(shaderio::InstanceData);
        const uint64_t baseInstanceAddress = (!m_instanceBuffers.empty() && frameIndex < m_instanceBuffers.size() && m_instanceBuffers[frameIndex])
                                                 ? m_instanceBuffers[frameIndex]->getDeviceAddress()
                                                 : 0;

        uint64_t currentCmdOffset = 0;
        uint64_t currentInstanceOffset = 0;

        shaderio::PushConstantMeshlets references{};
        if (baseInstanceAddress != 0)
        {
            references.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
            references.instanceReference = baseInstanceAddress;
            bool hasBoneBuffers = frameIndex < m_boneBuffers.size() && m_boneBuffers[frameIndex] != nullptr;
            bool hasBones = !m_boneMatrices.empty();
            references.boneMatrixReference = (hasBones && hasBoneBuffers) ? m_boneBuffers[frameIndex]->getDeviceAddress() : 0;

            bool hasPageTables = frameIndex < m_vertexPageTableBuffers.size() && m_vertexPageTableBuffers[frameIndex] != nullptr;
            references.vertexPageTableReference = hasPageTables ? m_vertexPageTableBuffers[frameIndex]->getDeviceAddress() : 0;
            references.meshletBoundsPageTableReference = (hasPageTables && frameIndex < m_meshletBoundPageTableBuffers.size() && m_meshletBoundPageTableBuffers[frameIndex])
                                                             ? m_meshletBoundPageTableBuffers[frameIndex]->getDeviceAddress()
                                                             : 0;
            references.meshletDrawsPageTableReference = (hasPageTables && frameIndex < m_meshletDrawPageTableBuffers.size() && m_meshletDrawPageTableBuffers[frameIndex])
                                                            ? m_meshletDrawPageTableBuffers[frameIndex]->getDeviceAddress()
                                                            : 0;
            references.meshletVerticesPageTableReference = (hasPageTables && frameIndex < m_meshletVertPageTableBuffers.size() && m_meshletVertPageTableBuffers[frameIndex])
                                                               ? m_meshletVertPageTableBuffers[frameIndex]->getDeviceAddress()
                                                               : 0;
            references.meshletTrianglesPageTableReference = (hasPageTables && frameIndex < m_meshletTriPageTableBuffers.size() && m_meshletTriPageTableBuffers[frameIndex])
                                                                ? m_meshletTriPageTableBuffers[frameIndex]->getDeviceAddress()
                                                                : 0;
        }

        NRI::Pipeline* boundPipeline = nullptr;

        auto drawPass = [&](uint32_t count, NRI::Pipeline& pipeline, NRI::CullMode cullMode, bool depthWrite, bool blendEnable)
        {
            if (count == 0 || frameIndex >= m_indirectBuffers.size() || !m_indirectBuffers[frameIndex]) return;

            references.instanceReference = baseInstanceAddress;
            references.instanceBaseIndex = static_cast<uint32_t>(currentInstanceOffset);
            m_commandBuffers->pushData(&references, sizeof(shaderio::PushConstantMeshlets));

            if (boundPipeline != &pipeline)
            {
                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, pipeline);
                boundPipeline = &pipeline;
            }

            m_commandBuffers->setCullMode(cullMode);
            m_commandBuffers->setDepthWriteEnable(depthWrite);
            m_commandBuffers->setColorBlendEnable(0, blendEnable);

            m_commandBuffers->drawMeshTasksIndirect(*m_indirectBuffers[frameIndex], currentCmdOffset, count, cmdStride);

            currentCmdOffset += static_cast<uint64_t>(count) * cmdStride;
            currentInstanceOffset += count;
        };

        std::vector<NRI::RenderAttachDesc> colorAttachments;
        // 1. VISIBILITY BUFFER TARGET (R32G32_UINT)
        colorAttachments.push_back({
            .attachment = m_visibilityResource.get(),
            .loadOP = NRI::LoadOP::clear,
            .storeOP = NRI::StoreOP::store,
            .clearColor = {0.0f, 0.0f, 0.0f, 0.0f}
        });

        NRI::RenderAttachDesc depthAttachment =
        {
            .attachment = m_depthResource.get(),
            .loadOP = NRI::LoadOP::clear,
            .storeOP = NRI::StoreOP::store, // store depth so subsequent passes can read it
            .clearDepth = {0.0f, 0} // Reverse-Z
        };

        // renderExtent/rw/rh: render resolution - what DLSS actually upscales from. Used by every
        // pass through section 4 (visibility, G-buffer, lighting, unlit/skybox forward) plus the DLSS
        // evaluate itself. outputExtent/ow/oh (computed further down, before section 5) is the final
        // display/swapchain resolution used by everything after DLSS.
        NRI::Extent2D renderExtent = m_renderSize;
        float rw = static_cast<float>(renderExtent.width);
        float rh = static_cast<float>(renderExtent.height);

        NRI::RenderDesc desc =
        {
            .renderArea = renderExtent,
            .colorAttachments = colorAttachments,
            .depthAttachment = depthAttachment
        };
        NOX_PROFILE_GPU_BEGIN(*m_commandBuffers, "Visibility");
        m_commandBuffers->beginRendering(desc);

        // Viewport / scissor (counts and values are both dynamic).
        m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
        m_commandBuffers->setScissorWithCount(renderExtent);

        /*
        // Vertex input empty since we use vertex fetch BDA but still needs to be called empty
        m_commandBuffers->setVertexInput();
        */

        /*// Input assembly.
        m_commandBuffers->setPrimitiveTopology(NRI::PrimitiveTopology::TriangleList);
        m_commandBuffers->setPrimitiveRestartEnable(false);*/

        // Rasterization (most of these come from VK_EXT_extended_dynamic_state_3).
        m_commandBuffers->setRasterizerDiscardEnable(false);
        m_commandBuffers->setPolygonMode(NRI::PolygonMode::Fill);
        m_commandBuffers->setCullMode(NRI::CullMode::Back);
        m_commandBuffers->setFrontFace(NRI::FrontFace::CounterClockWise);
        m_commandBuffers->setDepthBiasEnable(false);
        m_commandBuffers->setDepthClampEnable(false); //LineWidth maybe ?
        m_commandBuffers->setLineWidth(1.0f);

        // Multisampling.
        uint32_t sampleCount = 1;
        m_commandBuffers->setRasterizationSamples(sampleCount);
        const uint32_t sampleMask = 0xFFFFFFFF;
        m_commandBuffers->setSampleMask(sampleCount, sampleMask);
        m_commandBuffers->setAlphaToCoverageEnable(false);
        // alphaToOne is required by the spec when its device feature is enabled and a
        // shader object is bound, even if we don't actually use it.
        m_commandBuffers->setAlphaToOneEnableEXT(false);

        // Depth / stencil.
        m_commandBuffers->setDepthTestEnable(true);
        m_commandBuffers->setDepthWriteEnable(true);
        m_commandBuffers->setDepthCompareOp(NRI::CompareOp::Greater);
        m_commandBuffers->setDepthBoundsTestEnable(false);
        m_commandBuffers->setStencilTestEnable(false);

        // Color blend (for one color attachment). Match the previous pipeline's
        // alpha-blend setup; nothing varies between draws so we set it once.
        {
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
            m_commandBuffers->setColorBlendEnable(0, false);
            m_commandBuffers->setColorBlendEquation(0, blendEquation);
            m_commandBuffers->setColorWriteMask(0, colorWriteMask);
        }

        m_commandBuffers->setLogicOpEnable(false);

        if ((!m_instanceBufferObjects.empty() || !m_drawMeshTasksIndirectCommands.empty()) && m_visibilityPipeline)
        {
            // =========================================================================
            // 1. ALL PBR OPAQUE & MASK (Rasterizes to Visibility Buffer & Depth)
            // =========================================================================
            drawPass(m_opaqueCount, *m_visibilityPipeline, NRI::CullMode::Back, true, false);
            drawPass(m_opaqueDoubleSidedCount, *m_visibilityPipeline, NRI::CullMode::None, true, false);
            drawPass(m_maskCount, *m_visibilityPipeline, NRI::CullMode::Back, true, false);
            drawPass(m_maskDoubleSidedCount, *m_visibilityPipeline, NRI::CullMode::None, true, false);

            // Restore defaults for subsequent passes
            m_commandBuffers->setCullMode(NRI::CullMode::Back);
            m_commandBuffers->setDepthWriteEnable(true);
            m_commandBuffers->setColorBlendEnable(0, false);
        }

        m_commandBuffers->endRendering();
        m_commandBuffers->executionBarrier();
        NOX_PROFILE_GPU_END(*m_commandBuffers);

        // =========================================================================
        // 2. G-BUFFER MATERIAL GENERATION PASS (Decoupled Material Resolve)
        // Only run if there are active meshes in the scene!
        // =========================================================================
        if (m_gbufferPipeline && baseInstanceAddress != 0)
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "G-Buffer");
            std::vector<NRI::RenderAttachDesc> gbufferAttachments;
            // 0. Albedo (RGBA8)
            gbufferAttachments.push_back({
                .attachment = m_gbufferAlbedo.get(),
                .loadOP = NRI::LoadOP::clear,
                .storeOP = NRI::StoreOP::store,
                .clearColor = {0.0f, 0.0f, 0.0f, 0.0f}
            });
            // 1. World Normal (R16G16B16A16_SFLOAT)
            gbufferAttachments.push_back({
                .attachment = m_gbufferNormal.get(),
                .loadOP = NRI::LoadOP::clear,
                .storeOP = NRI::StoreOP::store,
                .clearColor = {0.0f, 0.0f, 0.0f, 0.0f}
            });
            // 2. Material (RGBA8: Roughness, Metallic, Workflow)
            gbufferAttachments.push_back({
                .attachment = m_gbufferMaterial.get(),
                .loadOP = NRI::LoadOP::clear,
                .storeOP = NRI::StoreOP::store,
                .clearColor = {0.0f, 0.0f, 0.0f, 0.0f}
            });
            // 3. Emission (R16G16B16A16_SFLOAT)
            gbufferAttachments.push_back({
                .attachment = m_gbufferEmission.get(),
                .loadOP = NRI::LoadOP::clear,
                .storeOP = NRI::StoreOP::store,
                .clearColor = {0.0f, 0.0f, 0.0f, 0.0f}
            });
            // 4. Entity ID (R32SINT)
            gbufferAttachments.push_back({
                .attachment = m_entityResource.get(),
                .loadOP = NRI::LoadOP::clear,
                .storeOP = NRI::StoreOP::store,
                .clearColor = {-1.0f, 0.0f, 0.0f, 0.0f}
            });
            // 5. Velocity (R16G16_SFLOAT)
            gbufferAttachments.push_back({
                .attachment = m_gbufferVelocity.get(),
                .loadOP = NRI::LoadOP::clear,
                .storeOP = NRI::StoreOP::store,
                .clearColor = {0.0f, 0.0f, 0.0f, 0.0f}
            });
            // 6. Specular Albedo (RGBA8)
            gbufferAttachments.push_back({
                .attachment = m_gbufferSpecular.get(),
                .loadOP = NRI::LoadOP::clear,
                .storeOP = NRI::StoreOP::store,
                .clearColor = {0.0f, 0.0f, 0.0f, 0.0f}
            });

            NRI::RenderDesc gbufferDesc = {
                .renderArea = renderExtent,
                .colorAttachments = gbufferAttachments
            };

            m_commandBuffers->beginRendering(gbufferDesc);

            m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
            m_commandBuffers->setScissorWithCount(renderExtent);

            m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_gbufferPipeline);
            m_commandBuffers->setCullMode(NRI::CullMode::None);
            m_commandBuffers->setDepthTestEnable(false);
            m_commandBuffers->setDepthWriteEnable(false);

            for (uint32_t a = 0; a < gbufferAttachments.size(); ++a)
            {
                m_commandBuffers->setColorBlendEnable(a, false);
                m_commandBuffers->setColorWriteMask(a, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);
            }

            shaderio::PushConstantVisibilityDebug gbufferPush{};
            gbufferPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
            gbufferPush.instanceReference = baseInstanceAddress;

            bool hasBoneBuffers = frameIndex < m_boneBuffers.size() && m_boneBuffers[frameIndex] != nullptr;
            bool hasBones = !m_boneMatrices.empty();
            gbufferPush.boneMatrixReference = (hasBones && hasBoneBuffers) ? m_boneBuffers[frameIndex]->getDeviceAddress() : 0;

            bool hasPageTables = frameIndex < m_vertexPageTableBuffers.size() && m_vertexPageTableBuffers[frameIndex] != nullptr;
            gbufferPush.vertexPageTableReference = hasPageTables ? m_vertexPageTableBuffers[frameIndex]->getDeviceAddress() : 0;
            gbufferPush.meshletDrawsPageTableReference = (hasPageTables && frameIndex < m_meshletDrawPageTableBuffers.size() && m_meshletDrawPageTableBuffers[frameIndex])
                                                             ? m_meshletDrawPageTableBuffers[frameIndex]->getDeviceAddress()
                                                             : 0;
            gbufferPush.meshletVerticesPageTableReference = (hasPageTables && frameIndex < m_meshletVertPageTableBuffers.size() && m_meshletVertPageTableBuffers[frameIndex])
                                                                ? m_meshletVertPageTableBuffers[frameIndex]->getDeviceAddress()
                                                                : 0;
            gbufferPush.meshletTrianglesPageTableReference = (hasPageTables && frameIndex < m_meshletTriPageTableBuffers.size() && m_meshletTriPageTableBuffers[frameIndex])
                                                                 ? m_meshletTriPageTableBuffers[frameIndex]->getDeviceAddress()
                                                                 : 0;
            gbufferPush.visibilityTextureIndex = m_visibilityResource->GetDescriptorIndexSlot();
            gbufferPush.viewportSize = glm::vec2(rw, rh);
            gbufferPush.debugMode = m_debugMode;
            m_commandBuffers->pushData(&gbufferPush, sizeof(shaderio::PushConstantVisibilityDebug));

            m_commandBuffers->drawMeshTasks(1, 1, 1);
            m_commandBuffers->endRendering();
            m_commandBuffers->executionBarrier();
        }

        // =========================================================================
        // 2.45. NRD PER-FRAME TICK
        // =========================================================================
        // NRD's integration layer requires its internal frame counter to advance by exactly 1 every
        // real frame (see Device::tickNRD). The three evaluate*() calls below each also tick it
        // internally, but only when they actually run -- which, since they're correctly gated on their
        // corresponding RT feature being enabled, is no longer guaranteed every frame. Ticking here
        // unconditionally keeps NRD's counter in sync regardless of which (if any) denoiser runs this
        // frame; each gated evaluate*() call below still ticks fine too since the tick is idempotent
        // per unique frameIndex.
        if (m_device->isNRDInitialized())
        {
            m_device->tickNRD(static_cast<uint32_t>(m_sceneFrameCounter), m_isFirstFrame || m_resetNRD,
                uniformData.view, uniformData.nonJitteredProj, uniformData.prevView, uniformData.prevProj);
        }

        // =========================================================================
        // 2.5. SHADOW MASK PASS (Evaluates 1-SPP RT Shadow -> m_rawShadowMask, m_viewZ, m_nrdNormalRoughness)
        // This is only needed for the hybrid RT shadow path. ReSTIR DI traces its own selected
        // light, and basic raster PBR/IBL must not pay for a full-resolution ray-query pass.
        // =========================================================================
        if ((uniformData.enableRTShadows != 0) &&
            m_shadowMaskPipeline && m_rawShadowMask && m_viewZ && m_nrdNormalRoughness)
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "RT Shadows");
            std::vector<NRI::RenderAttachDesc> shadowAttachments;
            shadowAttachments.push_back({
                .attachment = m_rawShadowMask.get(),
                .loadOP = NRI::LoadOP::clear,
                .storeOP = NRI::StoreOP::store,
                .clearColor = {65504.0f, 1.0f, 0.0f, 0.0f}
            });
            shadowAttachments.push_back({
                .attachment = m_viewZ.get(),
                .loadOP = NRI::LoadOP::clear,
                .storeOP = NRI::StoreOP::store,
                .clearColor = {500000.0f, 0.0f, 0.0f, 0.0f}
            });
            shadowAttachments.push_back({
                .attachment = m_nrdNormalRoughness.get(),
                .loadOP = NRI::LoadOP::clear,
                .storeOP = NRI::StoreOP::store,
                .clearColor = {0.0f, 0.0f, 0.0f, 0.0f}
            });

            NRI::RenderDesc shadowDesc = {
                .renderArea = renderExtent,
                .colorAttachments = shadowAttachments
            };

            m_commandBuffers->beginRendering(shadowDesc);
            m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
            m_commandBuffers->setScissorWithCount(renderExtent);

            m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_shadowMaskPipeline);
            m_commandBuffers->setCullMode(NRI::CullMode::None);
            m_commandBuffers->setDepthTestEnable(false);
            m_commandBuffers->setDepthWriteEnable(false);
            for (uint32_t a = 0; a < shadowAttachments.size(); ++a)
            {
                m_commandBuffers->setColorBlendEnable(a, false);
                m_commandBuffers->setColorWriteMask(a, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                    NRI::ColorComponent::B | NRI::ColorComponent::A);
            }

            glm::mat4 viewProj = uniformData.proj * uniformData.view;
            shaderio::PushConstantShadowMask shadowPush{};
            shadowPush.invViewProj = glm::inverse(viewProj);
            shadowPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
            shadowPush.depthTextureIndex = m_depthResource->GetDescriptorIndexSlot();
            shadowPush.gbufferNormalIndex = m_gbufferNormal->GetDescriptorIndexSlot();
            shadowPush.viewportSize = glm::vec2(rw, rh);
            shadowPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
            m_commandBuffers->pushData(&shadowPush, sizeof(shaderio::PushConstantShadowMask));

            m_commandBuffers->drawMeshTasks(1, 1, 1);
            m_commandBuffers->endRendering();
            m_commandBuffers->executionBarrier();
        }

        // =========================================================================
        // 2.6. NRD DENOISING PASS (Denoises 1-SPP RT Shadow -> m_denoisedShadowMask)
        // =========================================================================
        if (m_nrdShadowsEnabled && (uniformData.enableRTShadows != 0) &&
            m_device->isNRDInitialized() && m_rawShadowMask && m_denoisedShadowMask && m_viewZ && m_nrdNormalRoughness)
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "NRD Shadows");
            NRI::NRDShadowParams nrdParams{};
            nrdParams.inShadowData = m_rawShadowMask.get();
            nrdParams.inMotionVectors = m_gbufferVelocity.get();
            nrdParams.inNormalRoughness = m_nrdNormalRoughness.get();
            nrdParams.inViewZ = m_viewZ.get();
            nrdParams.outDenoisedShadow = m_denoisedShadowMask.get();
            nrdParams.commandBuffer = m_commandBuffers.get();

            nrdParams.view = uniformData.view;
            nrdParams.proj = uniformData.nonJitteredProj;
            nrdParams.prevView = uniformData.prevView;
            nrdParams.prevProj = uniformData.prevProj;

            glm::vec3 lightDir = glm::vec3(0.0f, 1.0f, 0.0f);
            if (!m_lightBufferObjects.empty())
            {
                lightDir = glm::normalize(glm::vec3(m_lightBufferObjects[0].direction));
            }
            nrdParams.lightDirection[0] = lightDir.x;
            nrdParams.lightDirection[1] = lightDir.y;
            nrdParams.lightDirection[2] = lightDir.z;

            nrdParams.motionVectorScale = glm::vec2(1.0f, 1.0f);
            nrdParams.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
            nrdParams.resetHistory = m_isFirstFrame || m_resetNRD;

            m_device->evaluateNRDShadows(nrdParams);
            m_commandBuffers->executionBarrier();
            m_commandBuffers->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
        }

        // Computed here (rather than down at the lighting-pass switch) so every hybrid-only pass below
        // can skip itself while path tracing. ReSTIR DI/GI are the exception when explicitly requested:
        // the plain path tracer can consume their primary-surface lighting buffers in an RTXPT-style
        // hybrid mode.
        bool runPathTracer = (m_pathTracingEnabled || m_debugMode == 18 || m_debugMode == 19);
        bool pathTracerUsesRTXDI = runPathTracer && m_pathTracerUsesRTXDI;

        // =========================================================================
        // 2.65. RAY TRACED REFLECTION PASS (Evaluates 1-SPP GGX VNDF -> m_rawReflection)
        // =========================================================================
        if (!runPathTracer && m_reflectionPipeline && m_rawReflection && (uniformData.enableRTReflections != 0))
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "RT Reflections");
            std::vector<NRI::RenderAttachDesc> reflectionAttachments;
            reflectionAttachments.push_back({
                .attachment = m_rawReflection.get(),
                .loadOP = NRI::LoadOP::clear,
                .storeOP = NRI::StoreOP::store,
                .clearColor = {0.0f, 0.0f, 0.0f, 10000.0f}
            });

            NRI::RenderDesc reflectionDesc = {
                .renderArea = renderExtent,
                .colorAttachments = reflectionAttachments
            };

            m_commandBuffers->beginRendering(reflectionDesc);
            m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
            m_commandBuffers->setScissorWithCount(renderExtent);

            m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_reflectionPipeline);
            m_commandBuffers->setCullMode(NRI::CullMode::None);
            m_commandBuffers->setDepthTestEnable(false);
            m_commandBuffers->setDepthWriteEnable(false);
            for (uint32_t a = 0; a < reflectionAttachments.size(); ++a)
            {
                m_commandBuffers->setColorBlendEnable(a, false);
                m_commandBuffers->setColorWriteMask(a, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                    NRI::ColorComponent::B | NRI::ColorComponent::A);
            }

            glm::mat4 viewProj = uniformData.proj * uniformData.view;
            shaderio::PushConstantReflection reflPush{};
            reflPush.invViewProj = glm::inverse(viewProj);
            reflPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
            reflPush.depthTextureIndex = m_depthResource->GetDescriptorIndexSlot();
            reflPush.gbufferNormalIndex = m_gbufferNormal->GetDescriptorIndexSlot();
            reflPush.gbufferMaterialIndex = m_gbufferMaterial->GetDescriptorIndexSlot();
            reflPush.visibilityTextureIndex = m_visibilityResource->GetDescriptorIndexSlot();
            reflPush.viewportSize = glm::vec2(rw, rh);
            reflPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
            reflPush.denoiserMode = static_cast<uint32_t>(m_nrdReflectionDenoiser);
            m_commandBuffers->pushData(&reflPush, sizeof(shaderio::PushConstantReflection));

            m_commandBuffers->drawMeshTasks(1, 1, 1);
            m_commandBuffers->endRendering();
            m_commandBuffers->executionBarrier();
        }

        // =========================================================================
        // 2.7. NRD REFLECTIONS DENOISING PASS (Denoises 1-SPP Reflections via REBLUR / RELAX)
        // =========================================================================
        if (!runPathTracer && m_nrdReflectionDenoiser != NRI::NRDReflectionDenoiser::Off && (uniformData.enableRTReflections != 0) &&
            m_device->isNRDInitialized() &&
            m_rawReflection && m_denoisedReflection && m_viewZ && m_nrdNormalRoughness)
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "NRD Reflections");
            NRI::NRDReflectionParams reflParams{};
            reflParams.inSpecularRadianceHitDist = m_rawReflection.get();
            reflParams.inMotionVectors = m_gbufferVelocity.get();
            reflParams.inNormalRoughness = m_nrdNormalRoughness.get();
            reflParams.inViewZ = m_viewZ.get();
            reflParams.outDenoisedSpecular = m_denoisedReflection.get();
            reflParams.commandBuffer = m_commandBuffers.get();

            reflParams.view = uniformData.view;
            reflParams.proj = uniformData.nonJitteredProj;
            reflParams.prevView = uniformData.prevView;
            reflParams.prevProj = uniformData.prevProj;

            reflParams.motionVectorScale = glm::vec2(1.0f, 1.0f);
            reflParams.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
            reflParams.resetHistory = m_isFirstFrame || m_resetNRD;

            m_device->evaluateNRDReflections(reflParams, m_nrdReflectionDenoiser);
            m_commandBuffers->executionBarrier();
            m_commandBuffers->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
        }

        // NOTE: m_resetNRD is intentionally NOT cleared here anymore -- it used to be reset right after
        // reflections, which meant DDGI/ReSTIR GI/ReSTIR DI/path-tracer's own NRD passes further below
        // (all of which also read it for their own resetHistory flag) always saw it as already false,
        // silently defeating the "flush history on denoiser toggle" mutual-exclusion logic in
        // setNRDGIDenoiser/setNRDDIDenoiser/setNRDPTDenoiser for anything but shadows/reflections. It's
        // cleared once, after every NRD consumer this frame has had a chance to read it (see below,
        // right before the lighting pass switch).

        // =========================================================================
        // 2.8. DYNAMIC DIFFUSE GLOBAL ILLUMINATION (DDGI) PASSES
        // =========================================================================
        uint32_t totalDDGIProbes = m_ddgiProbeCountX * m_ddgiProbeCountY * m_ddgiProbeCountZ;
        // DDGI ray-traces every probe against the scene TLAS (see DDGIRadiance.slang), so it has no
        // meaning without ray tracing hardware access -- gate it on the master toggle too, or turning
        // "Enable Hybrid Ray Tracing" off silently leaves it (and its cost) running.
        bool runDDGI = !runPathTracer &&
                       m_rayTracingEnabled &&
                       (m_ddgiEnabled || m_debugMode == 16 || m_debugMode == 17) &&
                       m_ddgiRadiancePipeline && m_ddgiBlendIrradiancePipeline && m_ddgiBlendDistancePipeline &&
                       m_ddgiRayData && m_ddgiIrradiance[0] && m_ddgiIrradiance[1] &&
                       m_ddgiDistance[0] && m_ddgiDistance[1] && (totalDDGIProbes > 0);

        if (runDDGI)
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "DDGI");
            uint32_t probesPerRow = 64;
            uint32_t probeRows = (totalDDGIProbes + probesPerRow - 1) / probesPerRow;
            uint32_t irrWidth = probesPerRow * 10;
            uint32_t irrHeight = probeRows * 10;
            uint32_t distWidth = probesPerRow * 18;
            uint32_t distHeight = probeRows * 18;

            uint32_t writeIndex = 1 - m_ddgiHistoryIndex;
            uint32_t readIndex = m_ddgiHistoryIndex;

            // 1. Trace DDGI Radiance Rays: (Width = m_ddgiRaysPerProbe, Height = totalDDGIProbes)
            {
                NRI::Extent2D radExtent = {m_ddgiRaysPerProbe, totalDDGIProbes};
                std::vector<NRI::RenderAttachDesc> radAttachments;
                radAttachments.push_back({
                    .attachment = m_ddgiRayData.get(),
                    .loadOP = NRI::LoadOP::clear,
                    .storeOP = NRI::StoreOP::store,
                    .clearColor = {0.0f, 0.0f, 0.0f, 1000.0f}
                });

                NRI::RenderDesc radDesc = {
                    .renderArea = radExtent,
                    .colorAttachments = radAttachments
                };

                m_commandBuffers->beginRendering(radDesc);
                float radW = static_cast<float>(m_ddgiRaysPerProbe);
                float radH = static_cast<float>(totalDDGIProbes);
                m_commandBuffers->setViewportWithCount({0.0f, radH, radW, -radH}, 0.0f, 1.0f);
                m_commandBuffers->setScissorWithCount(radExtent);

                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_ddgiRadiancePipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(false);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                    NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantDDGIRadiance radPush{};
                radPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                radPush.raysPerProbe = m_ddgiRaysPerProbe;
                radPush.probeCountTotal = totalDDGIProbes;
                radPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                m_commandBuffers->pushData(&radPush, sizeof(shaderio::PushConstantDDGIRadiance));

                m_commandBuffers->drawMeshTasks(1, 1, 1);
                m_commandBuffers->endRendering();
                m_commandBuffers->executionBarrier();
            }

            // 2. Blend Irradiance Atlas: (Width = irrWidth, Height = irrHeight)
            {
                NRI::Extent2D irrExtent = {irrWidth, irrHeight};
                std::vector<NRI::RenderAttachDesc> irrAttachments;
                irrAttachments.push_back({
                    .attachment = m_ddgiIrradiance[writeIndex].get(),
                    .loadOP = NRI::LoadOP::clear,
                    .storeOP = NRI::StoreOP::store,
                    .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}
                });

                NRI::RenderDesc irrDesc = {
                    .renderArea = irrExtent,
                    .colorAttachments = irrAttachments
                };

                m_commandBuffers->beginRendering(irrDesc);
                float irrW = static_cast<float>(irrWidth);
                float irrH = static_cast<float>(irrHeight);
                m_commandBuffers->setViewportWithCount({0.0f, irrH, irrW, -irrH}, 0.0f, 1.0f);
                m_commandBuffers->setScissorWithCount(irrExtent);

                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_ddgiBlendIrradiancePipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(false);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                    NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantDDGIBlend blendIrrPush{};
                blendIrrPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                blendIrrPush.rayDataTextureIndex = m_ddgiRayData->GetDescriptorIndexSlot();
                blendIrrPush.prevAtlasTextureIndex = m_ddgiIrradiance[readIndex]->GetDescriptorIndexSlot();
                blendIrrPush.probesPerRow = probesPerRow;
                blendIrrPush.raysPerProbe = m_ddgiRaysPerProbe;
                blendIrrPush.probeCountTotal = totalDDGIProbes;
                blendIrrPush.hysteresis = m_ddgiHysteresis;
                blendIrrPush.firstFrame = (m_ddgiFirstFrame || m_isFirstFrame) ? 1 : 0;
                blendIrrPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                m_commandBuffers->pushData(&blendIrrPush, sizeof(shaderio::PushConstantDDGIBlend));

                m_commandBuffers->drawMeshTasks(1, 1, 1);
                m_commandBuffers->endRendering();
                m_commandBuffers->executionBarrier();
            }

            // 3. Blend Distance Atlas: (Width = distWidth, Height = distHeight)
            {
                NRI::Extent2D distExtent = {distWidth, distHeight};
                std::vector<NRI::RenderAttachDesc> distAttachments;
                distAttachments.push_back({
                    .attachment = m_ddgiDistance[writeIndex].get(),
                    .loadOP = NRI::LoadOP::clear,
                    .storeOP = NRI::StoreOP::store,
                    .clearColor = {0.0f, 0.0f, 0.0f, 0.0f}
                });

                NRI::RenderDesc distDesc = {
                    .renderArea = distExtent,
                    .colorAttachments = distAttachments
                };

                m_commandBuffers->beginRendering(distDesc);
                float distW = static_cast<float>(distWidth);
                float distH = static_cast<float>(distHeight);
                m_commandBuffers->setViewportWithCount({0.0f, distH, distW, -distH}, 0.0f, 1.0f);
                m_commandBuffers->setScissorWithCount(distExtent);

                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_ddgiBlendDistancePipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(false);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G);

                shaderio::PushConstantDDGIBlend blendDistPush{};
                blendDistPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                blendDistPush.rayDataTextureIndex = m_ddgiRayData->GetDescriptorIndexSlot();
                blendDistPush.prevAtlasTextureIndex = m_ddgiDistance[readIndex]->GetDescriptorIndexSlot();
                blendDistPush.probesPerRow = probesPerRow;
                blendDistPush.raysPerProbe = m_ddgiRaysPerProbe;
                blendDistPush.probeCountTotal = totalDDGIProbes;
                blendDistPush.hysteresis = m_ddgiHysteresis;
                blendDistPush.firstFrame = (m_ddgiFirstFrame || m_isFirstFrame) ? 1 : 0;
                blendDistPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                m_commandBuffers->pushData(&blendDistPush, sizeof(shaderio::PushConstantDDGIBlend));

                m_commandBuffers->drawMeshTasks(1, 1, 1);
                m_commandBuffers->endRendering();
                m_commandBuffers->executionBarrier();
            }

            // Swap history to writeIndex for subsequent lighting and next frame
            m_ddgiHistoryIndex = writeIndex;
            m_ddgiFirstFrame = false;

            // Update UBO so the Deferred Lighting pass in this same frame reads the fresh atlas
            uniformData.ddgiIrradianceTextureIndex = m_ddgiIrradiance[writeIndex]->GetDescriptorIndexSlot();
            uniformData.ddgiDistanceTextureIndex = m_ddgiDistance[writeIndex]->GetDescriptorIndexSlot();
            memcpy(m_uniformBuffersMapped[frameIndex], &uniformData, sizeof(shaderio::UniformBufferObject));
        }

        // =========================================================================
        // 2.9. RESTIR GI (SCREEN-SPACE DIFFUSE PATH RESAMPLING VIA RTXDI)
        // =========================================================================
        // ReSTIR GI ray-traces its initial candidate against the scene TLAS, so like DDGI it has no
        // meaning without ray tracing hardware access -- gate it on the master toggle too, or turning
        // "Enable Hybrid Ray Tracing" off silently leaves it (and its cost) running.
        bool runReSTIRGI = (!runPathTracer || pathTracerUsesRTXDI) &&
                           (m_rayTracingEnabled || pathTracerUsesRTXDI) &&
                           (m_diffuseGIMode == 2 || (m_debugMode == 16 && m_diffuseGIMode != 1)) &&
                           m_hasTLASBuild && m_sceneTLAS && (uniformData.tlasDeviceAddress != 0) &&
                           (uniformData.instanceLUTReference != 0) &&
                           m_restirGIInitialPipeline && m_restirGITemporalPipeline && m_restirGISpatialPipeline &&
                           m_restirGIRawDiffuse && m_restirGIReservoirBuffers[0] &&
                           m_restirGIReservoirBuffers[1] && m_restirGINeighborOffsetsBuffer;

        if (runReSTIRGI)
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "ReSTIR GI");
            RTXDI_ReservoirBufferParameters resParams = rtxdi::CalculateReservoirBufferParameters(
                m_renderSize.width, m_renderSize.height, rtxdi::CheckerboardMode::Off);

            // Fixed buffer roles, matching RTXPT's ReSTIRGIContext::UpdateBufferIndices for
            // TemporalAndSpatial mode (NOT ping-ponged by frame parity, unlike Temporal-only/Fused
            // modes): buffer 0 is pure this-frame scratch (Initial writes it, Temporal reads its own
            // candidate from it and overwrites it in place with the temporal result -- safe because
            // that read+write only ever touches a single pixel's own slot). Buffer 1 is the
            // persistent cross-frame result: Temporal reads it as history, and Spatial -- which reads
            // every neighbor out of buffer 0 -- writes its finished result there, never back into
            // buffer 0, so it can never race a neighboring pixel's still-in-flight read of buffer 0.
            constexpr uint32_t kScratchBuffer = 0;
            constexpr uint32_t kPersistentBuffer = 1;

            // 1. Initial Candidate Generation Pass
            {
                std::vector<NRI::RenderAttachDesc> initAttachments;
                initAttachments.push_back({
                    .attachment = m_restirGIRawDiffuse.get(),
                    .loadOP = NRI::LoadOP::clear,
                    .storeOP = NRI::StoreOP::store,
                    .clearColor = {0.0f, 0.0f, 0.0f, 0.0f}
                });

                NRI::RenderDesc initDesc = {
                    .renderArea = renderExtent,
                    .colorAttachments = initAttachments
                };

                m_commandBuffers->beginRendering(initDesc);
                m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
                m_commandBuffers->setScissorWithCount(renderExtent);

                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirGIInitialPipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(false);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                    NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRGIInitial initPush{};
                initPush.invViewProj = uniformData.invViewProj;
                initPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                initPush.reservoirBufferReference = m_restirGIReservoirBuffers[kScratchBuffer]->getDeviceAddress();
                initPush.depthTextureIndex = m_depthResource->GetDescriptorIndexSlot();
                initPush.gbufferNormalIndex = m_gbufferNormal->GetDescriptorIndexSlot();
                initPush.gbufferAlbedoIndex = m_gbufferAlbedo->GetDescriptorIndexSlot();
                initPush.gbufferMaterialIndex = m_gbufferMaterial->GetDescriptorIndexSlot();
                initPush.visibilityTextureIndex = m_visibilityResource->GetDescriptorIndexSlot();
                initPush.viewportSize = glm::vec2(rw, rh);
                initPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                initPush.reservoirBlockRowPitch = resParams.reservoirBlockRowPitch;
                initPush.reservoirArrayPitch = resParams.reservoirArrayPitch;
                m_commandBuffers->pushData(&initPush, sizeof(shaderio::PushConstantReSTIRGIInitial));

                m_commandBuffers->drawMeshTasks(1, 1, 1);
                m_commandBuffers->endRendering();
                m_commandBuffers->executionBarrier();
            }

            // 2. Temporal Resampling Pass: reads Pass 1's candidate + history, overwrites the
            // candidate in place with the temporally-resampled result.
            {
                std::vector<NRI::RenderAttachDesc> tAttachments;
                tAttachments.push_back({
                    .attachment = m_restirGIRawDiffuse.get(),
                    .loadOP = NRI::LoadOP::dontCare,
                    .storeOP = NRI::StoreOP::dontCare
                });

                NRI::RenderDesc tDesc = {
                    .renderArea = renderExtent,
                    .colorAttachments = tAttachments
                };

                m_commandBuffers->beginRendering(tDesc);
                m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
                m_commandBuffers->setScissorWithCount(renderExtent);

                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirGITemporalPipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(false);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                    NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRGITemporal tPush{};
                tPush.invViewProj = uniformData.invViewProj;
                // Last frame's depth/normal snapshot was rendered with last frame's camera matrix --
                // must unproject it with the SAME matrix, never this frame's, or the reconstructed
                // previous-frame world position is simply wrong as soon as the camera moves.
                tPush.prevInvViewProj = glm::inverse(uniformData.prevProj * uniformData.prevView);
                tPush.cameraWorldPos = uniformData.cameraWorldPos;
                tPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                tPush.currentReservoirReference = m_restirGIReservoirBuffers[kScratchBuffer]->getDeviceAddress();
                tPush.previousReservoirReference = m_restirGIReservoirBuffers[kPersistentBuffer]->getDeviceAddress();
                tPush.depthTextureIndex = m_depthResource->GetDescriptorIndexSlot();
                tPush.prevDepthTextureIndex = m_prevDepthResource ? m_prevDepthResource->GetDescriptorIndexSlot() : UINT32_MAX;
                tPush.gbufferNormalIndex = m_gbufferNormal->GetDescriptorIndexSlot();
                tPush.prevNormalTextureIndex = m_prevGbufferNormal ? m_prevGbufferNormal->GetDescriptorIndexSlot() : UINT32_MAX;
                tPush.gbufferVelocityIndex = m_gbufferVelocity->GetDescriptorIndexSlot();
                tPush.gbufferMaterialIndex = m_gbufferMaterial->GetDescriptorIndexSlot();
                tPush.viewportSize = glm::vec2(rw, rh);
                tPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                tPush.reservoirBlockRowPitch = resParams.reservoirBlockRowPitch;
                tPush.reservoirArrayPitch = resParams.reservoirArrayPitch;
                tPush.maxHistoryLength = m_restirGIMaxHistoryLength;
                tPush.normalThreshold = m_restirGINormalThreshold;
                tPush.depthThreshold = m_restirGIDepthThreshold;
                tPush.enablePermutationSampling = 1; // matches RTXPT's default (true)
                tPush.maxReservoirAge = 50; // matches RTXPT's GI-specific default
                m_commandBuffers->pushData(&tPush, sizeof(shaderio::PushConstantReSTIRGITemporal));

                m_commandBuffers->drawMeshTasks(1, 1, 1);
                m_commandBuffers->endRendering();
                m_commandBuffers->executionBarrier();
            }

            // 3. Spatial Resampling Pass: reads THIS frame's just-temporally-resampled scratch buffer
            // (for both its own-pixel input and every spatial neighbor) but writes its finished
            // result into the SEPARATE persistent buffer -- never back into the scratch buffer, since
            // that is still being read concurrently by other in-flight pixels of this same pass.
            {
                std::vector<NRI::RenderAttachDesc> sAttachments;
                sAttachments.push_back({
                    .attachment = m_restirGIRawDiffuse.get(),
                    .loadOP = NRI::LoadOP::clear,
                    .storeOP = NRI::StoreOP::store,
                    .clearColor = {0.0f, 0.0f, 0.0f, 0.0f}
                });

                NRI::RenderDesc sDesc = {
                    .renderArea = renderExtent,
                    .colorAttachments = sAttachments
                };

                m_commandBuffers->beginRendering(sDesc);
                m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
                m_commandBuffers->setScissorWithCount(renderExtent);

                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirGISpatialPipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(false);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                    NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRGISpatial sPush{};
                sPush.invViewProj = uniformData.invViewProj;
                sPush.cameraWorldPos = uniformData.cameraWorldPos;
                sPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                sPush.inputReservoirReference = m_restirGIReservoirBuffers[kScratchBuffer]->getDeviceAddress();
                sPush.outputReservoirReference = m_restirGIReservoirBuffers[kPersistentBuffer]->getDeviceAddress();
                sPush.neighborOffsetsReference = m_restirGINeighborOffsetsBuffer->getDeviceAddress();
                sPush.depthTextureIndex = m_depthResource->GetDescriptorIndexSlot();
                sPush.gbufferNormalIndex = m_gbufferNormal->GetDescriptorIndexSlot();
                sPush.gbufferMaterialIndex = m_gbufferMaterial->GetDescriptorIndexSlot();
                sPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                sPush.viewportSize = glm::vec2(rw, rh);
                sPush.reservoirBlockRowPitch = resParams.reservoirBlockRowPitch;
                sPush.reservoirArrayPitch = resParams.reservoirArrayPitch;
                sPush.samplingRadius = m_restirGISpatialRadius;
                sPush.numSamples = m_restirGINumSpatialSamples;
                sPush.normalThreshold = m_restirGINormalThreshold;
                sPush.depthThreshold = m_restirGIDepthThreshold;
                sPush.neighborOffsetMask = 127;
                sPush.enableBoilingFilter = m_restirGIEnableBoilingFilter ? 1 : 0;
                sPush.boilingFilterStrength = m_restirGIBoilingFilterStrength;
                sPush.denoiserMode = static_cast<uint32_t>(m_nrdGIDenoiser);
                m_commandBuffers->pushData(&sPush, sizeof(shaderio::PushConstantReSTIRGISpatial));

                m_commandBuffers->drawMeshTasks(1, 1, 1);
                m_commandBuffers->endRendering();
                m_commandBuffers->executionBarrier();
            }

            // =========================================================================
            // NRD DIFFUSE GI DENOISING PASS (Denoises 1-SPP ReSTIR GI via REBLUR / RELAX)
            // =========================================================================
            bool restirGIDenoised = false;
            if (m_nrdGIDenoiser != NRI::NRDDiffuseDenoiser::Off &&
                m_device->isNRDInitialized() &&
                m_restirGIRawDiffuse && m_denoisedReSTIRGIDiffuse && m_viewZ && m_nrdNormalRoughness)
            {
                NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "NRD GI");
                NRI::NRDDiffuseParams giDenoiseParams{};
                giDenoiseParams.inDiffuseRadianceHitDist = m_restirGIRawDiffuse.get();
                giDenoiseParams.inMotionVectors = m_gbufferVelocity.get();
                giDenoiseParams.inNormalRoughness = m_nrdNormalRoughness.get();
                giDenoiseParams.inViewZ = m_viewZ.get();
                giDenoiseParams.outDenoisedDiffuse = m_denoisedReSTIRGIDiffuse.get();
                giDenoiseParams.commandBuffer = m_commandBuffers.get();

                giDenoiseParams.view = uniformData.view;
                giDenoiseParams.proj = uniformData.nonJitteredProj;
                giDenoiseParams.prevView = uniformData.prevView;
                giDenoiseParams.prevProj = uniformData.prevProj;

                giDenoiseParams.motionVectorScale = glm::vec2(1.0f, 1.0f);
                giDenoiseParams.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                giDenoiseParams.resetHistory = m_isFirstFrame || m_resetNRD;

                restirGIDenoised = m_device->evaluateNRDDiffuse(giDenoiseParams, m_nrdGIDenoiser);
                m_commandBuffers->executionBarrier();
                m_commandBuffers->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
            }

            // Update UBO so Deferred Lighting reads the fresh ReSTIR GI diffuse
            uniformData.diffuseGIMode = m_diffuseGIMode;
            uniformData.restirGIDiffuseTextureIndex = restirGIDenoised
                ? m_denoisedReSTIRGIDiffuse->GetDescriptorIndexSlot()
                : m_restirGIRawDiffuse->GetDescriptorIndexSlot();
            uniformData.restirGIReservoirBufferIndex = 0;
            uniformData.restirGINeighborOffsetsBufferIndex = 0;
            memcpy(m_uniformBuffersMapped[frameIndex], &uniformData, sizeof(shaderio::UniformBufferObject));
        }
        else
        {
            if (m_diffuseGIMode == 2)
            {
                // Fallback to IBL ambient if ReSTIR GI cannot run yet (e.g. TLAS build pending)
                uniformData.diffuseGIMode = 0;
                uniformData.restirGIDiffuseTextureIndex = 0xFFFFFFFF;
            }
            else
            {
                uniformData.diffuseGIMode = m_diffuseGIMode;
                if (m_restirGIRawDiffuse)
                    uniformData.restirGIDiffuseTextureIndex = m_restirGIRawDiffuse->GetDescriptorIndexSlot();
                else
                    uniformData.restirGIDiffuseTextureIndex = 0xFFFFFFFF;
            }
            memcpy(m_uniformBuffersMapped[frameIndex], &uniformData, sizeof(shaderio::UniformBufferObject));
        }

        // Snapshot this frame's depth/normal into the "previous frame" buffers now that both ReSTIR
        // GI's and ReSTIR DI's temporal passes have already consumed last frame's snapshot -- these
        // feed next frame's temporal reprojection validity check (see declaration comment on
        // m_prevDepthResource). Must run unconditionally here, NOT only inside `if (runReSTIRGI)`:
        // ReSTIR DI's temporal pass reads these same "prev" textures too, so gating this behind GI
        // being enabled left DI reprojecting against a permanently stale snapshot whenever GI was off,
        // producing ghost shadows on camera movement that only "fixed themselves" when GI was toggled
        // on (which happened to keep this copy running as a side effect).
        if (m_prevDepthResource && m_prevGbufferNormal)
        {
            m_commandBuffers->copyTexture(*m_depthResource, *m_prevDepthResource, m_renderSize.width, m_renderSize.height);
            m_commandBuffers->copyTexture(*m_gbufferNormal, *m_prevGbufferNormal, m_renderSize.width, m_renderSize.height);
            // ReSTIR PT's temporal resampling also needs the previous frame's material (see declaration
            // comment on m_prevGbufferAlbedo) -- same unconditional-copy reasoning as depth/normal above.
            if (m_prevGbufferAlbedo && m_prevGbufferMaterial)
            {
                m_commandBuffers->copyTexture(*m_gbufferAlbedo, *m_prevGbufferAlbedo, m_renderSize.width, m_renderSize.height);
                m_commandBuffers->copyTexture(*m_gbufferMaterial, *m_prevGbufferMaterial, m_renderSize.width, m_renderSize.height);
            }
            m_commandBuffers->executionBarrier();
        }

        // =========================================================================
        // 2.95. RESTIR DI (SCREEN-SPACE RESAMPLED DIRECT LIGHTING VIA RTXDI)
        // =========================================================================
        // Same master-toggle gating as DDGI/ReSTIR GI (see the master-toggle bug fixed for those
        // two): ReSTIR DI ray-traces its final shadow against the scene TLAS, so it has no meaning
        // without ray tracing hardware access.
        bool runReSTIRDI = (!runPathTracer || pathTracerUsesRTXDI) &&
                           (m_rayTracingEnabled || pathTracerUsesRTXDI) &&
                           m_directLightingMode == 1 &&
                           m_hasTLASBuild && m_sceneTLAS && (uniformData.tlasDeviceAddress != 0) &&
                           m_restirDIInitialPipeline && m_restirDITemporalPipeline &&
                           m_restirDISpatialPipeline && m_restirDIFinalShadingPipeline &&
                           m_restirDIPresamplePipeline && m_restirDIPresampleReGIRPipeline &&
                           m_restirDIWriteLightPDFPipeline && m_restirDIReduceLightPDFMipPipeline &&
                           m_restirDIDirectLighting && m_restirGINeighborOffsetsBuffer &&
                           m_restirDIRISBuffer && m_lightPDFTexture &&
                           m_restirDIReservoirBuffers[0] && m_restirDIReservoirBuffers[1] &&
                           m_restirDIReservoirBuffers[2] && (uniformData.lightDataReference != 0);

        if (runReSTIRDI)
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "ReSTIR DI");
            RTXDI_ReservoirBufferParameters diResParams = rtxdi::CalculateReservoirBufferParameters(
                m_renderSize.width, m_renderSize.height, rtxdi::CheckerboardMode::Off);

            // 3-buffer rotation (NOT GI's fixed 2-role scheme) -- see the rotation math documented
            // on PushConstantReSTIRDIInitial in shaderIO.h. Using only 2 buffers here would
            // reintroduce the exact read/write race already found and fixed once for GI's spatial pass.
            uint32_t diBufferA = (m_restirDILastFrameOutputReservoir + 1) % 3; // Initial writes here, Temporal overwrites in place
            uint32_t diBufferC = m_restirDILastFrameOutputReservoir;          // Temporal's history (read only)
            uint32_t diBufferB = (diBufferA + 1) % 3;                         // Spatial's output, FinalShading's input

            uint32_t regirCellCount = m_regirCellsX * m_regirCellsY * m_regirCellsZ;
            uint32_t risBufferOffset = m_restirDIRISTileSize * m_restirDIRISTileCount; // where ReGIR's segment starts

            // -1. Light PDF Mip Chain Rebuild (feeds RTXDI_PresampleLocalLights below) -- rebuilt every
            // frame alongside the RIS/ReGIR presample passes, same cadence as the rest of ReSTIR DI's
            // per-frame light data (lights can move/change color every frame).
            {
                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Compute, *m_restirDIWriteLightPDFPipeline);

                shaderio::PushConstantReSTIRDIWriteLightPDF wPush{};
                wPush.lightDataReference = uniformData.lightDataReference;
                wPush.firstLocalLightIndex = m_restirDIFirstLocalLight;
                wPush.numLocalLights = m_restirDINumLocalLights;
                wPush.pdfTextureStorageIndex = m_lightPDFMipStorageSlots[0];
                wPush.pdfTextureSize = m_lightPDFTextureSize;
                m_commandBuffers->pushData(&wPush, sizeof(shaderio::PushConstantReSTIRDIWriteLightPDF));

                uint32_t pdfGroups = (m_lightPDFTextureSize + 7) / 8;
                m_commandBuffers->dispatch(pdfGroups, pdfGroups, 1);
                m_commandBuffers->executionBarrier();

                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Compute, *m_restirDIReduceLightPDFMipPipeline);
                for (uint32_t mip = 1; mip < m_lightPDFMipLevels; mip++)
                {
                    shaderio::PushConstantReSTIRDIReduceLightPDFMip rPush{};
                    rPush.srcTextureIndex = m_lightPDFTexture->GetDescriptorIndexSlot();
                    rPush.dstStorageIndex = m_lightPDFMipStorageSlots[mip];
                    rPush.srcMip = mip - 1;
                    rPush.dstSize = m_lightPDFTextureSize >> mip;
                    m_commandBuffers->pushData(&rPush, sizeof(shaderio::PushConstantReSTIRDIReduceLightPDFMip));

                    uint32_t mipGroups = ((rPush.dstSize > 0 ? rPush.dstSize : 1) + 7) / 8;
                    m_commandBuffers->dispatch(mipGroups, mipGroups, 1);
                    m_commandBuffers->executionBarrier();
                }
            }

            // 0a. RIS Presample Pass (plain 1D compute -- calls the SDK's real RTXDI_PresampleLocalLights
            // against the light PDF mip chain above; also serves as ReGIR's fallback for out-of-grid pixels)
            {
                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Compute, *m_restirDIPresamplePipeline);

                shaderio::PushConstantReSTIRDIPresample risPush{};
                risPush.lightDataReference = uniformData.lightDataReference;
                risPush.risBufferReference = m_restirDIRISBuffer->getDeviceAddress();
                risPush.firstLocalLightIndex = m_restirDIFirstLocalLight;
                risPush.numLocalLights = m_restirDINumLocalLights;
                risPush.risTileSize = m_restirDIRISTileSize;
                risPush.risTileCount = m_restirDIRISTileCount;
                risPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                risPush.pdfTextureIndex = m_lightPDFTexture->GetDescriptorIndexSlot();
                risPush.pdfTextureSize = m_lightPDFTextureSize;
                m_commandBuffers->pushData(&risPush, sizeof(shaderio::PushConstantReSTIRDIPresample));

                uint32_t risThreads = m_restirDIRISTileSize * m_restirDIRISTileCount;
                m_commandBuffers->dispatch((risThreads + 63) / 64, 1, 1);
                m_commandBuffers->executionBarrier();
            }

            // 0b. ReGIR Presample Pass (plain 1D compute -- one thread per (cell, slot-within-cell))
            if (m_regirEnabled && regirCellCount > 0)
            {
                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Compute, *m_restirDIPresampleReGIRPipeline);

                shaderio::PushConstantReSTIRDIPresampleReGIR regirPush{};
                regirPush.lightDataReference = uniformData.lightDataReference;
                regirPush.risBufferReference = m_restirDIRISBuffer->getDeviceAddress();
                regirPush.gridCenterAndCellSize = glm::vec4(m_regirGridCenter, m_regirCellSize);
                regirPush.firstLocalLightIndex = m_restirDIFirstLocalLight;
                regirPush.numLocalLights = m_restirDINumLocalLights;
                regirPush.risBufferOffset = risBufferOffset;
                regirPush.lightsPerCell = m_regirLightsPerCell;
                regirPush.cellsX = m_regirCellsX;
                regirPush.cellsY = m_regirCellsY;
                regirPush.cellsZ = m_regirCellsZ;
                regirPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                regirPush.regirSamplingJitter = m_regirSamplingJitter;
                regirPush.risTileSize = m_restirDIRISTileSize;
                regirPush.risTileCount = m_restirDIRISTileCount;
                regirPush.numRegirBuildSamples = m_regirNumBuildSamples;
                m_commandBuffers->pushData(&regirPush, sizeof(shaderio::PushConstantReSTIRDIPresampleReGIR));

                uint32_t regirThreads = regirCellCount * m_regirLightsPerCell;
                m_commandBuffers->dispatch((regirThreads + 63) / 64, 1, 1);
                m_commandBuffers->executionBarrier();
            }

            m_commandBuffers->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());

            // 1. Initial Sampling Pass
            {
                std::vector<NRI::RenderAttachDesc> diInitAttachments;
                diInitAttachments.push_back({
                    .attachment = m_restirDIDirectLighting.get(),
                    .loadOP = NRI::LoadOP::dontCare,
                    .storeOP = NRI::StoreOP::dontCare
                });

                NRI::RenderDesc diInitDesc = {
                    .renderArea = renderExtent,
                    .colorAttachments = diInitAttachments
                };

                m_commandBuffers->beginRendering(diInitDesc);
                m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
                m_commandBuffers->setScissorWithCount(renderExtent);

                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirDIInitialPipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(false);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                    NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRDIInitial diInitPush{};
                diInitPush.invViewProj = uniformData.invViewProj;
                diInitPush.cameraWorldPos = uniformData.cameraWorldPos;
                diInitPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                diInitPush.lightDataReference = uniformData.lightDataReference;
                diInitPush.reservoirBufferReference = m_restirDIReservoirBuffers[diBufferA]->getDeviceAddress();
                diInitPush.depthTextureIndex = m_depthResource->GetDescriptorIndexSlot();
                diInitPush.gbufferNormalIndex = m_gbufferNormal->GetDescriptorIndexSlot();
                diInitPush.gbufferAlbedoIndex = m_gbufferAlbedo->GetDescriptorIndexSlot();
                diInitPush.gbufferMaterialIndex = m_gbufferMaterial->GetDescriptorIndexSlot();
                diInitPush.viewportSize = glm::vec2(rw, rh);
                diInitPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                diInitPush.reservoirBlockRowPitch = diResParams.reservoirBlockRowPitch;
                diInitPush.reservoirArrayPitch = diResParams.reservoirArrayPitch;
                diInitPush.firstLocalLightIndex = m_restirDIFirstLocalLight;
                diInitPush.numLocalLights = m_restirDINumLocalLights;
                diInitPush.firstInfiniteLightIndex = m_restirDIFirstInfiniteLight;
                diInitPush.numInfiniteLights = m_restirDINumInfiniteLights;
                diInitPush.numLocalLightSamples = m_restirDINumLocalLightSamples;
                diInitPush.numInfiniteLightSamples = m_restirDINumInfiniteLightSamples;
                diInitPush.risBufferReference = m_restirDIRISBuffer->getDeviceAddress();
                diInitPush.risBufferOffset = risBufferOffset;
                diInitPush.risTileSize = m_restirDIRISTileSize;
                diInitPush.risTileCount = m_restirDIRISTileCount;
                diInitPush.regirEnabled = (m_regirEnabled && regirCellCount > 0) ? 1 : 0;
                diInitPush.cellsX = m_regirCellsX;
                diInitPush.cellsY = m_regirCellsY;
                diInitPush.cellsZ = m_regirCellsZ;
                diInitPush.lightsPerCell = m_regirLightsPerCell;
                diInitPush.gridCenterAndCellSize = glm::vec4(m_regirGridCenter, m_regirCellSize);
                diInitPush.regirSamplingJitter = m_regirSamplingJitter;
                m_commandBuffers->pushData(&diInitPush, sizeof(shaderio::PushConstantReSTIRDIInitial));

                m_commandBuffers->drawMeshTasks(1, 1, 1);
                m_commandBuffers->endRendering();
                m_commandBuffers->executionBarrier();
            }

            // 2. Temporal Resampling Pass
            {
                std::vector<NRI::RenderAttachDesc> diTAttachments;
                diTAttachments.push_back({
                    .attachment = m_restirDIDirectLighting.get(),
                    .loadOP = NRI::LoadOP::dontCare,
                    .storeOP = NRI::StoreOP::dontCare
                });

                NRI::RenderDesc diTDesc = {
                    .renderArea = renderExtent,
                    .colorAttachments = diTAttachments
                };

                m_commandBuffers->beginRendering(diTDesc);
                m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
                m_commandBuffers->setScissorWithCount(renderExtent);

                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirDITemporalPipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(false);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                    NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRDITemporal diTPush{};
                diTPush.invViewProj = uniformData.invViewProj;
                diTPush.prevInvViewProj = glm::inverse(uniformData.prevProj * uniformData.prevView);
                diTPush.cameraWorldPos = uniformData.cameraWorldPos;
                diTPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                diTPush.lightDataReference = uniformData.lightDataReference;
                diTPush.currentReservoirReference = m_restirDIReservoirBuffers[diBufferA]->getDeviceAddress();
                diTPush.previousReservoirReference = m_restirDIReservoirBuffers[diBufferC]->getDeviceAddress();
                diTPush.depthTextureIndex = m_depthResource->GetDescriptorIndexSlot();
                diTPush.prevDepthTextureIndex = m_prevDepthResource ? m_prevDepthResource->GetDescriptorIndexSlot() : UINT32_MAX;
                diTPush.gbufferNormalIndex = m_gbufferNormal->GetDescriptorIndexSlot();
                diTPush.prevNormalTextureIndex = m_prevGbufferNormal ? m_prevGbufferNormal->GetDescriptorIndexSlot() : UINT32_MAX;
                diTPush.gbufferVelocityIndex = m_gbufferVelocity->GetDescriptorIndexSlot();
                diTPush.gbufferMaterialIndex = m_gbufferMaterial->GetDescriptorIndexSlot();
                diTPush.viewportSize = glm::vec2(rw, rh);
                diTPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                diTPush.reservoirBlockRowPitch = diResParams.reservoirBlockRowPitch;
                diTPush.reservoirArrayPitch = diResParams.reservoirArrayPitch;
                diTPush.maxHistoryLength = m_restirDIMaxHistoryLength;
                diTPush.normalThreshold = m_restirDINormalThreshold;
                diTPush.depthThreshold = m_restirDIDepthThreshold;
                diTPush.enablePermutationSampling = 1;
                m_commandBuffers->pushData(&diTPush, sizeof(shaderio::PushConstantReSTIRDITemporal));

                m_commandBuffers->drawMeshTasks(1, 1, 1);
                m_commandBuffers->endRendering();
                m_commandBuffers->executionBarrier();
            }

            // 3. Spatial Resampling Pass
            {
                std::vector<NRI::RenderAttachDesc> diSAttachments;
                diSAttachments.push_back({
                    .attachment = m_restirDIDirectLighting.get(),
                    .loadOP = NRI::LoadOP::dontCare,
                    .storeOP = NRI::StoreOP::dontCare
                });

                NRI::RenderDesc diSDesc = {
                    .renderArea = renderExtent,
                    .colorAttachments = diSAttachments
                };

                m_commandBuffers->beginRendering(diSDesc);
                m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
                m_commandBuffers->setScissorWithCount(renderExtent);

                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirDISpatialPipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(false);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                    NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRDISpatial diSPush{};
                diSPush.invViewProj = uniformData.invViewProj;
                diSPush.cameraWorldPos = uniformData.cameraWorldPos;
                diSPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                diSPush.lightDataReference = uniformData.lightDataReference;
                diSPush.inputReservoirReference = m_restirDIReservoirBuffers[diBufferA]->getDeviceAddress();
                diSPush.outputReservoirReference = m_restirDIReservoirBuffers[diBufferB]->getDeviceAddress();
                diSPush.neighborOffsetsReference = m_restirGINeighborOffsetsBuffer->getDeviceAddress();
                diSPush.depthTextureIndex = m_depthResource->GetDescriptorIndexSlot();
                diSPush.gbufferNormalIndex = m_gbufferNormal->GetDescriptorIndexSlot();
                diSPush.gbufferMaterialIndex = m_gbufferMaterial->GetDescriptorIndexSlot();
                diSPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                diSPush.viewportSize = glm::vec2(rw, rh);
                diSPush.reservoirBlockRowPitch = diResParams.reservoirBlockRowPitch;
                diSPush.reservoirArrayPitch = diResParams.reservoirArrayPitch;
                diSPush.samplingRadius = m_restirDISpatialRadius;
                diSPush.numSamples = m_restirDINumSpatialSamples;
                diSPush.normalThreshold = m_restirDINormalThreshold;
                diSPush.depthThreshold = m_restirDIDepthThreshold;
                diSPush.neighborOffsetMask = 127;
                m_commandBuffers->pushData(&diSPush, sizeof(shaderio::PushConstantReSTIRDISpatial));

                m_commandBuffers->drawMeshTasks(1, 1, 1);
                m_commandBuffers->endRendering();
                m_commandBuffers->executionBarrier();
            }

            // 4. Final Shading Pass: exactly one shadow ray per pixel, toward whichever light survived
            // resampling -- unlike the brute-force loop in DeferredLighting.slang, which only ever
            // shadows light index 0, every light is properly shadowed here regardless of light count.
            {
                std::vector<NRI::RenderAttachDesc> diFAttachments;
                diFAttachments.push_back({
                    .attachment = m_restirDIDirectLighting.get(),
                    .loadOP = NRI::LoadOP::clear,
                    .storeOP = NRI::StoreOP::store,
                    .clearColor = {0.0f, 0.0f, 0.0f, 0.0f}
                });

                NRI::RenderDesc diFDesc = {
                    .renderArea = renderExtent,
                    .colorAttachments = diFAttachments
                };

                m_commandBuffers->beginRendering(diFDesc);
                m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
                m_commandBuffers->setScissorWithCount(renderExtent);

                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirDIFinalShadingPipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(false);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                    NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRDIFinalShading diFPush{};
                diFPush.invViewProj = uniformData.invViewProj;
                diFPush.cameraWorldPos = uniformData.cameraWorldPos;
                diFPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                diFPush.lightDataReference = uniformData.lightDataReference;
                diFPush.reservoirReference = m_restirDIReservoirBuffers[diBufferB]->getDeviceAddress();
                diFPush.depthTextureIndex = m_depthResource->GetDescriptorIndexSlot();
                diFPush.gbufferNormalIndex = m_gbufferNormal->GetDescriptorIndexSlot();
                diFPush.gbufferAlbedoIndex = m_gbufferAlbedo->GetDescriptorIndexSlot();
                diFPush.gbufferMaterialIndex = m_gbufferMaterial->GetDescriptorIndexSlot();
                diFPush.viewportSize = glm::vec2(rw, rh);
                diFPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                diFPush.reservoirBlockRowPitch = diResParams.reservoirBlockRowPitch;
                diFPush.reservoirArrayPitch = diResParams.reservoirArrayPitch;
                diFPush.denoiserMode = static_cast<uint32_t>(m_nrdDIDenoiser);
                m_commandBuffers->pushData(&diFPush, sizeof(shaderio::PushConstantReSTIRDIFinalShading));

                m_commandBuffers->drawMeshTasks(1, 1, 1);
                m_commandBuffers->endRendering();
                m_commandBuffers->executionBarrier();
            }

            m_restirDILastFrameOutputReservoir = diBufferB;

            // =========================================================================
            // NRD DIRECT-LIGHTING DENOISING PASS (Denoises 1-SPP ReSTIR DI via REBLUR / RELAX)
            // =========================================================================
            bool restirDIDenoised = false;
            if (m_nrdDIDenoiser != NRI::NRDDiffuseDenoiser::Off &&
                m_device->isNRDInitialized() &&
                m_restirDIDirectLighting && m_denoisedReSTIRDIDirectLighting && m_viewZ && m_nrdNormalRoughness)
            {
                NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "NRD DI");
                NRI::NRDDiffuseParams diDenoiseParams{};
                diDenoiseParams.inDiffuseRadianceHitDist = m_restirDIDirectLighting.get();
                diDenoiseParams.inMotionVectors = m_gbufferVelocity.get();
                diDenoiseParams.inNormalRoughness = m_nrdNormalRoughness.get();
                diDenoiseParams.inViewZ = m_viewZ.get();
                diDenoiseParams.outDenoisedDiffuse = m_denoisedReSTIRDIDirectLighting.get();
                diDenoiseParams.commandBuffer = m_commandBuffers.get();

                diDenoiseParams.view = uniformData.view;
                diDenoiseParams.proj = uniformData.nonJitteredProj;
                diDenoiseParams.prevView = uniformData.prevView;
                diDenoiseParams.prevProj = uniformData.prevProj;

                diDenoiseParams.motionVectorScale = glm::vec2(1.0f, 1.0f);
                diDenoiseParams.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                diDenoiseParams.resetHistory = m_isFirstFrame || m_resetNRD;

                restirDIDenoised = m_device->evaluateNRDDiffuseDI(diDenoiseParams, m_nrdDIDenoiser);
                m_commandBuffers->executionBarrier();
                m_commandBuffers->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
            }

            uniformData.directLightingMode = m_directLightingMode;
            uniformData.restirDIDirectLightingTextureIndex = restirDIDenoised
                ? m_denoisedReSTIRDIDirectLighting->GetDescriptorIndexSlot()
                : m_restirDIDirectLighting->GetDescriptorIndexSlot();
            memcpy(m_uniformBuffersMapped[frameIndex], &uniformData, sizeof(shaderio::UniformBufferObject));
        }
        else
        {
            uniformData.directLightingMode = 0; // Fall back to the brute-force loop if ReSTIR DI cannot run yet (e.g. TLAS build pending)
            uniformData.restirDIDirectLightingTextureIndex = m_restirDIDirectLighting ? m_restirDIDirectLighting->GetDescriptorIndexSlot() : 0xFFFFFFFF;
            memcpy(m_uniformBuffersMapped[frameIndex], &uniformData, sizeof(shaderio::UniformBufferObject));
        }

        // =========================================================================
        // 3. LIGHTING PASS: PATH TRACER (Modes 18 & 19) OR DEFERRED LIGHTING
        // =========================================================================
        // runPathTracer computed earlier (before the reflections/DDGI/ReSTIR GI/DI sections); ReSTIR
        // DI/GI may still have run above when m_pathTracerUsesRTXDI is enabled.
        m_restirPTOutputValid = false;
        if (runPathTracer && m_restirPTEnabled && m_restirPTInitialPipeline && m_restirPTFinalShadingPipeline &&
            m_restirPTOutput && m_restirPTContext &&
            m_restirPTReservoirBuffers[0] && m_restirPTReservoirBuffers[1] && m_restirPTReservoirBuffers[2] &&
            (uniformData.tlasDeviceAddress != 0) && (uniformData.lightDataReference != 0))
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "ReSTIR PT");
            m_restirPTContext->SetFrameIndex(static_cast<uint32_t>(m_sceneFrameCounter));
            RTXDI_PTBufferIndices ptBufferIndices = m_restirPTContext->GetBufferIndices();
            RTXDI_ReservoirBufferParameters ptResParams = m_restirPTContext->GetReservoirBufferParameters();
            glm::mat4 ptViewProj = uniformData.proj * uniformData.view;

            // 1. Initial Sampling Pass
            {
                std::vector<NRI::RenderAttachDesc> ptiAttachments;
                ptiAttachments.push_back({
                    .attachment = m_restirPTOutput.get(), // debug-only output; real result lives in the reservoir buffer
                    .loadOP = NRI::LoadOP::clear,
                    .storeOP = NRI::StoreOP::store,
                    .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}
                });
                ptiAttachments.push_back({
                    .attachment = m_restirPTPrimaryDirect.get(), // bounce-1 direct lighting, added in by Final Shading
                    .loadOP = NRI::LoadOP::clear,
                    .storeOP = NRI::StoreOP::store,
                    .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}
                });

                NRI::RenderDesc ptiDesc = {
                    .renderArea = renderExtent,
                    .colorAttachments = ptiAttachments
                };

                m_commandBuffers->beginRendering(ptiDesc);
                m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
                m_commandBuffers->setScissorWithCount(renderExtent);

                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirPTInitialPipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(false);
                m_commandBuffers->setDepthWriteEnable(false);
                for (uint32_t a = 0; a < ptiAttachments.size(); ++a)
                {
                    m_commandBuffers->setColorBlendEnable(a, false);
                    m_commandBuffers->setColorWriteMask(a, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                        NRI::ColorComponent::B | NRI::ColorComponent::A);
                }

                shaderio::PushConstantReSTIRPTInitial ptiPush{};
                ptiPush.invViewProj = glm::inverse(ptViewProj);
                ptiPush.cameraWorldPos = uniformData.cameraWorldPos;
                ptiPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                ptiPush.lightDataReference = uniformData.lightDataReference;
                ptiPush.reservoirBufferReference = m_restirPTReservoirBuffers[ptBufferIndices.initialPathTracerOutputBufferIndex]->getDeviceAddress();
                ptiPush.preservedReservoirReference = m_restirPTReservoirBuffers[ptBufferIndices.initialPathTracerPreservedBufferIndex]->getDeviceAddress();
                ptiPush.depthTextureIndex = m_depthResource->GetDescriptorIndexSlot();
                ptiPush.gbufferNormalIndex = m_gbufferNormal->GetDescriptorIndexSlot();
                ptiPush.gbufferAlbedoIndex = m_gbufferAlbedo->GetDescriptorIndexSlot();
                ptiPush.gbufferMaterialIndex = m_gbufferMaterial->GetDescriptorIndexSlot();
                ptiPush.viewportSize = glm::vec2(rw, rh);
                ptiPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                ptiPush.reservoirBlockRowPitch = ptResParams.reservoirBlockRowPitch;
                ptiPush.reservoirArrayPitch = ptResParams.reservoirArrayPitch;
                ptiPush.firstLocalLightIndex = m_restirDIFirstLocalLight;
                ptiPush.numLocalLights = m_restirDINumLocalLights;
                ptiPush.firstInfiniteLightIndex = m_restirDIFirstInfiniteLight;
                ptiPush.numInfiniteLights = m_restirDINumInfiniteLights;
                ptiPush.numInitialSamples = m_restirPTNumInitialSamples;
                ptiPush.maxBounceDepth = m_restirPTMaxBounceDepth;
                ptiPush.maxRcVertexLength = m_restirPTMaxRcVertexLength;
                ptiPush.numNeeSamples = m_restirPTNumNeeSamples;
                ptiPush.roughnessThreshold = m_restirPTRoughnessThreshold;
                ptiPush.distanceThreshold = m_restirPTDistanceThreshold;
                ptiPush.skyboxTextureIndex = m_environmentCubemap ? m_environmentCubemap->GetDescriptorIndexSlot() : 0xFFFFFFFF;
                m_commandBuffers->pushData(&ptiPush, sizeof(shaderio::PushConstantReSTIRPTInitial));

                m_commandBuffers->drawMeshTasks(1, 1, 1);
                m_commandBuffers->endRendering();
                m_commandBuffers->executionBarrier();
            }

            // 2. Temporal Resampling Pass (RandomReplay/hybrid-shift reconnection against last frame's
            // finalized reservoir) -- only runs once the resampling mode actually needs it, so buffer
            // indices genuinely differ (None mode leaves both equal to 0, matching Initial's own gate).
            bool restirPTTemporalActive = m_restirPTTemporalPipeline &&
                ptBufferIndices.temporalResamplingInputBufferIndex != ptBufferIndices.initialPathTracerOutputBufferIndex;
            if (restirPTTemporalActive)
            {
                std::vector<NRI::RenderAttachDesc> pttAttachments;
                pttAttachments.push_back({
                    .attachment = m_restirPTOutput.get(), // debug-only output; real result lives in the reservoir buffer
                    .loadOP = NRI::LoadOP::clear,
                    .storeOP = NRI::StoreOP::store,
                    .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}
                });

                NRI::RenderDesc pttDesc = {
                    .renderArea = renderExtent,
                    .colorAttachments = pttAttachments
                };

                m_commandBuffers->beginRendering(pttDesc);
                m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
                m_commandBuffers->setScissorWithCount(renderExtent);

                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirPTTemporalPipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(false);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                    NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRPTTemporal pttPush{};
                // invViewProj deliberately omitted -- see the struct's own comment in shaderIO.h: the
                // shader reads it from g_UBO->invViewProj instead (same value, already computed there
                // as glm::inverse(uniformData.proj * uniformData.view)), which was needed to fit this
                // struct back under the device's 256-byte push-constant limit.
                pttPush.prevInvViewProj = glm::inverse(uniformData.prevProj * uniformData.prevView);
                pttPush.cameraWorldPos = uniformData.cameraWorldPos;
                pttPush.prevCameraWorldPos = glm::vec4(m_prevCameraWorldPos, 0.0f);
                pttPush.prevPrevCameraWorldPos = glm::vec4(m_prevPrevCameraWorldPos, 0.0f);
                pttPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                pttPush.lightDataReference = uniformData.lightDataReference;
                pttPush.currentReservoirReference = m_restirPTReservoirBuffers[ptBufferIndices.initialPathTracerOutputBufferIndex]->getDeviceAddress();
                pttPush.historyReservoirReference = m_restirPTReservoirBuffers[ptBufferIndices.temporalResamplingInputBufferIndex]->getDeviceAddress();
                pttPush.viewportSize = glm::vec2(rw, rh);
                pttPush.depthTextureIndex = m_depthResource->GetDescriptorIndexSlot();
                pttPush.prevDepthTextureIndex = m_prevDepthResource ? m_prevDepthResource->GetDescriptorIndexSlot() : 0xFFFFFFFF;
                pttPush.gbufferNormalIndex = m_gbufferNormal->GetDescriptorIndexSlot();
                pttPush.prevNormalTextureIndex = m_prevGbufferNormal ? m_prevGbufferNormal->GetDescriptorIndexSlot() : 0xFFFFFFFF;
                pttPush.gbufferAlbedoIndex = m_gbufferAlbedo->GetDescriptorIndexSlot();
                pttPush.prevAlbedoTextureIndex = m_prevGbufferAlbedo ? m_prevGbufferAlbedo->GetDescriptorIndexSlot() : 0xFFFFFFFF;
                pttPush.gbufferMaterialIndex = m_gbufferMaterial->GetDescriptorIndexSlot();
                pttPush.prevMaterialTextureIndex = m_prevGbufferMaterial ? m_prevGbufferMaterial->GetDescriptorIndexSlot() : 0xFFFFFFFF;
                pttPush.gbufferVelocityIndex = m_gbufferVelocity ? m_gbufferVelocity->GetDescriptorIndexSlot() : 0xFFFFFFFF;
                pttPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                pttPush.reservoirBlockRowPitch = ptResParams.reservoirBlockRowPitch;
                pttPush.reservoirArrayPitch = ptResParams.reservoirArrayPitch;
                pttPush.maxBounceDepth = m_restirPTMaxBounceDepth;
                pttPush.maxRcVertexLength = m_restirPTMaxRcVertexLength;
                pttPush.roughnessThreshold = m_restirPTRoughnessThreshold;
                pttPush.distanceThreshold = m_restirPTDistanceThreshold;
                pttPush.depthThreshold = m_restirPTDepthThreshold;
                pttPush.normalThreshold = m_restirPTNormalThreshold;
                pttPush.maxHistoryLength = m_restirPTMaxHistoryLength;
                pttPush.maxReservoirAge = m_restirPTMaxReservoirAge;
                pttPush.enablePermutationSampling = m_restirPTEnablePermutationSampling ? 1u : 0u;
                pttPush.skyboxTextureIndex = m_environmentCubemap ? m_environmentCubemap->GetDescriptorIndexSlot() : 0xFFFFFFFF;
                m_commandBuffers->pushData(&pttPush, sizeof(shaderio::PushConstantReSTIRPTTemporal));

                m_commandBuffers->drawMeshTasks(1, 1, 1);
                m_commandBuffers->endRendering();
                m_commandBuffers->executionBarrier();
            }

            // 3. Final Shading Pass
            {
                std::vector<NRI::RenderAttachDesc> ptfAttachments;
                ptfAttachments.push_back({
                    .attachment = m_restirPTOutput.get(),
                    .loadOP = NRI::LoadOP::clear,
                    .storeOP = NRI::StoreOP::store,
                    .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}
                });

                NRI::RenderDesc ptfDesc = {
                    .renderArea = renderExtent,
                    .colorAttachments = ptfAttachments
                };

                m_commandBuffers->beginRendering(ptfDesc);
                m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
                m_commandBuffers->setScissorWithCount(renderExtent);

                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirPTFinalShadingPipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(false);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                    NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRPTFinalShading ptfPush{};
                ptfPush.invViewProj = glm::inverse(ptViewProj);
                ptfPush.cameraWorldPos = uniformData.cameraWorldPos;
                ptfPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                ptfPush.lightDataReference = uniformData.lightDataReference;
                ptfPush.reservoirReference = m_restirPTReservoirBuffers[ptBufferIndices.finalShadingInputBufferIndex]->getDeviceAddress();
                ptfPush.preservedReservoirReference = m_restirPTReservoirBuffers[ptBufferIndices.initialPathTracerPreservedBufferIndex]->getDeviceAddress();
                ptfPush.depthTextureIndex = m_depthResource->GetDescriptorIndexSlot();
                ptfPush.gbufferNormalIndex = m_gbufferNormal->GetDescriptorIndexSlot();
                ptfPush.primaryDirectTextureIndex = m_restirPTPrimaryDirect->GetDescriptorIndexSlot();
                ptfPush.viewportSize = glm::vec2(rw, rh);
                ptfPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                ptfPush.reservoirBlockRowPitch = ptResParams.reservoirBlockRowPitch;
                ptfPush.reservoirArrayPitch = ptResParams.reservoirArrayPitch;
                // Shares the same "PT Denoiser" setting/combo as the plain path tracer -- REBLUR needs the
                // shader's own YCoCg pre-encode (see LinearToYCoCg in ReSTIRPTFinalShading.slang), RELAX
                // reads plain linear color, matching PathTracer.slang's own convention exactly.
                ptfPush.denoiserMode = static_cast<uint32_t>(m_nrdPTDenoiser);
                // RTXDI PT's final-shading decorrelation path: randomly use the preserved, unresampled
                // initial reservoir to break temporal over-correlation. Stagnancy mode needs the SDK
                // duplication-map pass; this renderer does not have that pass yet, so use Uniform mode.
                ptfPush.decorrelationFactor = restirPTTemporalActive ? 0.4f : 0.0f;
                ptfPush.decorrelationMode = restirPTTemporalActive ? 1u : 0u; // RTXDI_PT_DECORRELATION_MODE_UNIFORM/NONE
                m_commandBuffers->pushData(&ptfPush, sizeof(shaderio::PushConstantReSTIRPTFinalShading));

                m_commandBuffers->drawMeshTasks(1, 1, 1);
                m_commandBuffers->endRendering();
                m_commandBuffers->executionBarrier();
            }

            m_restirPTOutputValid = true;

            // =========================================================================
            // NRD DENOISING PASS -- shares m_nrdPTDenoiser/m_denoisedPathTracer/m_pathTracerDenoised
            // with the plain path tracer (see the `else if` branch below) since the two are mutually
            // exclusive per frame (only one of them ever runs), so there's no benefit to separate state.
            // DLSS Ray Reconstruction needs no equivalent block here: dlssParams.inputColor already
            // prefers m_restirPTOutput whenever m_restirPTOutputValid is true (see evaluateDLSS's call
            // site below), and DLSS-RR denoises whatever raw HDR color it's given directly -- it doesn't
            // care which technique produced that color, only that the G-buffer guide textures (normal/
            // roughness/motion/depth) it also reads are valid, which they already are here.
            // =========================================================================
            m_pathTracerDenoised = false;
            bool restirPTCameraMoved = (uniformData.view != m_pathTracerPrevView);
            m_pathTracerPrevView = uniformData.view;
            if (m_nrdPTDenoiser != NRI::NRDDiffuseDenoiser::Off && m_device->isNRDInitialized() &&
                m_denoisedPathTracer && m_viewZ && m_nrdNormalRoughness)
            {
                NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "NRD PT");
                NRI::NRDDiffuseParams ptDenoiseParams{};
                ptDenoiseParams.inDiffuseRadianceHitDist = m_restirPTOutput.get();
                ptDenoiseParams.inMotionVectors = m_gbufferVelocity.get();
                ptDenoiseParams.inNormalRoughness = m_nrdNormalRoughness.get();
                ptDenoiseParams.inViewZ = m_viewZ.get();
                ptDenoiseParams.outDenoisedDiffuse = m_denoisedPathTracer.get();
                ptDenoiseParams.commandBuffer = m_commandBuffers.get();

                ptDenoiseParams.view = uniformData.view;
                ptDenoiseParams.proj = uniformData.nonJitteredProj;
                ptDenoiseParams.prevView = uniformData.prevView;
                ptDenoiseParams.prevProj = uniformData.prevProj;

                ptDenoiseParams.motionVectorScale = glm::vec2(1.0f, 1.0f);
                ptDenoiseParams.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                ptDenoiseParams.resetHistory = m_isFirstFrame || m_resetNRD || restirPTCameraMoved;

                m_pathTracerDenoised = m_device->evaluateNRDDiffusePT(ptDenoiseParams, m_nrdPTDenoiser);
                m_commandBuffers->executionBarrier();
                m_commandBuffers->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());

                if (m_pathTracerDenoised && m_nrdPTDenoiser == NRI::NRDDiffuseDenoiser::REBLUR && m_ycocgDecodePipeline)
                {
                    m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Compute, *m_ycocgDecodePipeline);
                    shaderio::PushConstantYCoCgDecode decodePush{};
                    decodePush.readTextureIndex = m_denoisedPathTracer->GetDescriptorIndexSlot();
                    decodePush.writeTextureIndex = m_denoisedPathTracerWriteSlot;
                    decodePush.width = m_renderSize.width;
                    decodePush.height = m_renderSize.height;
                    m_commandBuffers->pushData(&decodePush, sizeof(shaderio::PushConstantYCoCgDecode));
                    m_commandBuffers->dispatch((m_renderSize.width + 7) / 8, (m_renderSize.height + 7) / 8, 1);
                    m_commandBuffers->executionBarrier();
                }
            }
        }
        else if (runPathTracer && m_pathTracerPipeline && m_pathTracerAccum[0] && m_pathTracerAccum[1])
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "Path Tracer");
            bool dlssRRActive = m_dlssEnabled && (m_dlssMode != NRI::UpscaleMode::Off) && m_dlssRayReconstructionEnabled;
            bool nrdPTActive = m_nrdPTDenoiser != NRI::NRDDiffuseDenoiser::Off;
            bool cameraMoved = (uniformData.view != m_pathTracerPrevView);

            // If DLSS-RR or NRD is active, it denoises 1-SPP per frame via motion vectors instead --
            // running our own progressive accumulation on top would double up on temporal blending.
            // Accumulate progressively only when neither denoiser is handling that job, and static.
            bool accumulate = m_pathTracingAccumulation && (m_debugMode != 18) && !dlssRRActive && !nrdPTActive;
            if (cameraMoved || !accumulate)
            {
                m_pathTracerSampleCount = 0;
            }
            m_pathTracerPrevView = uniformData.view;
            m_pathTracerSampleCount++;

            uint32_t writeIndex = m_pathTracerSampleCount % 2;
            uint32_t readIndex = 1 - writeIndex;

            std::vector<NRI::RenderAttachDesc> ptAttachments;
            ptAttachments.push_back({
                .attachment = m_pathTracerAccum[writeIndex].get(),
                .loadOP = NRI::LoadOP::clear,
                .storeOP = NRI::StoreOP::store,
                .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}
            });

            NRI::RenderDesc ptDesc = {
                .renderArea = renderExtent,
                .colorAttachments = ptAttachments
            };

            m_commandBuffers->beginRendering(ptDesc);
            m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
            m_commandBuffers->setScissorWithCount(renderExtent);

            m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_pathTracerPipeline);
            m_commandBuffers->setCullMode(NRI::CullMode::None);
            m_commandBuffers->setDepthTestEnable(false);
            m_commandBuffers->setDepthWriteEnable(false);
            m_commandBuffers->setColorBlendEnable(0, false);
            m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

            glm::mat4 viewProj = uniformData.proj * uniformData.view;
            shaderio::PushConstantPathTracer ptPush{};
            ptPush.invViewProj = glm::inverse(viewProj);
            ptPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
            ptPush.viewportSize = glm::vec2(rw, rh);
            ptPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
            ptPush.maxBounces = 3;
            ptPush.accumulationTextureIndex = m_pathTracerAccum[readIndex]->GetDescriptorIndexSlot();
            ptPush.sampleCount = accumulate ? m_pathTracerSampleCount : 1;
            ptPush.debugMode = accumulate ? 19 : 18;
            ptPush.skyboxTextureIndex = m_environmentCubemap ? m_environmentCubemap->GetDescriptorIndexSlot() : 0xFFFFFFFF;
            ptPush.denoiserMode = static_cast<uint32_t>(m_nrdPTDenoiser);
            ptPush.restirGIDiffuseTextureIndex = uniformData.restirGIDiffuseTextureIndex;
            bool restirGIActuallyDenoisedForPT = m_denoisedReSTIRGIDiffuse &&
                uniformData.restirGIDiffuseTextureIndex == m_denoisedReSTIRGIDiffuse->GetDescriptorIndexSlot();
            ptPush.restirGIDenoiserMode = restirGIActuallyDenoisedForPT ? static_cast<uint32_t>(m_nrdGIDenoiser) : 0u;
            ptPush.restirDIDirectLightingTextureIndex = uniformData.restirDIDirectLightingTextureIndex;
            bool restirDIActuallyDenoisedForPT = m_denoisedReSTIRDIDirectLighting &&
                uniformData.restirDIDirectLightingTextureIndex == m_denoisedReSTIRDIDirectLighting->GetDescriptorIndexSlot();
            ptPush.restirDIDenoiserMode = restirDIActuallyDenoisedForPT ? static_cast<uint32_t>(m_nrdDIDenoiser) : 0u;
            m_commandBuffers->pushData(&ptPush, sizeof(shaderio::PushConstantPathTracer));

            m_commandBuffers->drawMeshTasks(1, 1, 1);
            m_commandBuffers->endRendering();
            m_commandBuffers->executionBarrier();

            // =========================================================================
            // NRD PATH TRACER DENOISING PASS (fallback for hardware/preference without DLSS-RR)
            // =========================================================================
            m_pathTracerDenoised = false;
            if (nrdPTActive && m_device->isNRDInitialized() &&
                m_pathTracerAccum[writeIndex] && m_denoisedPathTracer && m_viewZ && m_nrdNormalRoughness)
            {
                NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "NRD PT");
                NRI::NRDDiffuseParams ptDenoiseParams{};
                ptDenoiseParams.inDiffuseRadianceHitDist = m_pathTracerAccum[writeIndex].get();
                ptDenoiseParams.inMotionVectors = m_gbufferVelocity.get();
                ptDenoiseParams.inNormalRoughness = m_nrdNormalRoughness.get();
                ptDenoiseParams.inViewZ = m_viewZ.get();
                ptDenoiseParams.outDenoisedDiffuse = m_denoisedPathTracer.get();
                ptDenoiseParams.commandBuffer = m_commandBuffers.get();

                ptDenoiseParams.view = uniformData.view;
                ptDenoiseParams.proj = uniformData.nonJitteredProj;
                ptDenoiseParams.prevView = uniformData.prevView;
                ptDenoiseParams.prevProj = uniformData.prevProj;

                ptDenoiseParams.motionVectorScale = glm::vec2(1.0f, 1.0f);
                ptDenoiseParams.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                ptDenoiseParams.resetHistory = m_isFirstFrame || m_resetNRD || cameraMoved;

                m_pathTracerDenoised = m_device->evaluateNRDDiffusePT(ptDenoiseParams, m_nrdPTDenoiser);
                m_commandBuffers->executionBarrier();
                m_commandBuffers->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());

                // REBLUR's output is still YCoCg-encoded (see LinearToYCoCg in PathTracer.slang) --
                // decode it back to linear in place before DLSS/tonemapping (which don't know about
                // YCoCg) ever read it. RELAX never encodes YCoCg, so this is skipped for it.
                if (m_pathTracerDenoised && m_nrdPTDenoiser == NRI::NRDDiffuseDenoiser::REBLUR && m_ycocgDecodePipeline)
                {
                    m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Compute, *m_ycocgDecodePipeline);
                    shaderio::PushConstantYCoCgDecode decodePush{};
                    decodePush.readTextureIndex = m_denoisedPathTracer->GetDescriptorIndexSlot();
                    decodePush.writeTextureIndex = m_denoisedPathTracerWriteSlot;
                    decodePush.width = m_renderSize.width;
                    decodePush.height = m_renderSize.height;
                    m_commandBuffers->pushData(&decodePush, sizeof(shaderio::PushConstantYCoCgDecode));
                    m_commandBuffers->dispatch((m_renderSize.width + 7) / 8, (m_renderSize.height + 7) / 8, 1);
                    m_commandBuffers->executionBarrier();
                }
            }
        }
        else if (m_deferredLightingPipeline)
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "Deferred Lighting");
            std::vector<NRI::RenderAttachDesc> lightingAttachments;
            lightingAttachments.push_back({
                .attachment = m_hdrSceneResource.get(),
                .loadOP = NRI::LoadOP::clear,
                .storeOP = NRI::StoreOP::store,
                .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}
            });

            NRI::RenderDesc lightingDesc = {
                .renderArea = renderExtent,
                .colorAttachments = lightingAttachments
            };

            m_commandBuffers->beginRendering(lightingDesc);
            m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
            m_commandBuffers->setScissorWithCount(renderExtent);

            m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_deferredLightingPipeline);
            m_commandBuffers->setCullMode(NRI::CullMode::None);
            m_commandBuffers->setDepthTestEnable(false);
            m_commandBuffers->setDepthWriteEnable(false);
            m_commandBuffers->setColorBlendEnable(0, false);
            m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

            glm::mat4 viewProj = uniformData.proj * uniformData.view;
            shaderio::PushConstantDeferredLighting lightingPush{};
            lightingPush.invViewProj = glm::inverse(viewProj);
            lightingPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
            lightingPush.visibilityTextureIndex = m_visibilityResource->GetDescriptorIndexSlot();
            lightingPush.gbufferAlbedoIndex = m_gbufferAlbedo->GetDescriptorIndexSlot();
            lightingPush.gbufferNormalIndex = m_gbufferNormal->GetDescriptorIndexSlot();
            lightingPush.gbufferMaterialIndex = m_gbufferMaterial->GetDescriptorIndexSlot();
            lightingPush.gbufferEmissionIndex = m_gbufferEmission->GetDescriptorIndexSlot();
            lightingPush.depthTextureIndex = m_depthResource->GetDescriptorIndexSlot();
            lightingPush.viewportSize = glm::vec2(rw, rh);
            lightingPush.debugMode = m_debugMode;
            lightingPush.gbufferVelocityIndex = m_gbufferVelocity->GetDescriptorIndexSlot();
            lightingPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
            // m_denoisedShadowMask is only re-evaluated when BOTH m_nrdShadowsEnabled AND
            // (uniformData.enableRTShadows != 0) are true (see the NRD shadow denoising pass's own
            // gate) -- checking m_rayTracingShadows alone here isn't enough, because enableRTShadows is
            // (m_rayTracingEnabled && m_rayTracingShadows): turning off the MASTER "Hybrid Ray Tracing"
            // toggle leaves m_rayTracingShadows sitting at true (setRayTracingEnabled is a separate,
            // independent setter), so a m_rayTracingShadows-only check still thought the denoised
            // texture was fresh even though the denoiser pass had correctly stopped updating it -- same
            // frozen-shadow-follows-the-camera bug, just reachable through the other toggle too. Using
            // the same uniformData.enableRTShadows the shadow mask/NRD passes themselves already
            // computed this frame guarantees this can never drift out of sync with them again.
            bool shadowDenoisedAvailable = m_nrdShadowsEnabled && (uniformData.enableRTShadows != 0) && m_denoisedShadowMask;
            lightingPush.shadowMaskTextureIndex = shadowDenoisedAvailable
                ? m_denoisedShadowMask->GetDescriptorIndexSlot()
                : (m_rawShadowMask ? m_rawShadowMask->GetDescriptorIndexSlot() : 0);
            uint32_t nrdShadowBit = shadowDenoisedAvailable ? 1 : 0;
            // Only report REBLUR/RELAX to the shader when the denoised texture is actually what's
            // bound below -- if NRD hasn't initialized yet (or the resource is momentarily null during
            // a resize) this falls back to the raw buffer, which is plain linear RGB either way, so
            // reporting the denoiser mode in that case would make the shader wrongly YCoCg-decode it.
            // Same reasoning as shadowDenoisedAvailable above -- uniformData.enableRTReflections (not
            // m_rayTracingReflections alone) matches exactly what the NRD reflection denoising pass
            // itself gates on.
            bool reflectionActuallyDenoised = m_nrdReflectionDenoiser != NRI::NRDReflectionDenoiser::Off && (uniformData.enableRTReflections != 0) && m_denoisedReflection;
            uint32_t nrdReflMode = reflectionActuallyDenoised ? static_cast<uint32_t>(m_nrdReflectionDenoiser) : 0;
            lightingPush.nrdShadowsEnabled = nrdShadowBit | (nrdReflMode << 1);
            lightingPush.reflectionTextureIndex = reflectionActuallyDenoised
                ? m_denoisedReflection->GetDescriptorIndexSlot()
                : (m_rawReflection ? m_rawReflection->GetDescriptorIndexSlot() : 0);
            lightingPush.diffuseGIMode = uniformData.diffuseGIMode;
            lightingPush.restirGIDiffuseTextureIndex = uniformData.restirGIDiffuseTextureIndex;
            // Only meaningful when the bound texture is actually the denoised one -- if REBLUR/RELAX
            // is selected but denoising was skipped this frame (e.g. NRD not initialized yet),
            // uniformData.restirGIDiffuseTextureIndex falls back to the raw buffer, which is already
            // plain linear RGB, so compare against the denoised texture's own slot rather than trusting
            // the m_nrdGIDenoiser setting alone.
            bool restirGIActuallyDenoised = m_denoisedReSTIRGIDiffuse &&
                uniformData.restirGIDiffuseTextureIndex == m_denoisedReSTIRGIDiffuse->GetDescriptorIndexSlot();
            lightingPush.restirGIDenoiserMode = restirGIActuallyDenoised ? static_cast<uint32_t>(m_nrdGIDenoiser) : 0;
            lightingPush.directLightingMode = uniformData.directLightingMode;
            lightingPush.restirDIDirectLightingTextureIndex = uniformData.restirDIDirectLightingTextureIndex;
            // Same reasoning as restirGIActuallyDenoised above: only decode YCoCg when the bound
            // texture is actually the denoised one this frame.
            bool restirDIActuallyDenoised = m_denoisedReSTIRDIDirectLighting &&
                uniformData.restirDIDirectLightingTextureIndex == m_denoisedReSTIRDIDirectLighting->GetDescriptorIndexSlot();
            lightingPush.restirDIDenoiserMode = restirDIActuallyDenoised ? static_cast<uint32_t>(m_nrdDIDenoiser) : 0;
            m_commandBuffers->pushData(&lightingPush, sizeof(shaderio::PushConstantDeferredLighting));

            m_commandBuffers->drawMeshTasks(1, 1, 1);
            m_commandBuffers->endRendering();
            m_commandBuffers->executionBarrier();
        }

        // =========================================================================
        // 4. FORWARD 3D PASS: UNLIT & SKYBOX (Rendered in HDR into m_hdrSceneResource)
        // =========================================================================
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "Forward 3D");
            std::vector<NRI::RenderAttachDesc> forward3DAttachments;
            forward3DAttachments.push_back({
                .attachment = m_hdrSceneResource.get(),
                .loadOP = NRI::LoadOP::load,
                .storeOP = NRI::StoreOP::store
            });
            forward3DAttachments.push_back({
                .attachment = m_entityResource.get(),
                .loadOP = NRI::LoadOP::load,
                .storeOP = NRI::StoreOP::store
            });

            NRI::RenderAttachDesc forwardDepthAttachment = {
                .attachment = m_depthResource.get(),
                .loadOP = NRI::LoadOP::load,
                .storeOP = NRI::StoreOP::store
            };

            NRI::RenderDesc forward3DDesc = {
                .renderArea = renderExtent,
                .colorAttachments = forward3DAttachments,
                .depthAttachment = forwardDepthAttachment
            };

            m_commandBuffers->beginRendering(forward3DDesc);
            m_commandBuffers->setViewportWithCount({0.0f, rh, rw, -rh}, 0.0f, 1.0f);
            m_commandBuffers->setScissorWithCount(renderExtent);

            // A. UNLIT MESHES
            if (m_unlitPipeline && (m_unlitCount > 0 || m_unlitDoubleSidedCount > 0))
            {
                boundPipeline = nullptr;
                m_commandBuffers->setDepthTestEnable(true);
                m_commandBuffers->setDepthWriteEnable(true);
                m_commandBuffers->setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);
                m_commandBuffers->setColorBlendEnable(1, false);
                m_commandBuffers->setColorWriteMask(1, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                drawPass(m_unlitCount, *m_unlitPipeline, NRI::CullMode::Back, true, false);
                drawPass(m_unlitDoubleSidedCount, *m_unlitPipeline, NRI::CullMode::None, true, false);
            }

            // B. SKYBOX (Tested against depth == 0.0)
            if (m_skyboxPipeline && m_environmentCubemap && m_debugMode == 0)
            {
                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_skyboxPipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(true);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);
                m_commandBuffers->setColorBlendEnable(1, false);
                m_commandBuffers->setColorWriteMask(1, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantSkybox skyboxPush{};
                skyboxPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                skyboxPush.cubemapIndex = m_environmentCubemap->GetDescriptorIndexSlot();
                m_commandBuffers->pushData(&skyboxPush, sizeof(shaderio::PushConstantSkybox));
                m_commandBuffers->drawMeshTasks(1, 1, 1);
            }

            // C. TRANSPARENT FORWARD PASS (Back-to-front sorted, Alpha Blending)
            bool hasAnyTransparent = m_transparentCount > 0 || m_transparentDoubleSidedCount > 0 ||
                                      m_transparentUnlitCount > 0 || m_transparentUnlitDoubleSidedCount > 0;
            if (m_unlitPipeline && m_transparentLitPipeline && hasAnyTransparent)
            {
                boundPipeline = nullptr;
                m_commandBuffers->setDepthTestEnable(true);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                m_commandBuffers->setColorBlendEnable(0, true);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);
                m_commandBuffers->setColorBlendEnable(1, false);
                m_commandBuffers->setColorWriteMask(1, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                // Non-unlit Blend objects get real shading now (same BRDF/IBL the deferred opaque
                // path uses, via PBRLighting.slang) instead of a flat baseColor pass-through, so
                // they don't look like they're glowing/self-lit next to properly shaded Opaque/Mask
                // geometry. True KHR_materials_unlit objects still use the flat shader, correctly.
                //
                // CullMode matches each object's own doubleSided flag (mirroring the opaque/mask
                // queues) instead of a blanket CullMode::None - a closed, single-sided translucent
                // shape (a sphere/box shell, not a flat card) needs its own backface actually culled,
                // or both the near and far hemisphere/wall triangles rasterize and alpha-blend on
                // top of each other, which washes the result out instead of a clean single surface.
                drawPass(m_transparentCount, *m_transparentLitPipeline, NRI::CullMode::Back, false, true);
                drawPass(m_transparentDoubleSidedCount, *m_transparentLitPipeline, NRI::CullMode::None, false, true);
                drawPass(m_transparentUnlitCount, *m_unlitPipeline, NRI::CullMode::Back, false, true);
                drawPass(m_transparentUnlitDoubleSidedCount, *m_unlitPipeline, NRI::CullMode::None, false, true);

                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setDepthWriteEnable(true);
            }

            // D. DDGI PROBE SPHERES (Debug Mode 17: Visualizes 3D Probe Grid with Irradiance)
            if (m_ddgiDebugSpheresPipeline && m_debugMode == 17 && totalDDGIProbes > 0 && m_ddgiIrradiance[m_ddgiHistoryIndex])
            {
                m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_ddgiDebugSpheresPipeline);
                m_commandBuffers->setCullMode(NRI::CullMode::None);
                m_commandBuffers->setDepthTestEnable(!m_ddgiDebugXRay);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);
                m_commandBuffers->setColorBlendEnable(1, false);
                m_commandBuffers->setColorWriteMask(1, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantDDGIDebug debugPush{};
                debugPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                debugPush.probeCountTotal = totalDDGIProbes;
                debugPush.sphereRadius = m_ddgiDebugSphereRadius;
                debugPush.irradianceAtlasIndex = m_ddgiIrradiance[m_ddgiHistoryIndex]->GetDescriptorIndexSlot();
                m_commandBuffers->pushData(&debugPush, sizeof(shaderio::PushConstantDDGIDebug));

                m_commandBuffers->drawMeshTasks(totalDDGIProbes, 1, 1);
            }

            m_commandBuffers->endRendering();
            m_commandBuffers->executionBarrier();
        }

        // Cleared here, now that every NRD consumer this frame (shadows/reflections above, and
        // GI/DI/path-tracer's own denoise passes inside the lighting-pass switch just above) has had a
        // chance to read it as a one-shot "a denoiser was just toggled, flush history" signal.
        m_resetNRD = false;

        // 4.5 DLSS EVALUATION PASS (via NRI Device Abstraction)
        // =========================================================================
        bool dlssActive = false;
        if (m_dlssEnabled && m_dlssMode != NRI::UpscaleMode::Off && m_device->isDLSSSupported() && m_dlssOutputResource)
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "DLSS");
            uint32_t ptWriteIndex = m_pathTracerSampleCount % 2;

            NRI::DLSSParams dlssParams{};
            // Route the noisy 1-SPP Path Tracer buffer to DLSS when Path Tracing is active -- unless
            // NRD already denoised it this frame (mutually exclusive with DLSS-RR, so DLSS here is
            // acting as pure upscaling), in which case feed it the already-denoised, already-decoded
            // linear result instead of the raw noisy one. ReSTIR PT's output (already RIS-resampled,
            // one path per pixel from numInitialSamples candidates) takes priority over the plain path
            // tracer's raw accumulation buffer when it ran this frame.
            dlssParams.inputColor = m_restirPTOutputValid
                ? (m_pathTracerDenoised ? m_denoisedPathTracer.get() : m_restirPTOutput.get())
                : (runPathTracer
                    ? (m_pathTracerDenoised ? m_denoisedPathTracer.get() : m_pathTracerAccum[ptWriteIndex].get())
                    : m_hdrSceneResource.get());
            dlssParams.outputColor = m_dlssOutputResource.get();
            dlssParams.depth = m_depthResource.get();
            dlssParams.motionVectors = m_gbufferVelocity.get();
            dlssParams.albedo = m_gbufferAlbedo.get();
            dlssParams.specularAlbedo = m_gbufferSpecular.get();
            dlssParams.normal = m_gbufferNormal.get();
            dlssParams.roughness = m_gbufferMaterial.get();
            dlssParams.rayReconstruction = m_dlssRayReconstructionEnabled;
            dlssParams.commandBuffer = m_commandBuffers.get();

            dlssParams.nonJitteredProj = uniformData.nonJitteredProj;
            dlssParams.view = uniformData.view;
            dlssParams.prevNonJitteredProj = uniformData.prevProj;
            dlssParams.prevView = uniformData.prevView;

            dlssParams.jitterOffset = m_currentJitter;
            dlssParams.cameraPos = m_cameraPosition;
            dlssParams.cameraUp = m_cameraUp;
            dlssParams.cameraRight = m_cameraRight;
            dlssParams.cameraFwd = m_cameraForward;
            dlssParams.cameraNear = m_cameraNear;
            dlssParams.cameraFovRad = glm::radians(m_cameraFOV);
            dlssParams.mode = m_dlssMode;
            dlssParams.reset = m_isFirstFrame || m_resetDLSS;
            m_resetDLSS = false;

            dlssActive = m_device->evaluateDLSS(dlssParams);
        }
        m_commandBuffers->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());

        // outputExtent/ow/oh: final display/swapchain resolution, used by everything from here on
        // (post-process, 2D overlays, outline, present) - as opposed to renderExtent/rw/rh above,
        // which is the (possibly smaller, when DLSS is scaling) resolution the 3D scene rendered at.
        NRI::Extent2D outputExtent = m_isEditor ? m_viewportSize : m_swapChainExtent;
        float ow = static_cast<float>(outputExtent.width);
        float oh = static_cast<float>(outputExtent.height);

        // 4.6 Propagate render-resolution entity IDs/depth up to display resolution. The 2D overlay
        // pass right below composites against the final image and needs to depth-test world-space 2D
        // content (e.g. in-world text/signs) against the 3D scene, and mouse-picking/the outline effect
        // need entity IDs at full display resolution - a nearest blit is the cheapest way to give them
        // that without re-rendering the 3D scene a second time at display resolution. Everything
        // downstream reads exclusively from the Hi copies, so this always has to run even at 1:1
        // (DLSS off/DLAA), where it's just a same-size copy.
        NOX_PROFILE_GPU_BEGIN(*m_commandBuffers, "Entity/Depth Blit");
        m_entityResource->blitTo(*m_commandBuffers, *m_entityResourceHi);
        m_depthResource->blitTo(*m_commandBuffers, *m_depthResourceHi);
        m_commandBuffers->executionBarrier();
        NOX_PROFILE_GPU_END(*m_commandBuffers);

        // =========================================================================
        // 5. POST-PROCESSING & TONEMAPPING (HDR m_hdrSceneResource -> LDR m_sceneResource)
        // =========================================================================
        if (m_postProcessPipeline)
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "Post Process");
            std::vector<NRI::RenderAttachDesc> postAttachments;
            postAttachments.push_back({
                .attachment = m_sceneResource.get(),
                .loadOP = NRI::LoadOP::clear,
                .storeOP = NRI::StoreOP::store,
                .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}
            });

            NRI::RenderDesc postDesc = {
                .renderArea = outputExtent,
                .colorAttachments = postAttachments
            };

            m_commandBuffers->beginRendering(postDesc);
            m_commandBuffers->setViewportWithCount({0.0f, oh, ow, -oh}, 0.0f, 1.0f);
            m_commandBuffers->setScissorWithCount(outputExtent);

            m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_postProcessPipeline);
            m_commandBuffers->setCullMode(NRI::CullMode::None);
            m_commandBuffers->setDepthTestEnable(false);
            m_commandBuffers->setDepthWriteEnable(false);
            m_commandBuffers->setColorBlendEnable(0, false);
            m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

            shaderio::PushConstantPostProcess postPush{};
            postPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
            uint32_t activeHdrSlot = m_hdrSceneResource->GetDescriptorIndexSlot();

            if (dlssActive && m_dlssOutputResource)
            {
                activeHdrSlot = m_dlssOutputResource->GetDescriptorIndexSlot();
            }
            else if (runPathTracer)
            {
                if (m_restirPTOutputValid && m_pathTracerDenoised && m_denoisedPathTracer)
                {
                    activeHdrSlot = m_denoisedPathTracer->GetDescriptorIndexSlot();
                }
                else if (m_restirPTOutputValid && m_restirPTOutput)
                {
                    activeHdrSlot = m_restirPTOutput->GetDescriptorIndexSlot();
                }
                else if (m_pathTracerDenoised && m_denoisedPathTracer)
                {
                    activeHdrSlot = m_denoisedPathTracer->GetDescriptorIndexSlot();
                }
                else
                {
                    uint32_t writeIndex = m_pathTracerSampleCount % 2;
                    if (m_pathTracerAccum[writeIndex])
                        activeHdrSlot = m_pathTracerAccum[writeIndex]->GetDescriptorIndexSlot();
                }
            }

            postPush.hdrTextureIndex = activeHdrSlot;
            postPush.debugMode = m_debugMode;
            postPush.tonemapMode = m_tonemapMode;
            m_commandBuffers->pushData(&postPush, sizeof(shaderio::PushConstantPostProcess));

            m_commandBuffers->drawMeshTasks(1, 1, 1);
            m_commandBuffers->endRendering();
        }

        // =========================================================================
        // 6. FORWARD 2D OVERLAYS (Quads, Circles, Text, Gizmos - Rendered on LDR Scene)
        // =========================================================================
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "2D Overlay");
            std::vector<NRI::RenderAttachDesc> forward2DAttachments;
            forward2DAttachments.push_back({
                .attachment = m_sceneResource.get(),
                .loadOP = NRI::LoadOP::load,
                .storeOP = NRI::StoreOP::store
            });
            forward2DAttachments.push_back({
                .attachment = m_entityResourceHi.get(),
                .loadOP = NRI::LoadOP::load,
                .storeOP = NRI::StoreOP::store
            });

            NRI::RenderAttachDesc forward2DDepth = {
                .attachment = m_depthResourceHi.get(),
                .loadOP = NRI::LoadOP::load,
                .storeOP = NRI::StoreOP::store
            };

            NRI::RenderDesc forward2DDesc = {
                .renderArea = outputExtent,
                .colorAttachments = forward2DAttachments,
                .depthAttachment = forward2DDepth
            };

            m_commandBuffers->beginRendering(forward2DDesc);
            m_commandBuffers->setViewportWithCount({0.0f, oh, ow, -oh}, 0.0f, 1.0f);
            m_commandBuffers->setScissorWithCount(outputExtent);

            const NRI::ColorBlendEquation blendEquation{
                .srcColorBlendFactor = NRI::BlendFactor::SrcAlpha,
                .dstColorBlendFactor = NRI::BlendFactor::OneMinusSrcAlpha,
                .colorBlendOp = NRI::BlendOp::Add,
                .srcAlphaBlendFactor = NRI::BlendFactor::Zero,
                .dstAlphaBlendFactor = NRI::BlendFactor::One,
                .alphaBlendOp = NRI::BlendOp::Add,
            };

            m_commandBuffers->setCullMode(NRI::CullMode::None);
            m_commandBuffers->setDepthTestEnable(true);
            m_commandBuffers->setDepthWriteEnable(false);
            m_commandBuffers->setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);

            m_commandBuffers->setColorBlendEnable(0, true);
            m_commandBuffers->setColorBlendEquation(0, blendEquation);
            m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

            m_commandBuffers->setColorBlendEnable(1, false);
            m_commandBuffers->setColorWriteMask(1, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

            m_renderer2D->Flush(*m_commandBuffers, *m_uniformBuffers[frameIndex], frameIndex);
            m_commandBuffers->endRendering();
        }

        // --- OUTLINE POST-PROCESS ---
        if (!m_SelectedEntityIDs.empty() && m_outlinePipeline)
        {
            NOX_PROFILE_GPU_SCOPE(*m_commandBuffers, "Outline");
            std::vector<NRI::RenderAttachDesc> outlineColorAttachments;
            outlineColorAttachments.push_back({
                .attachment = m_sceneResource.get(),
                .loadOP = NRI::LoadOP::load,
                .storeOP = NRI::StoreOP::store,
            });

            NRI::RenderDesc outlineDesc =
            {
                .renderArea = m_isEditor
                                  ? m_viewportSize
                                  : m_swapChainExtent,
                .colorAttachments = outlineColorAttachments,
            };

            m_commandBuffers->beginRendering(outlineDesc);

            const NRI::Extent2D locViewportSize =
                m_isEditor ? m_viewportSize : m_swapChainExtent;

            const float w = static_cast<float>(locViewportSize.width);
            const float h = static_cast<float>(locViewportSize.height);

            // Same coordinate convention as the main rendering pass.
            m_commandBuffers->setViewportWithCount(
                {0.0f, h, w, -h},
                0.0f,
                1.0f
            );

            m_commandBuffers->setScissorWithCount(locViewportSize);

            // Rasterization
            m_commandBuffers->setRasterizerDiscardEnable(false);
            m_commandBuffers->setPolygonMode(NRI::PolygonMode::Fill);
            m_commandBuffers->setCullMode(NRI::CullMode::None);
            m_commandBuffers->setFrontFace(NRI::FrontFace::CounterClockWise);
            m_commandBuffers->setDepthBiasEnable(false);
            m_commandBuffers->setDepthClampEnable(false);

            // This is a fullscreen post-process, so one sample is correct.
            m_commandBuffers->setRasterizationSamples(1);
            m_commandBuffers->setSampleMask(1, 0xFFFFFFFF);
            m_commandBuffers->setAlphaToCoverageEnable(false);
            m_commandBuffers->setAlphaToOneEnableEXT(false);

            // ------------------------------------------------------------
            // IMPORTANT:
            // No depth test/write here.
            //
            // The outline is determined entirely from entityResolveResource.
            // Reverse-Z is therefore irrelevant to this fullscreen pass.
            // ------------------------------------------------------------
            m_commandBuffers->setDepthTestEnable(false);
            m_commandBuffers->setDepthWriteEnable(false);
            m_commandBuffers->setDepthBoundsTestEnable(false);
            m_commandBuffers->setStencilTestEnable(false);

            // Alpha blending for the orange outline.
            const NRI::ColorBlendEquation blendEquation
            {
                .srcColorBlendFactor = NRI::BlendFactor::SrcAlpha,
                .dstColorBlendFactor = NRI::BlendFactor::OneMinusSrcAlpha,
                .colorBlendOp = NRI::BlendOp::Add,

                .srcAlphaBlendFactor = NRI::BlendFactor::SrcAlpha,
                .dstAlphaBlendFactor = NRI::BlendFactor::OneMinusSrcAlpha,
                .alphaBlendOp = NRI::BlendOp::Add,
            };

            const uint32_t colorWriteMask =
                NRI::ColorComponent::R |
                NRI::ColorComponent::G |
                NRI::ColorComponent::B |
                NRI::ColorComponent::A;

            m_commandBuffers->setColorBlendEnable(0, true);
            m_commandBuffers->setColorBlendEquation(0, blendEquation);
            m_commandBuffers->setColorWriteMask(0, colorWriteMask);
            m_commandBuffers->setLogicOpEnable(false);

            m_commandBuffers->bindPipeline(
                NRI::PipelineBindPoint::Graphics,
                *m_outlinePipeline
            );

            shaderio::PushConstantOutline references{};
            references.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
            references.selectedEntityIDsReference = m_selectedEntityIDBuffers[frameIndex]->getDeviceAddress();
            references.selectedEntityCount = static_cast<uint32_t>(m_SelectedEntityIDs.size());

            m_commandBuffers->pushData(
                &references,
                sizeof(shaderio::PushConstantOutline)
            );

            m_commandBuffers->drawMeshTasks(1, 1, 1);

            m_commandBuffers->endRendering();
        }

        std::vector<NRI::RenderAttachDesc> imguiColorAttachments;
        imguiColorAttachments.push_back({
            .attachmentSwapchain = m_swapChain.get(),
            .resolveImageIndex = imageIndex,
            .loadOP = NRI::LoadOP::load,
            .storeOP = NRI::StoreOP::store,
        });

        NRI::RenderDesc imguiDesc =
        {
            .renderArea = {m_swapChainExtent.width, m_swapChainExtent.height},
            .colorAttachments = imguiColorAttachments,
        };

        NOX_PROFILE_GPU_BEGIN(*m_commandBuffers, "ImGui / Present Pass");
        m_commandBuffers->beginRendering(imguiDesc);
        if (m_isEditor)
            m_commandBuffers->renderImGui();
        else
        {
            // Viewport / scissor (counts and values are both dynamic).
            float w = static_cast<float>(m_swapChainExtent.width);
            float h = static_cast<float>(m_swapChainExtent.height);
            m_commandBuffers->setViewportWithCount({0.0f, 0.0f, w, h}, 0.0f, 1.0f);
            m_commandBuffers->setScissorWithCount(m_swapChainExtent);

            // Vertex input empty since we use vertex fetch BDA but still needs to be called empty
            m_commandBuffers->setVertexInput();

            // Input assembly.
            m_commandBuffers->setPrimitiveTopology(NRI::PrimitiveTopology::TriangleList);
            m_commandBuffers->setPrimitiveRestartEnable(false);

            // Rasterization (most of these come from VK_EXT_extended_dynamic_state_3).
            m_commandBuffers->setRasterizerDiscardEnable(false);
            m_commandBuffers->setPolygonMode(NRI::PolygonMode::Fill);
            m_commandBuffers->setCullMode(NRI::CullMode::None);
            m_commandBuffers->setFrontFace(NRI::FrontFace::CounterClockWise);
            m_commandBuffers->setDepthBiasEnable(false);
            m_commandBuffers->setDepthClampEnable(false); //LineWidth maybe ?

            // Multisampling.
            uint32_t sampleCount = 1;
            m_commandBuffers->setRasterizationSamples(sampleCount);
            const uint32_t sampleMask = 0xFFFFFFFF;
            m_commandBuffers->setSampleMask(sampleCount, sampleMask);
            m_commandBuffers->setAlphaToCoverageEnable(false);
            // alphaToOne is required by the spec when its device feature is enabled and a
            // shader object is bound, even if we don't actually use it.
            m_commandBuffers->setAlphaToOneEnableEXT(false);

            // Depth / stencil.
            m_commandBuffers->setDepthTestEnable(false);
            m_commandBuffers->setDepthWriteEnable(false);
            m_commandBuffers->setDepthCompareOp(NRI::CompareOp::Less);
            m_commandBuffers->setDepthBoundsTestEnable(false);
            m_commandBuffers->setStencilTestEnable(false);

            // Color blend (for one color attachment). Match the previous pipeline's
            // alpha-blend setup; nothing varies between draws so we set it once.
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
            m_commandBuffers->setColorBlendEnable(0, false);
            m_commandBuffers->setColorBlendEquation(0, blendEquation);
            m_commandBuffers->setColorWriteMask(0, colorWriteMask);
            m_commandBuffers->setLogicOpEnable(false);

            m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_presentPipeline);

            PushConstantBlock references{};
            // Pass pointer to the global matrix via a buffer device address
            references.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
            references.vertexReference = -1;
            references.instanceReference = -1;
            m_commandBuffers->pushData(&references, sizeof(PushConstantBlock));

            m_commandBuffers->draw(3, 1, 0, 0);
        }
        m_commandBuffers->endRendering();
        NOX_PROFILE_GPU_END(*m_commandBuffers);

        m_commandBuffers->transitionSwapchainLayout(*m_swapChain, imageIndex, NRI::TextureLayout::ColorAttachment, NRI::TextureLayout::Present);

        NOX_PROFILE_GPU_FRAME_END(*m_commandBuffers);

        if (m_pickRequest.active && m_pickRequest.x >= 0 && m_pickRequest.y >= 0)
        {
            uint32_t width = m_isEditor ? m_viewportSize.width : m_swapChainExtent.width;
            uint32_t height = m_isEditor ? m_viewportSize.height : m_swapChainExtent.height;

            // Apply Vulkan negative-height viewport inversion (Top-Left -> Bottom-Left)
            uint32_t sampleX = static_cast<uint32_t>(m_pickRequest.x);
            uint32_t sampleY = static_cast<uint32_t>(m_pickRequest.y);

            if (sampleX < width && sampleY < height)
            {
                uint32_t copyWidth = std::min(m_pickRequest.width, width - sampleX);
                uint32_t copyHeight = std::min(m_pickRequest.height, height - sampleY);

                m_entityResourceHi->copyImageToBuffer(*m_commandBuffers, *m_pickerStagingBuffers[frameIndex], sampleX, sampleY, copyWidth, copyHeight);
                m_pickerReadbackRequests[frameIndex] = m_pickRequest;
                m_pickerReadbackRequests[frameIndex].width = copyWidth;
                m_pickerReadbackRequests[frameIndex].height = copyHeight;

                m_pickRequest.active = false;
            }

            m_commandBuffers->end(frameIndex);
        }
        else
        {
            m_commandBuffers->end(frameIndex);
        }
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

        uniformData.entityTextureIndex = m_entityResourceHi->GetDescriptorIndexSlot();
        uniformData.entityGBufferTextureIndex = m_entityResource->GetDescriptorIndexSlot();

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

    void Renderer::updateInstanceAndIndirectBuffer(uint32_t currentImage)
    {
        //fix maybe dont recreate all buffers only the next frame ?
        if (m_instanceBufferObjects.empty() || m_drawMeshTasksIndirectCommands.empty())
        {
            return;
        }

        uint64_t requiredInstanceSize = sizeof(shaderio::InstanceData) * m_instanceBufferObjects.size();
        if (requiredInstanceSize > m_InstanceBufferCapacity)
        {
            m_InstanceBufferCapacity = requiredInstanceSize * 2;

            for (auto& oldBuffer : m_instanceBuffers)
            {
                if (oldBuffer)
                {
                    m_deferredBufferDeletions.push_back({
                        std::move(oldBuffer),
                        frameIndex + MAX_FRAMES_IN_FLIGHT
                    });
                }
            }

            createInstanceBuffer(m_InstanceBufferCapacity);
        }
        memcpy(m_instanceBuffersMapped[currentImage], m_instanceBufferObjects.data(), requiredInstanceSize);

        uint64_t requiredIndirectSize = sizeof(DrawMeshTasksIndirectCommand) * m_drawMeshTasksIndirectCommands.size();
        if (requiredIndirectSize > m_IndirectBufferCapacity)
        {
            m_IndirectBufferCapacity = requiredIndirectSize * 2;

            for (auto& oldBuffer : m_indirectBuffers)
            {
                if (oldBuffer)
                {
                    m_deferredBufferDeletions.push_back({
                        std::move(oldBuffer),
                        frameIndex + MAX_FRAMES_IN_FLIGHT
                    });
                }
            }

            createIndirectBuffer(m_IndirectBufferCapacity);
        }
        memcpy(m_indirectBuffersMapped[currentImage], m_drawMeshTasksIndirectCommands.data(), requiredIndirectSize);
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
                    m_deferredBufferDeletions.push_back({
                        std::move(oldBuffer),
                        frameIndex + MAX_FRAMES_IN_FLIGHT
                    });
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

    void Renderer::processDeferredDeletions()
    {
        // Dropping the last Ref here runs the asset's destructor (texture slot release, mesh
        // geometry free) only after every frame that could still reference it has finished.
        std::erase_if(m_deferredAssetReleases, [](DeferredAssetRelease& deferred)
        {
            if (deferred.framesRemaining == 0)
                return true;

            deferred.framesRemaining--;
            return false;
        });

        std::erase_if(m_deferredBufferDeletions, [](DeferredBuffer& deferred)
        {
            if (deferred.framesRemaining == 0)
                return true; // Deletes unique_ptr

            deferred.framesRemaining--;
            return false;
        });
    }

    void Renderer::processDeferredMeshFrees()
    {
        std::erase_if(m_deferredMeshFrees, [this](DeferredMeshFree& deferred)
        {
            if (deferred.framesRemaining == 0)
            {
                const auto& h = deferred.handle;
                std::unique_ptr<NRI::Buffer> emptyBuffer;

                // Free slots. If page becomes 100% empty, emptyBuffer is populated for deletion!
                if (m_vertexPages.Free(h.vertices.pageIndex, h.vertices.offset, h.vertices.count, emptyBuffer))
                {
                    m_deferredBufferDeletions.push_back({std::move(emptyBuffer), MAX_FRAMES_IN_FLIGHT});
                }

                if (m_meshletDrawPages.Free(h.meshletDraws.pageIndex, h.meshletDraws.offset, h.meshletDraws.count, emptyBuffer))
                {
                    m_deferredBufferDeletions.push_back({std::move(emptyBuffer), MAX_FRAMES_IN_FLIGHT});
                }

                // Bounds are allocated 1:1 with draws (same count, same allocation order), so the
                // draw allocation's page/offset is also the bounds allocation.
                if (m_meshletBoundsPages.Free(h.meshletDraws.pageIndex, h.meshletDraws.offset, h.meshletDraws.count, emptyBuffer))
                {
                    m_deferredBufferDeletions.push_back({std::move(emptyBuffer), MAX_FRAMES_IN_FLIGHT});
                }

                if (h.blasId != UINT32_MAX && h.blasId < m_meshBLASes.size() && m_meshBLASes[h.blasId].as)
                {
                    m_meshBLASes[h.blasId] = MeshBLAS{};
                    m_freeBLASIds.push_back(h.blasId);
                    m_tlasNeedFullBuild = true;
                }

                if (m_meshletVertPages.Free(h.meshletVertices.pageIndex, h.meshletVertices.offset, h.meshletVertices.count, emptyBuffer))
                {
                    m_deferredBufferDeletions.push_back({std::move(emptyBuffer), MAX_FRAMES_IN_FLIGHT});
                }

                if (m_meshletTriPages.Free(h.meshletTriangles.pageIndex, h.meshletTriangles.offset, h.meshletTriangles.count, emptyBuffer))
                {
                    m_deferredBufferDeletions.push_back({std::move(emptyBuffer), MAX_FRAMES_IN_FLIGHT});
                }

                return true; // Done freeing mesh
            }

            deferred.framesRemaining--;
            return false;
        });
    }

    // with compute there might be a snyc issue idk
    // according to gpt no async since compute and graphics same commandbuffer and executed in order
    void Renderer::sampleMemoryStats()
    {
        // Memory changes slowly compared to frame time; a few samples per second are enough for the overlay/plots.
        constexpr auto SampleInterval = std::chrono::milliseconds(250);
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastMemoryStatsSample < SampleInterval)
            return;
        m_lastMemoryStatsSample = now;

        m_device->getMemoryStats(m_memoryHeapStats);
        Profiler::Get().SubmitMemoryStats(m_memoryHeapStats, m_device->isMemoryBudgetSupported(), Platform::QueryProcessMemory());
    }

    void Renderer::drawFrame()
    {
        NOX_PROFILE_SCOPE("Renderer::drawFrame");

        // Before any allocation of this frame: refreshes the VMA memory budget (VMA "Staying within budget").
        m_device->beginFrame(static_cast<uint32_t>(m_sceneFrameCounter));
#if NOX_PROFILING_ENABLED
        sampleMemoryStats();
#endif

        {
            NOX_PROFILE_SCOPE("Deferred Deletions");
            processDeferredDeletions();
            processDeferredMeshFrees();
        }

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
            NOX_PROFILE_SCOPE("Page Tables");
            updatePageTables(frameIndex);
        }

        {
            NOX_PROFILE_SCOPE("Entity IDs + Lights");
            updateEntityIDBuffer(frameIndex);
            updateLightBuffer(frameIndex);
        }

        {
            NOX_PROFILE_SCOPE("BuildBuffers");
            BuildBuffers();
        }
        NOX_PROFILE_COUNTER("Instances", m_instanceBufferObjects.size());
        NOX_PROFILE_COUNTER("Indirect Draws", m_drawMeshTasksIndirectCommands.size());
        NOX_PROFILE_COUNTER("Transparent Draws", m_transparentCount + m_transparentDoubleSidedCount + m_transparentUnlitCount + m_transparentUnlitDoubleSidedCount);
        NOX_PROFILE_COUNTER("Lights", m_lightBufferObjects.size());

        {
            NOX_PROFILE_SCOPE("Instance + Indirect Upload");
            updateInstanceAndIndirectBuffer(frameIndex);
        }

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

        {
            NOX_PROFILE_SCOPE("Record Commands");
            recordCommandBuffer(imageIndex);
        }

        {
            NOX_PROFILE_SCOPE("Submit");
            m_device->submitCommandBuffer(*m_commandBuffers, *m_swapChain, frameIndex, imageIndex);
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

        m_instanceBufferObjects.clear();
        m_drawMeshTasksIndirectCommands.clear();
        m_boneMatrices.clear();
        m_lightBufferObjects.clear();

        m_opaqueQueue.clear();
        m_opaqueDoubleSidedQueue.clear();
        m_maskQueue.clear();
        m_maskDoubleSidedQueue.clear();
        m_unlitQueue.clear();
        m_unlitDoubleSidedQueue.clear();
        m_transparentQueue.clear();
        m_transparentDoubleSidedQueue.clear();
        m_transparentUnlitQueue.clear();
        m_transparentUnlitDoubleSidedQueue.clear();
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
            m_hasTLASBuild = false;
            m_tlasNeedFullBuild = false;
            m_tlasNeedUpdate = false;
            m_tlasInstanceSignatureValid = false;
            m_tlasStructureSignatureValid = false;
            uniformData.tlasDeviceAddress = 0;
            uniformData.instanceLUTReference = 0;
            uniformData.enableRTShadows = 0;
            uniformData.enableRTReflections = 0;
            return;
        }

        // Collect all active render packets that have valid BLASes
        std::vector<NRI::AccelerationStructureInstance> rtInstances;
        std::vector<shaderio::InstanceLUT> rtInstanceLUTs;

        auto collectFromQueue = [&](const std::vector<RenderPacket>& queue)
        {
            for (const auto& packet : queue)
            {
                if (packet.blasId >= m_meshBLASes.size() || !m_meshBLASes[packet.blasId].as)
                    continue;

                const glm::mat4& model = packet.instance.modelMatrix;
                NRI::AccelerationStructureInstance inst{};

                // Convert column-major glm::mat4 to row-major 3x4 transform matrix
                for (int r = 0; r < 3; ++r)
                {
                    for (int c = 0; c < 4; ++c)
                    {
                        inst.transform.matrix[r][c] = model[c][r];
                    }
                }

                inst.instanceCustomIndex = static_cast<uint32_t>(rtInstances.size());
                inst.mask = 0x01;
                inst.instanceShaderBindingTableRecordOffset = 0;
                inst.flags = 0;

                // Dynamic runtime AlphaMode handling:
                // 0x04 = VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR
                // 0x08 = VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR
                // 0x01 = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR
                if (packet.instance.alphaMode == 0)
                {
                    inst.flags |= 0x00000004; // Force Opaque in hardware traversal
                }
                else if (packet.instance.alphaMode == 1)
                {
                    inst.flags |= 0x00000008; // Force Non-Opaque for any-hit alpha testing
                }

                if (packet.instance.doubleSided != 0)
                {
                    inst.flags |= 0x00000001; // Disable face culling
                }

                inst.accelerationStructureReference = m_meshBLASes[packet.blasId].as->getDeviceAddress();

                rtInstances.push_back(inst);

                // Tutorial TASK09: Store the instance look-up table entry
                shaderio::InstanceLUT lut{};
                lut.vertexBufferAddress = m_meshBLASes[packet.blasId].vertexBufferAddress;
                lut.indexBufferAddress = m_meshBLASes[packet.blasId].indexBuffer ? m_meshBLASes[packet.blasId].indexBuffer->getDeviceAddress() : 0;
                lut.normalMatrix = packet.instance.normalMatrix;
                lut.baseColorFactor = packet.instance.baseColorFactor;
                lut.emissiveFactor = glm::vec4(packet.instance.emissiveFactor, packet.instance.emissiveStrength);
                lut.baseColorTextureIndex = packet.instance.baseColorTextureIndex;
                lut.alphaCutoff = packet.instance.alphaMaskCutoff;
                lut.alphaMode = packet.instance.alphaMode;
                lut.doubleSided = packet.instance.doubleSided;
                lut.metallicFactor = packet.instance.metallicFactor;
                lut.roughnessFactor = packet.instance.roughnessFactor;
                lut.metallicRoughnessTextureIndex = packet.instance.metallicRoughnessTextureIndex;
                lut.normalTextureIndex = packet.instance.normalTextureIndex;
                lut.transmissionFactor = packet.instance.transmissionFactor;
                lut.transmissionTextureIndex = packet.instance.transmissionTextureIndex;
                lut.workflow = packet.instance.workflow;
                rtInstanceLUTs.push_back(lut);
            }
        };

        collectFromQueue(m_opaqueQueue);
        collectFromQueue(m_opaqueDoubleSidedQueue);
        collectFromQueue(m_maskQueue);
        collectFromQueue(m_maskDoubleSidedQueue);

        if (rtInstances.empty())
        {
            m_hasTLASBuild = false;
            m_tlasNeedFullBuild = false;
            m_tlasNeedUpdate = false;
            m_tlasInstanceSignatureValid = false;
            m_tlasStructureSignatureValid = false;
            uniformData.enableRTShadows = 0;
            uniformData.enableRTReflections = 0;
            return;
        }

        if (m_rtInstanceBuffers.size() < MAX_FRAMES_IN_FLIGHT)
            m_rtInstanceBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        if (m_instanceLUTBuffers.size() < MAX_FRAMES_IN_FLIGHT)
            m_instanceLUTBuffers.resize(MAX_FRAMES_IN_FLIGHT);

        uint32_t instanceCount = static_cast<uint32_t>(rtInstances.size());
        uint64_t instanceBufferSize = sizeof(NRI::AccelerationStructureInstance) * instanceCount;
        uint64_t lutBufferSize = sizeof(shaderio::InstanceLUT) * rtInstanceLUTs.size();

        // The ECS queues are the authoritative instance list. Hash the complete TLAS instance
        // records so additions/removals, transform changes, BLAS replacement, and instance flags
        // all invalidate the cached build without requiring ECS-specific callbacks here.
        const uint8_t* instanceBytes = reinterpret_cast<const uint8_t*>(rtInstances.data());
        uint64_t instanceSignature = Hash::compute(instanceBytes, instanceBufferSize);
        uint64_t structureSignature = 0;
        for (size_t i = 0; i < instanceCount; ++i)
        {
            const uint8_t* record = instanceBytes + i * sizeof(NRI::AccelerationStructureInstance);

            // The first 48 bytes are the 3x4 transform. The remaining fields describe the
            // TLAS topology/flags and cannot be changed through a Vulkan AS update.
            structureSignature = Hash::compute(
                record + 48,
                sizeof(NRI::AccelerationStructureInstance) - 48,
                structureSignature);
        }
        structureSignature = Hash::compute(&instanceCount, sizeof(instanceCount), structureSignature);
        bool instanceListChanged = !m_tlasInstanceSignatureValid ||
                                   instanceSignature != m_tlasInstanceSignature;
        bool structureChanged = !m_tlasStructureSignatureValid ||
                                structureSignature != m_tlasStructureSignature;
        m_tlasInstanceSignature = instanceSignature;
        m_tlasInstanceSignatureValid = true;
        m_tlasStructureSignature = structureSignature;
        m_tlasStructureSignatureValid = true;
        m_tlasNeedFullBuild = structureChanged || !m_sceneTLAS;
        m_tlasNeedUpdate = instanceListChanged && !m_tlasNeedFullBuild;

        // 1. Ensure TLAS Instance Buffer is allocated and populated
        if (!m_rtInstanceBuffers[currentFrameIndex] || m_rtInstanceBuffers[currentFrameIndex]->getSize() < instanceBufferSize)
        {
            m_rtInstanceBuffers[currentFrameIndex] = m_device->createBuffer(NRI::BufferDesc{
                .size = std::max(instanceBufferSize, static_cast<uint64_t>(64 * 1024)),
                .usage = NRI::BufferUsage::AccelerationStructureInstance
            });
        }
        void* mapped = m_rtInstanceBuffers[currentFrameIndex]->map(0, instanceBufferSize);
        memcpy(mapped, rtInstances.data(), instanceBufferSize);
        m_rtInstanceBuffers[currentFrameIndex]->unmap();

        // 2. Ensure Instance LUT Buffer is allocated and populated
        if (!m_instanceLUTBuffers[currentFrameIndex] || m_instanceLUTBuffers[currentFrameIndex]->getSize() < lutBufferSize)
        {
            m_instanceLUTBuffers[currentFrameIndex] = m_device->createBuffer(NRI::BufferDesc{
                .size = std::max(lutBufferSize, static_cast<uint64_t>(64 * 1024)),
                .usage = NRI::BufferUsage::Storage
            });
        }
        void* lutMapped = m_instanceLUTBuffers[currentFrameIndex]->map(0, lutBufferSize);
        memcpy(lutMapped, rtInstanceLUTs.data(), lutBufferSize);
        m_instanceLUTBuffers[currentFrameIndex]->unmap();

        uniformData.instanceLUTReference = m_instanceLUTBuffers[currentFrameIndex]->getDeviceAddress();

        // 2. Query TLAS build sizes
        m_tlasBuildDesc = NRI::AccelerationStructureBuildDesc{
            .type = NRI::AccelerationStructureType::TopLevel,
            .flags = NRI::AccelerationStructureBuildFlags::PreferFastTrace | NRI::AccelerationStructureBuildFlags::AllowUpdate,
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
                .size = std::max(tlasSizes.buildScratchSize, tlasSizes.updateScratchSize),
                .usage = NRI::BufferUsage::AccelerationStructureScratch
            });

            // Register TLAS into Descriptor Heap ONLY when newly created or resized
            m_tlasHeapSlot = m_resourceHeap->registerAccelerationStructure(*m_sceneTLAS, m_tlasHeapSlot);
            m_tlasHeapIndex = m_tlasHeapSlot;
            m_tlasNeedFullBuild = true;
            m_tlasNeedUpdate = false;
        }

        // 4. Update UBO flags (ready for updateUniformBuffer!)
        uniformData.tlasDeviceAddress = m_sceneTLAS ? m_sceneTLAS->getDeviceAddress() : 0;
        uniformData.tlasHeapIndex = m_tlasHeapIndex;
        uniformData.enableRTShadows = (m_rayTracingEnabled && m_rayTracingShadows) ? 1 : 0;
        uniformData.enableRTReflections = (m_rayTracingEnabled && m_rayTracingReflections) ? 1 : 0;

        m_hasTLASBuild = true;
    }

    void Renderer::BuildSceneAccelerationStructure(uint32_t currentFrameIndex)
    {
        if (!m_hasTLASBuild || !m_sceneTLAS || !m_tlasScratchBuffer ||
            (!m_tlasNeedFullBuild && !m_tlasNeedUpdate))
            return;

        // 1. Pre-build barrier: Host/Transfer instance writes -> AS Build Read
        m_commandBuffers->accelerationStructureBarrier(NRI::AccelerationStructureBarrierType::TransferToBuild);

        if (m_tlasNeedFullBuild)
        {
            m_commandBuffers->buildAccelerationStructure(m_tlasBuildDesc, m_tlasScratchBuffer->getDeviceAddress(), *m_sceneTLAS);
        }
        else
        {
            // Transform-only ECS changes preserve instance count, BLAS addresses, and flags, so
            // Vulkan's fast AS update path is valid and avoids rebuilding the entire TLAS.
            m_commandBuffers->updateAccelerationStructure(
                m_tlasBuildDesc,
                m_tlasScratchBuffer->getDeviceAddress(),
                *m_sceneTLAS,
                *m_sceneTLAS);
        }

        // 3. Post-build barrier: AS Build Write -> Fragment/Compute Shader Read
        m_commandBuffers->accelerationStructureBarrier(NRI::AccelerationStructureBarrierType::BuildToShaderRead);
        m_tlasNeedFullBuild = false;
        m_tlasNeedUpdate = false;
    }

    int32_t Renderer::getPickedEntityID()
    {
        int32_t clickedEntityID = -1;

        // frameIndex already points at the next frame to record. The newest completed pick copy
        // belongs to the frame submitted immediately before it.
        const uint32_t readbackFrame = (frameIndex + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
        void* mappedMemory = m_pickerStagingBuffers[readbackFrame]->map(0, sizeof(int32_t));

        if (mappedMemory)
        {
            memcpy(&clickedEntityID, mappedMemory, sizeof(int32_t));
            m_pickerStagingBuffers[readbackFrame]->unmap();
        }

        return clickedEntityID;
    }

    std::vector<int32_t> Renderer::getPickedEntityIDs()
    {
        std::vector<int32_t> uniqueIDs;
        const uint32_t readbackFrame = (frameIndex + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
        const PickRequest& readbackRequest = m_pickerReadbackRequests[readbackFrame];
        size_t pixelCount = static_cast<size_t>(readbackRequest.width) * readbackRequest.height;
        if (pixelCount == 0)
            return uniqueIDs;

        void* mappedMemory = m_pickerStagingBuffers[readbackFrame]->map(0, pixelCount * sizeof(int32_t));
        if (mappedMemory)
        {
            const int32_t* pixels = static_cast<const int32_t*>(mappedMemory);
            std::unordered_set<int32_t> seen;

            for (size_t i = 0; i < pixelCount; ++i)
            {
                int32_t id = pixels[i];
                if (id >= 0 && seen.insert(id).second)
                {
                    uniqueIDs.push_back(id);
                }
            }

            m_pickerStagingBuffers[readbackFrame]->unmap();
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
        uint32_t ddgiProbesPerRow = 64;
        uint32_t ddgiTotalProbes = m_ddgiProbeCountX * m_ddgiProbeCountY * m_ddgiProbeCountZ;
        uint32_t ddgiProbeRows = (ddgiTotalProbes + ddgiProbesPerRow - 1) / ddgiProbesPerRow;
        uniformData.ddgiAtlasParams = glm::vec4(
            static_cast<float>(ddgiProbesPerRow * 10),
            static_cast<float>(ddgiProbeRows * 10),
            static_cast<float>(ddgiProbesPerRow * 18),
            static_cast<float>(ddgiProbeRows * 18)
        );
        uniformData.ddgiIrradianceTextureIndex = m_ddgiIrradiance[m_ddgiHistoryIndex] ? m_ddgiIrradiance[m_ddgiHistoryIndex]->GetDescriptorIndexSlot() : 0xFFFFFFFF;
        uniformData.ddgiDistanceTextureIndex = m_ddgiDistance[m_ddgiHistoryIndex] ? m_ddgiDistance[m_ddgiHistoryIndex]->GetDescriptorIndexSlot() : 0xFFFFFFFF;

        // ReSTIR GI
        uniformData.diffuseGIMode = m_diffuseGIMode;
        uniformData.restirGIDiffuseTextureIndex = m_restirGIRawDiffuse ? m_restirGIRawDiffuse->GetDescriptorIndexSlot() : 0xFFFFFFFF;
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
        uint32_t ddgiProbesPerRow = 64;
        uint32_t ddgiTotalProbes = m_ddgiProbeCountX * m_ddgiProbeCountY * m_ddgiProbeCountZ;
        uint32_t ddgiProbeRows = (ddgiTotalProbes + ddgiProbesPerRow - 1) / ddgiProbesPerRow;
        uniformData.ddgiAtlasParams = glm::vec4(
            static_cast<float>(ddgiProbesPerRow * 10),
            static_cast<float>(ddgiProbeRows * 10),
            static_cast<float>(ddgiProbesPerRow * 18),
            static_cast<float>(ddgiProbeRows * 18)
        );
        uniformData.ddgiIrradianceTextureIndex = m_ddgiIrradiance[m_ddgiHistoryIndex] ? m_ddgiIrradiance[m_ddgiHistoryIndex]->GetDescriptorIndexSlot() : 0xFFFFFFFF;
        uniformData.ddgiDistanceTextureIndex = m_ddgiDistance[m_ddgiHistoryIndex] ? m_ddgiDistance[m_ddgiHistoryIndex]->GetDescriptorIndexSlot() : 0xFFFFFFFF;

        // ReSTIR GI
        uniformData.diffuseGIMode = m_diffuseGIMode;
        uniformData.restirGIDiffuseTextureIndex = m_restirGIRawDiffuse ? m_restirGIRawDiffuse->GetDescriptorIndexSlot() : 0xFFFFFFFF;
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

    void Renderer::BuildBuffers()
    {
        m_instanceBufferObjects.clear();
        m_drawMeshTasksIndirectCommands.clear();

        auto packQueue = [this](const std::vector<RenderPacket>& queue, uint32_t& outCount)
        {
            outCount = static_cast<uint32_t>(queue.size());
            for (const auto& packet : queue)
            {
                m_instanceBufferObjects.push_back(packet.instance);
                m_drawMeshTasksIndirectCommands.push_back(packet.command);
            }
        };

        // 1. Opaque PBR (Single-Sided & Double-Sided)
        packQueue(m_opaqueQueue, m_opaqueCount);
        packQueue(m_opaqueDoubleSidedQueue, m_opaqueDoubleSidedCount);

        // 2. Alpha Mask PBR (Single-Sided & Double-Sided)
        packQueue(m_maskQueue, m_maskCount);
        packQueue(m_maskDoubleSidedQueue, m_maskDoubleSidedCount);

        // 3. Unlit (Single-Sided & Double-Sided)
        packQueue(m_unlitQueue, m_unlitCount);
        packQueue(m_unlitDoubleSidedQueue, m_unlitDoubleSidedCount);

        // 4. Transparent (Sorted back-to-front)
        auto sortByDistance = [](std::vector<RenderPacket>& queue)
        {
            std::sort(queue.begin(), queue.end(), [](const RenderPacket& a, const RenderPacket& b)
            {
                return a.distanceToCamera > b.distanceToCamera;
            });
        };

        sortByDistance(m_transparentQueue);
        packQueue(m_transparentQueue, m_transparentCount);

        sortByDistance(m_transparentDoubleSidedQueue);
        packQueue(m_transparentDoubleSidedQueue, m_transparentDoubleSidedCount);

        sortByDistance(m_transparentUnlitQueue);
        packQueue(m_transparentUnlitQueue, m_transparentUnlitCount);

        sortByDistance(m_transparentUnlitDoubleSidedQueue);
        packQueue(m_transparentUnlitDoubleSidedQueue, m_transparentUnlitDoubleSidedCount);
    }

    void Renderer::DrawMesh(const glm::mat4& transform, Ref<Mesh> mesh, uint32_t submeshIndex, const MaterialComponent& materialOverrides, int entityID, const std::vector<glm::mat4>* boneTransforms)
    {
        const auto& submeshes = mesh->GetSubMeshes();
        if (submeshIndex >= submeshes.size())
            return;

        const MeshHandle& handle = submeshes[submeshIndex];

        MaterialData material = mesh->GetMaterial(submeshIndex);
        const auto& materialAssets = materialOverrides.MaterialAssets;
        if (submeshIndex < materialAssets.size() && materialAssets[submeshIndex] != 0)
        {
            Ref<Material> materialAsset = AssetManager::GetAsset<Material>(materialAssets[submeshIndex]);
            if (materialAsset)
                material = materialAsset->GetData();
        }

        shaderio::InstanceData instance{};
        instance.modelMatrix = transform;
        instance.normalMatrix = glm::transpose(glm::inverse(transform));

        // 1. Where do the meshlets live, and how many are there?
        instance.drawsPageIndex = handle.meshletDraws.pageIndex;
        instance.drawsOffset = handle.meshletDraws.offset; // (Replaces the old meshletOffset)
        instance.meshletCount = handle.meshletDraws.count; // (Replaces the old GetMeshletCount)

        // 2. Where do the vertices and triangles live?
        instance.verticesPageIndex = handle.vertices.pageIndex;
        instance.meshletVerticesPageIndex = handle.meshletVertices.pageIndex;
        instance.meshletTrianglesPageIndex = handle.meshletTriangles.pageIndex;

        PackMaterial(instance, material);
        AlphaMode mode = material.Mode;

        instance.entityID = entityID;

        // --- BONE MATRIX PACKING ---
        if (boneTransforms && !boneTransforms->empty())
        {
            // Store starting index in m_boneMatrices buffer for this instance
            instance.boneMatrixOffset = static_cast<uint32_t>(m_boneMatrices.size());
            m_boneMatrices.insert(m_boneMatrices.end(), boneTransforms->begin(), boneTransforms->end());
        }
        else
        {
            // Sentinel value for static/non-skinned mesh
            instance.boneMatrixOffset = 0xFFFFFFFF;
        }

        DrawMeshTasksIndirectCommand command{};
        command.groupCountX = (handle.GetMeshletCount() + shaderio::TASK_SHADER_DISPATCH_X - 1) / shaderio::TASK_SHADER_DISPATCH_X;
        command.groupCountY = 1;
        command.groupCountZ = 1;

        RenderPacket packet{};
        packet.instance = instance;
        packet.command = command;
        packet.blasId = handle.blasId;

        // Route to Render Queues (DO NOT push directly to buffers)
        bool isDoubleSided = (instance.doubleSided != 0);
        bool isUnlit = (instance.unlit != 0);

        if (mode == AlphaMode::Blend)
        {
            glm::vec3 camPos = glm::vec3(uniformData.cameraWorldPos);
            glm::vec3 objPos = glm::vec3(transform[3]);
            packet.distanceToCamera = glm::length(objPos - camPos);

            // A closed, single-sided (doubleSided=false) translucent shape (e.g. the alpha sphere
            // shell in CompareTransmission) needs its own backface actually culled here, exactly
            // like the opaque/mask queues already do - with CullMode::None, both the near and far
            // hemisphere triangles rasterize and alpha-blend on top of each other in whatever order
            // the meshlets happen to be processed (not depth-sorted per-triangle), which compounds
            // into a washed-out/flatter look instead of a clean single translucent surface.
            if (isUnlit)
            {
                if (isDoubleSided)
                    m_transparentUnlitDoubleSidedQueue.push_back(packet);
                else
                    m_transparentUnlitQueue.push_back(packet);
            }
            else
            {
                if (isDoubleSided)
                    m_transparentDoubleSidedQueue.push_back(packet);
                else
                    m_transparentQueue.push_back(packet);
            }
        }
        else if (mode == AlphaMode::Mask)
        {
            if (isUnlit)
            {
                if (isDoubleSided)
                    m_unlitDoubleSidedQueue.push_back(packet);
                else
                    m_unlitQueue.push_back(packet);
            }
            else
            {
                if (isDoubleSided)
                    m_maskDoubleSidedQueue.push_back(packet);
                else
                    m_maskQueue.push_back(packet);
            }
        }
        else // AlphaMode::Opaque
        {
            if (isUnlit)
            {
                if (isDoubleSided)
                    m_unlitDoubleSidedQueue.push_back(packet);
                else
                    m_unlitQueue.push_back(packet);
            }
            else
            {
                if (isDoubleSided)
                    m_opaqueDoubleSidedQueue.push_back(packet);
                else
                    m_opaqueQueue.push_back(packet);
            }
        }
    }

    void Renderer::DrawStaticMesh(const glm::mat4& transform, Ref<StaticMesh> staticMesh, const MaterialComponent& materialOverrides, int entityID, uint32_t firstSubmesh, uint32_t submeshCount)
    {
        uint32_t first = std::min(firstSubmesh, static_cast<uint32_t>(staticMesh->GetSubMeshes().size()));
        uint32_t count = submeshCount == UINT32_MAX ? static_cast<uint32_t>(staticMesh->GetSubMeshes().size()) : std::max(submeshCount, 1u);
        uint32_t last = std::min(first + count, static_cast<uint32_t>(staticMesh->GetSubMeshes().size()));

        for (size_t i = first; i < last; ++i)
        {
            MeshHandle handle = staticMesh->GetSubMeshes()[i];

            MaterialData material = staticMesh->GetMaterial(i);
            const auto& materialAssets = materialOverrides.MaterialAssets;
            if (i < materialAssets.size() && materialAssets[i] != 0)
            {
                Ref<Material> materialAsset = AssetManager::GetAsset<Material>(materialAssets[i]);
                if (materialAsset)
                    material = materialAsset->GetData();
            }

            shaderio::InstanceData instance{};
            instance.modelMatrix = transform;
            instance.normalMatrix = glm::transpose(glm::inverse(transform));

            // Page Table Info
            instance.drawsPageIndex = handle.meshletDraws.pageIndex;
            instance.drawsOffset = handle.meshletDraws.offset;
            instance.meshletCount = handle.meshletDraws.count;

            instance.verticesPageIndex = handle.vertices.pageIndex;
            instance.meshletVerticesPageIndex = handle.meshletVertices.pageIndex;
            instance.meshletTrianglesPageIndex = handle.meshletTriangles.pageIndex;

            PackMaterial(instance, material);
            AlphaMode mode = material.Mode;

            instance.entityID = entityID;

            DrawMeshTasksIndirectCommand command{};
            command.groupCountX = (handle.GetMeshletCount() + shaderio::TASK_SHADER_DISPATCH_X - 1) / shaderio::TASK_SHADER_DISPATCH_X;
            command.groupCountY = 1;
            command.groupCountZ = 1;

            RenderPacket packet{};
            packet.instance = instance;
            packet.command = command;
            packet.blasId = handle.blasId;

            // 5. Route to Render Queues
            bool isDoubleSided = (instance.doubleSided != 0);
            bool isUnlit = (instance.unlit != 0);

            if (mode == AlphaMode::Blend)
            {
                glm::vec3 camPos = glm::vec3(uniformData.cameraWorldPos);
                glm::vec3 objPos = glm::vec3(transform[3]);
                packet.distanceToCamera = glm::length(objPos - camPos);

                // See DrawMesh's identical comment: respect doubleSided so a closed, single-sided
                // translucent shape doesn't double-blend its near and far hemispheres together.
                if (isUnlit)
                {
                    if (isDoubleSided)
                        m_transparentUnlitDoubleSidedQueue.push_back(packet);
                    else
                        m_transparentUnlitQueue.push_back(packet);
                }
                else
                {
                    if (isDoubleSided)
                        m_transparentDoubleSidedQueue.push_back(packet);
                    else
                        m_transparentQueue.push_back(packet);
                }
            }
            else if (mode == AlphaMode::Mask)
            {
                if (isUnlit)
                {
                    if (isDoubleSided)
                        m_unlitDoubleSidedQueue.push_back(packet);
                    else
                        m_unlitQueue.push_back(packet);
                }
                else
                {
                    if (isDoubleSided)
                        m_maskDoubleSidedQueue.push_back(packet);
                    else
                        m_maskQueue.push_back(packet);
                }
            }
            else // AlphaMode::Opaque
            {
                if (isUnlit)
                {
                    if (isDoubleSided)
                        m_unlitDoubleSidedQueue.push_back(packet);
                    else
                        m_unlitQueue.push_back(packet);
                }
                else
                {
                    if (isDoubleSided)
                        m_opaqueDoubleSidedQueue.push_back(packet);
                    else
                        m_opaqueQueue.push_back(packet);
                }
            }
        }
    }

    void Renderer::SubmitMesh(const glm::mat4& transform, MeshComponent& src, MaterialComponent& srcMat, int entityID, const std::vector<glm::mat4>* boneTransforms)
    {
        if (src.Mesh == 0)
            return;

        AssetType type = AssetManager::GetAssetType(src.Mesh);

        if (type == AssetType::Mesh)
        {
            Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(src.Mesh);
            if (mesh)
            {
                MaterialComponent meshMaterial;
                const MaterialComponent* material = &srcMat;
                if (srcMat.MaterialAssets.empty() && !mesh->GetMaterialAssets().empty())
                {
                    meshMaterial.MaterialAssets = mesh->GetMaterialAssets();
                    material = &meshMaterial;
                }

                uint32_t firstSubmesh = std::min(src.SubmeshIndex, static_cast<uint32_t>(mesh->GetSubMeshCount()));
                uint32_t submeshCount = std::max(src.SubmeshCount, 1u);
                uint32_t lastSubmesh = std::min(firstSubmesh + submeshCount, static_cast<uint32_t>(mesh->GetSubMeshCount()));

                for (uint32_t i = firstSubmesh; i < lastSubmesh; i++)
                    DrawMesh(transform, mesh, i, *material, entityID, boneTransforms);
            }
        }
        else if (type == AssetType::StaticMesh)
        {
            Ref<StaticMesh> staticMesh = AssetManager::GetAsset<StaticMesh>(src.Mesh);
            if (staticMesh)
            {
                MaterialComponent meshMaterial;
                const MaterialComponent* material = &srcMat;
                if (srcMat.MaterialAssets.empty() && !staticMesh->GetMaterialAssets().empty())
                {
                    meshMaterial.MaterialAssets = staticMesh->GetMaterialAssets();
                    material = &meshMaterial;
                }
                DrawStaticMesh(transform, staticMesh, *material, entityID, src.SubmeshIndex, src.SubmeshCount);
            }
        }
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
