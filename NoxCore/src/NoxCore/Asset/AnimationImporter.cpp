#include "AnimationImporter.h"
#include "NoxCore/Core/VirtualFileSystem.h"

#include "MeshSerializer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"

#include <algorithm>
#include <fstream>
#include <yaml-cpp/yaml.h>

namespace Nox
{
    Ref<AnimationSequence> AnimationImporter::ImportAnimation(AssetHandle handle, const AssetMetadata& metadata)
    {
        std::filesystem::path cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;

        NOX_CORE_INFO("AnimationImporter::ImportAnimation loading animation from {}", cookedPath.string());

        Ref<AnimationSequence> animAsset = LoadAnimation(cookedPath);
        if (animAsset)
        {
            animAsset->Handle = handle;
        }

        return animAsset;
    }

    Ref<AnimationSequence> AnimationImporter::LoadAnimation(const std::filesystem::path& path)
    {
        if (!VFS::Exists(path))
        {
            NOX_CORE_ERROR("AnimationImporter::LoadAnimation - File does not exist: {}", path.string());
            return {};
        }

        Ref<AnimationSequence> anim = CreateRef<AnimationSequence>();
        bool success = AnimationSerializer::Deserialize(path, *anim);
        if (!success)
        {
            NOX_CORE_ERROR("AnimationImporter::LoadAnimation - Failed to deserialize .nanim file: {}", path.string());
            return {};
        }
        anim->Notifies = LoadNotifies(path);

        return anim;
    }

    std::filesystem::path AnimationImporter::NotifiesPath(const std::filesystem::path& clipPath)
    {
        return std::filesystem::path(clipPath.string() + ".notifies");
    }

    std::vector<AnimationNotify> AnimationImporter::LoadNotifies(const std::filesystem::path& clipPath)
    {
        std::vector<AnimationNotify> notifies;
        const std::filesystem::path path = NotifiesPath(clipPath);
        std::error_code error;
        if (!VFS::Exists(path))
            return notifies;
        try
        {
            const YAML::Node root = YAML::Load(VFS::ReadTextOrEmpty(path));
            for (const YAML::Node& entry : root["Notifies"])
            {
                AnimationNotify notify;
                notify.Time = entry["Time"] ? entry["Time"].as<float>() : 0.0f;
                notify.Name = entry["Name"] ? entry["Name"].as<std::string>() : std::string();
                notify.SyncMarker = entry["Sync"] && entry["Sync"].as<bool>();
                if (const YAML::Node sound = entry["Sound"])
                {
                    notify.Sound = sound["Asset"].as<uint64_t>(0);
                    notify.Bone = sound["Bone"].as<std::string>(std::string());
                    notify.TraceSurface = sound["TraceSurface"].as<bool>(false);
                    notify.TraceDistance = sound["TraceDistance"].as<float>(notify.TraceDistance);
                    notify.Volume = sound["Volume"].as<float>(notify.Volume);
                    notify.PitchVariation = sound["PitchVariation"].as<float>(notify.PitchVariation);
                    for (const YAML::Node& surface : sound["Surfaces"])
                        notify.SurfaceSounds.push_back({ surface["Surface"].as<std::string>(std::string()), AssetHandle(surface["Asset"].as<uint64_t>(0)) });
                }
                if (!notify.Name.empty())
                    notifies.push_back(std::move(notify));
            }
        }
        catch (const YAML::Exception& exception)
        {
            NOX_CORE_WARN("AnimationImporter: could not read the notifies of {}: {}", clipPath.string(), exception.what());
        }
        std::sort(notifies.begin(), notifies.end(), [](const AnimationNotify& a, const AnimationNotify& b) { return a.Time < b.Time; });
        return notifies;
    }

    bool AnimationImporter::SaveNotifies(const std::filesystem::path& clipPath, const std::vector<AnimationNotify>& notifies)
    {
        const std::filesystem::path path = NotifiesPath(clipPath);
        std::error_code error;
        if (notifies.empty())
        {
            std::filesystem::remove(path, error);
            return true;
        }
        YAML::Emitter out;
        out << YAML::BeginMap << YAML::Key << "Notifies" << YAML::Value << YAML::BeginSeq;
        for (const AnimationNotify& notify : notifies)
        {
            out << YAML::BeginMap << YAML::Key << "Time" << YAML::Value << notify.Time << YAML::Key << "Name" << YAML::Value << notify.Name;
            if (notify.SyncMarker)
                out << YAML::Key << "Sync" << YAML::Value << true;
            if (notify.HasSound())
            {
                out << YAML::Key << "Sound" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "Asset" << YAML::Value << static_cast<uint64_t>(notify.Sound);
                if (!notify.Bone.empty())
                    out << YAML::Key << "Bone" << YAML::Value << notify.Bone;
                out << YAML::Key << "TraceSurface" << YAML::Value << notify.TraceSurface;
                out << YAML::Key << "TraceDistance" << YAML::Value << notify.TraceDistance;
                out << YAML::Key << "Volume" << YAML::Value << notify.Volume;
                out << YAML::Key << "PitchVariation" << YAML::Value << notify.PitchVariation;
                if (!notify.SurfaceSounds.empty())
                {
                    out << YAML::Key << "Surfaces" << YAML::Value << YAML::BeginSeq;
                    for (const AnimationNotify::SurfaceSound& surface : notify.SurfaceSounds)
                        out << YAML::Flow << YAML::BeginMap << YAML::Key << "Surface" << YAML::Value << surface.Surface << YAML::Key << "Asset"
                            << YAML::Value << static_cast<uint64_t>(surface.Sound) << YAML::EndMap;
                    out << YAML::EndSeq;
                }
                out << YAML::EndMap;
            }
            out << YAML::EndMap;
        }
        out << YAML::EndSeq << YAML::EndMap;
        std::ofstream stream(path);
        stream << out.c_str();
        return static_cast<bool>(stream);
    }
}