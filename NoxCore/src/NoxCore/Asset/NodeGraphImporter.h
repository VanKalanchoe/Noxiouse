#pragma once

#include <filesystem>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/NodeGraph/NodeGraphAsset.h"

namespace Nox
{
    struct AssetMetadata;

    class NodeGraphImporter
    {
    public:
        // AssetMetadata FilePath is relative to project asset directory (.nanimgraph)
        static Ref<NodeGraphAsset> ImportNodeGraph(AssetHandle handle, const AssetMetadata& metadata);

        // Direct loader from absolute/relative disk path
        static Ref<NodeGraphAsset> LoadNodeGraph(const std::filesystem::path& path);
    };
}
