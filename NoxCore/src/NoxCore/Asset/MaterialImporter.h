#pragma once

#include "Material.h"
#include "AssetMetadata.h"

namespace Nox
{
    class MaterialImporter
    {
    public:
        static Ref<Material> ImportMaterial(AssetHandle handle, const AssetMetadata& metadata);
    };
}
