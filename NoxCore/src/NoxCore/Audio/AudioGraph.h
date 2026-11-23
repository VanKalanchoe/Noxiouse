#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <NoxAudio/AudioGraph.h>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/NodeGraph/NodeGraph.h"
#include "AudioClip.h"

namespace Nox
{
    using NoxAudio::kAudioGraphDomain;
    using NoxAudio::kAudioGraphBlockFrames;
    using NoxAudio::kMaxTriggersPerBlock;
    using NoxAudio::AudioPinType;
    using NoxAudio::AudioTriggers;
    using NoxAudio::AudioPinTypeFromName;
    using NoxAudio::AudioGraphProgram;
    using NoxAudio::AudioGraphInstance;

    std::shared_ptr<const AudioGraphProgram> GetAudioGraphProgram(AssetHandle graph, std::string* outError = nullptr);
    void CollectAudioGraphClips(const NodeGraph& graph, std::vector<AssetHandle>& outClips);
    void RegisterAudioGraphNodeTypes();
    NodeGraph CreateEmptyAudioGraph();
}
