#include "Renderer.h"

#include <iostream>
#include <algorithm> // Necessary for std::clamp
#include <chrono>
#include <fstream>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define TINYOBJLOADER_IMPLEMENTATION
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <tiny_obj_loader.h>
#include <SDL3/SDL_log.h>

#include "imgui.h"
static bool m_reloadShader = false;
Renderer::Renderer(SDL_Window& window) : m_window(&window)
{
    std::cout << "VulkanRenderer created\n";

    m_device = NRI::Device::create(NRI::GraphicsAPI::Vulkan, m_window);
    if (!m_device) throw std::runtime_error("Failed to create NRI device");

    initRenderer();
    initImGui();
    m_fileWatcher.watch(std::filesystem::path("../../shaders/shader.slang"), [this]() { m_reloadShader = true; });
}

void Renderer::initImGui()
{
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
    //io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows broken right now
    //io.ConfigFlags |= ImGuiConfigFlags_ViewportsNoTaskBarIcons;
    //io.ConfigFlags |= ImGuiConfigFlags_ViewportsNoMerge;
    
    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();
    
    // Setup scaling
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);        // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting Style + calling this again)
    style.FontScaleDpi = main_scale;        // Set initial font scale. (in docking branch: using io.ConfigDpiScaleFonts=true automatically overrides this for every window depending on the current monitor)
    io.ConfigDpiScaleFonts = true;          // [Experimental] Automatically overwrite style.FontScaleDpi in Begin() when Monitor DPI changes. This will scale fonts but _NOT_ scale sizes/padding for now.
    io.ConfigDpiScaleViewports = true;      // [Experimental] Scale Dear ImGui and Platform Windows when Monitor DPI changes.

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
    
    m_device->initImGui();
    
    // Load Fonts
    // - If fonts are not explicitly loaded, Dear ImGui will select an embedded font: either AddFontDefaultVector() or AddFontDefaultBitmap().
    //   This selection is based on (style.FontSizeBase * style.FontScaleMain * style.FontScaleDpi) reaching a small threshold.
    // - You can load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - If a file cannot be loaded, AddFont functions will return a nullptr. Please handle those errors in your code (e.g. use an assertion, display an error and quit).
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use FreeType for higher quality font rendering.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    //style.FontSizeBase = 20.0f;
    //io.Fonts->AddFontDefaultVector();
    //io.Fonts->AddFontDefaultBitmap();
    //io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf");
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf");
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf");
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf");
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf");
    //IM_ASSERT(font != nullptr);
}

Renderer::~Renderer()
{
    std::cout << "VulkanRenderer destroyed\n";
    
    m_device->waitIdle();
    
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
    createComputePipeline();
    createCommandPool();
    createColorResources();
    createDepthResources(); 
    loadModel();
    createVertexBuffer();
    createIndexBuffer();
    createUniformBuffers();
    createDescriptorHeaps();
    createTextureImage();
    createCommandBuffers();
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

    cleanupSwapChain();
    createSwapChain();
    createColorResources();
    createDepthResources();
}

void Renderer::createSwapChain()
{
    m_swapChain = m_device->createSwapchain(NRI::SwapchainDesc{.windowHandle = m_window});
    m_swapChainExtent = m_swapChain->getExtent();
}

void Renderer::createCompiler()
{
    m_shaderCompiler = NRI::CreateSlangCompiler();
}

void Renderer::createGraphicsPipeline(bool forceCompile)
{
    NRI::PipelineDesc desc{};
    desc.forceCompile = forceCompile;
    desc.shaders.push_back({
        .stage = NRI::ShaderStage::Vertex,
        .entryPoint = "vertMain",
        .sourcePath = "../../shaders/shader.slang"
    });
    desc.shaders.push_back({
        .stage = NRI::ShaderStage::Fragment,
        .entryPoint = "fragMain",
        .sourcePath = "../../shaders/shader.slang"
    });
    m_graphicsPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
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

void Renderer::createColorResources()
{
    m_colorResource = m_device->createTexture(NRI::TextureDesc{
        .width = m_swapChainExtent.width,
        .height = m_swapChainExtent.height,
        .mipLevels = 1,
        .sampleCount = m_device->getMSAASampleCount(),
        .usage = NRI::TextureUsage::ColorAttachment
    });
}

void Renderer::createDepthResources()
{
    m_depthResource = m_device->createTexture(NRI::TextureDesc{
        .width = m_swapChainExtent.width,
        .height = m_swapChainExtent.height,
        .mipLevels = 1,
        .sampleCount = m_device->getMSAASampleCount(),
        .usage = NRI::TextureUsage::DepthStencilAttachment
    });
}

void Renderer::createTextureImage()
{
    int texWidth, texHeight, texChannels;
    stbi_uc* pixels = stbi_load(TEXTURE_PATH.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
    uint64_t imageSize = texWidth * texHeight * 4;
    mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(texWidth, texHeight)))) + 1;

    if (!pixels)
    {
        throw std::runtime_error("failed to load texture image!");
    }
    
    std::unique_ptr<NRI::Buffer> stagingBuffer = m_device->createBuffer(NRI::BufferDesc{
        .size = imageSize,
        .usage = NRI::BufferUsage::Staging
    });
    
    void* data = stagingBuffer->map(0, imageSize);
    memcpy(data, pixels, imageSize);
    stagingBuffer->unmap();

    stbi_image_free(pixels);
    
    m_textureResource = m_device->createTexture(NRI::TextureDesc
    {
        .width = static_cast<uint32_t>(texWidth),
        .height = static_cast<uint32_t>(texHeight),
        .mipLevels = mipLevels,
        .sampleCount = 1,
        .usage = NRI::TextureUsage::ShaderResource
    });

    std::unique_ptr<NRI::CommandBuffer> commandBuffer = beginSingleTimeCommands();
    m_textureResource->uploadFromBuffer(*commandBuffer, *stagingBuffer, texWidth, texHeight, mipLevels);
    endSingleTimeCommands(std::move(commandBuffer));
    m_resourceHeap->registerTexture(*m_textureResource);
    uniformData.imageHeapIndexOffset = m_resourceHeap->getImageHeapIndexOffset();
}

