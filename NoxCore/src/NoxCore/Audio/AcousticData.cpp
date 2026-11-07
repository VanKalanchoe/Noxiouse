#include "AcousticData.h"

#include <cstring>
#include <fstream>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/VirtualFileSystem.h"

namespace Nox
{
    namespace
    {
        constexpr char kMagic[4] = { 'N', 'X', 'A', 'C' };

        // The file, little-endian as written by the editor's platforms: magic, version, origin (3 doubles), volume (16 doubles,
        // column-major), probe count, settings, baked-at time, then the probe batch's size and bytes.
        struct Writer
        {
            std::vector<char> Bytes;
            template <typename T> void Put(const T& value)
            {
                const char* raw = reinterpret_cast<const char*>(&value);
                Bytes.insert(Bytes.end(), raw, raw + sizeof(T));
            }
        };
        struct Reader
        {
            const std::vector<char>& Bytes;
            size_t Offset = 0;
            bool Ok = true;
            template <typename T> T Get()
            {
                T value{};
                if (Offset + sizeof(T) > Bytes.size())
                {
                    Ok = false;
                    return value;
                }
                std::memcpy(&value, Bytes.data() + Offset, sizeof(T));
                Offset += sizeof(T);
                return value;
            }
        };
    }

    bool AcousticData::Contains(const glm::dvec3& point) const
    {
        const glm::dvec3 local = glm::dvec3(glm::inverse(Volume) * glm::dvec4(point, 1.0));
        return glm::all(glm::lessThanEqual(glm::abs(local), glm::dvec3(0.5)));
    }

    bool SaveAcousticData(const std::filesystem::path& path, const AcousticData& data)
    {
        Writer out;
        out.Bytes.insert(out.Bytes.end(), kMagic, kMagic + 4);
        out.Put(AcousticData::Version);
        for (int i = 0; i < 3; ++i)
            out.Put(data.Origin[i]);
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                out.Put(data.Volume[column][row]);
        out.Put(data.ProbeCount);
        const AcousticBakeSettings& s = data.Settings;
        out.Put(s.Spacing);
        out.Put(s.Height);
        out.Put(static_cast<uint8_t>(s.Reverb));
        out.Put(static_cast<uint8_t>(s.Pathing));
        out.Put(s.Rays);
        out.Put(s.Bounces);
        out.Put(s.Duration);
        out.Put(s.VisSamples);
        out.Put(s.VisRadius);
        out.Put(s.VisThreshold);
        out.Put(s.VisRange);
        out.Put(s.PathRange);
        out.Put(data.BakedAt);
        out.Put(static_cast<uint64_t>(data.ProbeBatch.size()));
        out.Bytes.insert(out.Bytes.end(), data.ProbeBatch.begin(), data.ProbeBatch.end());

        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file.write(out.Bytes.data(), static_cast<std::streamsize>(out.Bytes.size()));
        return static_cast<bool>(file);
    }

    Ref<AcousticData> LoadAcousticData(const std::filesystem::path& path)
    {
        std::vector<char> bytes;
        if (!VFS::ReadBytes(path, bytes) || bytes.size() < 8 || std::memcmp(bytes.data(), kMagic, 4) != 0)
        {
            NOX_CORE_WARN("Acoustic data {}: missing or not a .nacoustics file", path.generic_string());
            return {};
        }
        Reader in{ bytes, 4 };
        const uint32_t version = in.Get<uint32_t>();
        if (version > AcousticData::Version)
        {
            NOX_CORE_WARN("Acoustic data {}: version {} is newer than this build reads ({})", path.generic_string(), version, AcousticData::Version);
            return {};
        }
        Ref<AcousticData> data = CreateRef<AcousticData>();
        for (int i = 0; i < 3; ++i)
            data->Origin[i] = in.Get<double>();
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                data->Volume[column][row] = in.Get<double>();
        data->ProbeCount = in.Get<uint32_t>();
        AcousticBakeSettings& s = data->Settings;
        s.Spacing = in.Get<float>();
        s.Height = in.Get<float>();
        s.Reverb = in.Get<uint8_t>() != 0;
        s.Pathing = in.Get<uint8_t>() != 0;
        s.Rays = in.Get<int32_t>();
        s.Bounces = in.Get<int32_t>();
        s.Duration = in.Get<float>();
        s.VisSamples = in.Get<int32_t>();
        s.VisRadius = in.Get<float>();
        s.VisThreshold = in.Get<float>();
        s.VisRange = in.Get<float>();
        s.PathRange = in.Get<float>();
        data->BakedAt = in.Get<uint64_t>();
        const uint64_t size = in.Get<uint64_t>();
        if (!in.Ok || in.Offset + size > bytes.size())
        {
            NOX_CORE_WARN("Acoustic data {}: truncated", path.generic_string());
            return {};
        }
        data->ProbeBatch.assign(bytes.begin() + static_cast<std::ptrdiff_t>(in.Offset), bytes.begin() + static_cast<std::ptrdiff_t>(in.Offset + size));
        return data;
    }
}
