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
    static bool m_reloadShader = false;

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

        m_fileWatcher.watch(std::filesystem::path("assets/shaders/Meshlet.slang"), [this](auto path) { m_reloadShader = true; });
        m_fileWatcher.watch(std::filesystem::path("assets/shaders/shader.slang"), [this](auto path) { m_reloadShader = true; });
        m_fileWatcher.watch(std::filesystem::path("assets/shaders/present.slang"), [this](auto path) { m_reloadShader = true; });

        m_whiteTexture = createSolidColorTexture(255, 255, 255, 255);

        m_pickerStagingBuffers.reserve(MAX_FRAMES_IN_FLIGHT);
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            m_pickerStagingBuffers.emplace_back(m_device->createBuffer(NRI::BufferDesc{
                .size = sizeof(int32_t),
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
        createGraphicsPipeline(false);
        if (!m_isEditor) createPresentPipeline(false);
        createComputePipeline();
        createCommandPool();
        createUniformBuffers();
        createDescriptorHeaps();
        createTextureImage();
        createSceneResources();
        createColorResources();
        createEntityResources();
        createDepthResources();
        createCommandBuffers();

        initGeometryBuffers();

        // Create every resource needed for PBR
        initPBR();
    }

    void Renderer::cleanupSwapChain()
    {
        m_swapChain.reset();
    }

    bool firstframe = true;

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
            firstframe = true;
            m_resourceHeap->unregisterTexture(m_sceneResource->GetDescriptorIndexSlot());
        }

        cleanupSwapChain();
        createSwapChain();
        createSceneResources();
        createColorResources();
        createEntityResources();
        createDepthResources();
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
        m_device->waitIdle();
        createSceneResources();
        createColorResources();
        createEntityResources();
        createDepthResources();
    }

    void Renderer::createCompiler()
    {
        m_shaderCompiler = NRI::CreateSlangCompiler();
    }

    void Renderer::createGraphicsPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;

        desc.colorFormats =
        {
            NRI::ImageFormat::Surface,
            NRI::ImageFormat::R32SINT
        };
        // notes i had to split task from mesh because of i think drawid otherwise weird flickering and not showing up correctly
        // might be a slang issue could change in the future fuck nvidia not testing slang
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/MeshletTask.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/Meshlet.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/Meshlet.slang"
        });
        m_graphicsPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
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
    }

    void Renderer::createColorResources()
    {
        //changed from m_swapChainExtent to m_viewportSize
        m_colorResource = m_device->createTexture(NRI::TextureDesc{
            .width = m_isEditor ? m_viewportSize.width : m_swapChainExtent.width,
            .height = m_isEditor ? m_viewportSize.height : m_swapChainExtent.height,
            .mipLevels = 1,
            .sampleCount = m_device->getMSAASampleCount(),
            .usage = NRI::TextureUsage::ColorResolveAttachment,
            .format = NRI::ImageFormat::Surface,
            .directFormat = UINT32_MAX
        });
    }

    void Renderer::createEntityResources()
    {
        //changed from m_swapChainExtent to m_viewportSize
        m_entityResource = m_device->createTexture(NRI::TextureDesc{
            .width = m_isEditor ? m_viewportSize.width : m_swapChainExtent.width,
            .height = m_isEditor ? m_viewportSize.height : m_swapChainExtent.height,
            .mipLevels = 1,
            .sampleCount = m_device->getMSAASampleCount(),
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R32SINT,
            .directFormat = UINT32_MAX
        });

        m_entityResolveResource = m_device->createTexture(NRI::TextureDesc{
            .width = m_isEditor ? m_viewportSize.width : m_swapChainExtent.width,
            .height = m_isEditor ? m_viewportSize.height : m_swapChainExtent.height,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI::TextureUsage::ColorAttachment,
            .format = NRI::ImageFormat::R32SINT,
            .directFormat = UINT32_MAX
        });
    }

    void Renderer::createDepthResources()
    {
        //changed from m_swapChainExtent to m_viewportSize
        m_depthResource = m_device->createTexture(NRI::TextureDesc{
            .width = m_isEditor ? m_viewportSize.width : m_swapChainExtent.width,
            .height = m_isEditor ? m_viewportSize.height : m_swapChainExtent.height,
            .mipLevels = 1,
            .sampleCount = m_device->getMSAASampleCount(),
            .usage = NRI::TextureUsage::DepthStencilAttachment
        });
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
        Ref<Texture2D> enviromentHDR = TextureImporter::LoadTexture2D("assets/enviroments/papermill/papermill.hdr", {}, this);

        // 2. Create the permanent Cubemap Texture (512x512 per face, 6 array layers)
        constexpr uint32_t cubemapSize = 512;

        m_environmentCubemap = m_device->createTexture(NRI::TextureDesc{
            .width = cubemapSize,
            .height = cubemapSize,
            .arrayLayers = 6,
            .isCubeMap = true,
            .mipLevels = 1, // Can be increased if generating specular mips later
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
        
        // Transition Cubemap layout: Undefined -> General (required for storage writes)
        cmd->transitionTextureLayout(*m_environmentCubemap, NRI::TextureLayout::Undefined, NRI::TextureLayout::General);
        
        // Bind compute pipeline & push constant parameters
        cmd->bindPipeline(NRI::PipelineBindPoint::Compute, *equirectPipeline);
        cmd->pushData(&pushData, sizeof(EquirectPushConstants));
        
        // Calculate thread group counts (16x16 threads per group in compute shader)
        uint32_t groupCountX = (cubemapSize + 15) / 16;
        uint32_t groupCountY = (cubemapSize + 15) / 16;

        // Dispatch work: X and Y cover the resolution, Z=6 covers all 6 cubemap faces
        cmd->dispatch(groupCountX, groupCountY, 6);
        
        // Transition Cubemap layout: General -> ShaderResource (ready for graphics sampling)
        cmd->transitionTextureLayout(*m_environmentCubemap, NRI::TextureLayout::General, NRI::TextureLayout::ShaderResource);
        
        // Submit command buffer and wait for execution to complete
        endSingleTimeCommands(std::move(cmd));
        
        // 7. Overwrite descriptor slot in heap with Sampled Image Descriptor
        m_resourceHeap->registerTexture(*m_environmentCubemap, NRI::TextureUsage::ShaderResource);
        
        // equiRectPipeline and enviromentHDR cleanly go out of scope and release temporary resources
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

    MeshHandle Renderer::UploadMeshGeometry(const MeshData& data)
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

        if (maxPagesRequired == 0) return;

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

    void Renderer::createDescriptorHeaps()
    {
        // todo: imgui needs more space to if you want to register it
        // currently 1000 in initimgui devicevk.cpp

        //hardcoded samplerinfos inside descriptorheapvk cosntructor
        m_samplerHeap = m_device->createDescriptorHeap(NRI::DescriptorHeapDesc{
            .type = NRI::DescriptorHeapType::Sampler,
            .maxSamplerDescriptors = 2
        });

        m_resourceHeap = m_device->createDescriptorHeap(NRI::DescriptorHeapDesc{
            .type = NRI::DescriptorHeapType::Resource,
            .maxBufferDescriptors = 0,
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

        m_commandBuffers->transitionSwapchainLayout(*m_swapChain, imageIndex, NRI::TextureLayout::Undefined, NRI::TextureLayout::ColorAttachment);
        if (firstframe)
        {
            if (!m_isEditor)
                firstframe = false;
            m_commandBuffers->transitionTextureLayout(*m_sceneResource, NRI::TextureLayout::Undefined, NRI::TextureLayout::ColorAttachment);
        }
        else
        {
            m_commandBuffers->transitionTextureLayout(*m_sceneResource, NRI::TextureLayout::ShaderResource, NRI::TextureLayout::ColorAttachment);
        }

        m_commandBuffers->transitionTextureLayout(*m_entityResource, NRI::TextureLayout::Undefined, NRI::TextureLayout::ColorAttachment);
        m_commandBuffers->transitionTextureLayout(*m_entityResolveResource, NRI::TextureLayout::Undefined, NRI::TextureLayout::ColorAttachment);
        m_commandBuffers->transitionTextureLayout(*m_colorResource, NRI::TextureLayout::Undefined, NRI::TextureLayout::ColorAttachment);
        m_commandBuffers->transitionTextureLayout(*m_depthResource, NRI::TextureLayout::Undefined, NRI::TextureLayout::DepthAttachment);

        std::vector<NRI::RenderAttachDesc> colorAttachments;
        colorAttachments.push_back({
            .attachment = m_colorResource.get(),
            .resolve = m_sceneResource.get(),
            .loadOP = NRI::LoadOP::clear,
            .storeOP = NRI::StoreOP::store,
            .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}
        });

        colorAttachments.push_back({
            .attachment = m_entityResource.get(),
            .resolve = m_entityResolveResource.get(),
            .loadOP = NRI::LoadOP::clear,
            .storeOP = NRI::StoreOP::store,
            .clearColor = {-1.0f, 0.0f, 0.0f, 1.0f}
        });

        NRI::RenderAttachDesc depthAttachment =
        {
            .attachment = m_depthResource.get(),
            .loadOP = NRI::LoadOP::clear,
            .storeOP = NRI::StoreOP::dontCare,
            .clearDepth = {0.0f, 0}
        };

        NRI::RenderDesc desc =
        {
            .renderArea = m_isEditor ? NRI::Extent2D{m_viewportSize.width, m_viewportSize.height} : NRI::Extent2D{m_swapChainExtent.width, m_swapChainExtent.height},
            .colorAttachments = colorAttachments,
            .depthAttachment = depthAttachment
        };
        m_commandBuffers->beginRendering(desc);

        // Viewport / scissor (counts and values are both dynamic).
        NRI::Extent2D locViewportSize = m_isEditor ? m_viewportSize : m_swapChainExtent;
        float w = static_cast<float>(locViewportSize.width);
        float h = static_cast<float>(locViewportSize.height);
        m_commandBuffers->setViewportWithCount({0.0f, h, w, -h}, 0.0f, 1.0f);
        m_commandBuffers->setScissorWithCount(locViewportSize);

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
        m_commandBuffers->setCullMode(NRI::CullMode::None);
        m_commandBuffers->setFrontFace(NRI::FrontFace::CounterClockWise);
        m_commandBuffers->setDepthBiasEnable(false);
        m_commandBuffers->setDepthClampEnable(false); //LineWidth maybe ?
        m_commandBuffers->setLineWidth(1.0f);

        // Multisampling.
        uint32_t sampleCount = m_device->getMSAASampleCount();
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
            m_commandBuffers->setColorBlendEnable(0, true);
            m_commandBuffers->setColorBlendEquation(0, blendEquation);
            m_commandBuffers->setColorWriteMask(0, colorWriteMask);
        }

        {
            const NRI::ColorBlendEquation blendEquation
            {
                .srcColorBlendFactor = NRI::BlendFactor::One,
                .dstColorBlendFactor = NRI::BlendFactor::Zero,
                .colorBlendOp = NRI::BlendOp::Add,
                .srcAlphaBlendFactor = NRI::BlendFactor::One,
                .dstAlphaBlendFactor = NRI::BlendFactor::Zero,
                .alphaBlendOp = NRI::BlendOp::Add,
            };
            uint32_t colorWriteMask = NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A;
            m_commandBuffers->setColorBlendEnable(1, false);
            m_commandBuffers->setColorBlendEquation(1, blendEquation);
            m_commandBuffers->setColorWriteMask(1, colorWriteMask);
        }

        m_commandBuffers->setLogicOpEnable(false);

        if (!m_instanceBufferObjects.empty() || !m_drawMeshTasksIndirectCommands.empty())
        {
            m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_graphicsPipeline);

            shaderio::PushConstantMeshlets references{};
            // Pass pointer to the global matrix via a buffer device address
            /*references.modelMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f));*/
            references.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
            references.instanceReference = m_instanceBuffers[frameIndex]->getDeviceAddress();
            bool hasBoneBuffers = frameIndex < m_boneBuffers.size() && m_boneBuffers[frameIndex] != nullptr;
            bool hasBones = !m_boneMatrices.empty();

            references.boneMatrixReference = (hasBones && hasBoneBuffers)
                                                 ? m_boneBuffers[frameIndex]->getDeviceAddress()
                                                 : 0;
            references.vertexPageTableReference = m_vertexPageTableBuffers[frameIndex]->getDeviceAddress();
            references.meshletBoundsPageTableReference = m_meshletBoundPageTableBuffers[frameIndex]->getDeviceAddress();
            references.meshletDrawsPageTableReference = m_meshletDrawPageTableBuffers[frameIndex]->getDeviceAddress();
            references.meshletVerticesPageTableReference = m_meshletVertPageTableBuffers[frameIndex]->getDeviceAddress();
            references.meshletTrianglesPageTableReference = m_meshletTriPageTableBuffers[frameIndex]->getDeviceAddress();
            m_commandBuffers->pushData(&references, sizeof(shaderio::PushConstantMeshlets));

            m_commandBuffers->drawMeshTasksIndirect(*m_indirectBuffers[frameIndex], 0, static_cast<uint32_t>(m_drawMeshTasksIndirectCommands.size()), sizeof(DrawMeshTasksIndirectCommand));
        }

        m_renderer2D->Flush(*m_commandBuffers, *m_uniformBuffers[frameIndex], frameIndex);

        m_commandBuffers->endRendering();

        m_commandBuffers->transitionTextureLayout(*m_sceneResource, NRI::TextureLayout::ColorAttachment, NRI::TextureLayout::ShaderResource);

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
                m_commandBuffers->transitionTextureLayout(*m_entityResource, NRI::TextureLayout::ColorAttachment, NRI::TextureLayout::TransferSrc);
                /*m_commandBuffers->transitionTextureLayout(*m_entityResolveResource, NRI::TextureLayout::ColorAttachment, NRI::TextureLayout::TransferDst);*/

                /*m_commandBuffers->resolveImage(*m_entityResource, *m_entityResolveResource, width, height);*/

                m_commandBuffers->transitionTextureLayout(*m_entityResolveResource, NRI::TextureLayout::ColorAttachment, NRI::TextureLayout::TransferSrc);

                m_entityResolveResource->copyImageToBuffer(*m_commandBuffers, *m_pickerStagingBuffers[frameIndex], sampleX, sampleY, 1, 1);

                m_commandBuffers->transitionTextureLayout(*m_entityResource, NRI::TextureLayout::TransferSrc, NRI::TextureLayout::ColorAttachment);

                m_pickRequest.active = false;
            }

            m_commandBuffers->end(frameIndex);
        }
        else
        {
            m_commandBuffers->end(frameIndex);
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
        uniformData.samplerIndex = selectedSampler;

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

        if (m_reloadShader)
        {
            m_reloadShader = false;
            createGraphicsPipeline(true);
            createPresentPipeline(true);
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

        updateUniformBuffer(frameIndex);

        updateInstanceAndIndirectBuffer(frameIndex);

        updateBoneBuffer(frameIndex);

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

        m_instanceBufferObjects.clear();
        m_drawMeshTasksIndirectCommands.clear();
        m_boneMatrices.clear();
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

    void Renderer::BeginScene(const Camera& camera, const glm::mat4& transform)
    {
        uniformData.proj = camera.GetProjection();
        uniformData.view = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) * glm::inverse(transform);
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
        uniformData.proj = camera.GetProjection();
        uniformData.view = camera.GetViewMatrix();
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

    void Renderer::DrawMesh(const glm::mat4& transform, Ref<Mesh> mesh, uint32_t submeshIndex, const MaterialComponent& material, int entityID, const std::vector<glm::mat4>* boneTransforms)
    {
        const auto& submeshes = mesh->GetSubMeshes();
        if (submeshIndex >= submeshes.size())
            return;

        const MeshHandle& handle = submeshes[submeshIndex];

        shaderio::InstanceData instance{};
        instance.modelMatrix = transform;

        // 1. Where do the meshlets live, and how many are there?
        instance.drawsPageIndex = handle.meshletDraws.pageIndex;
        instance.drawsOffset = handle.meshletDraws.offset; // (Replaces the old meshletOffset)
        instance.meshletCount = handle.meshletDraws.count; // (Replaces the old GetMeshletCount)

        // 2. Where do the vertices and triangles live?
        instance.verticesPageIndex = handle.vertices.pageIndex;
        instance.meshletVerticesPageIndex = handle.meshletVertices.pageIndex;
        instance.meshletTrianglesPageIndex = handle.meshletTriangles.pageIndex;

        instance.albedoColor = material.AlbedoColor;
        instance.albedoTextureIndex = -1;

        // Select slot matching submeshIndex, fallback to slot 0 if child entity only holds 1 texture
        uint32_t mapIndex = (submeshIndex < material.AlbedoMaps.size()) ? submeshIndex : 0;

        if (!material.AlbedoMaps.empty() && mapIndex < material.AlbedoMaps.size())
        {
            const AssetHandle texHandle = material.AlbedoMaps[mapIndex];

            // Ensure handle is valid before looking up
            if (texHandle != 0)
            {
                Ref<Texture2D> texture = AssetManager::GetAsset<Texture2D>(texHandle);
                if (texture)
                {
                    instance.albedoTextureIndex = texture->GetDescriptorIndexSlot();
                }
            }
        }

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

        m_instanceBufferObjects.push_back(instance);

        DrawMeshTasksIndirectCommand command{};
        command.groupCountX = (handle.GetMeshletCount() + shaderio::TASK_SHADER_DISPATCH_X - 1) / shaderio::TASK_SHADER_DISPATCH_X;
        command.groupCountY = 1;
        command.groupCountZ = 1;

        m_drawMeshTasksIndirectCommands.push_back(command);
    }

    void Renderer::DrawStaticMesh(const glm::mat4& transform, Ref<StaticMesh> staticMesh, const MaterialComponent& material, int entityID)
    {
        for (size_t i = 0; i < staticMesh->GetSubMeshes().size(); ++i)
        {
            MeshHandle handle = staticMesh->GetSubMeshes()[i];

            shaderio::InstanceData instance{};
            instance.modelMatrix = transform;

            // --- UPDATED: Page Table Info ---
            instance.drawsPageIndex = handle.meshletDraws.pageIndex;
            instance.drawsOffset = handle.meshletDraws.offset;
            instance.meshletCount = handle.meshletDraws.count;

            instance.verticesPageIndex = handle.vertices.pageIndex;
            instance.meshletVerticesPageIndex = handle.meshletVertices.pageIndex;
            instance.meshletTrianglesPageIndex = handle.meshletTriangles.pageIndex;
            // --------------------------------

            instance.albedoColor = material.AlbedoColor;
            AssetHandle texHandle = 0;
            if (i < material.AlbedoMaps.size())
            {
                texHandle = material.AlbedoMaps[i];
            }

            if (texHandle != 0)
            {
                Ref<Texture2D> texture = AssetManager::GetAsset<Texture2D>(texHandle);
                instance.albedoTextureIndex = texture ? texture->GetDescriptorIndexSlot() : -1;
            }
            else
            {
                instance.albedoTextureIndex = -1;
            }

            instance.entityID = entityID;

            m_instanceBufferObjects.push_back(instance);

            DrawMeshTasksIndirectCommand command{};
            command.groupCountX = (handle.GetMeshletCount() + shaderio::TASK_SHADER_DISPATCH_X - 1) / shaderio::TASK_SHADER_DISPATCH_X;
            command.groupCountY = 1;
            command.groupCountZ = 1;

            m_drawMeshTasksIndirectCommands.push_back(command);
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
}
