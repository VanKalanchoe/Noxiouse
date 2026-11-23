#include "AcousticBake.h"
#include "AcousticData.h"
#include "AudioClip.h"
#include "AudioEngine.h"
#include "AudioGraph.h"
#include "AudioMixer.h"
#include "SpatialAudio.h"

#include <mutex>

#include <NoxAudio/AudioHooks.h>
#include <NoxAudio/AudioLog.h>
#include <NoxAudio/NodeGraphAsset.h>
#include <NoxAudio/NodeType.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/VirtualFileSystem.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/NodeGraph/NodeGraphAsset.h"
#include "NoxCore/NodeGraph/NodeType.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Project/Project.h"
#include <NoxTasks/JobSystem.h>

namespace Nox
{
    namespace
    {
        NoxAudio::NodeGraphValue ToNoxAudioValue(const NodeGraphValue& v)
        {
            return std::visit([](const auto& val) -> NoxAudio::NodeGraphValue
            {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, NodeGraphCurve>)
                    return NoxAudio::NodeGraphCurve{ val.Keys, val.Smooth };
                else
                    return val;
            }, v);
        }

        NodeGraphValue FromNoxAudioValue(const NoxAudio::NodeGraphValue& v)
        {
            return std::visit([](const auto& val) -> NodeGraphValue
            {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, NoxAudio::NodeGraphCurve>)
                    return NodeGraphCurve{ val.Keys, val.Smooth };
                else
                    return val;
            }, v);
        }

        NoxAudio::NodeGraph ToNoxAudioGraph(const NodeGraph& src)
        {
            NoxAudio::NodeGraph dst;
            dst.Domain = src.Domain;
            dst.OutputNode = src.OutputNode;
            dst.NextNodeId = src.NextNodeId;
            dst.Nodes.reserve(src.Nodes.size());
            for (const GraphNode& n : src.Nodes)
            {
                NoxAudio::GraphNode gn;
                gn.Id = n.Id;
                gn.TypeName = n.TypeName;
                gn.EditorPosition = n.EditorPosition;
                for (const auto& [k, v] : n.Properties)
                    gn.Properties.emplace(k, ToNoxAudioValue(v));
                for (const NodeSubGraph& sg : n.SubGraphs)
                    gn.SubGraphs.push_back({ sg.Id, sg.Name, sg.EditorPosition, ToNoxAudioGraph(sg.Graph) });
                for (const NodeTransition& tr : n.Transitions)
                {
                    NoxAudio::NodeTransition nt;
                    nt.FromState = tr.FromState;
                    nt.ToState = tr.ToState;
                    nt.Duration = tr.Duration;
                    nt.EaseInOut = tr.EaseInOut;
                    nt.AtStateEnd = tr.AtStateEnd;
                    for (const TransitionRule& r : tr.Rules)
                        nt.Rules.push_back({ r.Parameter, static_cast<NoxAudio::TransitionCompare>(r.Compare), ToNoxAudioValue(r.Value) });
                    gn.Transitions.push_back(std::move(nt));
                }
                dst.Nodes.push_back(std::move(gn));
            }
            dst.Links.reserve(src.Links.size());
            for (const GraphLink& l : src.Links)
                dst.Links.push_back({ l.FromNode, l.FromPin, l.ToNode, l.ToPin, l.Route });
            dst.Parameters.reserve(src.Parameters.size());
            for (const GraphParameter& p : src.Parameters)
                dst.Parameters.push_back({ p.Name, ToNoxAudioValue(p.DefaultValue), p.IsTrigger });
            return dst;
        }

        NodeGraph FromNoxAudioGraph(const NoxAudio::NodeGraph& src)
        {
            NodeGraph dst;
            dst.Domain = src.Domain;
            dst.OutputNode = src.OutputNode;
            dst.NextNodeId = src.NextNodeId;
            for (const NoxAudio::GraphNode& n : src.Nodes)
            {
                GraphNode gn;
                gn.Id = n.Id;
                gn.TypeName = n.TypeName;
                gn.EditorPosition = n.EditorPosition;
                for (const auto& [k, v] : n.Properties)
                    gn.Properties.emplace(k, FromNoxAudioValue(v));
                dst.Nodes.push_back(std::move(gn));
            }
            for (const NoxAudio::GraphLink& l : src.Links)
                dst.Links.push_back({ l.FromNode, l.FromPin, l.ToNode, l.ToPin, l.Route });
            for (const NoxAudio::GraphParameter& p : src.Parameters)
                dst.Parameters.push_back({ p.Name, FromNoxAudioValue(p.DefaultValue), p.IsTrigger });
            return dst;
        }

        NoxAudio::Ref<NoxAudio::AcousticData> ToNoxAudioAcoustic(const AcousticData& src)
        {
            auto dst = NoxAudio::CreateRef<NoxAudio::AcousticData>();
            dst->Origin = src.Origin;
            dst->Volume = src.Volume;
            dst->ProbeCount = src.ProbeCount;
            dst->ProbePositions = src.ProbePositions;
            dst->Settings = src.Settings;
            dst->BakedAt = src.BakedAt;
            dst->ProbeBatch = src.ProbeBatch;
            return dst;
        }

        Ref<AcousticData> FromNoxAudioAcoustic(const NoxAudio::Ref<NoxAudio::AcousticData>& src)
        {
            if (!src)
                return nullptr;
            Ref<AcousticData> dst = CreateRef<AcousticData>();
            dst->Origin = src->Origin;
            dst->Volume = src->Volume;
            dst->ProbeCount = src->ProbeCount;
            dst->ProbePositions = src->ProbePositions;
            dst->Settings = src->Settings;
            dst->BakedAt = src->BakedAt;
            dst->ProbeBatch = src->ProbeBatch;
            return dst;
        }

        void InstallAudioHooksOnce()
        {
            static std::once_flag s_Once;
            std::call_once(s_Once, []()
            {
                NoxAudio::SetLogCallback([](NoxAudio::LogLevel level, std::string_view message)
                {
                    switch (level)
                    {
                    case NoxAudio::LogLevel::Info:  NOX_CORE_INFO("{}", message); break;
                    case NoxAudio::LogLevel::Warn:  NOX_CORE_WARN("{}", message); break;
                    case NoxAudio::LogLevel::Error: NOX_CORE_ERROR("{}", message); break;
                    }
                });
                NoxAudio::SetFileReadCallback([](const std::filesystem::path& path, std::vector<uint8_t>& outBytes) -> bool
                {
                    std::vector<char> chars;
                    if (!VFS::ReadBytes(path, chars))
                        return false;
                    outBytes.assign(reinterpret_cast<const uint8_t*>(chars.data()),
                                    reinterpret_cast<const uint8_t*>(chars.data()) + chars.size());
                    return true;
                });
                NoxAudio::SetParallelForCallback([](uint32_t itemCount, const std::function<void(uint32_t index)>& body)
                {
                    NoxTasks::JobSystem::Get().Dispatch("NoxAudio", itemCount, 1, body);
                });
                NoxAudio::SetAcousticMeshReadCallback([](uint64_t meshId, uint32_t firstSubmesh, uint32_t submeshCount, std::vector<NoxAudio::AcousticMaterial> materials) -> AcousticMeshRead
                {
                    return StartAcousticMeshRead(AssetHandle(meshId), firstSubmesh, submeshCount, std::move(materials));
                });
                NoxAudio::AudioClipRegistry::SetResolver([](NoxAudio::AudioClipId id) -> NoxAudio::Ref<NoxAudio::AudioClip>
                {
                    if (!id || !Project::GetActive())
                        return nullptr;
                    if (AssetManager::GetAssetType(AssetHandle(id)) != AssetType::AudioClip)
                        return nullptr;
                    if (AssetManager::RequestAsset(AssetHandle(id)) != AssetState::Ready)
                        return nullptr;
                    Ref<AudioClip> clip = AssetManager::GetAsset<AudioClip>(AssetHandle(id));
                    return clip ? clip->Inner() : nullptr;
                });
                NoxAudio::AudioGraphRegistry::SetResolver([](NoxAudio::AudioGraphId id) -> NoxAudio::Ref<NoxAudio::NodeGraphAsset>
                {
                    if (!id || !Project::GetActive())
                        return nullptr;
                    if (AssetManager::GetAssetType(AssetHandle(id)) != AssetType::AudioGraph)
                        return nullptr;
                    if (AssetManager::RequestAsset(AssetHandle(id)) != AssetState::Ready)
                        return nullptr;
                    Ref<NodeGraphAsset> asset = AssetManager::GetAsset<NodeGraphAsset>(AssetHandle(id));
                    if (!asset)
                        return nullptr;
                    auto out = NoxAudio::CreateRef<NoxAudio::NodeGraphAsset>();
                    out->Handle = id;
                    out->Graph = ToNoxAudioGraph(asset->Graph);
                    out->CompileVersion = asset->CompileVersion;
                    return out;
                });
            });
        }
    }

    Ref<AudioClip> AudioClip::Load(const std::filesystem::path& path, NoxAudio::AudioLoadType loadType)
    {
        InstallAudioHooksOnce();
        auto inner = NoxAudio::AudioClip::Load(path, loadType);
        return inner ? ::Nox::CreateRef<AudioClip>(std::move(inner)) : nullptr;
    }

    bool AcousticData::Contains(const glm::dvec3& point) const
    {
        return ToNoxAudioAcoustic(*this)->Contains(point);
    }

    bool SaveAcousticData(const std::filesystem::path& path, const AcousticData& data)
    {
        InstallAudioHooksOnce();
        return NoxAudio::SaveAcousticData(path, *ToNoxAudioAcoustic(data));
    }

    Ref<AcousticData> LoadAcousticData(const std::filesystem::path& path)
    {
        InstallAudioHooksOnce();
        return FromNoxAudioAcoustic(NoxAudio::LoadAcousticData(path));
    }

    void SpatialAudio::SetProbeBatch(uint64_t key, Ref<AcousticData> data)
    {
        NoxAudio::SpatialAudio::SetProbeBatch(key, data ? ToNoxAudioAcoustic(*data) : nullptr);
    }

    std::vector<AcousticBaker::Result> AcousticBaker::TakeResults()
    {
        auto raw = m_Baker.TakeResults();
        std::vector<Result> out;
        out.reserve(raw.size());
        for (auto& r : raw)
            out.push_back({ r.Key, FromNoxAudioAcoustic(r.Data), std::move(r.Error) });
        return out;
    }

    void AudioEngine::Init(bool openDevice)
    {
        InstallAudioHooksOnce();
        NoxAudio::SetMetersPerUnit(WorldUnits::ToMeters(1.0f));
        NoxAudio::AudioEngine::Init(openDevice);
    }

    void AudioEngine::Update(float deltaSeconds)
    {
        NoxAudio::SetMetersPerUnit(WorldUnits::ToMeters(1.0f));
        NoxAudio::AudioEngine::Update(deltaSeconds);
    }

    NoxAudio::AudioVoiceHandle AudioEngine::Play(const AudioPlayDesc& desc)
    {
        NoxAudio::AudioPlayDesc d;
        d.Clip = desc.Clip ? desc.Clip->Inner() : nullptr;
        d.Graph = desc.Graph;
        for (const auto& [k, v] : desc.GraphParameters)
            d.GraphParameters.emplace(k, ToNoxAudioValue(v));
        d.Volume = desc.Volume;
        d.Pitch = desc.Pitch;
        d.Loop = desc.Loop;
        d.Bus = desc.Bus;
        d.Group = desc.Group;
        d.Spatial = desc.Spatial;
        d.Position = desc.Position;
        d.Spatialization = desc.Spatialization;
        d.StartTime = desc.StartTime;
        d.FadeIn = desc.FadeIn;
        d.Environment = desc.Environment;
        d.SoundAsset = desc.SoundAsset;
        d.Sound = desc.Sound;
        return NoxAudio::AudioEngine::Play(d);
    }

    void AudioMixer::LoadProjectMixer(bool create)
    {
        InstallAudioHooksOnce();
        Ref<Project> project = Project::GetActive();
        if (!project)
            return;
        std::filesystem::path relative = project->GetConfig().AudioMixer;
        if (relative.empty())
            relative = "Audio/DefaultMixer.nmixer";
        NoxAudio::AudioMixer::LoadMixer(Project::GetActiveAssetDirectory() / relative, create);
    }

    std::shared_ptr<const AudioGraphProgram> GetAudioGraphProgram(AssetHandle graph, std::string* outError)
    {
        InstallAudioHooksOnce();
        return NoxAudio::GetAudioGraphProgram(static_cast<uint64_t>(graph), outError);
    }

    void CollectAudioGraphClips(const NodeGraph& graph, std::vector<AssetHandle>& outClips)
    {
        std::vector<NoxAudio::AssetHandle> raw;
        NoxAudio::AudioGraphProgram::CollectClips(ToNoxAudioGraph(graph), raw);
        outClips.reserve(outClips.size() + raw.size());
        for (NoxAudio::AssetHandle h : raw)
            outClips.push_back(AssetHandle(h));
    }

    void RegisterAudioGraphNodeTypes()
    {
        InstallAudioHooksOnce();
        NoxAudio::RegisterAudioGraphNodeTypes();

        // Mirror NoxAudio's AudioGraph node descriptors into NoxCore's NodeTypeRegistry for the node graph editor.
        for (const NoxAudio::NodeTypeDesc& src : NoxAudio::NodeTypeRegistry::All())
        {
            if (src.Domain != kAudioGraphDomain)
                continue;
            if (NodeTypeRegistry::Find(src.Domain, src.TypeName))
                continue;
            NodeTypeDesc dst;
            dst.TypeName = src.TypeName;
            dst.Domain = src.Domain;
            dst.Category = src.Category;
            for (const auto& p : src.Inputs)
                dst.Inputs.push_back({ p.Name, p.Type });
            for (const auto& p : src.Outputs)
                dst.Outputs.push_back({ p.Name, p.Type });
            for (const auto& [k, v] : src.DefaultProperties)
                dst.DefaultProperties.emplace_back(k, FromNoxAudioValue(v));
            dst.PropertyAssetTypeHints = src.PropertyAssetTypeHints;
            dst.PropertyRanges = src.PropertyRanges;
            dst.OwnsSubGraphs = src.OwnsSubGraphs;
            for (const auto& p : src.VariableInputs)
                dst.VariableInputs.push_back({ p.Name, p.Type });
            for (const auto& [k, v] : src.VariableInputLiterals)
                dst.VariableInputLiterals.emplace(k, FromNoxAudioValue(v));
            dst.MinVariableInputs = src.MinVariableInputs;
            dst.ParameterType = src.ParameterType;
            dst.PropertyEnums = src.PropertyEnums;
            if (src.NodePins)
            {
                auto fn = src.NodePins;
                dst.NodePins = [fn](const std::unordered_map<std::string, NodeGraphValue>& props, NodePinList& out)
                {
                    std::unordered_map<std::string, NoxAudio::NodeGraphValue> inProps;
                    for (const auto& [k, v] : props)
                        inProps.emplace(k, ToNoxAudioValue(v));
                    NoxAudio::NodePinList audioOut;
                    fn(inProps, audioOut);
                    for (const auto& p : audioOut.Inputs)
                        out.Inputs.push_back({ p.Name, p.Type });
                    for (const auto& p : audioOut.Outputs)
                        out.Outputs.push_back({ p.Name, p.Type });
                    for (const auto& [k, v] : audioOut.Literals)
                        out.Literals.emplace_back(k, FromNoxAudioValue(v));
                };
            }
            NodeTypeRegistry::Register(std::move(dst));
        }
    }

    NodeGraph CreateEmptyAudioGraph()
    {
        return FromNoxAudioGraph(NoxAudio::CreateEmptyAudioGraph());
    }
}
