#pragma once

#include "Renderer2D.h"
#include "NoxCore/Core/Window.h"

namespace std
{
    template <>
    struct std::hash<shaderio::Vertex>
    {
        size_t operator()(shaderio::Vertex const& vertex) const noexcept
        {
            return (hash<glm::vec3>()(vertex.pos) >> 1) ^ (hash<glm::vec2>()(vertex.texCoord) << 1);
        }
    };
}

const std::string MODEL_PATH = "assets/models/viking_room.obj";
const std::string MODEL_PATH_GLTF = "assets/models/viking_room.glb";
const std::string MODEL_PATH_GLTF_STANDFORD = "assets/models/stanford_bunny/stanford_bunny.gltf";
const std::string MODEL_PATH_FOX_GLTF = "assets/models/Fox/Fox.gltf";
const std::string MODEL_PATH_GLTF_BIOHAZAR_Crate = "assets/models/Biohazard_Crate/Model/Untitled.gltf";
const std::string TEXTURE_PATH_FOX = "assets/models/Fox/Texture.png";
const std::string TEXTURE_PATH = "assets/textures/viking_room.ktx2";

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
struct ModelData // not used was a example for descriptor heap buffer 
//but im using bda need to remove this later with createDescriptorHeaps commented out thing
{
    glm::vec4 pos;
    glm::vec4 color;
};

struct PushConstantBlock
{
    uint64_t matrixReference;
    uint64_t vertexReference;
    uint64_t instanceReference;
};

inline int32_t selectedSampler{0};

inline std::vector<std::string> samplerNames{"Linear", "Nearest"};

namespace Nox
{
    struct PickRequest
    {
        int32_t x = -1;
        int32_t y = -1;
        bool active = false;
    };
    
    struct ModelHandle
    {
        uint32_t firstMeshlet = 0;
        uint32_t meshletCount = 0;
    };
    
    class Renderer
    {
    public:
        Renderer(std::shared_ptr<Nox::Window> window, bool isEditor);
        ~Renderer();
        
        // Call this from EditorLayer during mouse hover/click
        void setPickRequest(int32_t x, int32_t y, bool active = true)
        {
            m_pickRequest.x = x;
            m_pickRequest.y = y;
            m_pickRequest.active = active;
        }

        // Call this in drawFrame() or EditorLayer to read the result
        int32_t getPickedEntityID();
        
        void drawFrame();
        void resizeWindow();
        void initImGui();
        void shutdownImGui();
        void beginImGui();
        void endImGui();
        
        void BeginScene(const Camera& camera, const glm::mat4& transform);
        void BeginScene(const EditorCamera& camera);
        void EndScene();
        
        Texture2D* GetSceneResource() const { return m_sceneResource.get(); }
        
        void setVSync(bool enabled);
        void onViewportSizeChange(NRI::Extent2D size);
        bool getVSync() const { return m_vSync; }
        NRI::Extent2D getViewPortSize() const { return m_viewportSize; }
        Renderer2D* getRenderer2D() const { return m_renderer2D.get(); }
        void setFrozen(bool temp) { m_frozen = temp; }
        bool getFrozen() { return m_frozen; }
        void setFrozenDone(bool temp) { m_frozen = temp; }
        
        Ref<Texture2D> UploadTexture(const TextureData& cpuData);
        Ref<Texture2D> createSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    private:
        void initRenderer();
        void cleanupSwapChain();
        void recreateSwapChain();

        void createSwapChain();
        void createCompiler();
        void createGraphicsPipeline(bool forceCompile);
        void createPresentPipeline(bool forceCompile);
        void createComputePipeline();
        void createCommandPool();
        void createSceneResources();
        void createColorResources();
        void createEntityResources();
        void createDepthResources();
        void createTextureImage();
        ModelHandle loadModel(const std::string& path);
        void createMeshletBuffers();
        void createUniformBuffers();
        void createInstanceBuffer();
        void createIndirectBuffer();
        void createDescriptorHeaps();
        std::unique_ptr<NRI::CommandBuffer> beginSingleTimeCommands();
        void endSingleTimeCommands(std::unique_ptr<NRI::CommandBuffer>&& commandBuffer);
        void createCommandBuffers();
        void recordCommandBuffer(uint32_t imageIndex);
        void updateUniformBuffer(uint32_t currentImage);
        std::vector<char> readFile(const std::string& filename);

    private:
        std::unique_ptr<Renderer2D> m_renderer2D;
        std::shared_ptr<Nox::Window> m_window;
        std::unique_ptr<NRI::Device> m_device = nullptr;
        std::unique_ptr<NRI::Swapchain> m_swapChain = nullptr;
        NRI::Extent2D m_swapChainExtent{640, 480};
        NRI::Extent2D m_viewportSize{640, 480};
        bool m_vSync = false;
        bool m_isEditor = false;

        Utils::NOXWatcher m_fileWatcher;
        std::unique_ptr<NRI::ShaderCompiler> m_shaderCompiler = nullptr;
        std::unique_ptr<NRI::Pipeline> m_graphicsPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_presentPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_computePipeline = nullptr;
        
        std::unique_ptr<NRI::CommandAllocator> m_commandAllocator = nullptr;
        std::unique_ptr<NRI::CommandBuffer> m_commandBuffers = nullptr;

        //decsriptor
        std::unique_ptr<NRI::DescriptorHeap> m_samplerHeap = nullptr;
        std::unique_ptr<NRI::DescriptorHeap> m_resourceHeap = nullptr;

        // Scene Data + Frustum Freeze
        std::vector<std::unique_ptr<NRI::Buffer>> m_uniformBuffers;
        std::vector<void*> m_uniformBuffersMapped;
        shaderio::UniformBufferObject uniformData = {};
        shaderio::UniformBufferObject frozenUniformData = {};
        bool m_frozen = false;
        bool m_frozenDone = false;

        Ref<Texture2D> m_whiteTexture;
        Ref<Texture2D> m_sceneResource;
        Ref<Texture2D> m_colorResource;
        
        // Entiity ID + readback
        Ref<Texture2D> m_entityResource;
        Ref<Texture2D> m_entityResolveResource;
        std::vector<std::unique_ptr<NRI::Buffer>> m_pickerStagingBuffers;
        PickRequest m_pickRequest;
        
        // Textures
        Ref<Texture2D> m_depthResource;
        Ref<Texture2D> m_textureResource;
        Ref<Texture2D> m_textureResource2;
        Ref<Texture2D> m_textureResource3;
        uint32_t mipLevels;

        std::unique_ptr<NRI::Buffer> m_indirectBuffer;
        
        std::unique_ptr<NRI::Buffer> m_instanceBuffer;
        
        std::vector<shaderio::Vertex> m_vertices;
        std::unique_ptr<NRI::Buffer> m_verticesBuffer;
        
        std::vector<shaderio::MeshletBounds> m_meshletBounds;
        std::unique_ptr<NRI::Buffer> m_meshletBoundsBuffer;
        
        std::vector<shaderio::MeshletDraw> m_meshletDraws;
        std::unique_ptr<NRI::Buffer> m_meshletDrawsBuffer;
        
        std::vector<uint32_t> m_meshletVertices;
        std::unique_ptr<NRI::Buffer> m_meshletVerticesBuffer;
        
        std::vector<uint8_t> m_meshletTriangles;
        std::unique_ptr<NRI::Buffer> m_meshletTrianglesBuffer;
        
        uint32_t frameIndex = 0;

        bool framebufferResized = false;
    };
}
