#include "Renderer.h"

#include <iostream>
#include <algorithm> // Necessary for std::clamp
#include <chrono>
#include <fstream>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/MeshImporter.h"
#include "NoxCore/Core/Log.h"

namespace Nox
{
    Renderer::Renderer(std::shared_ptr<Window> window, bool isEditor) : m_window(std::move(window)), m_isEditor(isEditor)
    {
        NOX_CORE_INFO("Renderer Start");

        s_Instance = this;

        m_device = NRI::Device::create(NRI::GraphicsAPI::Vulkan, *m_window);
        if (!m_device) NOX_CORE_ASSERT("Failed to create NRI device");

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
        // Visibility Buffer
        watchShader("assets/shaders/VisibilityBuffer.slang", "VisBuffer", [this]() { createVisibilityPipeline(true); });
        // G-Buffer (Fix watcher to recompile m_gbufferPipeline)
        watchShader("assets/shaders/GBufferMaterial.slang", "GBuffer", [this]() { createGBufferPipeline(true); });
        // Deferred PBR Lighting
        watchShader("assets/shaders/DeferredLighting.slang", "DeferredLighting", [this]() { createDeferredLightingPipeline(true); });
        // Post Process
        watchShader("assets/shaders/PostProcess.slang", "PostProcess", [this]() { createPostProcessPipeline(true); });

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

        if (s_Instance == this) s_Instance = nullptr;

        m_device->waitIdle();
        m_device->shutdown(); // needed for texture to not remove imguitexture
        m_renderer2D.reset();
        // Cleanup Vulkan resources here
    }

    void Renderer::resizeWindow()
    {
        framebufferResized = true;
    }

    void Renderer::initRenderer()
    {
        createSwapChain();
        createCompiler();
        createUnlitPipeline(false);
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
        m_viewportSize = size;
        applyRenderResolution();
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

        m_device->waitIdle();
        createSceneResources();
        createEntityResources();
        createDepthResources();

        //Visability
        createVisibilityResources();

        // G-Buffer
        createGBufferResources();

        // NGX's internal DLSS feature is fixed-size once created; it must be explicitly freed here
        // so it gets recreated at the new resolution on the next evaluate, otherwise evaluate silently
        // no-ops forever once our tagged resources no longer match the size it was created with.
        m_device->resetDLSSViewport();
        m_resetDLSS = true;
    }

    void Renderer::applyPendingRenderResolutionIfNeeded()
    {
        if (!m_pendingRenderResolutionUpdate)
            return;
        m_pendingRenderResolutionUpdate = false;
        applyRenderResolution();
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
            NRI::ImageFormat::RGBA8,
            NRI::ImageFormat::R16G16B16A16_SFLOAT,
            NRI::ImageFormat::RGBA8,
            NRI::ImageFormat::R16G16B16A16_SFLOAT,
            NRI::ImageFormat::R32SINT,
            NRI::ImageFormat::R32G32_SFLOAT
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

                // 5. Create temporary scratch buffer and build on GPU
                std::unique_ptr<NRI::Buffer> scratchBuffer = m_device->createBuffer(NRI::BufferDesc{
                    .size = buildSizes.buildScratchSize,
                    .usage = NRI::BufferUsage::AccelerationStructureScratch
                });

                std::unique_ptr<NRI::CommandBuffer> cmd = beginSingleTimeCommands();
                cmd->buildAccelerationStructure(buildDesc, scratchBuffer->getDeviceAddress(), *blas);
                cmd->accelerationStructureBarrier(NRI::AccelerationStructureBarrierType::BuildToBuild);
                endSingleTimeCommands(std::move(cmd));

                // 6. Cache BLAS and store ID in handle
                handle.blasId = static_cast<uint32_t>(m_meshBLASes.size());
                m_meshBLASes.push_back(MeshBLAS{
                    .storageBuffer = std::move(asBuffer),
                    .as = std::move(blas),
                    .indexBuffer = std::move(indexBuffer),
                    .vertexBufferAddress = vertexBufferBDA,
                    .indexCount = static_cast<uint32_t>(indices.size())
                });
            }
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
                bdas.push_back(pageAllocator.GetBuffer(i)->getDeviceAddress());
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

