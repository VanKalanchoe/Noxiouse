#pragma once
#include <memory>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "Swapchain.h"
#include "Pipeline.h"
#include "CommandAllocator.h"
#include "CommandBuffer.h"
#include "Texture.h"
#include "Buffer.h"
#include "DescriptorHeap.h"
#include "AccelerationStructure.h"
#include "GpuProfiler.h"
#include "TimelineSemaphore.h"
#include "QueryPool.h"
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
        bool normalRoughnessPacked = false; // normal.rgb + linear roughness.a in one texture
        Texture* specularHitDistance = nullptr; // world-space primary surface -> reflection hit (optional)
        Texture* specularMotionVectors = nullptr; // mutually exclusive with hit distance; UV-space prev-current motion

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
        Texture* inNormalRoughness = nullptr;    // NRD-packed normal/roughness
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
        Texture* inNormalRoughness = nullptr;         // NRD-packed normal/roughness (R10G10B10A2_UNORM)
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

    enum class NRDDiffuseDenoiser : uint8_t
    {
        Off = 0,
        REBLUR = 1,
        RELAX = 2
    };

    struct NRDDiffuseParams
    {
        Texture* inDiffuseRadianceHitDist = nullptr;  // Raw 1-SPP diffuse GI radiance + hit distance (RGBA16_SFLOAT)
        Texture* inSpecularRadianceHitDist = nullptr; // diffuse-specular signals only (direct lighting, path tracing)
        Texture* inMotionVectors = nullptr;           // Screen-space Motion Vectors (RG16_SFLOAT)
        Texture* inNormalRoughness = nullptr;         // NRD-packed normal/roughness (R10G10B10A2_UNORM)
        Texture* inViewZ = nullptr;                   // Linear View-Z (R16_SFLOAT)
        Texture* outDenoisedDiffuse = nullptr;        // Denoised diffuse GI output (RGBA16_SFLOAT)
        Texture* outDenoisedSpecular = nullptr;       // diffuse-specular signals only

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
        virtual std::unique_ptr<CommandAllocator> createCommandAllocator(CommandBufferReset resetMode, QueueType queue = QueueType::Graphics) = 0;
        virtual std::unique_ptr<TimelineSemaphore> createTimelineSemaphore(uint64_t initialValue = 0) = 0;
        // Whether Transfer is its own queue family (a copy engine running beside graphics) or the graphics queue.
        virtual bool hasDedicatedTransferQueue() const = 0;
        virtual Nox::Ref<Texture2D> createTexture(const TextureDesc& desc) = 0;
        virtual std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) = 0;
        virtual std::unique_ptr<DescriptorHeap> createDescriptorHeap(const DescriptorHeapDesc& desc) = 0;
        virtual AccelerationStructureBuildSizes getAccelerationStructureBuildSizes(const AccelerationStructureBuildDesc& desc) = 0;
        virtual std::unique_ptr<AccelerationStructure> createAccelerationStructure(const AccelerationStructureDesc& desc) = 0;
        virtual std::unique_ptr<QueryPool> createQueryPool(const QueryPoolDesc& desc) = 0;
        virtual std::unique_ptr<GpuProfiler> createGpuProfiler(uint32_t framesInFlight) = 0;

        // Once per frame before any allocation of that frame; refreshes the memory budget.
        virtual void beginFrame(uint32_t frameNumber) = 0;
        virtual bool isMemoryBudgetSupported() const = 0;
        virtual void getMemoryStats(std::vector<MemoryHeapStats>& outHeaps) const = 0;
        // Estimated device memory of every live texture (§5.8.3 accounts for images through their descriptions).
        virtual uint64_t getTextureBytes() const = 0;

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
        virtual DLSSRenderExtent getDLSSOptimalRenderSize(UpscaleMode mode, Extent2D outputSize,
            bool rayReconstruction = false) { return {outputSize, 0.0f}; }
        
        // NRD (NVIDIA Real-Time Denoisers)
        virtual bool initNRD(uint32_t width, uint32_t height) { return false; }
        // NRD's integration layer requires SetCommonSettings()/NewFrame() to be called exactly once
        // every real frame, with frameIndex incrementing by exactly 1 each time, or its internal frame
        // counter desyncs and the NEXT SetCommonSettings() call hits an assert ("'frameIndex' must be
        // incremented by 1 on each frame"). Call this unconditionally once per frame whenever
        // isNRDInitialized() is true, regardless of whether any specific denoiser (shadows/reflections
        // /GI) actually has work to do this frame -- evaluateNRDShadows/Reflections/Diffuse each call
        // the same underlying tick internally too, but only when THEY run, which is no longer every
        // frame now that they're gated on their feature being active.
        virtual bool tickNRD(uint32_t frameIndex, bool resetHistory,
            const glm::mat4& view, const glm::mat4& proj,
            const glm::mat4& prevView, const glm::mat4& prevProj,
            const glm::vec2& motionVectorScale = glm::vec2(1.0f, 1.0f),
            const glm::vec2& cameraJitter = glm::vec2(0.0f),
            const glm::vec2& cameraJitterPrev = glm::vec2(0.0f)) { return false; }
        virtual bool evaluateNRDShadows(const struct NRDShadowParams& params) { return false; }
        virtual bool evaluateNRDReflections(const struct NRDReflectionParams& params, NRDReflectionDenoiser denoiser) { return false; }
        virtual bool evaluateNRDDiffuse(const struct NRDDiffuseParams& params, NRDDiffuseDenoiser denoiser) { return false; }
        // ReSTIR DI's direct lighting: de-modulated diffuse and specular (both inputs and outputs of the params), denoised
        // together by NRD's diffuse-specular denoiser under its own identifiers (its own temporal history, apart from GI's).
        virtual bool evaluateNRDDirectLighting(const struct NRDDiffuseParams& params, NRDDiffuseDenoiser denoiser) { return false; }
        // The path tracer's de-modulated diffuse and specular (both inputs and outputs), under its own identifiers.
        virtual bool evaluateNRDPathTracing(const struct NRDDiffuseParams& params, NRDDiffuseDenoiser denoiser) { return false; }
        // ReSTIR PT's combined radiance signal: one diffuse denoiser, its own identifiers.
        virtual bool evaluateNRDDiffusePT(const struct NRDDiffuseParams& params, NRDDiffuseDenoiser denoiser) { return false; }
        virtual void destroyNRD() {}
        virtual bool isNRDInitialized() const { return false; }
        
        virtual void shutdown() = 0;
        virtual uint32_t getMSAASampleCount() const = 0;
        virtual void submitAndWait(CommandBuffer& cmdBuffer, uint32_t slotIndex = 0) = 0;
        // One queue submission of the frame's command buffers, executed in span order (slot 0 of each buffer), waiting for
        // the swapchain image and signaling the frame slot's fence.
        // The frame: waits on the swapchain image and on timelineWaits (uploads it reads), signals presentation.
        virtual void submitCommandBuffers(std::span<CommandBuffer* const> cmdBuffers, Swapchain& swapchain, uint32_t frameIndex,
                                          uint32_t imageIndex, std::span<const TimelinePoint> timelineWaits = {}) = 0;
        // Work outside the frame: starts after every wait is reached, signals every point when done. Main thread only
        // (queues are externally synchronized).
        virtual void submit(QueueType queue, std::span<CommandBuffer* const> cmdBuffers, std::span<const TimelinePoint> waits,
                            std::span<const TimelinePoint> signals) = 0;
        virtual void waitIdle() = 0;
        virtual void initImGui(Nox::Window& window) = 0;
        virtual void shutdownImGui() = 0;
        virtual void beginImGui() = 0;
        virtual void endImGui() = 0;
        
        virtual uint32_t getShaderGroupHandleSize() const { return 32; }
        virtual uint32_t getShaderGroupBaseAlignment() const { return 64; }
        
        virtual ~Device() = default;
    };
}
