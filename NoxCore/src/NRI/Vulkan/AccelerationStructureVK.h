 #pragma once
#include "../AccelerationStructure.h"
#include <vulkan/vulkan_raii.hpp>

namespace NRI
{
    class DeviceVK;
    
    inline vk::BuildAccelerationStructureFlagsKHR toVkBuildFlags(AccelerationStructureBuildFlags flags)
    {
        vk::BuildAccelerationStructureFlagsKHR vkFlags{};
        if (flags & AccelerationStructureBuildFlags::AllowUpdate)
            vkFlags |= vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate;
        if (flags & AccelerationStructureBuildFlags::PreferFastTrace)
            vkFlags |= vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
        if (flags & AccelerationStructureBuildFlags::PreferFastBuild)
            vkFlags |= vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
        if (flags & AccelerationStructureBuildFlags::LowMemory)
            vkFlags |= vk::BuildAccelerationStructureFlagBitsKHR::eLowMemory;
        return vkFlags;
    }

    class AccelerationStructureVK final : public AccelerationStructure
    {
    public:
        AccelerationStructureVK(DeviceVK& device, const AccelerationStructureDesc& desc);
        ~AccelerationStructureVK() override = default;

        uint64_t getDeviceAddress() const override { return m_deviceAddress; }
        uint64_t getSize() const override { return m_size; }
        AccelerationStructureType getType() const override { return m_type; }
        Buffer* getBuffer() const override { return m_storageBuffer; }

        vk::raii::AccelerationStructureKHR& getNativeHandle() { return m_handle; }
        const vk::raii::AccelerationStructureKHR& getNativeHandle() const { return m_handle; }

    private:
        DeviceVK& m_deviceVK;
        AccelerationStructureType m_type;
        Buffer* m_storageBuffer = nullptr;
        uint64_t m_bufferOffset = 0;
        uint64_t m_size = 0;
        uint64_t m_deviceAddress = 0;
        vk::raii::AccelerationStructureKHR m_handle;
    };
}