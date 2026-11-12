#include "AudioGraph.h"

#include <algorithm>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/NodeGraph/NodeGraphAsset.h"

namespace Nox
{
    namespace
    {
        struct CachedProgram
        {
            uint32_t Version = UINT32_MAX;
            std::vector<std::pair<AssetHandle, uint32_t>> Dependencies; // the graphs its Audio Graph nodes play, at the versions built
            std::shared_ptr<const AudioGraphProgram> Program;
            std::string Error; // the build failed for this version
        };
        std::unordered_map<uint64_t, CachedProgram>& Cache()
        {
            static std::unordered_map<uint64_t, CachedProgram> cache;
            return cache;
        }
    }

    std::shared_ptr<const AudioGraphProgram> GetAudioGraphProgram(AssetHandle graph, std::string* outError)
    {
        if (graph == 0 || AssetManager::GetAssetType(graph) != AssetType::AudioGraph)
        {
            if (outError)
                *outError = "not an audio graph";
            return nullptr;
        }
        const AssetState state = AssetManager::RequestAsset(graph);
        if (state == AssetState::Failed)
        {
            if (outError)
                *outError = "the graph file could not be loaded";
            return nullptr;
        }
        const NodeGraphAsset* asset = state == AssetState::Ready ? AssetManager::FindLoadedAsset<NodeGraphAsset>(graph) : nullptr;
        if (!asset)
            return nullptr; // (loading)

        CachedProgram& cached = Cache()[graph];
        // Current while neither it nor a graph it plays (an Audio Graph node's) was edited since.
        const bool dependenciesCurrent = std::all_of(cached.Dependencies.begin(), cached.Dependencies.end(), [](const auto& dependency)
        {
            const NodeGraphAsset* played = AssetManager::FindLoadedAsset<NodeGraphAsset>(dependency.first);
            return played && played->CompileVersion == dependency.second;
        });
        if (cached.Version == asset->CompileVersion && dependenciesCurrent)
        {
            if (outError)
                *outError = cached.Error;
            return cached.Program;
        }
        std::vector<AssetHandle> missing;
        std::string error;
        std::vector<std::pair<AssetHandle, uint32_t>> dependencies;
        std::shared_ptr<AudioGraphProgram> program = AudioGraphProgram::Build(asset->Graph, missing, error, &dependencies);
        if (!missing.empty())
            return nullptr; // its clips / the graphs it plays are loading (asked for by the build); built on a later call
        cached.Version = asset->CompileVersion;
        cached.Dependencies = std::move(dependencies);
        cached.Program = program;
        cached.Error = program ? std::string() : error;
        if (!program)
            NOX_CORE_WARN("Audio graph {}: cannot play it: {}", static_cast<uint64_t>(graph), error);
        if (outError)
            *outError = cached.Error;
        return cached.Program;
    }
}
