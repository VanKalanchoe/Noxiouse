#pragma once

#include <SDL3/SDL_video.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/hash.hpp>

#include "NRI/NRI.h"
#include "Utils/NOXWatcher.h"

const std::string MODEL_PATH = "../../models/viking_room.obj";
const std::string TEXTURE_PATH = "../../textures/viking_room.png";
constexpr int MAX_FRAMES_IN_FLIGHT = 2; // currently in swapchainvk and devicevk headers seperate combine them in the future

struct Vertex
{
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;
    
    bool operator==(const Vertex& other) const
    {
        return pos == other.pos && color == other.color && texCoord == other.texCoord;
    }
};

template <>
struct std::hash<Vertex>
{
    size_t operator()(Vertex const& vertex) const noexcept
    {
        return ((hash<glm::vec3>()(vertex.pos) ^ (hash<glm::vec3>()(vertex.color) << 1)) >> 1) ^ (hash<glm::vec2>()(vertex.texCoord) << 1);
    }
};

inline struct UniformBufferObject
{
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    uint32_t samplerIndex{0};
    uint32_t imageHeapIndexOffset{0};
} uniformData;

// 2 quads
/*const std::vector<Vertex> vertices = {
    {{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
    {{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
    {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
    {{-0.5f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},

    {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
    {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
    {{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}}
};

const std::vector<uint16_t> indices = {
    0, 1, 2, 2, 3, 0,
    4, 5, 6, 6, 7, 4
};*/

// This per-model data will be accessed via resource heaps
struct ModelData
{
    glm::vec4 pos;
    glm::vec4 color;
};

struct PushConstantBlock
{
    uint64_t matrixReference;
    uint64_t vertexReference;
};

inline int32_t selectedSampler{0};

inline std::vector<std::string> samplerNames{"Linear", "Nearest"};

class Renderer
{
public:
    Renderer(SDL_Window& window);
    void initImGui();
    ~Renderer();
    void drawFrame();
    void resizeWindow();

private:
    void initRenderer();
    void cleanupSwapChain();
    void recreateSwapChain();
 
    void createSwapChain();
    void createCompiler();
    void createGraphicsPipeline(bool forceCompile);
    void createComputePipeline();
    void createCommandPool();
    void createColorResources();
    void createDepthResources();
    void createTextureImage();
    void loadModel();
    void createVertexBuffer();
    void createIndexBuffer();
    void createUniformBuffers();
    void createDescriptorHeaps();
    std::unique_ptr<NRI::CommandBuffer> beginSingleTimeCommands();
    void endSingleTimeCommands(std::unique_ptr<NRI::CommandBuffer>&& commandBuffer);
    void createCommandBuffers();
    void recordCommandBuffer(uint32_t imageIndex);
    void updateUniformBuffer(uint32_t currentImage);
    std::vector<char> readFile(const std::string& filename);

private:
    SDL_Window* m_window = nullptr;
    std::unique_ptr<NRI::Device> m_device = nullptr;
    std::unique_ptr<NRI::Swapchain> m_swapChain = nullptr;
    NRI::Extent2D m_swapChainExtent;
    
    Utils::NOXWatcher m_fileWatcher;
    std::unique_ptr<NRI::ShaderCompiler> m_shaderCompiler = nullptr;
    std::unique_ptr<NRI::Pipeline> m_graphicsPipeline = nullptr;
    std::unique_ptr<NRI::Pipeline> m_computePipeline = nullptr;
    std::unique_ptr<NRI::CommandAllocator> m_commandAllocator = nullptr;
    std::unique_ptr<NRI::CommandBuffer> m_commandBuffers = nullptr;
    
    //decsriptor
    std::unique_ptr<NRI::DescriptorHeap> m_samplerHeap = nullptr;
    std::unique_ptr<NRI::DescriptorHeap> m_resourceHeap = nullptr;

    // Abstract storage tracking vectors
    std::vector<std::unique_ptr<NRI::Buffer>> m_modelDataBuffers;
    //decsriptor
    
    std::unique_ptr<NRI::Buffer> m_vertexBuffer = nullptr;
    std::unique_ptr<NRI::Buffer> m_indexBuffer = nullptr;
    
    std::vector<std::unique_ptr<NRI::Buffer>> m_uniformBuffers;
    std::vector<void*> m_uniformBuffersMapped;
    
    std::unique_ptr<NRI::Texture> m_colorResource = nullptr;
    std::unique_ptr<NRI::Texture> m_depthResource = nullptr;
    std::unique_ptr<NRI::Texture> m_textureResource = nullptr;

    uint32_t mipLevels;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    
    uint32_t frameIndex = 0;

    bool framebufferResized = false;
};
