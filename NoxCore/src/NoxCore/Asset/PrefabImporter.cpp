#include "PrefabImporter.h"

#include <fstream>

#include <yaml-cpp/yaml.h>

#include "NoxCore/Asset/AssetMetadata.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/SceneSerializer.h"

namespace Nox
{
    Ref<Prefab> PrefabImporter::ImportPrefab(AssetHandle handle, const AssetMetadata& metadata)
    {
        const std::filesystem::path path = Project::GetActiveAssetDirectory() / metadata.FilePath;
        NOX_CORE_INFO("PrefabImporter::ImportPrefab loading prefab from {}", path.string());

        YAML::Node data;
        try
        {
            data = YAML::LoadFile(path.string());
        }
        catch (const YAML::Exception& error)
        {
            NOX_CORE_ERROR("Failed to load .nprefab file '{}': {}", path.string(), error.what());
            return {};
        }

        if (!data["Prefab"] || !data["Entities"] || !data["Root"])
        {
            NOX_CORE_ERROR("'{}' is not a prefab file (needs Prefab, Root and Entities)", path.string());
            return {};
        }

        Ref<Prefab> prefab = CreateRef<Prefab>();
        prefab->Handle = handle;
        prefab->Name = data["Prefab"].as<std::string>();
        prefab->Root = data["Root"].as<uint64_t>();
        prefab->Entities = std::make_shared<YAML::Node>(data["Entities"]);
        if (data["Base"])
        {
            // A variant: the base, and its changes to it.
            prefab->Base = data["Base"].as<uint64_t>();
            SceneSerializer::ReadPrefabChanges(data, prefab->Overrides, prefab->Structure, prefab->Nested);
        }
        return prefab;
    }

    bool PrefabImporter::SavePrefab(const Prefab& prefab, const std::filesystem::path& absolutePath)
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Prefab" << YAML::Value << prefab.Name;
        out << YAML::Key << "Root" << YAML::Value << prefab.Root;
        if (prefab.Base != 0)
        {
            out << YAML::Key << "Base" << YAML::Value << static_cast<uint64_t>(prefab.Base);
            SceneSerializer::WritePrefabChanges(out, prefab.Overrides, prefab.Structure, prefab.Nested);
        }
        out << YAML::Key << "Entities" << YAML::Value;
        if (prefab.Entities)
            out << *prefab.Entities;
        else
            out << YAML::BeginSeq << YAML::EndSeq;
        out << YAML::EndMap;

        std::ofstream fout(absolutePath);
        if (!fout)
            return false;
        fout << out.c_str();
        fout.close();
        return static_cast<bool>(fout);
    }

    bool PrefabImporter::CreateVariantFile(const std::string& name, uint64_t baseRoot, AssetHandle base, const std::filesystem::path& absolutePath)
    {
        Prefab variant;
        variant.Name = name;
        variant.Root = baseRoot;
        variant.Base = base;
        return SavePrefab(variant, absolutePath);
    }
}
