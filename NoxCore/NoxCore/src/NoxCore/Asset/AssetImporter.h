#pragma once
#include "Asset.h"
#include "AssetMetadata.h"
#include "NoxCore/Core/core.h"

namespace Nox
{
    class AssetImporter
    {
    public:
        static Ref<Asset> ImportAsset(AssetHandle handle, const AssetMetadata& metadata);
    };
}
