#pragma once

#include <filesystem>

#include "NoxCore/Animation/Timeline.h"

namespace Nox
{
    struct AssetMetadata;

    // Sequencer timelines (.ntimeline, YAML).
    class TimelineImporter
    {
    public:
        static Ref<Timeline> ImportTimeline(AssetHandle handle, const AssetMetadata& metadata);
        static bool Serialize(const std::filesystem::path& path, const Timeline& timeline);
        static bool Deserialize(const std::filesystem::path& path, Timeline& timeline);
    };
}
