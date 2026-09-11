#pragma once
#include <memory>

#include <glm/glm.hpp>

#include "Swapchain.h"
#include "Pipeline.h"
#include "CommandAllocator.h"
#include "CommandBuffer.h"
#include "Texture.h"
#include "Buffer.h"
#include "DescriptorHeap.h"
#include "AccelerationStructure.h"
#include "ShaderCompiler.h"
#include "NoxCore/Core/Window.h"

namespace NRI
{
    enum class GraphicsAPI : uint8_t
    {
        Vulkan,
        Metal
    };
    
    enum class UpscaleMode : uint8_t
    {
        Off,
        DLAA,
        Quality,
        Balanced,
        Performance,
        UltraPerformance
    };

    struct DLSSParams
    {
        Texture* inputColor = nullptr;
        Texture* outputColor = nullptr;
        Texture* depth = nullptr;
        Texture* motionVectors = nullptr;
        CommandBuffer* commandBuffer = nullptr;
        
        // Ray Reconstruction (DLSS 3.5 Denoiser)
        bool rayReconstruction = false;
        Texture* albedo = nullptr;
        Texture* specularAlbedo = nullptr;
        Texture* normal = nullptr;
        Texture* roughness = nullptr;

        glm::mat4 nonJitteredProj{ 1.0f };
        glm::mat4 view{ 1.0f };
        glm::mat4 prevNonJitteredProj{ 1.0f };
        glm::mat4 prevView{ 1.0f };

        glm::vec2 jitterOffset{ 0.0f };
        glm::vec3 cameraPos{ 0.0f };
        glm::vec3 cameraUp{ 0.0f, 1.0f, 0.0f };
        glm::vec3 cameraRight{ 1.0f, 0.0f, 0.0f };
        glm::vec3 cameraFwd{ 0.0f, 0.0f, -1.0f };

        float cameraNear = 0.1f;
        float cameraFovRad = 0.523599f;
        bool reset = false;
        UpscaleMode mode = UpscaleMode::DLAA;
    };

    struct NRDShadowParams
    {
        Texture* inShadowData = nullptr;         // Raw 1-SPP shadow mask (R16G16: x = visibility, y = hitDist)
        Texture* inMotionVectors = nullptr;      // Screen-space / UV-space Motion Vectors
        Texture* inNormalRoughness = nullptr;    // World-space Normal (RGB) and Roughness (A)
        Texture* inViewZ = nullptr;              // Linear View-Z (R16_SFLOAT or R32_SFLOAT)
        Texture* outDenoisedShadow = nullptr;    // Denoised Shadow output (R16G16 or R8)

        CommandBuffer* commandBuffer = nullptr;

        glm::mat4 view{ 1.0f };
        glm::mat4 proj{ 1.0f };
        glm::mat4 prevView{ 1.0f };
        glm::mat4 prevProj{ 1.0f };

        float lightDirection[3] = { 0.0f, 0.0f, 0.0f }; // Direction to primary light source
        glm::vec2 motionVectorScale{ 1.0f, 1.0f };
        uint32_t frameIndex = 0;
        bool resetHistory = false;
    };

    enum class NRDReflectionDenoiser : uint8_t
    {
        Off = 0,
        REBLUR = 1,
        RELAX = 2
    };

    struct NRDReflectionParams
    {
        Texture* inSpecularRadianceHitDist = nullptr; // Raw 1-SPP reflection radiance + hit distance (RGBA16_SFLOAT)
        Texture* inMotionVectors = nullptr;           // Screen-space Motion Vectors (RG16_SFLOAT)
        Texture* inNormalRoughness = nullptr;         // World-space Normal (RGB) and Roughness (A) (R10G10B10A2_UNORM)
        Texture* inViewZ = nullptr;                   // Linear View-Z (R16_SFLOAT)
        Texture* outDenoisedSpecular = nullptr;       // Denoised reflection output (RGBA16_SFLOAT)

        CommandBuffer* commandBuffer = nullptr;

        glm::mat4 view{ 1.0f };
        glm::mat4 proj{ 1.0f };
        glm::mat4 prevView{ 1.0f };
        glm::mat4 prevProj{ 1.0f };

        glm::vec2 motionVectorScale{ 1.0f, 1.0f };
        uint32_t frameIndex = 0;
        bool resetHistory = false;
    };
    
    class Device
    {
    public:
        // Factory function: The ONLY place that knows about specific backends
        static std::unique_ptr<Device> create(GraphicsAPI api, Nox::Window& window);
        
        virtual std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) = 0;
        virtual std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc, ShaderCompiler& compiler) = 0;
        virtual std::unique_ptr<CommandAllocator> createCommandAllocator() = 0;
        virtual Nox::Ref<Texture2D> createTexture(const TextureDesc& desc) = 0;
        virtual std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) = 0;
        virtual std::unique_ptr<DescriptorHeap> createDescriptorHeap(const DescriptorHeapDesc& desc) = 0;
        virtual AccelerationStructureBuildSizes getAccelerationStructureBuildSizes(const AccelerationStructureBuildDesc& desc) = 0;
        virtual std::unique_ptr<AccelerationStructure> createAccelerationStructure(const AccelerationStructureDesc& desc) = 0;
        
        virtual bool evaluateDLSS(const DLSSParams& params) { return false; }
        virtual bool isDLSSSupported() const { return false; }
        virtual bool isDLSSRayReconstructionSupported() const { return false; }
        virtual bool isStreamlineInitialized() const { return false; }
        // Must be called whenever the DLSS input/output resolution changes (e.g. viewport resize).
        // NGX's internal DLSSContext is created at a fixed resolution on first evaluate and is NOT
        // resized just by tagging differently-sized resources later - it must be explicitly freed
        // so it gets recreated at the new size on the next evaluate.
        virtual void resetDLSSViewport() {}

        struct DLSSRenderExtent
        {
            Extent2D size{};
            float optimalSharpness = 0.0f;
        };
        // For a given upscale mode and desired output size, returns the resolution the scene should
        // actually be rendered at (DLSS then upscales render size -> outputSize). UpscaleMode::Off or
        // a backend without DLSS just returns outputSize back unchanged (1:1, no scaling).
        virtual DLSSRenderExtent getDLSSOptimalRenderSize(UpscaleMode mode, Extent2D outputSize) { return {outputSize, 0.0f}; }
        
        // NRD (NVIDIA Real-Time Denoisers)
        virtual bool initNRD(uint32_t width, uint32_t height) { return false; }
        virtual bool evaluateNRDShadows(const struct NRDShadowParams& params) { return false; }
        virtual bool evaluateNRDReflections(const struct NRDReflectionParams& params, NRDReflectionDenoiser denoiser) { return false; }
        virtual void destroyNRD() {}
        virtual bool isNRDInitialized() const { return false; }
        
        virtual void shutdown() = 0;
        virtual uint32_t getMSAASampleCount() const = 0;
        virtual void submitAndWait(CommandBuffer& cmdBuffer, uint32_t slotIndex = 0) = 0;
        virtual void submitCommandBuffer(CommandBuffer& cmdBuffer, Swapchain& swapchain, uint32_t frameIndex, uint32_t imageIndex) = 0;
        virtual void waitIdle() = 0;
        virtual void initImGui(Nox::Window& window) = 0;
        virtual void shutdownImGui() = 0;
        virtual void beginImGui() = 0;
        virtual void endImGui() = 0;
        
        virtual ~Device() = default;
    };
}
