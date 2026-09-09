#pragma once
#include "Renderer2D.h"
#include "NoxCore/Core/Window.h"
#include "Mesh.h"
#include "PagedAllocator.h"

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
    struct MeshBLAS
    {
        std::unique_ptr<NRI::Buffer> storageBuffer;
        std::unique_ptr<NRI::AccelerationStructure> as;
        std::unique_ptr<NRI::Buffer> indexBuffer;
        uint64_t vertexBufferAddress = 0;
        uint32_t indexCount = 0;
    };

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
        uint32_t width = 1; // Default 1 for single click
        uint32_t height = 1; // Default 1 for single click
        bool active = false;
    };

    struct DrawMeshTasksIndirectCommand
    {
        uint32_t groupCountX;
        uint32_t groupCountY;
        uint32_t groupCountZ;
    };

    struct RenderPacket
    {
        shaderio::InstanceData instance;
        DrawMeshTasksIndirectCommand command;
        float distanceToCamera; // Only really needed for transparent objects now
        uint32_t blasId = UINT32_MAX;
    };

    // The Render Queues
    // The Render Queues (Matching Sascha Willems 1:1)
    inline std::vector<RenderPacket> m_opaqueQueue;
    inline std::vector<RenderPacket> m_opaqueDoubleSidedQueue;
    inline std::vector<RenderPacket> m_maskQueue;
    inline std::vector<RenderPacket> m_maskDoubleSidedQueue;
    inline std::vector<RenderPacket> m_unlitQueue;
    inline std::vector<RenderPacket> m_unlitDoubleSidedQueue;
    inline std::vector<RenderPacket> m_transparentQueue;
    inline std::vector<RenderPacket> m_transparentUnlitQueue;

    inline uint32_t m_opaqueCount = 0;
    inline uint32_t m_opaqueDoubleSidedCount = 0;
    inline uint32_t m_maskCount = 0;
    inline uint32_t m_maskDoubleSidedCount = 0;
    inline uint32_t m_unlitCount = 0;
    inline uint32_t m_unlitDoubleSidedCount = 0;
    inline uint32_t m_transparentCount = 0;
    inline uint32_t m_transparentUnlitCount = 0;

    class Renderer
    {
    public:
        Renderer(std::shared_ptr<Nox::Window> window, bool isEditor);
        ~Renderer();

        // Call this from EditorLayer during mouse hover/click
        // Single click (1x1)
        void setPickRequest(int32_t x, int32_t y, bool active = true)
        {
            m_pickRequest = {x, y, 1, 1, active};
        }

        // Box drag (WxH)
        void setBoxPickRequest(int32_t x, int32_t y, uint32_t width, uint32_t height)
        {
            m_pickRequest = {x, y, width, height, true};
        }

        // Call this in drawFrame() or EditorLayer to read the result
        int32_t getPickedEntityID();

        // Read back all unique entity IDs in the selected box area
        std::vector<int32_t> getPickedEntityIDs();

        void SetSelectedEntityID(const std::vector<int32_t>& entityIDs) { m_SelectedEntityIDs = entityIDs; }

        void drawFrame();
        void resizeWindow();
        void initImGui();
        void shutdownImGui();
        void beginImGui();
        void endImGui();

        void BeginScene(const Camera& camera, const glm::mat4& transform);
        void BeginScene(const EditorCamera& camera);
        void EndScene();
        void BuildBuffers();

        void DrawMesh(const glm::mat4& transform, Ref<Mesh> mesh, uint32_t submeshIndex, const MaterialComponent& material, int entityID, const std::vector<glm::mat4>* boneTransforms = nullptr);
        void DrawStaticMesh(const glm::mat4& transform, Ref<StaticMesh> staticMesh, const MaterialComponent& material, int entityID);
        void SubmitMesh(const glm::mat4& transform, MeshComponent& src, MaterialComponent& srcMat, int entityID, const std::vector<glm::mat4>* boneTransforms = nullptr);

        void SubmitLight(const glm::mat4& transform, const DirectionalLightComponent& light);
        void SubmitLight(const glm::mat4& transform, const PointLightComponent& light);
        void SubmitLight(const glm::mat4& transform, const SpotLightComponent& light);

        Texture2D* GetSceneResource() const { return m_sceneResource.get(); }

        void setVSync(bool enabled);
        void onViewportSizeChange(NRI::Extent2D size);
        bool getVSync() const { return m_vSync; }
        NRI::Extent2D getViewPortSize() const { return m_viewportSize; }
        Renderer2D* getRenderer2D() const { return m_renderer2D.get(); }
        void setFrozen(bool temp) { m_frozen = temp; }
        bool getFrozen() { return m_frozen; }
        void setFrozenDone(bool temp) { m_frozen = temp; }

        // EditorLayer Settings
        void setDebugMode(uint32_t mode) { m_debugMode = mode; }
        uint32_t getDebugMode() const { return m_debugMode; }
        void setTonemapMode(uint32_t mode) { m_tonemapMode = mode; }
        uint32_t getTonemapMode() const { return m_tonemapMode; }
        float getExposure() const { return m_exposure; }
        void setExposure(float exposure) { m_exposure = exposure; }
        float getGamma() const { return m_gamma; }
        void setGamma(float gamma) { m_gamma = gamma; }
        float getScaleIBLAmbient() const { return m_scaleIBLAmbient; }
        void setScaleIBLAmbient(float scale) { m_scaleIBLAmbient = scale; }
        // RayTracing
        void setRayTracingEnabled(bool enabled) { m_rayTracingEnabled = enabled; }
        bool getRayTracingEnabled() const { return m_rayTracingEnabled; }
        void setRayTracingShadows(bool enabled) { m_rayTracingShadows = enabled; }
        bool getRayTracingShadows() const { return m_rayTracingShadows; }
        void setRayTracingReflections(bool enabled) { m_rayTracingReflections = enabled; }
        bool getRayTracingReflections() const { return m_rayTracingReflections; }

        void setCameraJitterEnabled(bool enabled) { m_cameraJitterEnabled = enabled; }
        bool getCameraJitterEnabled() const { return m_cameraJitterEnabled; }
        glm::vec2 getCurrentJitter() const { return m_currentJitter; }

        Ref<Texture2D> UploadTexture(const TextureData& cpuData);
        Ref<Texture2D> createSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
        void initPBR();

        template <class T>
        void UploadBufferSlice(NRI::Buffer& dstBuffer, const T* data, uint32_t elementOffset, uint32_t elementCount);

        MeshHandle UploadMeshGeometry(const MeshData& data, bool isOpaque = true);

        static MeshHandle UploadMesh(const MeshData& data, bool isOpaque = true)
        {
            NOX_CORE_ASSERT(s_Instance, "Renderer instance does not exist!");

            return s_Instance->UploadMeshGeometry(data, isOpaque);
        }

        void UnloadMeshGeometry(const MeshHandle& handle);
        void updatePageTables(uint32_t currentImage);

        static void UnloadMesh(const MeshHandle& handle)
        {
            NOX_CORE_ASSERT(s_Instance, "Renderer instance does not exist!");
            s_Instance->UnloadMeshGeometry(handle);
        }
        
        void setDLSSEnabled(bool enabled) { m_dlssEnabled = enabled; }
        bool isDLSSEnabled() const { return m_dlssEnabled; }
        void setUpscaleMode(NRI::UpscaleMode mode) { m_dlssMode = mode; }
        NRI::UpscaleMode getUpscaleMode() const { return m_dlssMode; }

    private:
        void initRenderer();
        void cleanupSwapChain();
        void recreateSwapChain();
        void createSwapChain();
        void createCompiler();
        void watchShader(const std::filesystem::path& path, const std::string& pipelineKey, std::function<void()> reloadFn);

        void createUnlitPipeline(bool forceCompile);
        void createPresentPipeline(bool forceCompile);
        void createComputePipeline();
        void createSkyboxPipeline(bool forceCompile);
        void createCommandPool();

        void createSceneResources();
        void createEntityResources();
        void createDepthResources();

        // Visability
        void createVisibilityResources();
        void createVisibilityPipeline(bool forceCompile);
        // G-Buffer
        void createGBufferResources();
        void createGBufferPipeline(bool forceCompile = false);
        // PBR
        void createDeferredLightingPipeline(bool forceCompile = false);
        // Post Process
        void createPostProcessPipeline(bool forceCompile = false);

        void createTextureImage();
        void initGeometryBuffers();
        void markPageTablesDirty();
        void createUniformBuffers();
        void createInstanceBuffer(uint64_t bufferSize);
        void createIndirectBuffer(uint64_t bufferSize);
        void createSelectedEntityIDBuffers();
        void createDescriptorHeaps();
        std::unique_ptr<NRI::CommandBuffer> beginSingleTimeCommands();
        void endSingleTimeCommands(std::unique_ptr<NRI::CommandBuffer>&& commandBuffer);
        void createCommandBuffers();
        void recordCommandBuffer(uint32_t imageIndex);
        void updateEntityIDBuffer(uint32_t currentImage);
        void updateUniformBuffer(uint32_t currentImage);
        void updateInstanceAndIndirectBuffer(uint32_t currentImage);
        void processDeferredDeletions();
        void processDeferredMeshFrees();
        std::vector<char> readFile(const std::string& filename);
        void createPageTableBuffers(uint64_t elementCapacity);
        void createLightBuffer(uint64_t bufferSize);
        void updateLightBuffer(uint32_t currentImage);

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
        std::mutex m_reloadMutex;
        std::unordered_map<std::string, std::function<void()>> m_pendingReloads;
        std::unique_ptr<NRI::ShaderCompiler> m_shaderCompiler = nullptr;
        std::unique_ptr<NRI::Pipeline> m_unlitPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_presentPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_computePipeline = nullptr;

        // Visability
        std::unique_ptr<NRI::Pipeline> m_visibilityPipeline = nullptr;
        // G-Buffer
        std::unique_ptr<NRI::Pipeline> m_gbufferPipeline = nullptr;
        // PBR
        std::unique_ptr<NRI::Pipeline> m_deferredLightingPipeline = nullptr;
        // Post Process
        std::unique_ptr<NRI::Pipeline> m_postProcessPipeline = nullptr;

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

        // EditorLayer Settings
        uint32_t m_debugMode = 0;
        uint32_t m_tonemapMode = 4; // Default: KhronosPbrNeutral
        float m_exposure = 1.0f;
        float m_gamma = 2.2f;
        float m_scaleIBLAmbient = 1.0f;

        // Visability
        Ref<Texture2D> m_visibilityResource;

        // G-Buffer Render Targets (Decoupled Material Pass)
        Ref<Texture2D> m_gbufferAlbedo; // RGBA8_UNORM: RGB = BaseColor, A = Occlusion
        Ref<Texture2D> m_gbufferNormal; // R16G16B16A16_SFLOAT: RGB = World Normal
        Ref<Texture2D> m_gbufferMaterial; // RGBA8_UNORM: R = Roughness, G = Metallic, B = Workflow
        Ref<Texture2D> m_gbufferEmission; // R16G16B16A16_SFLOAT: RGB = Emissive
        Ref<Texture2D> m_gbufferVelocity; // R16G16_SFLOAT: Screen-space motion vectors

        // Post Process
        Ref<Texture2D> m_hdrSceneResource;

        Ref<Texture2D> m_whiteTexture;
        Ref<Texture2D> m_sceneResource;

        // Entiity ID + readback
        Ref<Texture2D> m_entityResource;
        std::vector<std::unique_ptr<NRI::Buffer>> m_pickerStagingBuffers;
        PickRequest m_pickRequest;
        std::vector<int32_t> m_SelectedEntityIDs;

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

        PagedBufferAllocator<shaderio::Vertex> m_vertexPages;
        PagedBufferAllocator<shaderio::MeshletDraw> m_meshletDrawPages;
        PagedBufferAllocator<shaderio::MeshletBounds> m_meshletBoundsPages;
        PagedBufferAllocator<uint32_t> m_meshletVertPages;
        PagedBufferAllocator<uint8_t> m_meshletTriPages;
        std::vector<MeshBLAS> m_meshBLASes;
        // --- Hardware Ray Tracing: Scene TLAS ---
        void updateSceneAccelerationStructure(uint32_t currentFrameIndex);
        void BuildSceneAccelerationStructure(uint32_t currentFrameIndex);

        bool m_rayTracingEnabled = true;
        bool m_rayTracingShadows = true;
        bool m_rayTracingReflections = true;

        std::unique_ptr<NRI::AccelerationStructure> m_sceneTLAS;
        std::unique_ptr<NRI::Buffer> m_tlasBuffer;
        std::unique_ptr<NRI::Buffer> m_tlasScratchBuffer;
        std::vector<std::unique_ptr<NRI::Buffer>> m_instanceLUTBuffers;
        std::vector<std::unique_ptr<NRI::Buffer>> m_rtInstanceBuffers;
        uint32_t m_sceneTLASCapacity = 0;
        uint32_t m_tlasHeapIndex = 0;
        uint32_t m_tlasHeapSlot = ~0u;
        bool m_hasTLASBuild = false;
        bool m_tlasNeedFullBuild = false;
        NRI::AccelerationStructureBuildDesc m_tlasBuildDesc{};

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
        bool m_pageTablesDirty[MAX_FRAMES_IN_FLIGHT] = {true, true, /* add 'true' for however many max frames you have */};
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

        // Outline
        std::vector<std::unique_ptr<NRI::Buffer>> m_selectedEntityIDBuffers;
        std::vector<void*> m_selectedEntityIDBuffersMapped;
        std::unique_ptr<NRI::Pipeline> m_outlinePipeline = nullptr;

        void createOutlinePipeline(bool forceCompile = false);

        // PBR stuff
        Ref<Texture2D> m_environmentCubemap;
        std::unique_ptr<NRI::Pipeline> m_skyboxPipeline = nullptr;
        Ref<Texture2D> m_irradianceCubemap;
        Ref<Texture2D> m_prefilteredEnvMap;
        uint32_t prefilterCubeMipLevels = 0;
        Ref<Texture2D> m_brdfLUT;

        // Lighting
        std::vector<shaderio::LightData> m_lightBufferObjects;
        std::vector<std::unique_ptr<NRI::Buffer>> m_lightBuffers;
        std::vector<void*> m_lightBuffersMapped;
        uint64_t m_LightBufferCapacity = 0;

        // Temporal & Motion Vector State
        glm::mat4 m_prevView = glm::mat4(1.0f);
        glm::mat4 m_prevNonJitteredProj = glm::mat4(1.0f);
        glm::mat4 m_currentNonJitteredProj = glm::mat4(1.0f);
        glm::mat4 m_currentView = glm::mat4(1.0f);
        uint64_t m_sceneFrameCounter = 0;
        uint64_t m_lastSceneFrameCounter = UINT64_MAX;
        bool m_isFirstFrame = true;

        bool m_cameraJitterEnabled = false;
        uint32_t m_jitterPhase = 0;
        glm::vec2 m_currentJitter = glm::vec2(0.0f);
        
        // DLSS Super Resolution
        Ref<Texture2D> m_dlssOutputResource;
        bool m_dlssEnabled = true;
        NRI::UpscaleMode m_dlssMode = NRI::UpscaleMode::DLAA;
        bool m_resetDLSS = true;

        // Camera Cache (Reverse-Z: no far clip)
        glm::vec3 m_cameraPosition{0.0f};
        glm::vec3 m_cameraUp{0.0f, 1.0f, 0.0f};
        glm::vec3 m_cameraRight{1.0f, 0.0f, 0.0f};
        glm::vec3 m_cameraForward{0.0f, 0.0f, -1.0f};
        float m_cameraNear = 0.1f;
        float m_cameraFOV = 30.0f;
    };
}
