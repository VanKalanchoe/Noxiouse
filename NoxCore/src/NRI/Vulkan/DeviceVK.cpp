#include "DeviceVK.h"
#include <iostream>
#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL_loadso.h>

#include "imgui_impl_vulkan.h"
#include "imgui_impl_sdl3.h"

#include "SwapchainVK.h"
#include "PipelineVK.h"
#include "CommandAllocatorVK.h"
#include "CommandBufferVK.h"
#include "TextureVK.h"
#include "BufferVK.h"
#include "DescriptorHeapVK.h"
#include "AccelerationStructureVK.h"
#include "MemoryAllocatorVK.h"
#include "NoxCore/Core/core.h"
#include "NoxCore/Core/Log.h"

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

    static sl::DLSSMode ToSLDLSSMode(UpscaleMode mode)
    {
        switch (mode)
        {
        case UpscaleMode::Off: return sl::DLSSMode::eOff;
        case UpscaleMode::DLAA: return sl::DLSSMode::eDLAA;
        case UpscaleMode::Quality: return sl::DLSSMode::eMaxQuality;
        case UpscaleMode::Balanced: return sl::DLSSMode::eBalanced;
        case UpscaleMode::Performance: return sl::DLSSMode::eMaxPerformance;
        case UpscaleMode::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
        }
        return sl::DLSSMode::eOff;
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

    AccelerationStructureBuildSizes DeviceVK::getAccelerationStructureBuildSizes(const AccelerationStructureBuildDesc& desc)
    {
        vk::BuildAccelerationStructureFlagsKHR vkFlags{};
        if (desc.flags & AccelerationStructureBuildFlags::AllowUpdate)
            vkFlags |= vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate;
        if (desc.flags & AccelerationStructureBuildFlags::PreferFastTrace)
            vkFlags |= vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
        if (desc.flags & AccelerationStructureBuildFlags::PreferFastBuild)
            vkFlags |= vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
        if (desc.flags & AccelerationStructureBuildFlags::LowMemory)
            vkFlags |= vk::BuildAccelerationStructureFlagBitsKHR::eLowMemory;

        if (desc.type == AccelerationStructureType::BottomLevel)
        {
            std::vector<vk::AccelerationStructureGeometryKHR> geometries;
            geometries.reserve(desc.triangles.size());
            std::vector<uint32_t> maxPrimitiveCounts;
            maxPrimitiveCounts.reserve(desc.triangles.size());

            for (const auto& tri : desc.triangles)
            {
                vk::AccelerationStructureGeometryTrianglesDataKHR trianglesData{
                    .vertexFormat = vk::Format::eR32G32B32Sfloat,
                    .vertexData = tri.vertexBufferAddress,
                    .vertexStride = tri.vertexStride,
                    .maxVertex = tri.maxVertex,
                    .indexType = vk::IndexType::eUint32,
                    .indexData = tri.indexBufferAddress
                };

                vk::AccelerationStructureGeometryKHR geometry{
                    .geometryType = vk::GeometryTypeKHR::eTriangles,
                    .geometry = trianglesData,
                    .flags = tri.isOpaque ? vk::GeometryFlagBitsKHR::eOpaque : vk::GeometryFlagsKHR{}
                };

                geometries.push_back(geometry);
                maxPrimitiveCounts.push_back(tri.primitiveCount);
            }

            vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{
                .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
                .flags = vkFlags,
                .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
                .geometryCount = static_cast<uint32_t>(geometries.size()),
                .pGeometries = geometries.data()
            };

            vk::AccelerationStructureBuildSizesInfoKHR vkSizes = m_device.getAccelerationStructureBuildSizesKHR(
                vk::AccelerationStructureBuildTypeKHR::eDevice,
                buildInfo,
                maxPrimitiveCounts
            );

            return AccelerationStructureBuildSizes{
                .accelerationStructureSize = vkSizes.accelerationStructureSize,
                .buildScratchSize = vkSizes.buildScratchSize,
                .updateScratchSize = vkSizes.updateScratchSize
            };
        }
        else // TopLevel
        {
            vk::AccelerationStructureGeometryInstancesDataKHR instancesData{
                .arrayOfPointers = vk::False,
                .data = desc.instances.instanceBufferAddress
            };

            vk::AccelerationStructureGeometryKHR geometry{
                .geometryType = vk::GeometryTypeKHR::eInstances,
                .geometry = instancesData
            };

            vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{
                .type = vk::AccelerationStructureTypeKHR::eTopLevel,
                .flags = vkFlags,
                .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
                .geometryCount = 1,
                .pGeometries = &geometry
            };

            vk::AccelerationStructureBuildSizesInfoKHR vkSizes = m_device.getAccelerationStructureBuildSizesKHR(
                vk::AccelerationStructureBuildTypeKHR::eDevice,
                buildInfo,
                {desc.instances.instanceCount}
            );

            return AccelerationStructureBuildSizes{
                .accelerationStructureSize = vkSizes.accelerationStructureSize,
                .buildScratchSize = vkSizes.buildScratchSize,
                .updateScratchSize = vkSizes.updateScratchSize
            };
        }
    }

    std::unique_ptr<AccelerationStructure> DeviceVK::createAccelerationStructure(const AccelerationStructureDesc& desc)
    {
        return std::make_unique<AccelerationStructureVK>(*this, desc);
    }

    std::vector<const char*> requiredDeviceExtension =
    {
        vk::KHRSwapchainExtensionName,
        vk::KHRUnifiedImageLayoutsExtensionName,
        vk::KHRPushDescriptorExtensionName, // <-- Required by Streamline

        // NVIDIA DLSS Extensions (Required by NGX on Vulkan, confirmed via slGetFeatureRequirements)
        "VK_NVX_binary_import",
        "VK_NVX_image_view_handle",
        vk::KHRBufferDeviceAddressExtensionName,

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

        // Hardware Ray Tracing (Khronos Tutorial Course 18)
        vk::KHRAccelerationStructureExtensionName,
        vk::KHRRayQueryExtensionName,
        vk::KHRDeferredHostOperationsExtensionName,
        vk::KHRRayTracingPipelineExtensionName,
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
        if (m_streamlineInitialized)
        {
            slShutdown();
            m_streamlineInitialized = false;
        }
        m_deviceInitialized = false;
    }

    void DeviceVK::initVulkan(Nox::Window& window)
    {
        // 1. Initialize NVIDIA Streamline before Vulkan creation
        sl::Preferences pref{};
        pref.showConsole = false;
        pref.logLevel = sl::LogLevel::eVerbose; // Mutes verbose NGX / Streamline config dumps
        pref.pathsToPlugins = nullptr;
        pref.numPathsToPlugins = 0; // Searches application exe directory
        pref.applicationId = 231313;
        pref.engine = sl::EngineType::eCustom;
        pref.engineVersion = "1.0.0";
        pref.projectId = "09481010-0ae3-4697-92e9-7ba7e9ee7ead";
        pref.renderAPI = sl::RenderAPI::eVulkan;
        // Explicitly clear eAllowOTA so Streamline doesn't scan ProgramData or contact OTA servers
        pref.flags = sl::PreferenceFlags::eDisableCLStateTracking
            | sl::PreferenceFlags::eDisableDebugText
            | sl::PreferenceFlags::eUseManualHooking
            | sl::PreferenceFlags::eUseFrameBasedResourceTagging;

        // Only log critical errors and DLSS context messages from Streamline / NGX
        pref.logMessageCallback = [](sl::LogType type, const char* msg)
        {
            if (type == sl::LogType::eError)
            {
                NOX_CORE_ERROR("[Streamline] {}", msg);
            }
            else if (type == sl::LogType::eWarn)
            {
                NOX_CORE_WARN("[Streamline] {}", msg);
            }
            else if (type == sl::LogType::eInfo)
            {
                std::string_view sv(msg);
                if (sv.find("DLSS") != std::string_view::npos || sv.find("NGX") != std::string_view::npos ||
                    sv.find("extents") != std::string_view::npos || sv.find("optimal") != std::string_view::npos)
                {
                    NOX_CORE_INFO("[Streamline] {}", msg);
                }
            }
        };
        static const sl::Feature s_FeaturesToLoad[] = {
            sl::kFeatureDLSS,
            sl::kFeatureDLSS_RR
        };
        pref.featuresToLoad = s_FeaturesToLoad;
        pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(s_FeaturesToLoad));

        sl::Result initRes = slInit(pref, sl::kSDKVersion);
        if (initRes == sl::Result::eOk)
        {
            m_streamlineInitialized = true;
            NOX_CORE_INFO("[Streamline] Initialized successfully.");
        }
        else
        {
            NOX_CORE_WARN("[Streamline] slInit failed with result: {}", (int)initRes);
        }

        // 1.5 Manual hooking requires US to query what SL/NGX needs from the VkInstance/VkDevice
        // BEFORE creating them (ProgrammingGuideManualHooking.md, section 5.2.1). We were previously
        // creating the device with a hand-guessed extension/feature list and never checking this at all.
        if (m_streamlineInitialized)
        {
            auto logFeatureRequirements = [](sl::Feature feature, const char* name)
            {
                sl::FeatureRequirements reqs{};
                sl::Result res = slGetFeatureRequirements(feature, reqs);
                if (res != sl::Result::eOk)
                {
                    NOX_CORE_WARN("[Streamline] slGetFeatureRequirements({}) failed: {}", name, (int)res);
                    return;
                }
                NOX_CORE_INFO("[Streamline] {} requirements: vkGraphicsQueues={}, vkComputeQueues={}, "
                              "deviceExt={}, instanceExt={}, vk12Features={}, vk13Features={}",
                              name, reqs.vkNumGraphicsQueuesRequired, reqs.vkNumComputeQueuesRequired,
                              reqs.vkNumDeviceExtensions, reqs.vkNumInstanceExtensions, reqs.vkNumFeatures12, reqs.vkNumFeatures13);
                for (uint32_t i = 0; i < reqs.vkNumDeviceExtensions; ++i)
                    NOX_CORE_INFO("[Streamline]   {} needs device extension: {}", name, reqs.vkDeviceExtensions[i]);
                for (uint32_t i = 0; i < reqs.vkNumInstanceExtensions; ++i)
                    NOX_CORE_INFO("[Streamline]   {} needs instance extension: {}", name, reqs.vkInstanceExtensions[i]);
                for (uint32_t i = 0; i < reqs.vkNumFeatures12; ++i)
                    NOX_CORE_INFO("[Streamline]   {} needs VK1.2 feature: {}", name, reqs.vkFeatures12[i]);
                for (uint32_t i = 0; i < reqs.vkNumFeatures13; ++i)
                    NOX_CORE_INFO("[Streamline]   {} needs VK1.3 feature: {}", name, reqs.vkFeatures13[i]);
            };
            logFeatureRequirements(sl::kFeatureDLSS, "DLSS");
            logFeatureRequirements(sl::kFeatureDLSS_RR, "DLSS_RR");
        }

        createInstance();
        setupDebugMessenger();
        createSurface(window);
        pickPhysicalDevice();
        m_msaaSamples = getMaxUsableSampleCount();
        createLogicalDevice();
        initDeviceCapabilities();

        // 2. Register Vulkan Device with Streamline
        if (m_streamlineInitialized)
        {
            sl::VulkanInfo vkInfo{};
            vkInfo.device = static_cast<VkDevice>(*m_device);
            vkInfo.instance = static_cast<VkInstance>(*m_instance);
            vkInfo.physicalDevice = static_cast<VkPhysicalDevice>(*m_physicalDevice);
            vkInfo.graphicsQueueFamily = m_queueIndex;
            vkInfo.graphicsQueueIndex = 0;
            vkInfo.computeQueueFamily = m_queueIndex;
            vkInfo.computeQueueIndex = 0;

            sl::Result vkRes = slSetVulkanInfo(vkInfo);
            if (vkRes == sl::Result::eOk)
            {
                NOX_CORE_INFO("[Streamline] Vulkan device registered successfully.");

                // Resolve Streamline's present proxy via SDL3 so SwapchainVK routes presentation through Streamline
                SDL_SharedObject* slInterposer = SDL_LoadObject("sl.interposer.dll");
                if (slInterposer)
                {
                    m_slQueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(SDL_LoadFunction(slInterposer,
                                                                                                   "vkQueuePresentKHR"));
                    if (m_slQueuePresentKHR)
                    {
                        NOX_CORE_INFO("[Streamline] Hooked vkQueuePresentKHR from sl.interposer.dll");
                    }
                }
                sl::AdapterInfo adapterInfo{};
                adapterInfo.vkPhysicalDevice = static_cast<VkPhysicalDevice>(*m_physicalDevice);

                bool dlssLoaded = false;
                slIsFeatureLoaded(sl::kFeatureDLSS, dlssLoaded);
                sl::Result dlssRes = slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo);
                m_slDLSSSupported = (dlssRes == sl::Result::eOk);

                bool dlssRRLoaded = false;
                slIsFeatureLoaded(sl::kFeatureDLSS_RR, dlssRRLoaded);
                sl::Result dlssRRRes = slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo);
                m_slDLSS_RRSupported = (dlssRRRes == sl::Result::eOk);

                NOX_CORE_INFO("[Streamline] DLSS Super Resolution - Loaded: {}, Supported: {} (code: {})", dlssLoaded,
                              m_slDLSSSupported, (int)dlssRes);
                NOX_CORE_INFO("[Streamline] DLSS Ray Reconstruction - Loaded: {}, Supported: {} (code: {})", dlssRRLoaded,
                              m_slDLSS_RRSupported, (int)dlssRRRes);
            }
            else
            {
                NOX_CORE_WARN("[Streamline] slSetVulkanInfo failed with result: {}", (int)vkRes);
            }
        }
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
                                                             vk::PhysicalDeviceMeshShaderFeaturesEXT,
                                                             vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
                                                             vk::PhysicalDeviceRayQueryFeaturesKHR,
                                                             vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>();
        bool supportsRequiredFeatures = features.template get<vk::PhysicalDeviceFeatures2>().features.samplerAnisotropy &&
            features.template get<vk::PhysicalDeviceFeatures2>().features.geometryShader && // Visability buffer
            features.template get<vk::PhysicalDeviceVulkan11Features>().shaderDrawParameters &&
            features.template get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering &&
            features.template get<vk::PhysicalDeviceVulkan13Features>().synchronization2 &&
            /*features.template get<vk::PhysicalDeviceShaderObjectFeaturesEXT>().shaderObject &&*/ // dont force to support both legacy pipeline and shaderobject
            features.template get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>().extendedDynamicState &&
            features.template get<vk::PhysicalDeviceMeshShaderFeaturesEXT>().meshShader &&
            features.template get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>().accelerationStructure &&
            features.template get<vk::PhysicalDeviceRayQueryFeaturesKHR>().rayQuery &&
            features.template get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTracingPipeline;
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
                           vk::PhysicalDeviceMeshShaderFeaturesEXT,
                           vk::PhysicalDeviceUnifiedImageLayoutsFeaturesKHR,
                           vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
                           vk::PhysicalDeviceRayQueryFeaturesKHR,
                           vk::PhysicalDeviceRayTracingPipelineFeaturesKHR
            >
            featureChain = {
                {
                    .features = {
                        .geometryShader = true,
                        .sampleRateShading = true,
                        .multiDrawIndirect = true,
                        .wideLines = true,
                        .samplerAnisotropy = true,
                        .shaderInt64 = true,
                    }
                }, // vk::PhysicalDeviceFeatures2
                {.shaderDrawParameters = true}, // vk::PhysicalDeviceVulkan11Features
                {
                    .storageBuffer8BitAccess = true,
                    .shaderInt8 = true,
                    .shaderSampledImageArrayNonUniformIndexing = true,
                    .shaderStorageBufferArrayNonUniformIndexing = true,
                    .runtimeDescriptorArray = true,
                    .scalarBlockLayout = true,
                    .timelineSemaphore = true, // <-- Required by Streamline
                    .bufferDeviceAddress = true // <-- Required by Streamline
                },
                // vk::PhysicalDeviceVulkan12Features
                {
                    .privateData = true // <-- Required by Streamline
                    ,
                    .shaderDemoteToHelperInvocation = true,
                    .synchronization2 = true,
                    .dynamicRendering = true // <-- Required by Streamline
                }, // vk::PhysicalDeviceVulkan13Features
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
                {.taskShader = true, .meshShader = true},
                {.unifiedImageLayouts = true},
                {.accelerationStructure = true},
                {.rayQuery = true},
                {.rayTracingPipeline = true}
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

        // Required by Streamline/NGX (confirmed via slGetFeatureRequirements) but never requested before,
        // since manual hooking means SL cannot inject these into instance creation itself.
        extensions.push_back(vk::KHRGetPhysicalDeviceProperties2ExtensionName);
        extensions.push_back(vk::KHRExternalMemoryCapabilitiesExtensionName);
        extensions.push_back(vk::KHRExternalSemaphoreCapabilitiesExtensionName);

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

    void DeviceVK::resetDLSSViewport()
    {
        if (!m_streamlineInitialized || !m_dlssContextEverEvaluated)
            return; // nothing has actually created a DLSSContext for this viewport yet - nothing to free

        // NGX creates its internal DLSSContext at whatever resolution it sees on the first evaluate
        // for this viewport and does NOT resize it just because we later tag differently-sized
        // resources - it must be explicitly freed so the next evaluateDLSS() recreates it at the
        // new size. Without this, evaluate silently no-ops forever after any resize.
        m_dlssContextEverEvaluated = false;
        sl::ViewportHandle viewport{0};
        if (m_slDLSSSupported)
        {
            sl::Result res = slFreeResources(sl::kFeatureDLSS, viewport);
            if (res != sl::Result::eOk)
            {
                NOX_CORE_WARN("[Streamline] slFreeResources(DLSS) failed: {}", (int)res);
            }
        }
        if (m_slDLSS_RRSupported)
        {
            slFreeResources(sl::kFeatureDLSS_RR, viewport);
        }
    }

    Device::DLSSRenderExtent DeviceVK::getDLSSOptimalRenderSize(UpscaleMode mode, Extent2D outputSize)
    {
        if (!m_streamlineInitialized || !m_slDLSSSupported || mode == UpscaleMode::Off ||
            outputSize.width == 0 || outputSize.height == 0)
        {
            return {outputSize, 0.0f};
        }

        sl::DLSSOptions opts{};
        opts.mode = ToSLDLSSMode(mode);
        opts.outputWidth = outputSize.width;
        opts.outputHeight = outputSize.height;

        sl::DLSSOptimalSettings settings{};
        sl::Result res = slDLSSGetOptimalSettings(opts, settings);
        if (res != sl::Result::eOk || settings.optimalRenderWidth == 0 || settings.optimalRenderHeight == 0)
        {
            NOX_CORE_WARN("[Streamline] slDLSSGetOptimalSettings failed: {}", (int)res);
            return {outputSize, 0.0f};
        }

        return {Extent2D{settings.optimalRenderWidth, settings.optimalRenderHeight}, settings.optimalSharpness};
    }

    void DeviceVK::ensureDummyDescriptorSet()
    {
        if (m_dummyDescriptorSetReady)
            return;

        vk::DescriptorSetLayoutCreateInfo layoutInfo{.bindingCount = 0, .pBindings = nullptr};
        m_dummyDescriptorSetLayout = vk::raii::DescriptorSetLayout(m_device, layoutInfo);

        vk::DescriptorPoolCreateInfo poolInfo{.maxSets = 1, .poolSizeCount = 0, .pPoolSizes = nullptr};
        m_dummyDescriptorPool = vk::raii::DescriptorPool(m_device, poolInfo);

        vk::DescriptorSetAllocateInfo allocInfo{
            .descriptorPool = *m_dummyDescriptorPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &*m_dummyDescriptorSetLayout
        };
        // Plain (non-RAII) allocation on purpose: we want a raw handle nothing ever auto-frees, owned
        // manually for the life of the device (freed implicitly when m_dummyDescriptorPool is destroyed).
        std::vector<vk::DescriptorSet> sets = (*m_device).allocateDescriptorSets(allocInfo);
        m_dummyDescriptorSet = sets.front();

        vk::PipelineLayoutCreateInfo pipelineLayoutInfo{
            .setLayoutCount = 1,
            .pSetLayouts = &*m_dummyDescriptorSetLayout
        };
        m_dummyPipelineLayout = vk::raii::PipelineLayout(m_device, pipelineLayoutInfo);

        m_dummyDescriptorSetReady = true;
    }

    bool DeviceVK::evaluateDLSS(const DLSSParams& params)
    {
        if (!m_streamlineInitialized || !m_slDLSSSupported)
            return false;

        if (!params.inputColor || !params.outputColor || !params.depth || !params.motionVectors || !params.commandBuffer)
            return false;

        auto* inputColorVK = static_cast<TextureVK*>(params.inputColor);
        auto* outputColorVK = static_cast<TextureVK*>(params.outputColor);
        auto* depthVK = static_cast<TextureVK*>(params.depth);
        auto* mvecVK = static_cast<TextureVK*>(params.motionVectors);
        auto* cmdBufferVK = static_cast<CommandBufferVK*>(params.commandBuffer);

        // 1. Obtain unique frame token
        sl::FrameToken* frameToken = nullptr;
        m_slFrameIndex++;
        sl::Result tokenRes = slGetNewFrameToken(frameToken, &m_slFrameIndex);
        if (tokenRes != sl::Result::eOk || !frameToken)
        {
            NOX_CORE_WARN("[Streamline] slGetNewFrameToken failed: {}", (int)tokenRes);
            return false;
        }

        sl::ViewportHandle viewport{0};

        auto glmToSl = [](const glm::mat4& m) -> sl::float4x4
        {
            sl::float4x4 r{};
            glm::mat4 t = glm::transpose(m);
            memcpy(&r, &t, sizeof(t));
            return r;
        };

        // Common Constants for Streamline DLSS
        sl::Constants consts{};
        consts.cameraViewToClip = glmToSl(params.nonJitteredProj);
        consts.clipToCameraView = glmToSl(glm::inverse(params.nonJitteredProj));

        glm::mat4 currentViewProj = params.nonJitteredProj * params.view;
        glm::mat4 prevViewProj = params.prevNonJitteredProj * params.prevView;
        glm::mat4 clipToPrevClip = prevViewProj * glm::inverse(currentViewProj);
        consts.clipToPrevClip = glmToSl(clipToPrevClip);
        consts.prevClipToClip = glmToSl(glm::inverse(clipToPrevClip));

        consts.jitterOffset = {params.jitterOffset.x, params.jitterOffset.y};
        consts.mvecScale = {1.0f, 1.0f};
        consts.cameraPinholeOffset = {0.0f, 0.0f};

        consts.cameraPos = {params.cameraPos.x, params.cameraPos.y, params.cameraPos.z};
        consts.cameraUp = {params.cameraUp.x, params.cameraUp.y, params.cameraUp.z};
        consts.cameraRight = {params.cameraRight.x, params.cameraRight.y, params.cameraRight.z};
        consts.cameraFwd = {params.cameraFwd.x, params.cameraFwd.y, params.cameraFwd.z};

        consts.cameraNear = params.cameraNear;
        consts.cameraFar = 10000.0f;
        consts.cameraFOV = params.cameraFovRad;
        float aspect = (outputColorVK->GetHeight() > 0)
                           ? (static_cast<float>(outputColorVK->GetWidth()) / static_cast<float>(outputColorVK->GetHeight()))
                           : 1.777f;
        consts.cameraAspectRatio = aspect;

        consts.depthInverted = sl::Boolean::eTrue;
        consts.cameraMotionIncluded = sl::Boolean::eTrue; // Our G-buffer motion vectors already include camera motion; Streamline uses them directly
        consts.motionVectors3D = sl::Boolean::eFalse;
        consts.motionVectorsJittered = sl::Boolean::eFalse;
        consts.motionVectorsDilated = sl::Boolean::eFalse;
        consts.orthographicProjection = sl::Boolean::eFalse;
        consts.reset = params.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;

        sl::Result constRes = slSetConstants(consts, *frameToken, viewport);
        if (constRes != sl::Result::eOk)
        {
            NOX_CORE_WARN("[Streamline] slSetConstants failed: {}", (int)constRes);
            return false;
        }

        bool useRayReconstruction = params.rayReconstruction && m_slDLSS_RRSupported;

        // 3. Set DLSS / DLSS-RR Options
        if (useRayReconstruction)
        {
            sl::DLSSDOptions dlssdOptions{};
            dlssdOptions.mode = ToSLDLSSMode(params.mode);
            dlssdOptions.outputWidth = outputColorVK->GetWidth();
            dlssdOptions.outputHeight = outputColorVK->GetHeight();
            dlssdOptions.colorBuffersHDR = sl::Boolean::eTrue;
            dlssdOptions.preExposure = 1.0f;
            dlssdOptions.exposureScale = 1.0f;
            dlssdOptions.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::eUnpacked;
            dlssdOptions.worldToCameraView = glmToSl(params.view);
            dlssdOptions.cameraViewToWorld = glmToSl(glm::inverse(params.view));

            sl::Result optRes = slDLSSDSetOptions(viewport, dlssdOptions);
            if (optRes != sl::Result::eOk)
            {
                NOX_CORE_WARN("[Streamline] slDLSSDSetOptions failed: {}", (int)optRes);
                return false;
            }
        }
        else
        {
            sl::DLSSOptions dlssOptions{};
            dlssOptions.mode = ToSLDLSSMode(params.mode);
            dlssOptions.outputWidth = outputColorVK->GetWidth();
            dlssOptions.outputHeight = outputColorVK->GetHeight();
            dlssOptions.colorBuffersHDR = sl::Boolean::eTrue;
            dlssOptions.preExposure = 1.0f;
            dlssOptions.exposureScale = 1.0f;
            dlssOptions.useAutoExposure = sl::Boolean::eTrue;
            dlssOptions.dlaaPreset = sl::DLSSPreset::ePresetF;
            dlssOptions.qualityPreset = sl::DLSSPreset::ePresetK;
            dlssOptions.balancedPreset = sl::DLSSPreset::ePresetK;
            dlssOptions.performancePreset = sl::DLSSPreset::ePresetM;
            dlssOptions.ultraPerformancePreset = sl::DLSSPreset::ePresetL;

            sl::Result optRes = slDLSSSetOptions(viewport, dlssOptions);
            if (optRes != sl::Result::eOk)
            {
                NOX_CORE_WARN("[Streamline] slDLSSSetOptions failed: {}", (int)optRes);
                return false;
            }
        }

        // 4. Tag Resources with explicit usage flags & subresource range for Depth
        VkCommandBuffer vkCmd = static_cast<VkCommandBuffer>(*cmdBufferVK->getActiveNativeBuffer());

        sl::Resource colorIn{};
        colorIn.type = sl::ResourceType::eTex2d;
        colorIn.native = static_cast<VkImage>(*inputColorVK->getNativeImage());
        colorIn.view = static_cast<VkImageView>(*inputColorVK->getNativeView());
        colorIn.nativeFormat = static_cast<uint32_t>(inputColorVK->getFormat());
        colorIn.width = inputColorVK->GetWidth();
        colorIn.height = inputColorVK->GetHeight();
        colorIn.state = VK_IMAGE_LAYOUT_GENERAL;
        colorIn.mipLevels = 1;
        colorIn.arrayLayers = 1;
        colorIn.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

        sl::Resource colorOut{};
        colorOut.type = sl::ResourceType::eTex2d;
        colorOut.native = static_cast<VkImage>(*outputColorVK->getNativeImage());
        colorOut.view = static_cast<VkImageView>(*outputColorVK->getNativeView());
        colorOut.nativeFormat = static_cast<uint32_t>(outputColorVK->getFormat());
        colorOut.width = outputColorVK->GetWidth();
        colorOut.height = outputColorVK->GetHeight();
        colorOut.state = VK_IMAGE_LAYOUT_GENERAL;
        colorOut.mipLevels = 1;
        colorOut.arrayLayers = 1;
        colorOut.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

        sl::SubresourceRange depthRange{};
        depthRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depthRange.baseMipLevel = 0;
        depthRange.levelCount = 1;
        depthRange.baseArrayLayer = 0;
        depthRange.layerCount = 1;

        sl::Resource depth{};
        depth.next = &depthRange;
        depth.type = sl::ResourceType::eTex2d;
        depth.native = static_cast<VkImage>(*depthVK->getNativeImage());
        depth.view = static_cast<VkImageView>(*depthVK->getNativeView());
        depth.nativeFormat = static_cast<uint32_t>(depthVK->getFormat());
        depth.width = depthVK->GetWidth();
        depth.height = depthVK->GetHeight();
        depth.state = VK_IMAGE_LAYOUT_GENERAL;
        depth.mipLevels = 1;
        depth.arrayLayers = 1;
        depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        sl::Resource mvec{};
        mvec.type = sl::ResourceType::eTex2d;
        mvec.native = static_cast<VkImage>(*mvecVK->getNativeImage());
        mvec.view = static_cast<VkImageView>(*mvecVK->getNativeView());
        mvec.nativeFormat = static_cast<uint32_t>(mvecVK->getFormat());
        mvec.width = mvecVK->GetWidth();
        mvec.height = mvecVK->GetHeight();
        mvec.state = VK_IMAGE_LAYOUT_GENERAL;
        mvec.mipLevels = 1;
        mvec.arrayLayers = 1;
        mvec.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

        sl::Extent inputExtent{0, 0, inputColorVK->GetWidth(), inputColorVK->GetHeight()};
        sl::Extent outputExtent{0, 0, outputColorVK->GetWidth(), outputColorVK->GetHeight()};
        sl::Extent depthExtent{0, 0, depthVK->GetWidth(), depthVK->GetHeight()};
        sl::Extent mvecExtent{0, 0, mvecVK->GetWidth(), mvecVK->GetHeight()};

        std::vector<sl::ResourceTag> tags;
        tags.push_back(sl::ResourceTag(&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
        tags.push_back(sl::ResourceTag(&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &outputExtent));
        tags.push_back(sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &depthExtent));
        tags.push_back(sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &mvecExtent));

        // When Ray Reconstruction is enabled, tag G-Buffer Albedo, Specular Albedo, Normals, and Roughness
        sl::Resource albedoRes{}, specularAlbedoRes{}, normalRes{}, roughnessRes{};
        if (useRayReconstruction && params.albedo && params.normal && params.roughness)
        {
            auto wrapTex = [](TextureVK* t) -> sl::Resource
            {
                sl::Resource r{};
                r.type = sl::ResourceType::eTex2d;
                r.native = static_cast<VkImage>(*t->getNativeImage());
                r.view = static_cast<VkImageView>(*t->getNativeView());
                r.nativeFormat = static_cast<uint32_t>(t->getFormat());
                r.width = t->GetWidth();
                r.height = t->GetHeight();
                r.state = VK_IMAGE_LAYOUT_GENERAL;
                r.mipLevels = 1;
                r.arrayLayers = 1;
                r.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
                return r;
            };

            albedoRes = wrapTex(dynamic_cast<TextureVK*>(params.albedo));
            specularAlbedoRes = wrapTex(dynamic_cast<TextureVK*>(params.specularAlbedo ? params.specularAlbedo : params.albedo));
            normalRes = wrapTex(dynamic_cast<TextureVK*>(params.normal));
            roughnessRes = wrapTex(dynamic_cast<TextureVK*>(params.roughness));

            tags.push_back(sl::ResourceTag(&albedoRes, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
            tags.push_back(sl::ResourceTag(&specularAlbedoRes, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
            tags.push_back(sl::ResourceTag(&normalRes, sl::kBufferTypeNormals, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
            tags.push_back(sl::ResourceTag(&roughnessRes, sl::kBufferTypeRoughness, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
        }

        sl::Result tagRes = slSetTagForFrame(*frameToken, viewport, tags.data(), static_cast<uint32_t>(tags.size()),
                                             reinterpret_cast<sl::CommandBuffer*>(vkCmd));
        if (tagRes != sl::Result::eOk)
        {
            NOX_CORE_WARN("[Streamline] slSetTagForFrame failed: {}", (int)tagRes);
            return false;
        }

        // 4.5 Dummy Descriptor Set Workaround for VK_EXT_descriptor_heap
        constexpr bool kEnableDescriptorHeapWorkaround = true;
        if constexpr (kEnableDescriptorHeapWorkaround)
        {
            ensureDummyDescriptorSet();
            vk::CommandBuffer(vkCmd).bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_dummyPipelineLayout, 0, {m_dummyDescriptorSet}, {});
        }

        // 5. Evaluate DLSS or DLSS Ray Reconstruction
        const sl::BaseStructure* inputs[] = {&viewport};
        sl::Feature featureToEval = useRayReconstruction ? sl::kFeatureDLSS_RR : sl::kFeatureDLSS;
        sl::Result evalRes = slEvaluateFeature(featureToEval, *frameToken, inputs, static_cast<uint32_t>(std::size(inputs)),
                                               reinterpret_cast<sl::CommandBuffer*>(vkCmd));
        static bool s_firstEvalLogged = false;
        if (!s_firstEvalLogged)
        {
            s_firstEvalLogged = true;
            NOX_CORE_INFO("[Streamline] First evaluateDLSS (Feature: {}): tagRes={}, evalRes={}",
                          useRayReconstruction ? "DLSS_RR" : "DLSS_SR", (int)tagRes, (int)evalRes);
        }
        if (evalRes != sl::Result::eOk)
        {
            NOX_CORE_WARN("[Streamline] slEvaluateFeature failed: {}", (int)evalRes);
            return false;
        }
        m_dlssContextEverEvaluated = true;

        // 6. Memory barrier to ensure subsequent shader reads see DLSS writes
        cmdBufferVK->executionBarrier();

        return true;
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
            {vk::DescriptorType::eSampledImage, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + maxCustomTextures},
            {vk::DescriptorType::eSampler, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE + maxCustomTextures},
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
            {vk::DescriptorType::eCombinedImageSampler, maxCustomTextures}
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