        m_commandBuffers->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());

        // Hardware Ray Tracing: Record TLAS build/update commands on GPU
        BuildSceneAccelerationStructure(frameIndex);

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

        // =========================================================================
        // 2. G-BUFFER MATERIAL GENERATION PASS (Decoupled Material Resolve)
        // Only run if there are active meshes in the scene!
        // =========================================================================
        if (m_gbufferPipeline && baseInstanceAddress != 0)
        {
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

            for (uint32_t a = 0; a < 6; ++a)
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
        // 3. DEFERRED LIGHTING PASS (Evaluates HDR Radiance -> m_hdrSceneResource)
        // =========================================================================
        if (m_deferredLightingPipeline)
        {
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
            m_commandBuffers->pushData(&lightingPush, sizeof(shaderio::PushConstantDeferredLighting));

            m_commandBuffers->drawMeshTasks(1, 1, 1);
            m_commandBuffers->endRendering();
            m_commandBuffers->executionBarrier();
        }

        // =========================================================================
        // 4. FORWARD 3D PASS: UNLIT & SKYBOX (Rendered in HDR into m_hdrSceneResource)
        // =========================================================================
        {
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
            if (m_unlitPipeline && (m_transparentCount > 0 || m_transparentUnlitCount > 0))
            {
                boundPipeline = nullptr;
                m_commandBuffers->setDepthTestEnable(true);
                m_commandBuffers->setDepthWriteEnable(false);
                m_commandBuffers->setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                m_commandBuffers->setColorBlendEnable(0, true);
                m_commandBuffers->setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);
                m_commandBuffers->setColorBlendEnable(1, false);
                m_commandBuffers->setColorWriteMask(1, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                drawPass(m_transparentCount, *m_unlitPipeline, NRI::CullMode::None, false, true);
                drawPass(m_transparentUnlitCount, *m_unlitPipeline, NRI::CullMode::None, false, true);

                m_commandBuffers->setColorBlendEnable(0, false);
                m_commandBuffers->setDepthWriteEnable(true);
            }

            m_commandBuffers->endRendering();
            m_commandBuffers->executionBarrier();
        }

        // =========================================================================
        // 4.5 DLSS EVALUATION PASS (via NRI Device Abstraction)
        // =========================================================================
        bool dlssActive = false;
        static bool s_loggedSlots = false;
        if (!s_loggedSlots && m_dlssEnabled)
        {
            s_loggedSlots = true;
            NOX_CORE_INFO("[DLSS Debug] m_dlssOutputResource slot: {}, m_hdrSceneResource slot: {}",
                          m_dlssOutputResource ? m_dlssOutputResource->GetDescriptorIndexSlot() : 999999,
                          m_hdrSceneResource ? m_hdrSceneResource->GetDescriptorIndexSlot() : 999999);
        }
        //debug good here or no should they be raw or not ?
        if (m_dlssEnabled && m_dlssMode != NRI::UpscaleMode::Off && m_device->isDLSSSupported() && m_dlssOutputResource)
        {
            NRI::DLSSParams dlssParams{};
            dlssParams.inputColor = m_hdrSceneResource.get();
            dlssParams.outputColor = m_dlssOutputResource.get();
            dlssParams.depth = m_depthResource.get();
            dlssParams.motionVectors = m_gbufferVelocity.get();
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
        m_entityResource->blitTo(*m_commandBuffers, *m_entityResourceHi);
        m_depthResource->blitTo(*m_commandBuffers, *m_depthResourceHi);
        m_commandBuffers->executionBarrier();

        // =========================================================================
        // 5. POST-PROCESSING & TONEMAPPING (HDR m_hdrSceneResource -> LDR m_sceneResource)
        // =========================================================================
        if (m_postProcessPipeline)
        {
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
            postPush.hdrTextureIndex = dlssActive ? m_dlssOutputResource->GetDescriptorIndexSlot() : m_hdrSceneResource->GetDescriptorIndexSlot();
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

        m_commandBuffers->transitionSwapchainLayout(*m_swapChain, imageIndex, NRI::TextureLayout::ColorAttachment, NRI::TextureLayout::Present);

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
    void Renderer::drawFrame()
    {
        processDeferredDeletions();
        processDeferredMeshFrees();

        // Process any queued shader hot-reloads
        if (!m_pendingReloads.empty())
        {
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

        if (m_swapChain->acquireNextImage(frameIndex, imageIndex) == NRI::FrameResult::ResizeRequired)
        {
            recreateSwapChain();
            return;
        }

        updatePageTables(frameIndex);

        updateEntityIDBuffer(frameIndex);

        updateLightBuffer(frameIndex);

        BuildBuffers();

        updateInstanceAndIndirectBuffer(frameIndex);

        updateBoneBuffer(frameIndex);

        // Hardware Ray Tracing: CPU gathering, instance buffer upload, TLAS heap registration
        updateSceneAccelerationStructure(frameIndex);

        updateUniformBuffer(frameIndex);

        m_renderer2D->Update(frameIndex);

        recordCommandBuffer(imageIndex);

        m_device->submitCommandBuffer(*m_commandBuffers, *m_swapChain, frameIndex, imageIndex);

        if (m_swapChain->present(frameIndex, imageIndex) == NRI::FrameResult::ResizeRequired || framebufferResized)
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
        m_transparentUnlitQueue.clear();
    }

    void Renderer::updateSceneAccelerationStructure(uint32_t currentFrameIndex)
    {
        if (!m_rayTracingEnabled || (!m_rayTracingShadows && !m_rayTracingReflections))
        {
            m_hasTLASBuild = false;
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
                inst.mask = 0xFF;
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
                lut.baseColorTextureIndex = packet.instance.baseColorTextureIndex;
                lut.alphaCutoff = packet.instance.alphaMaskCutoff;
                lut.alphaMode = packet.instance.alphaMode;
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
        if (!m_hasTLASBuild)
            return;

        // 1. Pre-build barrier: Host/Transfer instance writes -> AS Build Read
        m_commandBuffers->accelerationStructureBarrier(NRI::AccelerationStructureBarrierType::TransferToBuild);

        // 2. Always BUILD TLAS fresh every frame (matching Khronos tutorial line 1825).
        // This ensures deleted, swapped, or moved meshes update immediately with zero ghost geometry!
        m_commandBuffers->buildAccelerationStructure(m_tlasBuildDesc, m_tlasScratchBuffer->getDeviceAddress(), *m_sceneTLAS);

        // 3. Post-build barrier: AS Build Write -> Fragment/Compute Shader Read
        m_commandBuffers->accelerationStructureBarrier(NRI::AccelerationStructureBarrierType::BuildToShaderRead);
    }

    int32_t Renderer::getPickedEntityID()
    {
        int32_t clickedEntityID = -1;

        // Map the staging buffer for the current in-flight frame
        void* mappedMemory = m_pickerStagingBuffers[frameIndex]->map(0, sizeof(int32_t));

        if (mappedMemory)
        {
            memcpy(&clickedEntityID, mappedMemory, sizeof(int32_t));
            m_pickerStagingBuffers[frameIndex]->unmap();
        }

        return clickedEntityID;
    }

    std::vector<int32_t> Renderer::getPickedEntityIDs()
    {
        std::vector<int32_t> uniqueIDs;
        size_t pixelCount = static_cast<size_t>(m_pickRequest.width) * m_pickRequest.height;
        if (pixelCount == 0)
            return uniqueIDs;

        void* mappedMemory = m_pickerStagingBuffers[frameIndex]->map(0, pixelCount * sizeof(int32_t));
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

            m_pickerStagingBuffers[frameIndex]->unmap();
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

    void Renderer::BeginScene(const Camera& camera, const glm::mat4& transform)
    {
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

            if (m_isFirstFrame)
            {
                m_prevView = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) * glm::inverse(transform);
                m_prevNonJitteredProj = camera.GetProjection();
                m_isFirstFrame = false;
            }
            else
            {
                m_prevView = m_currentView;
                m_prevNonJitteredProj = m_currentNonJitteredProj;
            }

            m_currentView = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) * glm::inverse(transform);
            m_currentNonJitteredProj = camera.GetProjection();

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
        /*uniformData.cameraWorldPos = { camera.GetPosition(), 0.0f };*/
        uniformData.frustum = shaderio::Frustum{uniformData.proj * uniformData.view};

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

            if (m_isFirstFrame)
            {
                m_prevView = camera.GetViewMatrix();
                m_prevNonJitteredProj = camera.GetProjection();
                m_isFirstFrame = false;
            }
            else
            {
                m_prevView = m_currentView;
                m_prevNonJitteredProj = m_currentNonJitteredProj;
            }

            m_currentView = camera.GetViewMatrix();
            m_currentNonJitteredProj = camera.GetProjection();

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

        sortByDistance(m_transparentUnlitQueue);
        packQueue(m_transparentUnlitQueue, m_transparentUnlitCount);
    }

    void Renderer::DrawMesh(const glm::mat4& transform, Ref<Mesh> mesh, uint32_t submeshIndex, const MaterialComponent& material, int entityID, const std::vector<glm::mat4>* boneTransforms)
    {
        const auto& submeshes = mesh->GetSubMeshes();
        if (submeshIndex >= submeshes.size())
            return;

        const MeshHandle& handle = submeshes[submeshIndex];

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

        // Since child entities hold only their own single material, index 0 is always the correct target
        // Select slot matching submeshIndex, fallback to slot 0 if child entity only holds 1 texture
        uint32_t slotIdx = (material.BaseColorFactors.size() > 1) ? submeshIndex : 0;

        // Helper to safely fetch descriptor index
        auto getTextureIndex = [](const std::vector<AssetHandle>& maps, uint32_t idx) -> int
        {
            if (!maps.empty() && idx < maps.size() && maps[idx] != 0)
            {
                Ref<Texture2D> texture = AssetManager::GetAsset<Texture2D>(maps[idx]);
                if (texture) return texture->GetDescriptorIndexSlot();
            }
            return -1;
        };

        // --- MATERIAL PACKING ---

        instance.workflow = (slotIdx < material.Workflows.size()) ? material.Workflows[slotIdx] : 0.0f;
        instance.diffuseFactor = (slotIdx < material.DiffuseFactors.size()) ? material.DiffuseFactors[slotIdx] : glm::vec4(1.0f);
        instance.specularFactor = (slotIdx < material.SpecularFactors.size()) ? material.SpecularFactors[slotIdx] : glm::vec4(1.0f);

        // Base Color
        instance.baseColorFactor = (slotIdx < material.BaseColorFactors.size()) ? material.BaseColorFactors[slotIdx] : glm::vec4(1.0f);
        instance.baseColorTextureIndex = getTextureIndex(material.BaseColorMaps, slotIdx);
        instance.baseColorTextureSet = (slotIdx < material.BaseColorTextureSets.size()) ? material.BaseColorTextureSets[slotIdx] : 0;

        // PBR Properties
        instance.metallicFactor = (slotIdx < material.MetallicFactors.size()) ? material.MetallicFactors[slotIdx] : 1.0f;
        instance.roughnessFactor = (slotIdx < material.RoughnessFactors.size()) ? material.RoughnessFactors[slotIdx] : 1.0f;
        instance.metallicRoughnessTextureIndex = getTextureIndex(material.MetallicRoughnessMaps, slotIdx);
        instance.physicalDescriptorTextureSet = (slotIdx < material.PhysicalDescriptorTextureSets.size()) ? material.PhysicalDescriptorTextureSets[slotIdx] : 0;

        // Additional Maps
        instance.normalTextureIndex = getTextureIndex(material.NormalMaps, slotIdx);
        instance.normalTextureSet = (slotIdx < material.NormalTextureSets.size()) ? material.NormalTextureSets[slotIdx] : 0;

        instance.occlusionTextureIndex = getTextureIndex(material.OcclusionMaps, slotIdx);
        instance.occlusionTextureSet = (slotIdx < material.OcclusionTextureSets.size()) ? material.OcclusionTextureSets[slotIdx] : 0;

        // Emission
        instance.emissiveFactor = (slotIdx < material.EmissiveFactors.size()) ? material.EmissiveFactors[slotIdx] : glm::vec3(0.0f);
        instance.emissiveTextureIndex = getTextureIndex(material.EmissiveMaps, slotIdx);
        instance.emissiveTextureSet = (slotIdx < material.EmissiveTextureSets.size()) ? material.EmissiveTextureSets[slotIdx] : 0;
        instance.emissiveStrength = (slotIdx < material.EmissiveStrengths.size()) ? material.EmissiveStrengths[slotIdx] : 1.0f;

        // Transmission
        instance.transmissionFactor = (slotIdx < material.TransmissionFactors.size()) ? material.TransmissionFactors[slotIdx] : 0.0f;
        instance.transmissionTextureIndex = getTextureIndex(material.TransmissionMaps, slotIdx);
        instance.transmissionTextureSet = (slotIdx < material.TransmissionTextureSets.size()) ? material.TransmissionTextureSets[slotIdx] : 0;

        // Settings
        AlphaMode mode = (slotIdx < material.Modes.size()) ? material.Modes[slotIdx] : AlphaMode::Opaque;
        instance.alphaMode = static_cast<uint32_t>(mode);
        instance.alphaMaskCutoff = (slotIdx < material.AlphaMaskCutoffs.size()) ? material.AlphaMaskCutoffs[slotIdx] : 0.5f;
        instance.doubleSided = (slotIdx < material.DoubleSidedFlags.size()) ? (uint32_t)material.DoubleSidedFlags[slotIdx] : 0;
        instance.unlit = (slotIdx < material.UnlitFlags.size()) ? (uint32_t)material.UnlitFlags[slotIdx] : 0;

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

            if (isUnlit)
                m_transparentUnlitQueue.push_back(packet);
            else
                m_transparentQueue.push_back(packet);
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

    void Renderer::DrawStaticMesh(const glm::mat4& transform, Ref<StaticMesh> staticMesh, const MaterialComponent& material, int entityID)
    {
        // Helper to safely fetch descriptor index
        auto getTextureIndex = [](const std::vector<AssetHandle>& maps, uint32_t idx) -> int
        {
            if (!maps.empty() && idx < maps.size() && maps[idx] != 0)
            {
                Ref<Texture2D> texture = AssetManager::GetAsset<Texture2D>(maps[idx]);
                if (texture) return texture->GetDescriptorIndexSlot();
            }
            return -1;
        };

        for (size_t i = 0; i < staticMesh->GetSubMeshes().size(); ++i)
        {
            MeshHandle handle = staticMesh->GetSubMeshes()[i];

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

            // --- MATERIAL PACKING ---

            instance.workflow = (i < material.Workflows.size()) ? material.Workflows[i] : 0.0f;
            instance.diffuseFactor = (i < material.DiffuseFactors.size()) ? material.DiffuseFactors[i] : glm::vec4(1.0f);
            instance.specularFactor = (i < material.SpecularFactors.size()) ? material.SpecularFactors[i] : glm::vec4(1.0f);

            // Base Color
            instance.baseColorFactor = (i < material.BaseColorFactors.size()) ? material.BaseColorFactors[i] : glm::vec4(1.0f);
            instance.baseColorTextureIndex = getTextureIndex(material.BaseColorMaps, i);
            instance.baseColorTextureSet = (i < material.BaseColorTextureSets.size()) ? material.BaseColorTextureSets[i] : 0;

            // PBR Properties
            instance.metallicFactor = (i < material.MetallicFactors.size()) ? material.MetallicFactors[i] : 1.0f;
            instance.roughnessFactor = (i < material.RoughnessFactors.size()) ? material.RoughnessFactors[i] : 1.0f;
            instance.metallicRoughnessTextureIndex = getTextureIndex(material.MetallicRoughnessMaps, i);
            instance.physicalDescriptorTextureSet = (i < material.PhysicalDescriptorTextureSets.size()) ? material.PhysicalDescriptorTextureSets[i] : 0;

            // Additional Maps
            instance.normalTextureIndex = getTextureIndex(material.NormalMaps, i);
            instance.normalTextureSet = (i < material.NormalTextureSets.size()) ? material.NormalTextureSets[i] : 0;

            instance.occlusionTextureIndex = getTextureIndex(material.OcclusionMaps, i);
            instance.occlusionTextureSet = (i < material.OcclusionTextureSets.size()) ? material.OcclusionTextureSets[i] : 0;

            // Emission
            instance.emissiveFactor = (i < material.EmissiveFactors.size()) ? material.EmissiveFactors[i] : glm::vec3(0.0f);
            instance.emissiveTextureIndex = getTextureIndex(material.EmissiveMaps, i);
            instance.emissiveTextureSet = (i < material.EmissiveTextureSets.size()) ? material.EmissiveTextureSets[i] : 0;
            instance.emissiveStrength = (i < material.EmissiveStrengths.size()) ? material.EmissiveStrengths[i] : 1.0f;

            // Transmission
            instance.transmissionFactor = (i < material.TransmissionFactors.size()) ? material.TransmissionFactors[i] : 0.0f;
            instance.transmissionTextureIndex = getTextureIndex(material.TransmissionMaps, i);
            instance.transmissionTextureSet = (i < material.TransmissionTextureSets.size()) ? material.TransmissionTextureSets[i] : 0;

            // Settings
            AlphaMode mode = (i < material.Modes.size()) ? material.Modes[i] : AlphaMode::Opaque;
            instance.alphaMode = static_cast<uint32_t>(mode);
            instance.alphaMaskCutoff = (i < material.AlphaMaskCutoffs.size()) ? material.AlphaMaskCutoffs[i] : 0.5f;
            instance.doubleSided = (i < material.DoubleSidedFlags.size()) ? (uint32_t)material.DoubleSidedFlags[i] : 0;
            instance.unlit = (i < material.UnlitFlags.size()) ? (uint32_t)material.UnlitFlags[i] : 0;

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

                if (isUnlit)
                    m_transparentUnlitQueue.push_back(packet);
                else
                    m_transparentQueue.push_back(packet);
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
                DrawMesh(transform, mesh, src.SubmeshIndex, srcMat, entityID, boneTransforms);
            }
        }
        else if (type == AssetType::StaticMesh)
        {
            Ref<StaticMesh> staticMesh = AssetManager::GetAsset<StaticMesh>(src.Mesh);
            if (staticMesh)
            {
                DrawStaticMesh(transform, staticMesh, srcMat, entityID);
            }
        }
    }

    void Renderer::SubmitLight(const glm::mat4& transform, const DirectionalLightComponent& light)
    {
        glm::vec3 forward = glm::normalize(glm::vec3(transform * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

        shaderio::LightData l{};
        l.position = glm::vec4(0.0f, 0.0f, 0.0f, (float)shaderio::LightType::Directional);
        l.direction = glm::vec4(forward, 0.0f);
        l.color = glm::vec4(light.Color, light.Intensity);
        l.spotParams = glm::vec4(0.0f);

        m_lightBufferObjects.push_back(l);
    }

    void Renderer::SubmitLight(const glm::mat4& transform, const PointLightComponent& light)
    {
        glm::vec3 worldPos = glm::vec3(transform[3]);

        shaderio::LightData l{};
        l.position = glm::vec4(worldPos, (float)shaderio::LightType::Point);
        l.direction = glm::vec4(0.0f, 0.0f, 0.0f, light.Range);
        l.color = glm::vec4(light.Color, light.Intensity);
        l.spotParams = glm::vec4(0.0f);

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
        l.spotParams = glm::vec4(lightAngleScale, lightAngleOffset, 0.0f, 0.0f);

        m_lightBufferObjects.push_back(l);
    }
}
