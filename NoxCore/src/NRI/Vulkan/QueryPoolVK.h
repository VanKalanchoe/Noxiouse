#pragma once
#include "VulkanCommon.h"
#include "../QueryPool.h"

namespace NRI
{
    class DeviceVK;

    class QueryPoolVK final : public QueryPool
    {
    public:
        QueryPoolVK(DeviceVK& device, const QueryPoolDesc& desc);

        uint32_t getCapacity() const override { return m_capacity; }
        bool getResults(uint32_t first, std::span<uint64_t> out) const override;

        const vk::raii::QueryPool& getNativePool() const { return m_pool; }
        vk::QueryType getNativeType() const { return m_type; }

    private:
        uint32_t m_capacity = 0;
        vk::QueryType m_type;
        vk::raii::QueryPool m_pool = nullptr;
    };
}
