#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <NoxAudio/SpatialAudio.h>

#include "NoxCore/Asset/Asset.h"
#include "AcousticData.h"
#include <NoxAudio/AudioTypes.h>

namespace Nox
{
    using NoxAudio::AcousticMesh;
    using NoxAudio::AcousticMeshRead;
    using NoxAudio::ResolveAcousticMaterial;

    AcousticMeshRead StartAcousticMeshRead(AssetHandle mesh, uint32_t firstSubmesh, uint32_t submeshCount, std::vector<NoxAudio::AcousticMaterial> materials);

    class SpatialAudio
    {
    public:
        using Instance = NoxAudio::SpatialAudio::Instance;
        using SceneStats = NoxAudio::SpatialAudio::SceneStats;
        using SourceInputs = NoxAudio::SpatialAudio::SourceInputs;
        using SourceOutputs = NoxAudio::SpatialAudio::SourceOutputs;
        using ListenerReverb = NoxAudio::SpatialAudio::ListenerReverb;
        using ReflectionSettings = NoxAudio::SpatialAudio::ReflectionSettings;
        using PathingRequest = NoxAudio::SpatialAudio::PathingRequest;
        using PathingOutputs = NoxAudio::SpatialAudio::PathingOutputs;
        using ProbeStats = NoxAudio::SpatialAudio::ProbeStats;

        static constexpr int32_t MaxReflectionSources = NoxAudio::SpatialAudio::MaxReflectionSources;
        static constexpr int32_t MaxPathingSources = NoxAudio::SpatialAudio::MaxPathingSources;
        static constexpr float MaxReflectionDuration = NoxAudio::SpatialAudio::MaxReflectionDuration;
        static constexpr int32_t ReflectionOrder = NoxAudio::SpatialAudio::ReflectionOrder;
        static constexpr int32_t ReflectionChannels = NoxAudio::SpatialAudio::ReflectionChannels;
        static constexpr double OriginGridMeters = NoxAudio::SpatialAudio::OriginGridMeters;

        static bool Init(uint32_t sampleRate, uint32_t frameSize) { return NoxAudio::SpatialAudio::Init(sampleRate, frameSize); }
        static void Shutdown() { NoxAudio::SpatialAudio::Shutdown(); }
        static bool IsAvailable() { return NoxAudio::SpatialAudio::IsAvailable(); }
        static IPLContext Context() { return NoxAudio::SpatialAudio::Context(); }
        static IPLHRTF Hrtf() { return NoxAudio::SpatialAudio::Hrtf(); }

        static void SetInstance(uint64_t key, const Instance& instance) { NoxAudio::SpatialAudio::SetInstance(key, instance); }
        static void RemoveInstance(uint64_t key) { NoxAudio::SpatialAudio::RemoveInstance(key); }
        static void ClearInstances() { NoxAudio::SpatialAudio::ClearInstances(); }
        static void SetMeshInstance(uint64_t key, std::shared_ptr<const AcousticMesh> mesh, const glm::dmat4& world) { NoxAudio::SpatialAudio::SetMeshInstance(key, std::move(mesh), world); }
        static void UpdateScene(const glm::dvec3& listener) { NoxAudio::SpatialAudio::UpdateScene(listener); }
        static SceneStats GetSceneStats() { return NoxAudio::SpatialAudio::GetSceneStats(); }
        static void MaterialsChanged() { NoxAudio::SpatialAudio::MaterialsChanged(); }
        static uint32_t GetMaterialVersion() { return NoxAudio::SpatialAudio::GetMaterialVersion(); }

        static IPLSource CreateSource() { return NoxAudio::SpatialAudio::CreateSource(); }
        static void ReleaseSource(IPLSource& source) { NoxAudio::SpatialAudio::ReleaseSource(source); }
        static void SetListener(const glm::dvec3& position, const glm::quat& orientation) { NoxAudio::SpatialAudio::SetListener(position, orientation); }
        static void SetSourceInputs(IPLSource source, const SourceInputs& inputs) { NoxAudio::SpatialAudio::SetSourceInputs(source, inputs); }
        static void RunDirect() { NoxAudio::SpatialAudio::RunDirect(); }
        static SourceOutputs GetSourceOutputs(IPLSource source) { return NoxAudio::SpatialAudio::GetSourceOutputs(source); }

        static void SetReflectionSettings(const ReflectionSettings& settings) { NoxAudio::SpatialAudio::SetReflectionSettings(settings); }
        static ReflectionSettings GetReflectionSettings() { return NoxAudio::SpatialAudio::GetReflectionSettings(); }
        static bool ReflectionsIdle() { return NoxAudio::SpatialAudio::ReflectionsIdle(); }
        static bool TakeReflectionResults() { return NoxAudio::SpatialAudio::TakeReflectionResults(); }
        static bool StartSimulation(const std::vector<std::pair<int32_t, glm::dvec3>>& reflections, const std::vector<PathingRequest>& pathing) { return NoxAudio::SpatialAudio::StartSimulation(reflections, pathing); }
        static bool GetReflectionParams(int32_t slot, IPLReflectionEffectParams& out) { return NoxAudio::SpatialAudio::GetReflectionParams(slot, out); }
        static bool GetListenerReverbParams(IPLReflectionEffectParams& out) { return NoxAudio::SpatialAudio::GetListenerReverbParams(out); }
        static IPLSource ReflectionSource(int32_t slot) { return NoxAudio::SpatialAudio::ReflectionSource(slot); }
        static IPLSource ListenerReverbSource() { return NoxAudio::SpatialAudio::ListenerReverbSource(); }
        static PathingOutputs GetPathingOutputs(int32_t slot) { return NoxAudio::SpatialAudio::GetPathingOutputs(slot); }
        static float GetLastReflectionMilliseconds() { return NoxAudio::SpatialAudio::GetLastReflectionMilliseconds(); }

        static glm::dvec3 GridOrigin(const glm::dvec3& world) { return NoxAudio::SpatialAudio::GridOrigin(world); }
        static void SetProbeBatch(uint64_t key, Ref<AcousticData> data);
        static void RemoveProbeBatch(uint64_t key) { NoxAudio::SpatialAudio::RemoveProbeBatch(key); }
        static void ClearProbeBatches() { NoxAudio::SpatialAudio::ClearProbeBatches(); }
        static ProbeStats GetProbeStats() { return NoxAudio::SpatialAudio::GetProbeStats(); }
        static IPLVector3 ToAcoustic(const glm::dvec3& world) { return NoxAudio::SpatialAudio::ToAcoustic(world); }
    };
}
