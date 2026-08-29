#pragma once
#include "Renderer2D.h"
#include "NoxCore/Core/Window.h"
#include "Mesh.h"
#include "PagedAllocator.h"

namespace std
{
    template <>
    struct hash<shaderio::Vertex>
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
    struct DeferredBuffer
    {
        std::unique_ptr<NRI::Buffer> buffer;
        uint32_t framesRemaining = MAX_FRAMES_IN_FLIGHT;
    };

    struct DeferredMeshFree
    {
        MeshHandle handle;
        uint32_t framesRemaining = MAX_FRAMES_IN_FLIGHT;
    };
    
    struct PickRequest
    {
        int32_t x = -1;
        int32_t y = -1;
        bool active = false;
    };
    
    struct DrawMeshTasksIndirectCommand
    {
        uint32_t groupCountX;
        uint32_t groupCountY;
        uint32_t groupCountZ;
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
        
        void DrawMesh(const glm::mat4& transform, Ref<Mesh> mesh, uint32_t submeshIndex, const MaterialComponent& material, int entityID, const std::vector<glm::mat4>* boneTransforms = nullptr);
        void DrawStaticMesh(const glm::mat4& transform, Ref<StaticMesh> staticMesh, const MaterialComponent& material, int entityID);
        void SubmitMesh(const glm::mat4& transform, MeshComponent& src, MaterialComponent& srcMat, int entityID, const std::vector<glm::mat4>* boneTransforms = nullptr);
        
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
        void initPBR();

        template <class T>
        void UploadBufferSlice(NRI::Buffer& dstBuffer, const T* data, uint32_t elementOffset, uint32_t elementCount);
      
        MeshHandle UploadMeshGeometry(const MeshData& data);
        static MeshHandle UploadMesh(const MeshData& data)
        {
            NOX_CORE_ASSERT(s_Instance, "Renderer instance does not exist!");
            
            return s_Instance->UploadMeshGeometry(data);
        }
        void UnloadMeshGeometry(const MeshHandle& handle);
        void updatePageTables(uint32_t currentImage);

        static void UnloadMesh(const MeshHandle& handle)
        {
            NOX_CORE_ASSERT(s_Instance, "Renderer instance does not exist!");
            s_Instance->UnloadMeshGeometry(handle);
        }

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
        void initGeometryBuffers();
        void markPageTablesDirty();
        void createUniformBuffers();
        void createInstanceBuffer(uint64_t bufferSize);
        void createIndirectBuffer(uint64_t bufferSize);
        void createDescriptorHeaps();
        std::unique_ptr<NRI::CommandBuffer> beginSingleTimeCommands();
        void endSingleTimeCommands(std::unique_ptr<NRI::CommandBuffer>&& commandBuffer);
        void createCommandBuffers();
        void recordCommandBuffer(uint32_t imageIndex);
        void updateUniformBuffer(uint32_t currentImage);
        void updateInstanceAndIndirectBuffer(uint32_t currentImage);
        void processDeferredDeletions();
        void processDeferredMeshFrees();
        std::vector<char> readFile(const std::string& filename);
        void createPageTableBuffers(uint64_t elementCapacity);
        
    private:
        inline static Renderer* s_Instance = nullptr;
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
        
        // Animations
        std::vector<glm::mat4> m_boneMatrices;
        std::vector<std::unique_ptr<NRI::Buffer>> m_boneBuffers;
        std::vector<void*> m_boneBuffersMapped;
        uint64_t m_BoneBufferCapacity = 0;

        void updateBoneBuffer(uint32_t currentImage);
        void createBoneBuffer(uint64_t size);
        
        // Meshes
        // 2. Queue for sub-allocation range frees
        std::vector<DeferredMeshFree> m_deferredMeshFrees;

        // 3. Queue for whole NRI::Buffer destructions
        std::vector<DeferredBuffer> m_deferredBufferDeletions;
        
        PagedBufferAllocator<shaderio::Vertex>       m_vertexPages;
        PagedBufferAllocator<shaderio::MeshletDraw>  m_meshletDrawPages;
        PagedBufferAllocator<shaderio::MeshletBounds>m_meshletBoundsPages;
        PagedBufferAllocator<uint32_t>               m_meshletVertPages;
        PagedBufferAllocator<uint8_t>                m_meshletTriPages;
        std::vector<std::unique_ptr<NRI::Buffer>> m_vertexPageTableBuffers;
        std::vector<std::unique_ptr<NRI::Buffer>> m_meshletDrawPageTableBuffers;
        std::vector<std::unique_ptr<NRI::Buffer>> m_meshletBoundPageTableBuffers;
        std::vector<std::unique_ptr<NRI::Buffer>> m_meshletVertPageTableBuffers;
        std::vector<std::unique_ptr<NRI::Buffer>> m_meshletTriPageTableBuffers;
        std::vector<void*> m_vertexPageTableBuffersMapped;
        std::vector<void*> m_meshletDrawPageTableBuffersMapped;
        std::vector<void*> m_meshletBoundPageTableBuffersMapped;
        std::vector<void*> m_meshletVertPageTableBuffersMapped;
        std::vector<void*> m_meshletTriPageTableBuffersMapped;
        bool m_pageTablesDirty[MAX_FRAMES_IN_FLIGHT] = { true, true, /* add 'true' for however many max frames you have */ };
        uint64_t m_PageTableCapacity = 16; // Capacity in number of uint64_t elements
        
        uint64_t m_IndirectBufferCapacity = 0;
        std::vector<DrawMeshTasksIndirectCommand> m_drawMeshTasksIndirectCommands;
        std::vector<std::unique_ptr<NRI::Buffer>> m_indirectBuffers;
        std::vector<void*> m_indirectBuffersMapped;
        
        uint64_t m_InstanceBufferCapacity = 0;
        std::vector<shaderio::InstanceData> m_instanceBufferObjects;
        std::vector<std::unique_ptr<NRI::Buffer>> m_instanceBuffers;
        std::vector<void*> m_instanceBuffersMapped;
        
        uint32_t frameIndex = 0;

        bool framebufferResized = false;
        
        // PBR stuff
        Ref<Texture2D> m_environmentCubemap;
    };
}
