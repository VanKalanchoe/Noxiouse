#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    enum VolumeChannelFlags : uint32_t
    {
        VolumeChannel_None        = 0u,
        VolumeChannel_Density     = 1u << 0,
        VolumeChannel_Temperature = 1u << 1,
        VolumeChannel_Flame       = 1u << 2,
        VolumeChannel_Velocity    = 1u << 3
    };

    enum class VolumeChannel : uint8_t
    {
        Density = 0,
        Temperature = 1,
        Flame = 2
    };

    // Grid storage layout inside each .nvdb frame channel payload:
    // - DenseFP16: packed IEEE-754 float16 [DimZ][DimY][DimX] array (zero runtime deps, direct GPU BufferRef/3D texture upload)
    // - NanoVDB:   raw NVIDIA NanoVDB buffer (read by PNanoVDB.h in Slang)
    enum class VolumeGridFormat : uint32_t
    {
        DenseFP16 = 0u,
        NanoVDB   = 1u
    };

    struct VolumeHeader
    {
        char Magic[4] = { 'N', 'V', 'D', 'B' };
        uint32_t Version = 1u;
        uint32_t FrameCount = 0u;
        float Fps = 24.0f;
        uint32_t ChannelFlags = VolumeChannel_None;
        uint32_t GridFormat = static_cast<uint32_t>(VolumeGridFormat::DenseFP16);
        float VoxelSize = 0.05f; // meters
        uint32_t Reserved = 0u;
        glm::vec3 AabbMin{ -0.5f, 0.0f, -0.5f }; // overall sequence bounds (meters)
        float MaxDensity = 1.0f;
        glm::vec3 AabbMax{ 0.5f, 1.0f, 0.5f };
        float MaxTemperature = 1.0f;
    };
    static_assert(sizeof(VolumeHeader) == 64, "VolumeHeader must be 64 bytes");

    struct VolumeFrameRecord
    {
        uint64_t DensityOffset = 0;
        uint64_t DensityBytes = 0;
        uint64_t TemperatureOffset = 0;
        uint64_t TemperatureBytes = 0;
        uint64_t FlameOffset = 0;
        uint64_t FlameBytes = 0;
        glm::vec3 AabbMin{ 0.0f };
        uint32_t DimX = 0;
        glm::vec3 AabbMax{ 0.0f };
        uint32_t DimY = 0;
        uint32_t DimZ = 0;
        uint32_t Pad0 = 0;
        uint32_t Pad1 = 0;
        uint32_t Pad2 = 0;
    };
    static_assert(sizeof(VolumeFrameRecord) == 96, "VolumeFrameRecord must be 96 bytes");

    struct VolumeFrameCookData
    {
        glm::vec3 AabbMin{ 0.0f };
        glm::vec3 AabbMax{ 1.0f };
        uint32_t DimX = 0;
        uint32_t DimY = 0;
        uint32_t DimZ = 0;
        std::vector<uint8_t> DensityBytes;
        std::vector<uint8_t> TemperatureBytes;
        std::vector<uint8_t> FlameBytes;
    };

    // Cooked heterogeneous volume asset (.nvdb, single frame or animated sequence).
    // Zero OpenVDB runtime dependency: loads header + frame offset table instantly and streams frames on demand.
    class VolumeAsset : public Asset
    {
    public:
        VolumeAsset() = default;
        ~VolumeAsset() override = default;

        AssetType GetType() const override { return AssetType::Volume; }
        static AssetType GetStaticType() { return AssetType::Volume; }

        bool LoadHeader(const std::filesystem::path& filePath);
        bool ReadFrameChannel(uint32_t frameIndex, VolumeChannel channel, std::vector<uint8_t>& outBytes) const;

        static bool Save(const std::filesystem::path& filePath, const VolumeHeader& header,
                         const std::vector<VolumeFrameCookData>& frames);

        bool IsValid() const { return m_Valid && m_Header.FrameCount > 0u; }
        const std::filesystem::path& GetFilePath() const { return m_FilePath; }
        const VolumeHeader& GetHeader() const { return m_Header; }
        uint32_t GetFrameCount() const { return m_Header.FrameCount; }
        const VolumeFrameRecord& GetFrame(uint32_t index) const { return m_Frames[index]; }

    private:
        bool m_Valid = false;
        std::filesystem::path m_FilePath;
        VolumeHeader m_Header{};
        std::vector<VolumeFrameRecord> m_Frames;
    };
}
