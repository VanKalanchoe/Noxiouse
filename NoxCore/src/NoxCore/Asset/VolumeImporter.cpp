#include "VolumeImporter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include <glm/gtc/packing.hpp>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"

#if NOX_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/tools/Interpolation.h>
#endif

namespace Nox
{
    namespace
    {
        std::string ToLowerCopy(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        // Splits a stem like "tornado_0012" into prefix "tornado_" and trailing digits "0012".
        bool SplitTrailingDigits(const std::string& stem, std::string& outPrefix, int& outNumber)
        {
            size_t end = stem.size();
            size_t start = end;
            while (start > 0 && std::isdigit(static_cast<unsigned char>(stem[start - 1])))
                --start;
            if (start == end)
                return false;
            outPrefix = stem.substr(0, start);
            try
            {
                outNumber = std::stoi(stem.substr(start));
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        uint16_t FloatToHalf(float v)
        {
            return glm::packHalf1x16(v);
        }
    }

    bool VolumeImporter::IsOpenVdbEnabled()
    {
#if NOX_HAS_OPENVDB
        return true;
#else
        return false;
#endif
    }

    std::vector<std::filesystem::path> VolumeImporter::FindVdbSequenceFiles(const std::filesystem::path& firstVdbPath)
    {
        std::vector<std::filesystem::path> result;
        if (!std::filesystem::exists(firstVdbPath))
            return result;

        std::string prefix;
        int firstNum = 0;
        if (!SplitTrailingDigits(firstVdbPath.stem().string(), prefix, firstNum))
        {
            result.push_back(firstVdbPath);
            return result;
        }

        const std::filesystem::path dir = firstVdbPath.parent_path();
        std::vector<std::pair<int, std::filesystem::path>> numbered;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file())
                continue;
            if (ToLowerCopy(entry.path().extension().string()) != ".vdb")
                continue;
            std::string p;
            int n = 0;
            if (SplitTrailingDigits(entry.path().stem().string(), p, n) && p == prefix)
                numbered.emplace_back(n, entry.path());
        }
        std::sort(numbered.begin(), numbered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        result.reserve(numbered.size());
        for (auto& [n, path] : numbered)
            result.push_back(std::move(path));
        if (result.empty())
            result.push_back(firstVdbPath);
        return result;
    }

    bool VolumeImporter::CookVdbToNvdb(const std::filesystem::path& vdbPath, const std::filesystem::path& outNvdbPath,
                                       const VolumeCookOptions& options)
    {
#if !NOX_HAS_OPENVDB
        (void)outNvdbPath;
        (void)options;
        NOX_CORE_WARN("VolumeImporter: cannot cook '{}' because OpenVDB is not linked (NOX_HAS_OPENVDB=0). "
                      "Install 'openvdb:x64-windows' via vcpkg and reconfigure CMake to enable .vdb cooking.",
                      vdbPath.string());
        return false;
#else
        static std::once_flag s_vdbInit;
        std::call_once(s_vdbInit, []() { openvdb::initialize(); });

        std::vector<std::filesystem::path> files = options.IncludeSequence
            ? FindVdbSequenceFiles(vdbPath)
            : std::vector<std::filesystem::path>{ vdbPath };
        if (files.empty())
        {
            NOX_CORE_ERROR("VolumeImporter: VDB file not found '{}'", vdbPath.string());
            return false;
        }
        if (options.MaxFrames > 0u && files.size() > options.MaxFrames)
            files.resize(options.MaxFrames);

        VolumeHeader header{};
        header.Fps = options.Fps;
        header.GridFormat = static_cast<uint32_t>(VolumeGridFormat::DenseFP16);
        header.ChannelFlags = VolumeChannel_None;
        header.AabbMin = glm::vec3(1e30f);
        header.AabbMax = glm::vec3(-1e30f);
        header.MaxDensity = 0.0f;
        header.MaxTemperature = 0.0f;

        std::vector<VolumeFrameCookData> cookedFrames;
        cookedFrames.reserve(files.size());

        const uint32_t maxDim = std::clamp(options.MaxDimension, 16u, 256u);

        for (const std::filesystem::path& filePath : files)
        {
            try
            {
                openvdb::io::File file(filePath.string());
                file.open();

                openvdb::FloatGrid::Ptr densityGrid;
                openvdb::FloatGrid::Ptr tempGrid;
                openvdb::FloatGrid::Ptr flameGrid;

                for (auto it = file.beginName(); it != file.endName(); ++it)
                {
                    const std::string name = *it;
                    const std::string lower = ToLowerCopy(name);
                    if (!densityGrid && (lower == "density" || lower.find("dens") != std::string::npos))
                        densityGrid = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid(name));
                    else if (!tempGrid && (lower == "temperature" || lower.find("temp") != std::string::npos))
                        tempGrid = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid(name));
                    else if (!flameGrid && (lower == "flames" || lower == "flame" || lower == "fuel" || lower == "heat"))
                        flameGrid = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid(name));
                }
                file.close();

                openvdb::FloatGrid::Ptr primary = densityGrid ? densityGrid : (flameGrid ? flameGrid : tempGrid);
                if (!primary)
                {
                    NOX_CORE_WARN("VolumeImporter: no float density/temperature/flame grid in '{}', skipping frame", filePath.string());
                    continue;
                }

                const openvdb::CoordBBox bbox = primary->evalActiveVoxelBoundingBox();
                if (bbox.empty())
                    continue;

                const openvdb::Vec3d worldMin = primary->indexToWorld(bbox.min().asVec3d());
                const openvdb::Vec3d worldMax = primary->indexToWorld((bbox.max() + openvdb::Coord(1)).asVec3d());
                const glm::vec3 fMin(static_cast<float>(worldMin.x()), static_cast<float>(worldMin.y()), static_cast<float>(worldMin.z()));
                const glm::vec3 fMax(static_cast<float>(worldMax.x()), static_cast<float>(worldMax.y()), static_cast<float>(worldMax.z()));
                header.AabbMin = glm::min(header.AabbMin, fMin);
                header.AabbMax = glm::max(header.AabbMax, fMax);

                const openvdb::Coord dims = bbox.dim();
                const int longest = std::max({ dims.x(), dims.y(), dims.z(), 1 });
                const float scale = longest > static_cast<int>(maxDim)
                    ? static_cast<float>(maxDim) / static_cast<float>(longest)
                    : 1.0f;

                VolumeFrameCookData frame{};
                frame.AabbMin = fMin;
                frame.AabbMax = fMax;
                frame.DimX = std::max(1u, static_cast<uint32_t>(std::round(dims.x() * scale)));
                frame.DimY = std::max(1u, static_cast<uint32_t>(std::round(dims.y() * scale)));
                frame.DimZ = std::max(1u, static_cast<uint32_t>(std::round(dims.z() * scale)));
                header.VoxelSize = glm::length(fMax - fMin) / std::max(static_cast<float>(frame.DimX + frame.DimY + frame.DimZ) / 3.0f, 1.0f);

                auto sampleGridToFP16 = [&](const openvdb::FloatGrid::Ptr& grid, std::vector<uint8_t>& outBytes, float& outMaxVal)
                {
                    if (!grid)
                        return;
                    const size_t totalVoxels = static_cast<size_t>(frame.DimX) * frame.DimY * frame.DimZ;
                    outBytes.resize(totalVoxels * sizeof(uint16_t));
                    uint16_t* dst = reinterpret_cast<uint16_t*>(outBytes.data());

                    openvdb::tools::GridSampler<openvdb::FloatGrid, openvdb::tools::BoxSampler> sampler(*grid);
                    const glm::vec3 extent = fMax - fMin;
                    for (uint32_t z = 0; z < frame.DimZ; ++z)
                    {
                        const float wz = fMin.z + (static_cast<float>(z) + 0.5f) / static_cast<float>(frame.DimZ) * extent.z;
                        for (uint32_t y = 0; y < frame.DimY; ++y)
                        {
                            const float wy = fMin.y + (static_cast<float>(y) + 0.5f) / static_cast<float>(frame.DimY) * extent.y;
                            for (uint32_t x = 0; x < frame.DimX; ++x)
                            {
                                const float wx = fMin.x + (static_cast<float>(x) + 0.5f) / static_cast<float>(frame.DimX) * extent.x;
                                const float val = std::max(sampler.wsSample(openvdb::Vec3d(wx, wy, wz)), 0.0f);
                                if (val > outMaxVal)
                                    outMaxVal = val;
                                dst[(static_cast<size_t>(z) * frame.DimY + y) * frame.DimX + x] = FloatToHalf(val);
                            }
                        }
                    }
                };

                if (densityGrid)
                {
                    header.ChannelFlags |= VolumeChannel_Density;
                    sampleGridToFP16(densityGrid, frame.DensityBytes, header.MaxDensity);
                }
                if (tempGrid)
                {
                    header.ChannelFlags |= VolumeChannel_Temperature;
                    sampleGridToFP16(tempGrid, frame.TemperatureBytes, header.MaxTemperature);
                }
                if (flameGrid)
                {
                    header.ChannelFlags |= VolumeChannel_Flame;
                    float flameMax = 0.0f;
                    sampleGridToFP16(flameGrid, frame.FlameBytes, flameMax);
                    header.MaxTemperature = std::max(header.MaxTemperature, flameMax);
                }

                cookedFrames.push_back(std::move(frame));
            }
            catch (const std::exception& e)
            {
                NOX_CORE_ERROR("VolumeImporter: failed reading '{}': {}", filePath.string(), e.what());
            }
        }

