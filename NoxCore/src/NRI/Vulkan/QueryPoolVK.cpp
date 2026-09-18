#include "QueryPoolVK.h"

#include <vector>

#include "DeviceVK.h"

namespace NRI
{
    namespace
    {
        vk::QueryType toVkQueryType(QueryType type)
        {
            switch (type)
            {
            case QueryType::AccelerationStructureCompactedSize:
                return vk::QueryType::eAccelerationStructureCompactedSizeKHR;
            }
            return vk::QueryType::eAccelerationStructureCompactedSizeKHR;
        }
    }

    QueryPoolVK::QueryPoolVK(DeviceVK& device, const QueryPoolDesc& desc)
        : m_capacity(desc.capacity), m_type(toVkQueryType(desc.type))
    {
        m_pool = vk::raii::QueryPool(device.getDevice(), vk::QueryPoolCreateInfo{
            .queryType = m_type,
            .queryCount = desc.capacity
        });
    }

    bool QueryPoolVK::getResults(uint32_t first, std::span<uint64_t> out) const
    {
        if (out.empty())
            return true;

        // (value, availability) pairs; no WAIT flag, the caller reads after the writing submission completed.
        constexpr vk::DeviceSize stride = sizeof(uint64_t) * 2;
        std::vector<uint64_t> values(out.size() * 2);
        const vk::Result result = m_pool.getResults(first, static_cast<uint32_t>(out.size()), values.size() * sizeof(uint64_t), values.data(), stride,
                                                    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
        if (result != vk::Result::eSuccess && result != vk::Result::eNotReady)
            return false;

        for (size_t query = 0; query < out.size(); ++query)
        {
            if (values[query * 2 + 1] == 0)
                return false;
            out[query] = values[query * 2];
        }
        return true;
    }
}
