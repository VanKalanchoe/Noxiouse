#pragma once
#include <cstdint>
#include <span>

namespace NRI
{
    enum class QueryType : uint8_t
    {
        AccelerationStructureCompactedSize // bytes a BLAS built with AllowCompaction needs once compacted
    };

    struct QueryPoolDesc
    {
        QueryType type = QueryType::AccelerationStructureCompactedSize;
        uint32_t capacity = 0;
    };

    // Values the GPU writes into numbered queries (Vulkan query pool). A query is reset before it is written again, and
    // read by the host once the submission that wrote it has completed.
    class QueryPool
    {
    public:
        virtual ~QueryPool() = default;

        virtual uint32_t getCapacity() const = 0;
        // The values of queries [first, first + out.size()), without waiting: false when one of them is not available.
        virtual bool getResults(uint32_t first, std::span<uint64_t> out) const = 0;
    };
}
