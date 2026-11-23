#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <glm/glm.hpp>
#include <NoxAudio/AcousticData.h>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/core.h"
#include <NoxAudio/AudioTypes.h>

namespace Nox
{
    using NoxAudio::AcousticInstance;
    using NoxAudio::AcousticBakeSettings;

    class AcousticData : public Asset
    {
    public:
        static constexpr uint32_t Version = NoxAudio::AcousticData::Version;

        glm::dvec3 Origin{ 0.0 };
        glm::dmat4 Volume{ 1.0 };
        uint32_t ProbeCount = 0;
        std::vector<glm::vec3> ProbePositions;
        AcousticBakeSettings Settings;
        uint64_t BakedAt = 0;
        std::vector<uint8_t> ProbeBatch;

        static AssetType GetStaticType() { return AssetType::AcousticData; }
        AssetType GetType() const override { return GetStaticType(); }

        bool Contains(const glm::dvec3& point) const;
    };

    bool SaveAcousticData(const std::filesystem::path& path, const AcousticData& data);
    Ref<AcousticData> LoadAcousticData(const std::filesystem::path& path);
}
