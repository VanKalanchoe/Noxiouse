#include "TimelineImporter.h"
#include "NoxCore/Core/VirtualFileSystem.h"

#include <fstream>
#include <yaml-cpp/yaml.h>

#include "NoxCore/Asset/AssetMetadata.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    namespace
    {
        const char* InterpolationName(TimelineInterpolation interpolation)
        {
            switch (interpolation)
            {
            case TimelineInterpolation::Constant: return "Constant";
            case TimelineInterpolation::Linear: return "Linear";
            default: return "Smooth";
            }
        }

        TimelineInterpolation ParseInterpolation(const std::string& name)
        {
            if (name == "Constant")
                return TimelineInterpolation::Constant;
            if (name == "Linear")
                return TimelineInterpolation::Linear;
            return TimelineInterpolation::Smooth;
        }

        TimelineProperty ParseProperty(const std::string& name)
        {
            for (uint8_t index = 0; index < static_cast<uint8_t>(TimelineProperty::Count); ++index)
            {
                const TimelineProperty property = static_cast<TimelineProperty>(index);
                if (name == TimelineEvaluation::GetPropertyName(property))
                    return property;
            }
            return TimelineProperty::Count;
        }
    }

    Ref<Timeline> TimelineImporter::ImportTimeline(AssetHandle handle, const AssetMetadata& metadata)
    {
        Ref<Timeline> timeline = CreateRef<Timeline>();
        if (!Deserialize(Project::GetActiveAssetDirectory() / metadata.FilePath, *timeline))
            return {};
        timeline->Handle = handle;
        return timeline;
    }

    bool TimelineImporter::Serialize(const std::filesystem::path& path, const Timeline& timeline)
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Timeline" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Duration" << YAML::Value << timeline.Duration;
        out << YAML::Key << "Loop" << YAML::Value << timeline.Loop;
        out << YAML::Key << "Tracks" << YAML::Value << YAML::BeginSeq;
        for (const TimelineTrack& track : timeline.Tracks)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Target" << YAML::Value << track.Target;
            out << YAML::Key << "Property" << YAML::Value << TimelineEvaluation::GetPropertyName(track.Property);
            if (track.Muted)
                out << YAML::Key << "Muted" << YAML::Value << true;
            out << YAML::Key << "Keys" << YAML::Value << YAML::BeginSeq;
            for (const TimelineKey& key : track.Keys)
            {
                out << YAML::Flow << YAML::BeginMap;
                out << YAML::Key << "Time" << YAML::Value << key.Time;
                if (TimelineEvaluation::HasNamedKeys(track.Property))
                    out << YAML::Key << "Event" << YAML::Value << key.Event;
                else
                {
                    out << YAML::Key << "Value" << YAML::Value << YAML::Flow << YAML::BeginSeq << key.Value.x << key.Value.y << key.Value.z << key.Value.w << YAML::EndSeq;
                    out << YAML::Key << "Interpolation" << YAML::Value << InterpolationName(key.Interpolation);
                }
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        out << YAML::EndMap;
        out << YAML::EndMap;

        std::ofstream file(path);
        if (!file)
        {
            NOX_CORE_ERROR("Could not write the timeline {}", path.string());
            return false;
        }
        file << out.c_str();
        return true;
    }

    bool TimelineImporter::Deserialize(const std::filesystem::path& path, Timeline& timeline)
    {
        YAML::Node data;
        try
        {
            data = YAML::Load(VFS::ReadTextOrEmpty(path));
        }
        catch (const YAML::Exception& error)
        {
            NOX_CORE_ERROR("Could not read the timeline {}: {}", path.string(), error.what());
            return false;
        }

        const YAML::Node root = data["Timeline"];
        if (!root)
            return false;
        timeline.Duration = root["Duration"] ? std::max(root["Duration"].as<float>(), 0.01f) : 5.0f;
        timeline.Loop = root["Loop"] && root["Loop"].as<bool>();
        timeline.Tracks.clear();
        for (const YAML::Node& trackNode : root["Tracks"])
        {
            TimelineTrack track;
            track.Target = trackNode["Target"] ? trackNode["Target"].as<std::string>() : std::string();
            track.Property = ParseProperty(trackNode["Property"] ? trackNode["Property"].as<std::string>() : std::string());
            if (track.Property == TimelineProperty::Count)
                continue; // a property this version does not know
            track.Muted = trackNode["Muted"] && trackNode["Muted"].as<bool>();
            for (const YAML::Node& keyNode : trackNode["Keys"])
            {
                TimelineKey key;
                key.Time = keyNode["Time"] ? keyNode["Time"].as<float>() : 0.0f;
                if (const YAML::Node value = keyNode["Value"]; value && value.IsSequence())
                {
                    for (size_t component = 0; component < 4 && component < value.size(); ++component)
                        key.Value[static_cast<int>(component)] = value[component].as<float>();
                }
                if (keyNode["Interpolation"])
                    key.Interpolation = ParseInterpolation(keyNode["Interpolation"].as<std::string>());
                if (keyNode["Event"])
                    key.Event = keyNode["Event"].as<std::string>();
                track.Keys.push_back(std::move(key));
            }
            TimelineEvaluation::SortKeys(track);
            timeline.Tracks.push_back(std::move(track));
        }
        return true;
    }
}
