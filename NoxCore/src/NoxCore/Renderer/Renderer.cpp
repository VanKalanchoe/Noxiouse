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
                .mipLevels = cpuData.MipLevels,
                .sampleCount = 1,
                .usage = NRI::TextureUsage::ShaderResource,
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

    MeshHandle Renderer::UploadMeshGeometry(const MeshData& data)
    {
        MeshHandle handle{};
        handle.firstMeshlet = static_cast<uint32_t>(m_meshletDraws.size());

        uint32_t baseVertex = static_cast<uint32_t>(m_vertices.size());
        uint32_t meshletVertexOffset = static_cast<uint32_t>(m_meshletVertices.size());
        uint32_t meshletTrianglesOffset = static_cast<uint32_t>(m_meshletTriangles.size());

        // 1. Append vertices
        m_vertices.insert(m_vertices.end(), data.Vertices.begin(), data.Vertices.end());

        // 2. Append meshlet draw calls (adjusting global offsets)
        for (auto draw : data.Draws)
        {
            draw.vertexOffset += meshletVertexOffset;
            draw.triangleOffset += meshletTrianglesOffset;
            draw.globalVertexOffset += baseVertex;
            m_meshletDraws.push_back(draw);
        }

        // 3. Append bounds & raw meshlet buffers
        m_meshletBounds.insert(m_meshletBounds.end(), data.Bounds.begin(), data.Bounds.end());
        m_meshletVertices.insert(m_meshletVertices.end(), data.MeshletVertices.begin(), data.MeshletVertices.end());
        m_meshletTriangles.insert(m_meshletTriangles.end(), data.MeshletTriangles.begin(), data.MeshletTriangles.end());

        handle.meshletCount = static_cast<uint32_t>(data.Draws.size());
        return handle;
    }

    void Renderer::UpdateMeshletBuffers()
    {
        if (s_Instance)
            s_Instance->createMeshletBuffers();
        else
            NOX_CORE_ASSERT(false, "Renderer::updateMeshletBuffers() Renderer instance does not exist when updating meshlet buffers!");
    }

    void Renderer::createMeshletBuffers()
    {
        // Vertices
        {
            uint64_t bufferSize = sizeof(m_vertices[0]) * m_vertices.size();

            std::unique_ptr<NRI::Buffer> stagingBuffer = m_device->createBuffer(NRI::BufferDesc{
                .size = bufferSize,
                .usage = NRI::BufferUsage::Staging
            });

            void* mappedMemory = stagingBuffer->map(0, bufferSize);
            memcpy(mappedMemory, m_vertices.data(), bufferSize);
            stagingBuffer->unmap();

            m_verticesBuffer = m_device->createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::StorageStatic
                });

            std::unique_ptr<NRI::CommandBuffer> commandCopyBuffer = beginSingleTimeCommands();
            m_verticesBuffer->uploadData(*commandCopyBuffer, *stagingBuffer, m_vertices.data());
            endSingleTimeCommands(std::move(commandCopyBuffer));
        }
        
        // Meshlet Bounds
        {
            uint64_t bufferSize = sizeof(m_meshletBounds[0]) * m_meshletBounds.size();

            std::unique_ptr<NRI::Buffer> stagingBuffer = m_device->createBuffer(NRI::BufferDesc{
                .size = bufferSize,
                .usage = NRI::BufferUsage::Staging
            });

            void* mappedMemory = stagingBuffer->map(0, bufferSize);
            memcpy(mappedMemory, m_meshletBounds.data(), bufferSize);
            stagingBuffer->unmap();

            m_meshletBoundsBuffer = m_device->createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::StorageStatic
                });

            std::unique_ptr<NRI::CommandBuffer> commandCopyBuffer = beginSingleTimeCommands();
            m_meshletBoundsBuffer->uploadData(*commandCopyBuffer, *stagingBuffer, m_meshletBounds.data());
            endSingleTimeCommands(std::move(commandCopyBuffer));
        }
        
        // Meshlet Draws
        {
            uint64_t bufferSize = sizeof(m_meshletDraws[0]) * m_meshletDraws.size();

            std::unique_ptr<NRI::Buffer> stagingBuffer = m_device->createBuffer(NRI::BufferDesc{
                .size = bufferSize,
                .usage = NRI::BufferUsage::Staging
            });

            void* mappedMemory = stagingBuffer->map(0, bufferSize);
            memcpy(mappedMemory, m_meshletDraws.data(), bufferSize);
            stagingBuffer->unmap();

            m_meshletDrawsBuffer = m_device->createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::StorageStatic
                });

            std::unique_ptr<NRI::CommandBuffer> commandCopyBuffer = beginSingleTimeCommands();
            m_meshletDrawsBuffer->uploadData(*commandCopyBuffer, *stagingBuffer, m_meshletDraws.data());
            endSingleTimeCommands(std::move(commandCopyBuffer));
        }
        
        // Meshlet Vertices
        {
            uint64_t bufferSize = sizeof(m_meshletVertices[0]) * m_meshletVertices.size();

            std::unique_ptr<NRI::Buffer> stagingBuffer = m_device->createBuffer(NRI::BufferDesc{
                .size = bufferSize,
                .usage = NRI::BufferUsage::Staging
            });

            void* mappedMemory = stagingBuffer->map(0, bufferSize);
            memcpy(mappedMemory, m_meshletVertices.data(), bufferSize);
            stagingBuffer->unmap();

            m_meshletVerticesBuffer = m_device->createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::StorageStatic
                });

            std::unique_ptr<NRI::CommandBuffer> commandCopyBuffer = beginSingleTimeCommands();
            m_meshletVerticesBuffer->uploadData(*commandCopyBuffer, *stagingBuffer, m_meshletVertices.data());
            endSingleTimeCommands(std::move(commandCopyBuffer));
        }
        
        // Meshlet Triangles
        {
            uint64_t bufferSize = sizeof(m_meshletTriangles[0]) * m_meshletTriangles.size();

            std::unique_ptr<NRI::Buffer> stagingBuffer = m_device->createBuffer(NRI::BufferDesc{
                .size = bufferSize,
                .usage = NRI::BufferUsage::Staging
            });

            void* mappedMemory = stagingBuffer->map(0, bufferSize);
            memcpy(mappedMemory, m_meshletTriangles.data(), bufferSize);
            stagingBuffer->unmap();

            m_meshletTrianglesBuffer = m_device->createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::StorageStatic
                });

            std::unique_ptr<NRI::CommandBuffer> commandCopyBuffer = beginSingleTimeCommands();
            m_meshletTrianglesBuffer->uploadData(*commandCopyBuffer, *stagingBuffer, m_meshletTriangles.data());
            endSingleTimeCommands(std::move(commandCopyBuffer));
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
            references.verticesReference = m_verticesBuffer->getDeviceAddress();
            references.meshletBoundsReference = m_meshletBoundsBuffer->getDeviceAddress();
            references.meshletDrawsReference = m_meshletDrawsBuffer->getDeviceAddress();
            references.meshletVerticesReference = m_meshletVerticesBuffer->getDeviceAddress();
            references.meshletTrianglesReference = m_meshletTrianglesBuffer->getDeviceAddress();
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
    {//fix maybe dont recreate all buffers only the next frame ?
        if (m_instanceBufferObjects.empty() || m_drawMeshTasksIndirectCommands.empty())
        {
            return;
        }
        
        uint64_t requiredInstanceSize = sizeof(shaderio::InstanceData) * m_instanceBufferObjects.size();
        if (requiredInstanceSize > m_InstanceBufferCapacity)
        {
            m_InstanceBufferCapacity = requiredInstanceSize * 2;
            
            createInstanceBuffer(m_InstanceBufferCapacity);
        }
        memcpy(m_instanceBuffersMapped[currentImage], m_instanceBufferObjects.data(), requiredInstanceSize);
        
        uint64_t requiredIndirectSize = sizeof(DrawMeshTasksIndirectCommand) * m_drawMeshTasksIndirectCommands.size();
        if (requiredIndirectSize > m_IndirectBufferCapacity)
        {
            m_IndirectBufferCapacity = requiredIndirectSize * 2;
            
            createIndirectBuffer(m_IndirectBufferCapacity);
        }
        memcpy(m_indirectBuffersMapped[currentImage], m_drawMeshTasksIndirectCommands.data(), requiredIndirectSize);
    }

    // with compute there might be a snyc issue idk
    // according to gpt no async since compute and graphics same commandbuffer and executed in order
    void Renderer::drawFrame()
    {
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

        updateUniformBuffer(frameIndex);
        
        updateInstanceAndIndirectBuffer(frameIndex);

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
        uniformData.frustum = shaderio::Frustum{ uniformData.proj * uniformData.view };
        
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
        uniformData.cameraWorldPos = { camera.GetPosition(), 0.0f };
        uniformData.frustum = shaderio::Frustum{ uniformData.proj * uniformData.view };
        
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
    
    void Renderer::DrawMesh(const glm::mat4& transform, Ref<Mesh> mesh, int entityID)
    {
        for (const MeshHandle& handle : mesh->GetSubMeshes())
        {
            shaderio::InstanceData instance{};
            instance.modelMatrix = transform;
            instance.meshletOffset = handle.firstMeshlet;
            instance.meshletCount = handle.meshletCount;
            instance.entityID = entityID;
            
            m_instanceBufferObjects.push_back(instance);
            
            DrawMeshTasksIndirectCommand command{};
            command.groupCountX = (handle.meshletCount + shaderio::TASK_SHADER_DISPATCH_X - 1) / shaderio::TASK_SHADER_DISPATCH_X;
            command.groupCountY = 1;
            command.groupCountZ = 1;
            
            m_drawMeshTasksIndirectCommands.push_back(command);
        }
    }
    
    void Renderer::DrawStaticMesh(const glm::mat4& transform, Ref<StaticMesh> staticMesh, int entityID)
    {
        MeshHandle handle = staticMesh->GetHandle(); // Assuming StaticMesh has a getter for its single handle

        shaderio::InstanceData instance{};
        instance.modelMatrix = transform;
        instance.meshletOffset = handle.firstMeshlet;
        instance.meshletCount = handle.meshletCount;
        instance.entityID = entityID;
    
        m_instanceBufferObjects.push_back(instance);
    
        DrawMeshTasksIndirectCommand command{};
        command.groupCountX = (handle.meshletCount + shaderio::TASK_SHADER_DISPATCH_X - 1) / shaderio::TASK_SHADER_DISPATCH_X;
        command.groupCountY = 1;
        command.groupCountZ = 1;
    
        m_drawMeshTasksIndirectCommands.push_back(command);
    }

    void Renderer::SubmitMesh(const glm::mat4& transform, MeshComponent& src, int entityID)
    {
        if (src.Mesh == 0)
            return;

        AssetType type = AssetManager::GetAssetType(src.Mesh);

        if (type == AssetType::Mesh)
        {
            Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(src.Mesh);
            if (mesh)
            {
                DrawMesh(transform, mesh, entityID);
            }
        }
        else if (type == AssetType::StaticMesh)
        {
            Ref<StaticMesh> staticMesh = AssetManager::GetAsset<StaticMesh>(src.Mesh);
            if (staticMesh)
            {
                DrawStaticMesh(transform, staticMesh, entityID);
            }
        }
    }
}
