#include "VolumeAsset.h"

#include <cstring>
#include <fstream>

#include "NoxCore/Core/Log.h"

namespace Nox
{
    bool VolumeAsset::LoadHeader(const std::filesystem::path& filePath)
    {
        m_Valid = false;
        m_Frames.clear();
        m_FilePath = filePath;

        std::ifstream in(filePath, std::ios::binary);
        if (!in)
        {
            NOX_CORE_ERROR("VolumeAsset: failed to open '{}'", filePath.string());
            return false;
        }

        VolumeHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in || std::memcmp(header.Magic, "NVDB", 4) != 0 || header.Version != 1u || header.FrameCount == 0u)
        {
            NOX_CORE_ERROR("VolumeAsset: invalid .nvdb header in '{}'", filePath.string());
            return false;
        }

        m_Frames.resize(header.FrameCount);
        in.read(reinterpret_cast<char*>(m_Frames.data()), static_cast<std::streamsize>(sizeof(VolumeFrameRecord) * m_Frames.size()));
        if (!in)
        {
            NOX_CORE_ERROR("VolumeAsset: truncated frame table in '{}'", filePath.string());
            m_Frames.clear();
            return false;
        }

        m_Header = header;
        m_Valid = true;
        return true;
    }

    bool VolumeAsset::ReadFrameChannel(uint32_t frameIndex, VolumeChannel channel, std::vector<uint8_t>& outBytes) const
    {
        outBytes.clear();
        if (!m_Valid || frameIndex >= m_Frames.size())
            return false;

        const VolumeFrameRecord& rec = m_Frames[frameIndex];
        uint64_t offset = 0;
        uint64_t size = 0;
        switch (channel)
        {
        case VolumeChannel::Density:
            offset = rec.DensityOffset;
            size = rec.DensityBytes;
            break;
        case VolumeChannel::Temperature:
            offset = rec.TemperatureOffset;
            size = rec.TemperatureBytes;
            break;
        case VolumeChannel::Flame:
            offset = rec.FlameOffset;
            size = rec.FlameBytes;
            break;
        }
        if (size == 0)
            return false;

        std::ifstream in(m_FilePath, std::ios::binary);
        if (!in)
            return false;
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        outBytes.resize(static_cast<size_t>(size));
        in.read(reinterpret_cast<char*>(outBytes.data()), static_cast<std::streamsize>(size));
        return static_cast<bool>(in);
    }

    bool VolumeAsset::Save(const std::filesystem::path& filePath, const VolumeHeader& inHeader,
                           const std::vector<VolumeFrameCookData>& frames)
    {
        if (frames.empty())
            return false;

        std::error_code ec;
        if (filePath.has_parent_path())
            std::filesystem::create_directories(filePath.parent_path(), ec);

        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            NOX_CORE_ERROR("VolumeAsset::Save: failed to create '{}'", filePath.string());
            return false;
        }

        VolumeHeader header = inHeader;
        std::memcpy(header.Magic, "NVDB", 4);
        header.Version = 1u;
        header.FrameCount = static_cast<uint32_t>(frames.size());

        std::vector<VolumeFrameRecord> table(frames.size());
        uint64_t cursor = sizeof(VolumeHeader) + sizeof(VolumeFrameRecord) * frames.size();

        auto align16 = [](uint64_t v) { return (v + 15ull) & ~15ull; };

        for (size_t i = 0; i < frames.size(); ++i)
        {
            const VolumeFrameCookData& src = frames[i];
            VolumeFrameRecord& rec = table[i];
            rec.AabbMin = src.AabbMin;
            rec.AabbMax = src.AabbMax;
            rec.DimX = src.DimX;
            rec.DimY = src.DimY;
            rec.DimZ = src.DimZ;

            if (!src.DensityBytes.empty())
            {
                cursor = align16(cursor);
                rec.DensityOffset = cursor;
                rec.DensityBytes = src.DensityBytes.size();
                cursor += rec.DensityBytes;
            }
            if (!src.TemperatureBytes.empty())
            {
                cursor = align16(cursor);
                rec.TemperatureOffset = cursor;
                rec.TemperatureBytes = src.TemperatureBytes.size();
                cursor += rec.TemperatureBytes;
            }
            if (!src.FlameBytes.empty())
            {
                cursor = align16(cursor);
                rec.FlameOffset = cursor;
                rec.FlameBytes = src.FlameBytes.size();
                cursor += rec.FlameBytes;
            }
        }

        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(table.data()), static_cast<std::streamsize>(sizeof(VolumeFrameRecord) * table.size()));

        const char zeros[16] = {};
        uint64_t written = sizeof(VolumeHeader) + sizeof(VolumeFrameRecord) * table.size();
        auto writeAligned = [&](const std::vector<uint8_t>& bytes)
        {
            if (bytes.empty())
                return;
            const uint64_t aligned = align16(written);
            if (aligned > written)
            {
                out.write(zeros, static_cast<std::streamsize>(aligned - written));
                written = aligned;
            }
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            written += bytes.size();
        };

        for (const VolumeFrameCookData& src : frames)
        {
            writeAligned(src.DensityBytes);
            writeAligned(src.TemperatureBytes);
            writeAligned(src.FlameBytes);
        }

        return static_cast<bool>(out);
    }
}
