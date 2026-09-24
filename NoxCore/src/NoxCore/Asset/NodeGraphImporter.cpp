#include "NodeGraphImporter.h"

#include "NoxCore/Asset/AssetMetadata.h"
#include "NoxCore/Asset/NodeGraphSerializer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    Ref<NodeGraphAsset> NodeGraphImporter::ImportNodeGraph(AssetHandle handle, const AssetMetadata& metadata)
    {
        std::filesystem::path cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;

        NOX_CORE_INFO("NodeGraphImporter::ImportNodeGraph loading graph from {}", cookedPath.string());

        Ref<NodeGraphAsset> graphAsset = LoadNodeGraph(cookedPath);
        if (graphAsset)
        {
            graphAsset->Handle = handle;
        }

        return graphAsset;
    }

    Ref<NodeGraphAsset> NodeGraphImporter::LoadNodeGraph(const std::filesystem::path& path)
    {
        if (!std::filesystem::exists(path))
        {
            NOX_CORE_ERROR("NodeGraphImporter::LoadNodeGraph - File does not exist: {}", path.string());
            return {};
        }

        Ref<NodeGraphAsset> graph = CreateRef<NodeGraphAsset>();
        if (!NodeGraphSerializer::Deserialize(path, graph->Graph))
        {
            NOX_CORE_ASSERT(false, "NodeGraphImporter::LoadNodeGraph - Failed to deserialize .nanimgraph file: {}", path.string());
            return {};
        }

        graph->Recompile();
        return graph;
    }
}