void Renderer::loadModel()
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, MODEL_PATH.c_str()))
    {
        throw std::runtime_error(warn + err);
    }

    std::unordered_map<Vertex, uint32_t> uniqueVertices{};

    for (const auto& shape : shapes)
    {
        for (const auto& index : shape.mesh.indices)
        {
            Vertex vertex{};

            vertex.pos = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]
            };

            vertex.texCoord = {
                attrib.texcoords[2 * index.texcoord_index + 0],
                1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
            };

            vertex.color = {1.0f, 1.0f, 1.0f};

#if 1
            auto [it, inserted] = uniqueVertices.insert({vertex, static_cast<uint32_t>(vertices.size())});
            if (inserted)
            {
                vertices.push_back(vertex);
            }

            indices.push_back(it->second);
#else
            vertices.push_back(vertex);
            indices.push_back(static_cast<uint32_t>(indices.size()));
#endif
        }
    }
}

void Renderer::createVertexBuffer()
{
    uint64_t bufferSize = sizeof(vertices[0]) * vertices.size();
    
    std::unique_ptr<NRI::Buffer> stagingBuffer = m_device->createBuffer(NRI::BufferDesc{
        .size = bufferSize,
        .usage = NRI::BufferUsage::Staging
    });
    
    m_vertexBuffer = m_device->createBuffer(NRI::BufferDesc
    {
        .size = bufferSize,
        .usage = NRI::BufferUsage::Storage
    });
    
    std::unique_ptr<NRI::CommandBuffer> commandCopyBuffer = beginSingleTimeCommands();
    m_vertexBuffer->uploadData(*commandCopyBuffer, *stagingBuffer, vertices.data());
    endSingleTimeCommands(std::move(commandCopyBuffer));
}

void Renderer::createIndexBuffer()
{
    uint64_t bufferSize = sizeof(indices[0]) * indices.size();
    
    std::unique_ptr<NRI::Buffer> stagingBuffer = m_device->createBuffer(NRI::BufferDesc{
        .size = bufferSize,
        .usage = NRI::BufferUsage::Staging
    });
    
    m_indexBuffer = m_device->createBuffer(NRI::BufferDesc
    {
        .size = bufferSize,
        .usage = NRI::BufferUsage::Index
    });
    
    std::unique_ptr<NRI::CommandBuffer> commandCopyBuffer = beginSingleTimeCommands();
    m_indexBuffer->uploadData(*commandCopyBuffer, *stagingBuffer, indices.data());
    endSingleTimeCommands(std::move(commandCopyBuffer));
}

