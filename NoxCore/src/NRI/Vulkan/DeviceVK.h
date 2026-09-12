#pragma once
#include "VulkanCommon.h"

#ifdef NDEBUG
constexpr bool enableValidationLayers = false;
#else
constexpr bool enableValidationLayers = true;
#endif

#include "../Device.h"
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <sl_helpers_vk.h>

namespace NRI
{
    class MemoryAllocatorVK;
    class ShaderCompiler;

    class DeviceVK final : public Device
    {
    public:
        DeviceVK(Nox::Window& window);
        ~DeviceVK() override;
        void shutdown() override;

        bool isDeviceInit() const { return m_deviceInitialized; }
        
        MemoryAllocatorVK& getAllocator() { return *m_allocator; }
        
        vk::raii::Context& getContext() { return m_context; }
        vk::raii::Instance& getInstance() { return m_instance; }
        vk::raii::PhysicalDevice& getPhysicalDevice() { return m_physicalDevice; }
        vk::SampleCountFlagBits& getMSAASamples() { return m_msaaSamples; }
        uint32_t getMSAASampleCount() const override { return static_cast<uint32_t>(m_msaaSamples); }
        vk::raii::Device& getDevice() { return m_device; }
        bool isShaderObjectExtensionEnabled() const { return m_shaderObjectsEnabled; }
        uint32_t getQueueIndex() { return m_queueIndex; }
        vk::raii::Queue& getQueue() { return m_queue; }
        vk::raii::SurfaceKHR& getSurface() { return m_surface; }
        vk::Format& getDepthFormat() { return m_depthFormat; }
        vk::SurfaceFormatKHR& getSurfaceFormat() { return m_surfaceFormat; }
        
        // --- Streamline & DLSS ---
        bool isStreamlineInitialized() const override { return m_streamlineInitialized; }
        bool isDLSSSupported() const override { return m_slDLSSSupported; }
        bool isDLSSRayReconstructionSupported() const override { return m_slDLSS_RRSupported; }
        bool evaluateDLSS(const DLSSParams& params) override;
        void resetDLSSViewport() override;
        DLSSRenderExtent getDLSSOptimalRenderSize(UpscaleMode mode, Extent2D outputSize) override;
        PFN_vkQueuePresentKHR getStreamlinePresentFn() const { return m_slQueuePresentKHR; }
        
        // NRD
        bool initNRD(uint32_t width, uint32_t height) override;
        bool evaluateNRDShadows(const NRDShadowParams& params) override;
        bool evaluateNRDReflections(const NRDReflectionParams& params, NRDReflectionDenoiser denoiser) override;
        bool evaluateNRDDiffuse(const NRDDiffuseParams& params, NRDDiffuseDenoiser denoiser) override;
        void destroyNRD() override;
        bool isNRDInitialized() const override;
        
        // access to functions for other classses to use
        vk::raii::ImageView createImageView(vk::Image const& image, vk::Format format, vk::ImageAspectFlags aspectFlags, uint32_t mipLevels);
        uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties);
        void submitAndWait(CommandBuffer& cmdBuffer, uint32_t slotIndex) override;
        void submitCommandBuffer(CommandBuffer& cmdBuffer, Swapchain& swapchain, uint32_t frameIndex, uint32_t imageIndex) override;
        void waitIdle() override;
        void initImGui(Nox::Window& window) override;
        void shutdownImGui() override;
        void beginImGui() override;
        void endImGui() override;

