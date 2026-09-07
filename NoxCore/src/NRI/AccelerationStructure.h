#pragma once
    #include <cstdint>
    #include <vector>
    #include <memory>

    namespace NRI
    {
        class Buffer;

        enum class AccelerationStructureType : uint8_t
        {
            BottomLevel,
            TopLevel
        };

        enum class AccelerationStructureBuildFlags : uint32_t
        {
            None            = 0,
            AllowUpdate     = 1 << 0,
            PreferFastTrace = 1 << 1,
            PreferFastBuild = 1 << 2,
            LowMemory       = 1 << 3
        };

        inline AccelerationStructureBuildFlags operator|(AccelerationStructureBuildFlags a, AccelerationStructureBuildFlags b)
        {
            return static_cast<AccelerationStructureBuildFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
        }

        inline bool operator&(AccelerationStructureBuildFlags a, AccelerationStructureBuildFlags b)
        {
            return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
        }

        struct AccelerationStructureTrianglesDesc
        {
            uint64_t vertexBufferAddress = 0;
            uint32_t vertexStride = 0;
            uint32_t maxVertex = 0;
            uint64_t indexBufferAddress = 0;
            uint32_t primitiveCount = 0;
            uint32_t primitiveOffset = 0; // Byte offset in index buffer
            uint32_t firstVertex = 0;
            bool isOpaque = true;
        };

        struct AccelerationStructureInstancesDesc
        {
            uint64_t instanceBufferAddress = 0;
            uint32_t instanceCount = 0;
        };

        struct AccelerationStructureBuildDesc
        {
            AccelerationStructureType type = AccelerationStructureType::BottomLevel;
            AccelerationStructureBuildFlags flags = AccelerationStructureBuildFlags::PreferFastTrace;

            // Bottom-Level: list of triangle submeshes
            std::vector<AccelerationStructureTrianglesDesc> triangles;

            // Top-Level: instance records
            AccelerationStructureInstancesDesc instances;
        };

        struct AccelerationStructureBuildSizes
        {
            uint64_t accelerationStructureSize = 0;
            uint64_t buildScratchSize = 0;
            uint64_t updateScratchSize = 0;
        };

        struct AccelerationStructureDesc
        {
            AccelerationStructureType type = AccelerationStructureType::BottomLevel;
            Buffer* storageBuffer = nullptr;
            uint64_t bufferOffset = 0;
            uint64_t size = 0;
        };

        // Hardware standard 3x4 row-major transform
        struct TransformMatrix
        {
            float matrix[3][4] = {
                { 1.0f, 0.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 1.0f, 0.0f }
            };
        };

        // Exactly 64 bytes matching VkAccelerationStructureInstanceKHR
        struct AccelerationStructureInstance
        {
            TransformMatrix transform{};
            uint32_t instanceCustomIndex : 24 = 0;
            uint32_t mask : 8 = 0xFF;
            uint32_t instanceShaderBindingTableRecordOffset : 24 = 0;
            uint32_t flags : 8 = 0;
            uint64_t accelerationStructureReference = 0; // Device address of BLAS
        };
        static_assert(sizeof(AccelerationStructureInstance) == 64, "AccelerationStructureInstance must be exactly 64 bytes");

        class AccelerationStructure
        {
        public:
            virtual ~AccelerationStructure() = default;

            virtual uint64_t getDeviceAddress() const = 0;
            virtual uint64_t getSize() const = 0;
            virtual AccelerationStructureType getType() const = 0;
            virtual Buffer* getBuffer() const = 0;
        };
    }