void Renderer::createUniformBuffers()
{
    uint64_t bufferSize = sizeof(UniformBufferObject);
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

void Renderer::createDescriptorHeaps()
{
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
    
    /*m_modelDataBuffers.reserve(2);
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
    m_commandBuffers->transitionSwapchainLayout(*m_swapChain, imageIndex, NRI::TextureLayout::Undefined, NRI::TextureLayout::ColorAttachment);
    m_commandBuffers->transitionTextureLayout(*m_colorResource, NRI::TextureLayout::Undefined, NRI::TextureLayout::ColorAttachment);
    m_commandBuffers->transitionTextureLayout(*m_depthResource, NRI::TextureLayout::Undefined, NRI::TextureLayout::DepthAttachment);
    
    std::vector<NRI::RenderAttachDesc> colorAttachments;
    colorAttachments.push_back({
        .attachment = m_colorResource.get(),
        .resolveSwapchain = m_swapChain.get(),
        .resolveImageIndex = imageIndex,
        .loadOP = NRI::LoadOP::clear,
        .storeOP = NRI::StoreOP::store,
        .clearColor = { 0.0f, 0.0f, 0.0f, 1.0f }
    });
    
    NRI::RenderAttachDesc depthAttachment = 
    {
        .attachment = m_depthResource.get(),
        .loadOP = NRI::LoadOP::clear,
        .storeOP = NRI::StoreOP::dontCare,
        .clearDepth = { 1.0f, 0 }
    };
    
    NRI::RenderDesc desc = 
    {
        .renderArea = { m_swapChainExtent.width, m_swapChainExtent.height },
        .colorAttachments = colorAttachments,
        .depthAttachment = depthAttachment
    };
    m_commandBuffers->beginRendering(desc);
    
    // Viewport / scissor (counts and values are both dynamic).
    m_commandBuffers->setViewportWithCount(m_swapChainExtent);
    m_commandBuffers->setScissorWithCount(m_swapChainExtent);
    
    // Vertex input empty since we use vertex fetch BDA but still needs to be called empty
    m_commandBuffers->setVertexInput();
    
    // Input assembly.
    m_commandBuffers->setPrimitiveTopology(NRI::PrimitiveTopology::TriangleList);
    m_commandBuffers->setPrimitiveRestartEnable(false);
    
    // Rasterization (most of these come from VK_EXT_extended_dynamic_state_3).
    m_commandBuffers->setRasterizerDiscardEnable(false);
    m_commandBuffers->setPolygonMode(NRI::PolygonMode::Fill);
    m_commandBuffers->setCullMode(NRI::CullMode::Back);
    m_commandBuffers->setFrontFace(NRI::FrontFace::CounterClockWise);
    m_commandBuffers->setDepthBiasEnable(false);
    m_commandBuffers->setDepthClampEnable(false);//LineWidth maybe ?
    
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
    
    m_commandBuffers->bindPipeline(NRI::PipelineBindPoint::Graphics, *m_graphicsPipeline);
    m_commandBuffers->bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
    
    m_commandBuffers->bindIndexBuffer(*m_indexBuffer, 0);
    PushConstantBlock references{};
    // Pass pointer to the global matrix via a buffer device address
    references.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
    references.vertexReference = m_vertexBuffer->getDeviceAddress();
    m_commandBuffers->pushData(&references, sizeof(PushConstantBlock));
    
    m_commandBuffers->drawIndexed(static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
    
    m_commandBuffers->endRendering();
    
    std::vector<NRI::RenderAttachDesc> imguiColorAttachments;
    imguiColorAttachments.push_back({
        .attachmentSwapchain = m_swapChain.get(),
        .resolveImageIndex = imageIndex,
        .loadOP = NRI::LoadOP::load,
        .storeOP = NRI::StoreOP::store, 
    });
    
    NRI::RenderDesc imguiDesc = {
        .renderArea = { m_swapChainExtent.width, m_swapChainExtent.height },
        .colorAttachments = imguiColorAttachments,
    };
    m_commandBuffers->beginRendering(imguiDesc);
    m_commandBuffers->renderImGui();
    m_commandBuffers->endRendering();
    
    m_commandBuffers->transitionSwapchainLayout(*m_swapChain, imageIndex, NRI::TextureLayout::ColorAttachment, NRI::TextureLayout::Present);
    m_commandBuffers->end(frameIndex);
}

void Renderer::updateUniformBuffer(uint32_t currentImage)
{
    static auto startTime = std::chrono::high_resolution_clock::now();

    auto currentTime = std::chrono::high_resolution_clock::now();
    float time = std::chrono::duration<float>(currentTime - startTime).count();

    /*UniformBufferObject ubo{};*/
    uniformData.model = rotate(glm::mat4(1.0f), /*time **/ glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    uniformData.view = lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    uniformData.proj =
        glm::perspective(glm::radians(45.0f), static_cast<float>(m_swapChainExtent.width) / static_cast<float>(m_swapChainExtent.height), 0.1f, 10.0f);
    uniformData.proj[1][1] *= -1;
    uniformData.samplerIndex = selectedSampler;

    memcpy(m_uniformBuffersMapped[currentImage], &uniformData, sizeof(uniformData));
}

// with compute there might be a snyc issue idk
// according to gpt no async since compute and graphics same commandbuffer and executed in order
void Renderer::drawFrame()
{
    if (m_reloadShader)
    {
        m_reloadShader = false;
        createGraphicsPipeline(true);
        return;
    }
    
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("Hello, world!");  
    ImGui::End();
    
    uint32_t imageIndex = 0;
    
    if (m_swapChain->acquireNextImage(frameIndex, imageIndex) == NRI::FrameResult::ResizeRequired)
    {
        recreateSwapChain();
        return;
    }
    
    updateUniformBuffer(frameIndex);
    
    recordCommandBuffer(imageIndex);
    
    m_device->submitCommandBuffer(*m_commandBuffers, *m_swapChain, frameIndex, imageIndex);
    
    // Update and Render additional Platform Windows
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
    
    if (m_swapChain->present(frameIndex, imageIndex) == NRI::FrameResult::ResizeRequired || framebufferResized)
    {
        framebufferResized = false;
        recreateSwapChain();
    }
    frameIndex = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
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