        // Factory
        std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) override;
        std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc, ShaderCompiler& compiler) override;
        std::unique_ptr<CommandAllocator> createCommandAllocator() override;
        Nox::Ref<Texture2D> createTexture(const TextureDesc& desc) override;
        std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override;
        std::unique_ptr<DescriptorHeap> createDescriptorHeap(const DescriptorHeapDesc& desc) override;
        AccelerationStructureBuildSizes getAccelerationStructureBuildSizes(const AccelerationStructureBuildDesc& desc) override;
        std::unique_ptr<AccelerationStructure> createAccelerationStructure(const AccelerationStructureDesc& desc) override;
        
    private:
        void initVulkan(Nox::Window& window);
        void createInstance();
        void setupDebugMessenger();
        void createSurface(Nox::Window& window);
        bool isDeviceSuitable(vk::raii::PhysicalDevice const& physicalDevice);
        void pickPhysicalDevice();
        void createLogicalDevice();
        void initDeviceCapabilities();
        vk::SampleCountFlagBits getMaxUsableSampleCount();
        std::vector<char const*> getRequiredInstanceExtensions();
        static vk::Bool32 debugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT severity, vk::DebugUtilsMessageTypeFlagsEXT type, const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                        void* pUserData);
        vk::Format findSupportedFormat(const std::vector<vk::Format>& candidates, vk::ImageTiling tiling, vk::FormatFeatureFlags features);
        vk::Format findDepthFormat();
        vk::SurfaceFormatKHR chooseSurfaceFormat();
        vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats);
        void updateNRDCommonSettings(uint32_t frameIndex, bool resetHistory,
            const glm::mat4& proj, const glm::mat4& prevProj,
            const glm::mat4& view, const glm::mat4& prevView,
            const glm::vec2& mvScale);
        
    private:
        bool m_deviceInitialized = false;
        vk::raii::Context m_context;
        vk::raii::Instance m_instance = nullptr;
        vk::raii::DebugUtilsMessengerEXT m_debugMessenger = nullptr;
        vk::raii::PhysicalDevice m_physicalDevice = nullptr;
        vk::SampleCountFlagBits m_msaaSamples = vk::SampleCountFlagBits::e1;
        vk::raii::Device m_device = nullptr;
        bool m_shaderObjectsEnabled = false;
        uint32_t m_queueIndex = ~0;
        vk::raii::Queue m_queue = nullptr;
        vk::raii::SurfaceKHR m_surface = nullptr;
        vk::Format m_depthFormat = vk::Format::eUndefined;
        vk::SurfaceFormatKHR m_surfaceFormat = {};
        
        std::unique_ptr<MemoryAllocatorVK> m_allocator;
        
        vk::raii::DescriptorPool m_uiDescriptorPool = nullptr;
        
        const std::vector<char const*> validationLayers =
        {
            "VK_LAYER_KHRONOS_validation"
        };
        
        // Streamline state
        bool m_streamlineInitialized = false;
        bool m_slDLSSSupported = false;
        bool m_slDLSS_RRSupported = false;
        uint32_t m_slFrameIndex = 0;
        PFN_vkQueuePresentKHR m_slQueuePresentKHR = nullptr;
        // True once evaluateDLSS() has actually succeeded since the last resetDLSSViewport() - guards
        // resetDLSSViewport() from calling slFreeResources when there's no DLSSContext to free yet
        // (e.g. toggling DLSS off/on before a frame ever evaluated), which Streamline rejects.
        bool m_dlssContextEverEvaluated = false;

        // Workaround for https://github.com/NVIDIA-RTX/Streamline/issues/109 : Streamline's internal
        // Vulkan compute dispatch cannot read our tagged resources when the command buffer has only
        // ever used VK_EXT_descriptor_heap (vkCmdBindResourceHeapEXT/vkCmdBindSamplerHeapEXT) and never
        // a classic vkCmdBindDescriptorSets. Binding one dummy (empty) descriptor set right before
        // slEvaluateFeature works around it. NVIDIA has acknowledged this as a bug, no real fix yet.
        void ensureDummyDescriptorSet();
        vk::raii::DescriptorSetLayout m_dummyDescriptorSetLayout = nullptr;
        vk::raii::DescriptorPool m_dummyDescriptorPool = nullptr;
        vk::raii::PipelineLayout m_dummyPipelineLayout = nullptr;
        vk::DescriptorSet m_dummyDescriptorSet;
        bool m_dummyDescriptorSetReady = false;

        // NRD (Real-Time Denoisers)
        struct NRDContext;
        std::unique_ptr<NRDContext> m_nrdContext;
    };
}
