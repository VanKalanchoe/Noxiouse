#include "DeviceVK.h"
#include <iostream>
#include <SDL3/SDL_vulkan.h>

#include "imgui_impl_vulkan.h"
#include "imgui_impl_sdl3.h"

#include "SwapchainVK.h"
#include "PipelineVK.h"
#include "CommandAllocatorVK.h"
#include "CommandBufferVK.h"
#include "TextureVK.h"
#include "BufferVK.h"
#include "DescriptorHeapVK.h"
#include "MemoryAllocatorVK.h"
#include "NoxCore/Core/core.h"

// Vulkan-Hpp loads extension functions through a dispatcher.
// This can create a dispatch table to cache function pointers,
// and can skip the loader when calling Vulkan functions.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace NRI
{
    static void check_vk_result(VkResult err)
    {
        if (err == VK_SUCCESS)
            return;
        fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
        if (err < 0)
            abort();
    }

    // Factory
    std::unique_ptr<Swapchain> DeviceVK::createSwapchain(const SwapchainDesc& desc)
    {
        return std::make_unique<SwapchainVK>(*this, desc);
    }

    std::unique_ptr<Pipeline> DeviceVK::createPipeline(const PipelineDesc& desc, ShaderCompiler& compiler)
    {
        return std::make_unique<PipelineVK>(*this, desc, compiler);
    }

    std::unique_ptr<CommandAllocator> DeviceVK::createCommandAllocator()
    {
        return std::make_unique<CommandAllocatorVK>(*this);
    }

    Nox::Ref<Texture2D> DeviceVK::createTexture(const TextureDesc& desc)
    {
        return Nox::CreateRef<TextureVK>(*this, desc);
    }

    std::unique_ptr<Buffer> DeviceVK::createBuffer(const BufferDesc& desc)
    {
        return std::make_unique<BufferVK>(*this, desc);
    }

    std::unique_ptr<DescriptorHeap> DeviceVK::createDescriptorHeap(const DescriptorHeapDesc& desc)
    {
        return std::make_unique<DescriptorHeapVK>(*this, desc);
    }

    std::vector<const char*> requiredDeviceExtension =
    {
        vk::KHRSwapchainExtensionName,
        
        // Descriptorheap + untyped Pointer
        vk::EXTDescriptorHeapExtensionName,
        vk::KHRMaintenance5ExtensionName,
        vk::KHRShaderUntypedPointersExtensionName,
        vk::KHRShaderNonSemanticInfoExtensionName,
        
        // Shader Objects
        vk::EXTShaderObjectExtensionName,
        vk::EXTExtendedDynamicState3ExtensionName,
        vk::EXTVertexInputDynamicStateExtensionName,
        
        // Task + Mesh Shader
        vk::EXTMeshShaderExtensionName,
    };

    DeviceVK::DeviceVK(Nox::Window& window)
    {
        initVulkan(window);
        m_allocator = std::make_unique<MemoryAllocatorVK>(*this);
        m_deviceInitialized = true;
    }

    DeviceVK::~DeviceVK()
    {
        // VMA must be destroyed BEFORE the logical device goes out of scope
        if (m_allocator)
        {
            m_allocator.reset();
        }
    }
    
    void DeviceVK::shutdown()
    {
        m_deviceInitialized = false;
    }

    void DeviceVK::initVulkan(Nox::Window& window)
    {
        createInstance();
        setupDebugMessenger();
        createSurface(window);
        pickPhysicalDevice();
        m_msaaSamples = getMaxUsableSampleCount();
        createLogicalDevice();
        initDeviceCapabilities();
    }

    void DeviceVK::createInstance()
    {
        constexpr vk::ApplicationInfo appInfo{
            .pApplicationName = "Hello Triangle",
            .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
            .pEngineName = "No Engine",
            .engineVersion = VK_MAKE_VERSION(1, 0, 0),
            .apiVersion = vk::ApiVersion14
        };

        // Get the required layers
        std::vector<char const*> requiredLayers;
        if (enableValidationLayers)
        {
            requiredLayers.assign(validationLayers.begin(), validationLayers.end());
        }

        // Check if the required layers are supported by the Vulkan implementation.
        auto layerProperties = m_context.enumerateInstanceLayerProperties();
        auto unsupportedLayerIt = std::ranges::find_if(requiredLayers,
                                                       [&layerProperties](auto const& requiredLayer)
                                                       {
                                                           return std::ranges::none_of(layerProperties,
                                                                                       [requiredLayer](auto const& layerProperty) { return strcmp(layerProperty.layerName, requiredLayer) == 0; });
                                                       });
        if (unsupportedLayerIt != requiredLayers.end())
        {
            throw std::runtime_error("Required layer not supported: " + std::string(*unsupportedLayerIt));
        }

        // Get the required extensions.
        auto requiredExtensions = getRequiredInstanceExtensions();

        // Check if the required extensions are supported by the Vulkan implementation.
        auto extensionProperties = m_context.enumerateInstanceExtensionProperties();
        auto unsupportedPropertyIt =
            std::ranges::find_if(requiredExtensions,
                                 [&extensionProperties](auto const& requiredExtension)
                                 {
                                     return std::ranges::none_of(extensionProperties,
                                                                 [requiredExtension](auto const& extensionProperty) { return strcmp(extensionProperty.extensionName, requiredExtension) == 0; });
                                 });
        if (unsupportedPropertyIt != requiredExtensions.end())
        {
            throw std::runtime_error("Required extension not supported: " + std::string(*unsupportedPropertyIt));
        }

        vk::InstanceCreateInfo createInfo
        {
            .pApplicationInfo = &appInfo,
            .enabledLayerCount = static_cast<uint32_t>(requiredLayers.size()),
            .ppEnabledLayerNames = requiredLayers.data(),
            .enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size()),
            .ppEnabledExtensionNames = requiredExtensions.data()
        };
        m_instance = vk::raii::Instance(m_context, createInfo);
        
        // Load function pointers for the Vulkan instance.
        {
            // It is complicated to explain how Vulkan is loaded, but as an oversimplification:
            // When calling a Vulkan function, the application does not call the function directly.
            // First it calls the Vulkan loader, and the loader then obtains this function from the device or instance.
            // Vulkan allows us to use vkGetInstanceProcAddr to cache the final function pointers, to call them directly
            // and skip the lookup work done by the loader.
            //
            // The process of creating a table of function pointers is a bit tedious, so it is handled automatically by
            // Vulkan-Hpp. Applications using the C API are encouraged to use a library like Volk for similar
            // functionality. Most third-party bindings for other languages also handle this automatically.
            //
            // The VULKAN_HPP_DEFAULT_DISPATCHER contains the function pointers for the Vulkan instance and device
            // functions. It is used by Vulkan-Hpp to call C API functions directly without going through the loader.
            VULKAN_HPP_DEFAULT_DISPATCHER.init(*m_instance, vkGetInstanceProcAddr);
        }
    }

    void DeviceVK::setupDebugMessenger()
    {
        if (!enableValidationLayers) return;

        vk::DebugUtilsMessageSeverityFlagsEXT severityFlags(vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eError);
        vk::DebugUtilsMessageTypeFlagsEXT messageTypeFlags(
            vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation);
        vk::DebugUtilsMessengerCreateInfoEXT debugUtilsMessengerCreateInfoEXT{
            .messageSeverity = severityFlags,
            .messageType = messageTypeFlags,
            .pfnUserCallback = &debugCallback
        };
        m_debugMessenger = m_instance.createDebugUtilsMessengerEXT(debugUtilsMessengerCreateInfoEXT);
    }

    void DeviceVK::createSurface(Nox::Window& window)
    {
        VkSurfaceKHR _surface;
        if (!SDL_Vulkan_CreateSurface(window.getHandle(), *m_instance, nullptr, &_surface))
        {
            throw std::runtime_error("failed to create window surface!");
        }
        m_surface = vk::raii::SurfaceKHR(m_instance, _surface);
    }

    bool DeviceVK::isDeviceSuitable(vk::raii::PhysicalDevice const& physicalDevice)
    {
        // Check if the physicalDevice supports the Vulkan 1.3 API version
        bool supportsVulkan1_3 = physicalDevice.getProperties().apiVersion >= vk::ApiVersion13;

        // Check if any of the queue families support graphics operations
        auto queueFamilies = physicalDevice.getQueueFamilyProperties();
        bool supportsGraphics = std::ranges::any_of(queueFamilies, [](auto const& qfp) { return !!(qfp.queueFlags & vk::QueueFlagBits::eGraphics); });

        // Check if all required physicalDevice extensions are available
        auto availableDeviceExtensions = physicalDevice.enumerateDeviceExtensionProperties();
        bool supportsAllRequiredExtensions =
            std::ranges::all_of(requiredDeviceExtension,
                                [&availableDeviceExtensions](auto const& requiredDeviceExtension)
                                {
                                    return std::ranges::any_of(availableDeviceExtensions,
                                                               [requiredDeviceExtension](auto const& availableDeviceExtension)
                                                               {
                                                                   return strcmp(availableDeviceExtension.extensionName, requiredDeviceExtension) == 0;
                                                               });
                                });

        // Check if the physicalDevice supports the required features
        auto features = physicalDevice.template getFeatures2<vk::PhysicalDeviceFeatures2,
                                                             vk::PhysicalDeviceVulkan11Features,
                                                             vk::PhysicalDeviceVulkan13Features,
                                                             /*vk::PhysicalDeviceShaderObjectFeaturesEXT,*/
                                                             vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
                                                             vk::PhysicalDeviceMeshShaderFeaturesEXT>();
        bool supportsRequiredFeatures = features.template get<vk::PhysicalDeviceFeatures2>().features.samplerAnisotropy &&
            features.template get<vk::PhysicalDeviceVulkan11Features>().shaderDrawParameters &&
            features.template get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering &&
            features.template get<vk::PhysicalDeviceVulkan13Features>().synchronization2 &&
            /*features.template get<vk::PhysicalDeviceShaderObjectFeaturesEXT>().shaderObject &&*/ // dont force to support both legacy pipeline and shaderobject
            features.template get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>().extendedDynamicState;
            features.template get<vk::PhysicalDeviceMeshShaderFeaturesEXT>().meshShader;
        // Return true if the physicalDevice meets all the criteria
        return supportsVulkan1_3 && supportsGraphics && supportsAllRequiredExtensions && supportsRequiredFeatures;
    }

    void DeviceVK::pickPhysicalDevice()
    {
        std::vector<vk::raii::PhysicalDevice> physicalDevices = m_instance.enumeratePhysicalDevices();
        auto const devIter = std::ranges::find_if(physicalDevices, [&](auto const& physicalDevice) { return isDeviceSuitable(physicalDevice); });
        if (devIter == physicalDevices.end())
        {
            throw std::runtime_error("failed to find a suitable GPU!");
        }
        m_physicalDevice = *devIter;
    }

    void DeviceVK::createLogicalDevice()
    {
        std::vector<vk::QueueFamilyProperties> queueFamilyProperties = m_physicalDevice.getQueueFamilyProperties();

        // get the first index into queueFamilyProperties which supports both graphics and present
        for (uint32_t qfpIndex = 0; qfpIndex < queueFamilyProperties.size(); qfpIndex++)
        {
            if ((queueFamilyProperties[qfpIndex].queueFlags & vk::QueueFlagBits::eGraphics) &&
                (queueFamilyProperties[qfpIndex].queueFlags & vk::QueueFlagBits::eCompute) &&
                m_physicalDevice.getSurfaceSupportKHR(qfpIndex, *m_surface))
            {
                // found a queue family that supports both graphics and present
                m_queueIndex = qfpIndex;
                break;
            }
        }
        if (m_queueIndex == ~0)
        {
            throw std::runtime_error("Could not find a queue for graphics and present -> terminating");
        }
        auto features = m_physicalDevice.template getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceShaderObjectFeaturesEXT>();
        m_shaderObjectsEnabled = features.template get<vk::PhysicalDeviceShaderObjectFeaturesEXT>().shaderObject;

        // query for Vulkan 1.3 features
        vk::StructureChain<vk::PhysicalDeviceFeatures2,
                           vk::PhysicalDeviceVulkan11Features,
                           vk::PhysicalDeviceVulkan12Features,
                           vk::PhysicalDeviceVulkan13Features,
                           vk::PhysicalDeviceShaderObjectFeaturesEXT,
                           vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
                           vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT,
                           vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT,
                           vk::PhysicalDeviceDescriptorHeapFeaturesEXT,
                           vk::PhysicalDeviceShaderUntypedPointersFeaturesKHR,
                           vk::PhysicalDeviceMaintenance5FeaturesKHR,
                           vk::PhysicalDeviceMeshShaderFeaturesEXT
            >
            featureChain = {
                {
                    .features = {
                        .sampleRateShading = true,
                        .wideLines = true,
                        .samplerAnisotropy = true,
                        .shaderInt64 = true,
                    }
                }, // vk::PhysicalDeviceFeatures2
                {.shaderDrawParameters = true}, // vk::PhysicalDeviceVulkan11Features
                {.shaderSampledImageArrayNonUniformIndexing = true, .shaderStorageBufferArrayNonUniformIndexing = true, .runtimeDescriptorArray = true, .scalarBlockLayout = true, .bufferDeviceAddress = true},
                // vk::PhysicalDeviceVulkan12Features
                {.shaderDemoteToHelperInvocation = true, .synchronization2 = true, .dynamicRendering = true}, // vk::PhysicalDeviceVulkan13Features
                {.shaderObject = m_shaderObjectsEnabled}, // vk::PhysicalDeviceVulkan14Features
                {.extendedDynamicState = true}, // vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT
                {
                    .extendedDynamicState3DepthClampEnable = vk::True,
                    .extendedDynamicState3PolygonMode = vk::True,
                    .extendedDynamicState3RasterizationSamples = vk::True,
                    .extendedDynamicState3SampleMask = vk::True,
                    .extendedDynamicState3AlphaToCoverageEnable = vk::True,
                    .extendedDynamicState3AlphaToOneEnable = vk::True,
                    .extendedDynamicState3LogicOpEnable = vk::True,
                    .extendedDynamicState3ColorBlendEnable = vk::True,
                    .extendedDynamicState3ColorBlendEquation = vk::True,
                    .extendedDynamicState3ColorWriteMask = vk::True
                },
                {.vertexInputDynamicState = true},
                {.descriptorHeap = true},
                {.shaderUntypedPointers = true},
                {.maintenance5 = true},
                {.taskShader = true, .meshShader = true}
            };

        // create a Device
        float queuePriority = 0.5f;
        vk::DeviceQueueCreateInfo deviceQueueCreateInfo{.queueFamilyIndex = m_queueIndex, .queueCount = 1, .pQueuePriorities = &queuePriority};
        vk::DeviceCreateInfo deviceCreateInfo{
            .pNext = &featureChain.get<vk::PhysicalDeviceFeatures2>(),
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &deviceQueueCreateInfo,
            .enabledExtensionCount = static_cast<uint32_t>(requiredDeviceExtension.size()),
            .ppEnabledExtensionNames = requiredDeviceExtension.data()
        };

        m_device = vk::raii::Device(m_physicalDevice, deviceCreateInfo);
        m_queue = vk::raii::Queue(m_device, m_queueIndex, 0);
        
        // Load device-level function pointers for Vulkan-Hpp wrappers.
        // Before we only loaded the function pointers for the instance-level functions.
        // After creating the logical device, we can load the function pointers that depend on it to skip the
        // loader. Loading the function pointers is a bit tedious, but is handled automatically by Vulkan-Hpp.
        // Other libraries like Volk provide similar functionality for the C API.
        VULKAN_HPP_DEFAULT_DISPATCHER.init(*m_instance, vkGetInstanceProcAddr, *m_device);
    }

    void DeviceVK::initDeviceCapabilities()
    {
        m_depthFormat = findDepthFormat();
        m_surfaceFormat = chooseSurfaceFormat();
    }

    vk::SampleCountFlagBits DeviceVK::getMaxUsableSampleCount()
    {
        vk::PhysicalDeviceProperties physicalDeviceProperties = m_physicalDevice.getProperties();

        vk::SampleCountFlags counts = physicalDeviceProperties.limits.framebufferColorSampleCounts & physicalDeviceProperties.limits.framebufferDepthSampleCounts;
        if (counts & vk::SampleCountFlagBits::e64)
        {
            return vk::SampleCountFlagBits::e64;
        }
        if (counts & vk::SampleCountFlagBits::e32)
        {
            return vk::SampleCountFlagBits::e32;
        }
        if (counts & vk::SampleCountFlagBits::e16)
        {
            return vk::SampleCountFlagBits::e16;
        }
        if (counts & vk::SampleCountFlagBits::e8)
        {
            return vk::SampleCountFlagBits::e8;
        }
        if (counts & vk::SampleCountFlagBits::e4)
        {
            return vk::SampleCountFlagBits::e4;
        }
        if (counts & vk::SampleCountFlagBits::e2)
        {
            return vk::SampleCountFlagBits::e2;
        }

        return vk::SampleCountFlagBits::e1;
    }

    std::vector<const char*> DeviceVK::getRequiredInstanceExtensions()
    {
        uint32_t sdlExtensionCount = 0;
        auto sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);

        std::vector extensions(sdlExtensions, sdlExtensions + sdlExtensionCount);
        if (enableValidationLayers)
        {
            extensions.push_back(vk::EXTDebugUtilsExtensionName);
        }

        return extensions;
    }

    VKAPI_ATTR vk::Bool32 VKAPI_CALL DeviceVK::debugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
                                                             vk::DebugUtilsMessageTypeFlagsEXT type,
                                                             const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                             void* pUserData)
    {
        std::cerr << "validation layer: type " << to_string(type) << " msg: " << pCallbackData->pMessage << std::endl;

        return vk::False;
    }

    //----------------------------------------------
    vk::raii::ImageView DeviceVK::createImageView(vk::Image const& image, vk::Format format, vk::ImageAspectFlags aspectFlags, uint32_t mipLevels)
    {
        vk::ImageViewCreateInfo viewInfo{
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {.aspectMask = aspectFlags, .baseMipLevel = 0, .levelCount = mipLevels, .baseArrayLayer = 0, .layerCount = 1}
        };
        return vk::raii::ImageView(m_device, viewInfo);
    }

    vk::Format DeviceVK::findSupportedFormat(const std::vector<vk::Format>& candidates, vk::ImageTiling tiling, vk::FormatFeatureFlags features)
    {
        for (const auto format : candidates)
        {
            vk::FormatProperties props = m_physicalDevice.getFormatProperties(format);
            if (((tiling == vk::ImageTiling::eLinear) && ((props.linearTilingFeatures & features) == features)) ||
                ((tiling == vk::ImageTiling::eOptimal) && ((props.optimalTilingFeatures & features) == features)))
            {
                return format;
            }
        }

        throw std::runtime_error("failed to find supported format!");
    }

    vk::Format DeviceVK::findDepthFormat()
    {
        return findSupportedFormat({vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint},
                                   vk::ImageTiling::eOptimal,
                                   vk::FormatFeatureFlagBits::eDepthStencilAttachment);
    }

    vk::SurfaceFormatKHR DeviceVK::chooseSurfaceFormat()
    {
        std::vector<vk::SurfaceFormatKHR> availableFormats = m_physicalDevice.getSurfaceFormatsKHR(*m_surface);
        return chooseSwapSurfaceFormat(availableFormats);
    }

    vk::SurfaceFormatKHR DeviceVK::chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats)
    {
        const auto formatIt = std::ranges::find_if(
            availableFormats,
            [](const auto& format) { return format.format == vk::Format::eB8G8R8A8Srgb && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear; });
        return formatIt != availableFormats.end() ? *formatIt : availableFormats[0];
    }

    uint32_t DeviceVK::findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties)
    {
        vk::PhysicalDeviceMemoryProperties memProperties = m_physicalDevice.getMemoryProperties();

        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
        {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
            {
                return i;
            }
        }

        throw std::runtime_error("failed to find suitable memory type!");
    }

    void DeviceVK::submitAndWait(CommandBuffer& cmdBuffer, uint32_t slotIndex)
    {
        auto* cmdBufferVK = static_cast<CommandBufferVK*>(&cmdBuffer);

        vk::raii::CommandBuffer& nativeCB = cmdBufferVK->getNativeBuffer(slotIndex);
        vk::SubmitInfo submitInfo{.commandBufferCount = 1, .pCommandBuffers = &*nativeCB};
        m_queue.submit(submitInfo, nullptr);
        m_queue.waitIdle();
    }

    void DeviceVK::submitCommandBuffer(CommandBuffer& cmdBuffer, Swapchain& swapchain, uint32_t frameIndex, uint32_t imageIndex)
    {
        auto* vkCmd = static_cast<CommandBufferVK*>(&cmdBuffer);
        auto* vkSwap = static_cast<SwapchainVK*>(&swapchain);

        vk::PipelineStageFlags waitDestinationStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput);
        const vk::SubmitInfo submitInfo{
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &*vkSwap->getPresentCompleteSemaphore(frameIndex),
            .pWaitDstStageMask = &waitDestinationStageMask,
            .commandBufferCount = 1,
            .pCommandBuffers = &*vkCmd->getNativeBuffer(frameIndex),
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &*vkSwap->getRenderFinishedSemaphore(imageIndex)
        };
        m_queue.submit(submitInfo, *vkSwap->getInFlightFence(frameIndex));
    }

    void DeviceVK::waitIdle()
    {
        m_device.waitIdle();
    }

    void DeviceVK::initImGui(Nox::Window& window)
    {
        const uint32_t maxCustomTextures = 1000;

#if IMGUI_VERSION_NUM >= 19280
        vk::DescriptorPoolSize poolSizes[] =
        {
            { vk::DescriptorType::eSampledImage, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + maxCustomTextures },
            { vk::DescriptorType::eSampler, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE + maxCustomTextures },
        };

        uint32_t maxSets = 0;
        for (vk::DescriptorPoolSize& poolSize : poolSizes)
            maxSets += poolSize.descriptorCount;

        vk::DescriptorPoolCreateInfo poolInfo
        {
            .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
            .maxSets = maxSets,
            .poolSizeCount = static_cast<uint32_t>(IM_COUNTOF(poolSizes)),
            .pPoolSizes = poolSizes
        };

        m_uiDescriptorPool = vk::raii::DescriptorPool(m_device, poolInfo);
        
#elif IMGUI_VERSION_NUM >= 19250
        // Backend uses a small number of descriptors per font atlas + as many as additional calls done to ImGui_ImplVulkan_AddTexture().
        #define IM_COUNTOF(_ARR)            ((int)(sizeof(_ARR) / sizeof(*(_ARR))))     // Size of a static C-style array. Don't use on pointers!
        vk::DescriptorPoolSize poolSizes[] =
        {
            { vk::DescriptorType::eCombinedImageSampler, maxCustomTextures }
        };

        uint32_t maxSets = 0;
        for (vk::DescriptorPoolSize& poolSize : poolSizes)
            maxSets += poolSize.descriptorCount;

        vk::DescriptorPoolCreateInfo poolInfo
        {
            .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
            .maxSets = maxSets,
            .poolSizeCount = static_cast<uint32_t>(IM_COUNTOF(poolSizes)),
            .pPoolSizes = poolSizes
        };

        m_uiDescriptorPool = vk::raii::DescriptorPool(m_device, poolInfo);
#endif
        
        static VkFormat imageFormats[] = {static_cast<VkFormat>(getSurfaceFormat().format)};

        // Setup Platform/Renderer backends
        ImGui_ImplSDL3_InitForVulkan(window.getHandle());
        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.ApiVersion = VK_API_VERSION_1_4;
        init_info.Instance = *m_instance;
        init_info.PhysicalDevice = *m_physicalDevice;
        init_info.Device = *m_device;
        init_info.QueueFamily = m_queueIndex;
        init_info.Queue = *m_queue;
        /*init_info.PipelineCache = g_PipelineCache;*/ // optional i guess 
        init_info.DescriptorPool = *m_uiDescriptorPool;
        init_info.MinImageCount = 2;
        init_info.ImageCount = 3; // from swapchain images count size
        /*init_info.Allocator = g_Allocator;*/ // optional i guess
        /*init_info.PipelineInfoMain.RenderPass = wd->RenderPass;*/
        init_info.UseDynamicRendering = true;
        init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = static_cast<VkStructureType>(vk::StructureType::ePipelineRenderingCreateInfo);
        init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = imageFormats;
        init_info.PipelineInfoMain.Subpass = 0;
        init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.CheckVkResultFn = check_vk_result;
        ImGui_ImplVulkan_Init(&init_info);
    }
    
    void DeviceVK::shutdownImGui()
    {
        ImGui_ImplVulkan_Shutdown();
    }
    
    void DeviceVK::beginImGui()
    {
        ImGui_ImplVulkan_NewFrame();
    }
    
    void DeviceVK::endImGui()
    {
        // Update and Render additional Platform Windows
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
    }
}
