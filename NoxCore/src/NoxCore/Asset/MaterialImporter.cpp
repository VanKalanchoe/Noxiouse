#include "MaterialImporter.h"

#include "MaterialSerializer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    Ref<Material> MaterialImporter::ImportMaterial(AssetHandle handle, const AssetMetadata& metadata)
    {
        auto material = CreateRef<Material>();
        material->Handle = handle;

        const auto path = Project::GetActiveAssetDirectory() / metadata.FilePath;
        if (!MaterialSerializer::Deserialize(path, material->GetData()))
        {
            // Unreadable, or written in an older layout: deleting the file has the model's import write it again.
            NOX_CORE_ERROR("MaterialImporter: cannot read {} (missing or an older material format: delete it to have it made again)",
                           path.generic_string());
            return {};
        }

        return material;
    }
}
