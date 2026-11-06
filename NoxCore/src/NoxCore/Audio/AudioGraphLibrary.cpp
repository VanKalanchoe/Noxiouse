#include "AudioGraph.h"

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
        if (cached.Version == asset->CompileVersion)
        {
            if (outError)
                *outError = cached.Error;
            return cached.Program;
        }
        std::vector<AssetHandle> missing;
        std::string error;
        std::shared_ptr<AudioGraphProgram> program = AudioGraphProgram::Build(asset->Graph, missing, error);
        if (!missing.empty())
            return nullptr; // its clips are loading (asked for by the build); built on a later call
        cached.Version = asset->CompileVersion;
        cached.Program = program;
        cached.Error = program ? std::string() : error;
        if (!program)
            NOX_CORE_WARN("Audio graph {}: cannot play it: {}", static_cast<uint64_t>(graph), error);
        if (outError)
            *outError = cached.Error;
        return cached.Program;
    }
}
