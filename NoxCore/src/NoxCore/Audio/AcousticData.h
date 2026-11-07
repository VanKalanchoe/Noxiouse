#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/core.h"

namespace Nox
{
    // How an Acoustic Probe Volume is baked (docs/Audio_Architecture_Plan_2026.md §11 "A4c as built"). The defaults are Steam Audio's
    // own (its Unity plugin's probe batch and baking settings).
    struct AcousticBakeSettings
    {
        float Spacing = 5.0f;        // meters between probes (uniform over the floors inside the volume)
        float Height = 1.5f;         // meters above the floor (an ear's height)
        bool Reverb = true;          // the reverb heard at each probe (the Acoustic Reverb effect's, where the listener is)
        bool Pathing = true;         // paths between probes (sounds around corners and through doors)
        int32_t Rays = 16384;        // reverb: rays traced from each probe
        int32_t Bounces = 16;        // reverb: reflections followed per ray (more: a longer tail)
        float Duration = 1.0f;       // reverb: seconds of response kept (up to SpatialAudio::MaxReflectionDuration)
        int32_t VisSamples = 4;      // pathing: points per probe tested for visibility between probes
        float VisRadius = 1.0f;      // pathing: meters around a probe those points are spread over
        float VisThreshold = 0.1f;   // pathing: share of the rays between two probes that must be clear for them to see each other
        float VisRange = 1000.0f;    // pathing: meters beyond which two probes are not tested
        float PathRange = 1000.0f;   // pathing: meters beyond which no path is kept
        bool operator==(const AcousticBakeSettings&) const = default;
    };

    // A volume's baked acoustics (.nacoustics): Steam Audio's probe batch (its probes, the reverb at each, the paths between them)
    // with the frame it was baked in. Probes are positions, so the data is good only where it was baked: Volume says where, and the
    // runtime uses it only while the acoustic origin is Origin (SpatialAudio::GridOrigin of the volume's center).
    class AcousticData : public Asset
    {
    public:
        static constexpr uint32_t Version = 1;

        glm::dvec3 Origin{ 0.0 };          // world units: the frame's origin (Steam Audio works in meters from here)
        glm::dmat4 Volume{ 1.0 };          // world units: the unit cube (-0.5 .. 0.5) onto the baked box
        uint32_t ProbeCount = 0;
        AcousticBakeSettings Settings;
        uint64_t BakedAt = 0;              // seconds since 1970
        std::vector<uint8_t> ProbeBatch;   // Steam Audio's serialized probe batch

        static AssetType GetStaticType() { return AssetType::AcousticData; }
        AssetType GetType() const override { return GetStaticType(); }

        // Inside the baked box (a world-units point)?
        bool Contains(const glm::dvec3& point) const;
    };

    bool SaveAcousticData(const std::filesystem::path& path, const AcousticData& data);
    // Through the VFS (a packaged game reads it from its pack). Null when missing, not a .nacoustics or a newer version.
    Ref<AcousticData> LoadAcousticData(const std::filesystem::path& path);
}
