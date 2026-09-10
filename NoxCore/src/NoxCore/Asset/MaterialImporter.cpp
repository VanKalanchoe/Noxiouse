#include "MaterialImporter.h"

#include "MaterialSerializer.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    Ref<Material> MaterialImporter::ImportMaterial(AssetHandle handle, const AssetMetadata& metadata)
    {
        auto material = CreateRef<Material>();
        material->Handle = handle;

        const auto path = Project::GetActiveAssetDirectory() / metadata.FilePath;
        if (!MaterialSerializer::Deserialize(path, material->GetData()))
            return {};

        return material;
    }
}