        if (cookedFrames.empty())
            return false;

        if (!VolumeAsset::Save(outNvdbPath, header, cookedFrames))
            return false;

        NOX_CORE_INFO("VolumeImporter: cooked {} frame(s) from '{}' -> '{}' (channels=0x{:X}, maxDensity={:.2f}, maxTemp={:.2f})",
                      cookedFrames.size(), vdbPath.filename().string(), outNvdbPath.filename().string(),
                      header.ChannelFlags, header.MaxDensity, header.MaxTemperature);
        return true;
#endif
    }

    bool VolumeImporter::GenerateProceduralTestNvdb(const std::filesystem::path& outNvdbPath, uint32_t frameCount, uint32_t dim)
    {
        frameCount = std::clamp(frameCount, 1u, 120u);
        dim = std::clamp(dim, 16u, 128u);

        VolumeHeader header{};
        header.Fps = 24.0f;
        header.ChannelFlags = VolumeChannel_Density | VolumeChannel_Temperature;
        header.GridFormat = static_cast<uint32_t>(VolumeGridFormat::DenseFP16);
        header.VoxelSize = 2.0f / static_cast<float>(dim);
        header.AabbMin = glm::vec3(-1.0f, 0.0f, -1.0f);
        header.AabbMax = glm::vec3(1.0f, 2.5f, 1.0f);
        header.MaxDensity = 1.0f;
        header.MaxTemperature = 1.0f;

        std::vector<VolumeFrameCookData> frames;
        frames.reserve(frameCount);

        const size_t total = static_cast<size_t>(dim) * dim * dim;
        for (uint32_t f = 0; f < frameCount; ++f)
        {
            const float phase = static_cast<float>(f) / static_cast<float>(frameCount) * 6.2831853f;
            VolumeFrameCookData frame{};
            frame.AabbMin = header.AabbMin;
            frame.AabbMax = header.AabbMax;
            frame.DimX = dim;
            frame.DimY = dim;
            frame.DimZ = dim;
            frame.DensityBytes.resize(total * sizeof(uint16_t));
            frame.TemperatureBytes.resize(total * sizeof(uint16_t));
            uint16_t* densDst = reinterpret_cast<uint16_t*>(frame.DensityBytes.data());
            uint16_t* tempDst = reinterpret_cast<uint16_t*>(frame.TemperatureBytes.data());

            for (uint32_t z = 0; z < dim; ++z)
            {
                const float nz = (static_cast<float>(z) + 0.5f) / static_cast<float>(dim) * 2.0f - 1.0f;
                for (uint32_t y = 0; y < dim; ++y)
                {
                    const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(dim); // 0..1 bottom to top
                    const float twist = ny * 5.0f - phase;
                    const float cx = std::cos(twist) * 0.12f * ny;
                    const float cz = std::sin(twist) * 0.12f * ny;
                    const float funnelRadius = 0.18f + 0.52f * ny;
                    for (uint32_t x = 0; x < dim; ++x)
                    {
                        const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(dim) * 2.0f - 1.0f;
                        const float dx = nx - cx;
                        const float dz = nz - cz;
                        const float r = std::sqrt(dx * dx + dz * dz);
                        const float ang = std::atan2(dz, dx) + twist * 1.5f;
                        const float wisp = 0.5f + 0.5f * std::sin(ang * 3.0f + ny * 8.0f - phase);
                        const float radialMask = std::max(0.0f, 1.0f - r / std::max(funnelRadius, 0.05f));
                        const float heightFade = std::sin(ny * 3.14159265f);
                        const float density = std::clamp(radialMask * radialMask * (0.6f + 0.4f * wisp) * heightFade * 1.5f, 0.0f, 1.0f);
                        const float temp = std::clamp(radialMask * (1.0f - ny * 0.75f) * (0.5f + 0.5f * wisp), 0.0f, 1.0f);

                        const size_t idx = (static_cast<size_t>(z) * dim + y) * dim + x;
                        densDst[idx] = FloatToHalf(density);
                        tempDst[idx] = FloatToHalf(temp);
                    }
                }
            }
            frames.push_back(std::move(frame));
        }
        return VolumeAsset::Save(outNvdbPath, header, frames);
    }

    Ref<VolumeAsset> VolumeImporter::LoadVolume(const std::filesystem::path& path)
    {
        const std::string ext = ToLowerCopy(path.extension().string());
        std::filesystem::path nvdbPath = path;

        if (ext == ".vdb")
        {
            nvdbPath = path;
            nvdbPath.replace_extension(".nvdb");
            std::error_code ec;
            const bool needsCook = !std::filesystem::exists(nvdbPath, ec) ||
                std::filesystem::last_write_time(path, ec) > std::filesystem::last_write_time(nvdbPath, ec);
            if (needsCook)
            {
                if (!CookVdbToNvdb(path, nvdbPath))
                    return {};
            }
        }

        Ref<VolumeAsset> volume = CreateRef<VolumeAsset>();
        if (!volume->LoadHeader(nvdbPath))
            return {};
        return volume;
    }

    Ref<VolumeAsset> VolumeImporter::ImportVolume(AssetHandle handle, const AssetMetadata& metadata)
    {
        const std::filesystem::path fullPath = Project::GetActiveAssetDirectory() / metadata.FilePath;
        Ref<VolumeAsset> volume = LoadVolume(fullPath);
        if (volume)
            volume->Handle = handle;
        return volume;
    }
}
