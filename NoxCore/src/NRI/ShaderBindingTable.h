#pragma once
#include <cstdint>
#include <vector>
#include <span>
#include <cstring>
#include "CommandBuffer.h"

namespace NRI
{
    struct ShaderTableState
    {
        StridedDeviceAddressRegion rayGen;
        StridedDeviceAddressRegion miss;
        StridedDeviceAddressRegion hitGroups;
        StridedDeviceAddressRegion callable;
    };

    // Replicates NVIDIA RTXPT / NVRHI ShaderTable::bake:
    // (from E:/dev/VulkanAdventure/nvidiathegoat/RTXPT/External/Donut/nvrhi/src/vulkan/vulkan-raytracing.cpp:1221-1300)
    // Every entry stride is locked to shaderGroupBaseAlignment.
    // Total upload size = (numRayGen + numMiss + numHit + numCallable) * shaderGroupBaseAlignment.
    // Zero manual padding calculations needed, perfectly matches hardware alignment and Vulkan RT specs.
    inline uint64_t calculateShaderTableSize(uint32_t numRayGen, uint32_t numMiss, uint32_t numHit, uint32_t numCallable, uint32_t baseAlignment)
    {
        return static_cast<uint64_t>(numRayGen + numMiss + numHit + numCallable) * baseAlignment;
    }

    inline void bakeShaderTable(
        uint8_t* uploadCpuVA,
        uint64_t uploadGpuVA,
        const uint8_t* shaderGroupHandles,
        uint32_t handleSize,
        uint32_t baseAlignment,
        int32_t rayGenShaderGroupIndex,
        std::span<const uint32_t> missShaderGroupIndices,
        std::span<const uint32_t> hitShaderGroupIndices,
        std::span<const uint32_t> callableShaderGroupIndices,
        ShaderTableState& state)
    {
        uint32_t sbtIndex = 0;

        // ... RayGen
        if (rayGenShaderGroupIndex >= 0)
        {
            memcpy(uploadCpuVA + sbtIndex * baseAlignment,
                shaderGroupHandles + handleSize * rayGenShaderGroupIndex,
                handleSize);
            state.rayGen.deviceAddress = uploadGpuVA + sbtIndex * baseAlignment;
            state.rayGen.size = baseAlignment;
            state.rayGen.stride = baseAlignment;
            sbtIndex++;
        }
        else
        {
            state.rayGen = {};
        }

        // ... Miss
        if (!missShaderGroupIndices.empty())
        {
            state.miss.deviceAddress = uploadGpuVA + sbtIndex * baseAlignment;
            for (uint32_t shaderGroupIndex : missShaderGroupIndices)
            {
                memcpy(uploadCpuVA + sbtIndex * baseAlignment,
                    shaderGroupHandles + handleSize * shaderGroupIndex,
                    handleSize);
                sbtIndex++;
            }
            state.miss.size = static_cast<uint64_t>(baseAlignment) * missShaderGroupIndices.size();
            state.miss.stride = baseAlignment;
        }
        else
        {
            state.miss = {};
        }

        // ... Hit Groups
        if (!hitShaderGroupIndices.empty())
        {
            state.hitGroups.deviceAddress = uploadGpuVA + sbtIndex * baseAlignment;
            for (uint32_t shaderGroupIndex : hitShaderGroupIndices)
            {
                memcpy(uploadCpuVA + sbtIndex * baseAlignment,
                    shaderGroupHandles + handleSize * shaderGroupIndex,
                    handleSize);
                sbtIndex++;
            }
            state.hitGroups.size = static_cast<uint64_t>(baseAlignment) * hitShaderGroupIndices.size();
            state.hitGroups.stride = baseAlignment;
        }
        else
        {
            state.hitGroups = {};
        }

        // ... Callable
        if (!callableShaderGroupIndices.empty())
        {
            state.callable.deviceAddress = uploadGpuVA + sbtIndex * baseAlignment;
            for (uint32_t shaderGroupIndex : callableShaderGroupIndices)
            {
                memcpy(uploadCpuVA + sbtIndex * baseAlignment,
                    shaderGroupHandles + handleSize * shaderGroupIndex,
                    handleSize);
                sbtIndex++;
            }
            state.callable.size = static_cast<uint64_t>(baseAlignment) * callableShaderGroupIndices.size();
            state.callable.stride = baseAlignment;
        }
        else
        {
            state.callable = {};
        }
    }
}
