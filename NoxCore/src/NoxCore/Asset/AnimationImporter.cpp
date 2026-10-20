#include "AnimationImporter.h"

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
        if (!std::filesystem::exists(path))
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
        if (!std::filesystem::exists(path, error))
            return notifies;
        try
        {
            const YAML::Node root = YAML::LoadFile(path.string());
            for (const YAML::Node& entry : root["Notifies"])
            {
                AnimationNotify notify;
                notify.Time = entry["Time"] ? entry["Time"].as<float>() : 0.0f;
                notify.Name = entry["Name"] ? entry["Name"].as<std::string>() : std::string();
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
            out << YAML::BeginMap << YAML::Key << "Time" << YAML::Value << notify.Time << YAML::Key << "Name" << YAML::Value << notify.Name << YAML::EndMap;
        out << YAML::EndSeq << YAML::EndMap;
        std::ofstream stream(path);
        stream << out.c_str();
        return static_cast<bool>(stream);
    }
}