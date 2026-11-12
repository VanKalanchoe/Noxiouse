#include "AudioMixer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <unordered_map>

#include <yaml-cpp/yaml.h>

#include "AudioDSP.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/VirtualFileSystem.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    // ---- Data -----------------------------------------------------------------------------------------------------------------------

    AudioMixerBus* AudioMixerData::FindBus(uint64_t id)
    {
        for (AudioMixerBus& bus : Buses)
            if (bus.Id == id)
                return &bus;
        return nullptr;
    }

    const AudioMixerBus* AudioMixerData::FindBus(uint64_t id) const
    {
        return const_cast<AudioMixerData*>(this)->FindBus(id);
    }

    int32_t AudioMixerData::IndexOf(uint64_t id) const
    {
        for (size_t i = 0; i < Buses.size(); ++i)
            if (Buses[i].Id == id)
                return static_cast<int32_t>(i);
        return -1;
    }

    bool AudioMixerData::IsUnder(uint64_t bus, uint64_t ancestor) const
    {
        for (size_t guard = 0; guard <= Buses.size() && bus != 0; ++guard)
        {
            if (bus == ancestor)
                return true;
            const AudioMixerBus* found = FindBus(bus);
            if (!found || found->Id == kAudioBusMaster)
                return false;
            bus = found->Parent;
        }
        return false;
    }

    bool AudioMixerData::WouldLoop(uint64_t from, uint64_t to) const
    {
        // Does `to` already depend on `from`? Then `from` -> `to` closes a loop. Dependencies: a bus depends on its children, its send
        // sources, its sidechain and ducking sources.
        if (from == to)
            return true;
        std::vector<uint64_t> pending{ from };
        std::vector<uint64_t> seen;
        while (!pending.empty())
        {
            const uint64_t current = pending.back();
            pending.pop_back();
            if (std::find(seen.begin(), seen.end(), current) != seen.end())
                continue;
            seen.push_back(current);
            // Everything `current` depends on.
            for (const AudioMixerBus& bus : Buses)
            {
                bool feeds = bus.Parent == current && bus.Id != kAudioBusMaster;
                for (const AudioMixerSend& send : bus.Sends)
                    feeds |= send.Target == current;
                if (feeds)
                {
                    if (bus.Id == to)
                        return true;
                    pending.push_back(bus.Id);
                }
            }
            if (const AudioMixerBus* bus = FindBus(current))
            {
                std::vector<uint64_t> keys;
                for (const AudioMixerEffect& effect : bus->Effects)
                    if (effect.Sidechain)
                        keys.push_back(effect.Sidechain);
                if (bus->Duck.Source)
                    keys.push_back(bus->Duck.Source);
                for (uint64_t key : keys)
                {
                    if (key == to)
                        return true;
                    pending.push_back(key);
                }
            }
        }
        return false;
    }

    std::vector<float> DefaultEffectValues(AudioEffectType type)
    {
        std::vector<float> values;
        for (const AudioEffectParameterInfo& parameter : GetAudioEffectInfo(type).Parameters)
            values.push_back(parameter.Default);
        return values;
    }

    AudioMixerData CreateDefaultAudioMixer()
    {
        AudioMixerData data;
        auto bus = [&](uint64_t id, const char* name) -> AudioMixerBus&
        {
            AudioMixerBus& added = data.Buses.emplace_back();
            added.Id = id;
            added.Name = name;
            added.Parent = id == kAudioBusMaster ? 0 : kAudioBusMaster;
            return added;
        };
        auto effect = [](AudioMixerBus& target, AudioEffectType type, uint64_t id) -> AudioMixerEffect&
        {
            AudioMixerEffect& added = target.Effects.emplace_back();
            added.Id = id;
            added.Type = type;
            added.Values = DefaultEffectValues(type);
            return added;
        };
        AudioMixerBus& master = bus(kAudioBusMaster, "Master");
        effect(master, AudioEffectType::Limiter, 101);
        AudioMixerBus& music = bus(kAudioBusMusic, "Music");
        music.Duck.Source = kAudioBusVoice; // dialogue turns the music down (every game's mix does)
        bus(kAudioBusSFX, "SFX");
        bus(kAudioBusVoice, "Voice");
        bus(kAudioBusUI, "UI").PlaysWhilePaused = true;
        bus(kAudioBusAmbience, "Ambience");
        AudioMixerBus& reverb = bus(kAudioBusReverb, "Reverb");
        AudioMixerEffect& room = effect(reverb, AudioEffectType::Reverb, 701);
        // A send bus's reverb: all wet (the dry sound already went to its own bus).
        room.Values[static_cast<size_t>(FindAudioEffectParameter(AudioEffectType::Reverb, "Wet"))] = 0.0f;
        room.Values[static_cast<size_t>(FindAudioEffectParameter(AudioEffectType::Reverb, "Dry"))] = AudioDSP::kSilenceDb;
        return data;
    }

    namespace
    {
        const char* KindName(AudioMixerValueKind kind)
        {
            switch (kind)
            {
            case AudioMixerValueKind::Volume: return "Volume";
            case AudioMixerValueKind::Send: return "Send";
            case AudioMixerValueKind::Effect: return "Effect";
            }
            return "Volume";
        }
        AudioMixerValueKind KindFromName(const std::string& name)
        {
            if (name == "Send") return AudioMixerValueKind::Send;
            if (name == "Effect") return AudioMixerValueKind::Effect;
            return AudioMixerValueKind::Volume;
        }
    }

    bool SerializeAudioMixer(const std::filesystem::path& path, const AudioMixerData& data)
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "AudioMixer" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Headphones" << YAML::Value << data.Headphones;
        out << YAML::Key << "RealVoices" << YAML::Value << data.RealVoices;
        out << YAML::Key << "Reflections" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Enabled" << YAML::Value << data.Reflections.Enabled;
        out << YAML::Key << "Rays" << YAML::Value << data.Reflections.Rays;
        out << YAML::Key << "Bounces" << YAML::Value << data.Reflections.Bounces;
        out << YAML::Key << "Duration" << YAML::Value << data.Reflections.Duration;
        out << YAML::Key << "MaxSources" << YAML::Value << data.Reflections.MaxSources;
        out << YAML::Key << "Bus" << YAML::Value << data.Reflections.Bus;
        out << YAML::Key << "LevelDb" << YAML::Value << data.Reflections.LevelDb;
        out << YAML::Key << "ListenerReverb" << YAML::Value << std::string(AudioListenerReverbToString(data.Reflections.ListenerReverb));
        out << YAML::Key << "PathingSources" << YAML::Value << data.Reflections.PathingSources;
        out << YAML::EndMap;
        out << YAML::Key << "Buses" << YAML::Value << YAML::BeginSeq;
        for (const AudioMixerBus& bus : data.Buses)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Id" << YAML::Value << bus.Id;
            out << YAML::Key << "Name" << YAML::Value << bus.Name;
            out << YAML::Key << "Parent" << YAML::Value << bus.Parent;
            out << YAML::Key << "VolumeDb" << YAML::Value << bus.VolumeDb;
            out << YAML::Key << "Mute" << YAML::Value << bus.Mute;
            out << YAML::Key << "Solo" << YAML::Value << bus.Solo;
            out << YAML::Key << "Bypass" << YAML::Value << bus.Bypass;
            out << YAML::Key << "PlaysWhilePaused" << YAML::Value << bus.PlaysWhilePaused;
            out << YAML::Key << "Effects" << YAML::Value << YAML::BeginSeq;
            for (const AudioMixerEffect& effect : bus.Effects)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Id" << YAML::Value << effect.Id;
                out << YAML::Key << "Type" << YAML::Value << std::string(AudioEffectTypeToString(effect.Type));
                out << YAML::Key << "Bypass" << YAML::Value << effect.Bypass;
                if (effect.Impulse)
                    out << YAML::Key << "Impulse" << YAML::Value << static_cast<uint64_t>(effect.Impulse);
                if (effect.Sidechain)
                    out << YAML::Key << "Sidechain" << YAML::Value << effect.Sidechain;
                // Parameters by name: a parameter added to an effect later keeps the others' values.
                out << YAML::Key << "Parameters" << YAML::Value << YAML::BeginMap;
                const AudioEffectInfo& info = GetAudioEffectInfo(effect.Type);
                for (size_t p = 0; p < info.Parameters.size() && p < effect.Values.size(); ++p)
                    out << YAML::Key << info.Parameters[p].Name << YAML::Value << effect.Values[p];
                out << YAML::EndMap;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
            out << YAML::Key << "Sends" << YAML::Value << YAML::BeginSeq;
            for (const AudioMixerSend& send : bus.Sends)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Target" << YAML::Value << send.Target;
                out << YAML::Key << "LevelDb" << YAML::Value << send.LevelDb;
                out << YAML::Key << "PreFader" << YAML::Value << send.PreFader;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
            out << YAML::Key << "Duck" << YAML::Value << YAML::BeginMap;
            out << YAML::Key << "Source" << YAML::Value << bus.Duck.Source;
            out << YAML::Key << "DepthDb" << YAML::Value << bus.Duck.DepthDb;
            out << YAML::Key << "ThresholdDb" << YAML::Value << bus.Duck.ThresholdDb;
            out << YAML::Key << "Attack" << YAML::Value << bus.Duck.Attack;
            out << YAML::Key << "Release" << YAML::Value << bus.Duck.Release;
            out << YAML::EndMap;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        out << YAML::Key << "Snapshots" << YAML::Value << YAML::BeginSeq;
        for (const AudioMixerSnapshot& snapshot : data.Snapshots)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Name" << YAML::Value << snapshot.Name;
            out << YAML::Key << "Values" << YAML::Value << YAML::BeginSeq;
            for (const auto& [key, value] : snapshot.Values)
            {
                out << YAML::Flow << YAML::BeginMap;
                out << YAML::Key << "Bus" << YAML::Value << key.Bus;
                out << YAML::Key << "Kind" << YAML::Value << KindName(key.Kind);
                if (key.Kind != AudioMixerValueKind::Volume)
                    out << YAML::Key << "Item" << YAML::Value << key.Item;
                if (key.Kind == AudioMixerValueKind::Effect)
                {
                    // The parameter by name, as the effect saves its own.
                    std::string name;
                    if (const AudioMixerBus* bus = data.FindBus(key.Bus))
                        for (const AudioMixerEffect& effect : bus->Effects)
                            if (effect.Id == key.Item && key.Parameter < GetAudioEffectInfo(effect.Type).Parameters.size())
                                name = GetAudioEffectInfo(effect.Type).Parameters[key.Parameter].Name;
                    out << YAML::Key << "Parameter" << YAML::Value << name;
                }
                out << YAML::Key << "Value" << YAML::Value << value;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        out << YAML::EndMap;
        out << YAML::EndMap;

        std::error_code error;
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream file(path);
        if (!file)
        {
            NOX_CORE_ERROR("Audio: could not write the mixer {}", path.generic_string());
            return false;
        }
        file << out.c_str();
        return true;
    }

    bool DeserializeAudioMixer(const std::filesystem::path& path, AudioMixerData& outData)
    {
        YAML::Node document;
        try
        {
            document = YAML::Load(VFS::ReadTextOrEmpty(path));
        }
        catch (const YAML::Exception& exception)
        {
            NOX_CORE_ERROR("Audio: the mixer {} is not valid YAML: {}", path.generic_string(), exception.what());
            return false;
        }
        const YAML::Node root = document["AudioMixer"];
        if (!root)
            return false;
        AudioMixerData data;
        try
        {
            data.Headphones = root["Headphones"].as<bool>(true);
            data.RealVoices = std::clamp<uint32_t>(root["RealVoices"].as<uint32_t>(64), 1, AudioEngine::MaxVoices);
            if (const YAML::Node reflections = root["Reflections"])
            {
                data.Reflections.Enabled = reflections["Enabled"].as<bool>(true);
                data.Reflections.Rays = reflections["Rays"].as<int32_t>(4096);
                data.Reflections.Bounces = reflections["Bounces"].as<int32_t>(4);
                data.Reflections.Duration = reflections["Duration"].as<float>(1.0f);
                data.Reflections.MaxSources = reflections["MaxSources"].as<int32_t>(4);
                data.Reflections.Bus = reflections["Bus"].as<uint64_t>(kAudioBusMaster);
                data.Reflections.LevelDb = reflections["LevelDb"].as<float>(0.0f);
                data.Reflections.ListenerReverb = AudioListenerReverbFromString(reflections["ListenerReverb"].as<std::string>("Baked"));
                data.Reflections.PathingSources = reflections["PathingSources"].as<int32_t>(8);
            }
            for (const YAML::Node& node : root["Buses"])
            {
                AudioMixerBus bus;
                bus.Id = node["Id"].as<uint64_t>(0);
                bus.Name = node["Name"].as<std::string>("Bus");
                bus.Parent = node["Parent"].as<uint64_t>(kAudioBusMaster);
                bus.VolumeDb = node["VolumeDb"].as<float>(0.0f);
                bus.Mute = node["Mute"].as<bool>(false);
                bus.Solo = node["Solo"].as<bool>(false);
                bus.Bypass = node["Bypass"].as<bool>(false);
                bus.PlaysWhilePaused = node["PlaysWhilePaused"].as<bool>(false);
                for (const YAML::Node& effectNode : node["Effects"])
                {
                    AudioMixerEffect effect;
                    effect.Id = effectNode["Id"].as<uint64_t>(0);
                    effect.Type = AudioEffectTypeFromString(effectNode["Type"].as<std::string>(""));
                    if (effect.Type == AudioEffectType::Count)
                    {
                        NOX_CORE_WARN("Audio: the mixer's bus '{}' has an unknown effect '{}'", bus.Name, effectNode["Type"].as<std::string>(""));
                        continue;
                    }
                    effect.Bypass = effectNode["Bypass"].as<bool>(false);
                    effect.Impulse = AssetHandle(effectNode["Impulse"].as<uint64_t>(0));
                    effect.Sidechain = effectNode["Sidechain"].as<uint64_t>(0);
                    effect.Values = DefaultEffectValues(effect.Type);
                    const AudioEffectInfo& info = GetAudioEffectInfo(effect.Type);
                    if (const YAML::Node parameters = effectNode["Parameters"])
                        for (size_t p = 0; p < info.Parameters.size(); ++p)
                            if (const YAML::Node value = parameters[info.Parameters[p].Name])
                                effect.Values[p] = value.as<float>(info.Parameters[p].Default);
                    bus.Effects.push_back(std::move(effect));
                }
                for (const YAML::Node& sendNode : node["Sends"])
                {
                    AudioMixerSend send;
                    send.Target = sendNode["Target"].as<uint64_t>(0);
                    send.LevelDb = sendNode["LevelDb"].as<float>(0.0f);
                    send.PreFader = sendNode["PreFader"].as<bool>(false);
                    bus.Sends.push_back(send);
                }
                if (const YAML::Node duck = node["Duck"])
                {
                    bus.Duck.Source = duck["Source"].as<uint64_t>(0);
                    bus.Duck.DepthDb = duck["DepthDb"].as<float>(-10.0f);
                    bus.Duck.ThresholdDb = duck["ThresholdDb"].as<float>(-40.0f);
                    bus.Duck.Attack = duck["Attack"].as<float>(0.05f);
                    bus.Duck.Release = duck["Release"].as<float>(0.6f);
                }
                if (bus.Id != 0 && !data.FindBus(bus.Id))
                    data.Buses.push_back(std::move(bus));
            }
            for (const YAML::Node& node : root["Snapshots"])
            {
                AudioMixerSnapshot snapshot;
                snapshot.Name = node["Name"].as<std::string>("Snapshot");
                for (const YAML::Node& valueNode : node["Values"])
                {
                    AudioMixerValueKey key;
                    key.Bus = valueNode["Bus"].as<uint64_t>(0);
                    key.Kind = KindFromName(valueNode["Kind"].as<std::string>("Volume"));
                    key.Item = valueNode["Item"].as<uint64_t>(0);
                    if (key.Kind == AudioMixerValueKind::Effect)
                    {
                        const AudioMixerBus* bus = data.FindBus(key.Bus);
                        const AudioMixerEffect* effect = nullptr;
                        if (bus)
                            for (const AudioMixerEffect& candidate : bus->Effects)
                                if (candidate.Id == key.Item)
                                    effect = &candidate;
                        const int32_t parameter = effect ? FindAudioEffectParameter(effect->Type, valueNode["Parameter"].as<std::string>("")) : -1;
                        if (parameter < 0)
                            continue;
                        key.Parameter = static_cast<uint32_t>(parameter);
                    }
                    snapshot.Values.emplace_back(key, valueNode["Value"].as<float>(0.0f));
                }
                data.Snapshots.push_back(std::move(snapshot));
            }
        }
        catch (const YAML::Exception& exception)
        {
            NOX_CORE_ERROR("Audio: could not read the mixer {}: {}", path.generic_string(), exception.what());
            return false;
        }
        // Master first, always; a bus whose Output To is missing goes to Master.
        const int32_t master = data.IndexOf(kAudioBusMaster);
        if (master < 0)
        {
            AudioMixerBus bus;
            bus.Id = kAudioBusMaster;
            bus.Name = "Master";
            data.Buses.insert(data.Buses.begin(), bus);
        }
        else if (master > 0)
            std::rotate(data.Buses.begin(), data.Buses.begin() + master, data.Buses.begin() + master + 1);
        data.Buses[0].Parent = 0;
        for (size_t i = 1; i < data.Buses.size(); ++i)
            if (!data.FindBus(data.Buses[i].Parent) || data.Buses[i].Parent == data.Buses[i].Id)
                data.Buses[i].Parent = kAudioBusMaster;
        outData = std::move(data);
        return true;
    }

    // ---- The running mixer ----------------------------------------------------------------------------------------------------------

    namespace
    {
        // A value the engine gets: a bus's fader, a send's level, an effect's number parameter.
        struct Slot
        {
            AudioMixerValueKey Key;
            uint32_t Bus = 0;     // index
            uint32_t Item = 0;    // send / effect index
            bool Logarithmic = false;
        };

        struct MixerState
        {
            AudioMixerData Data = CreateDefaultAudioMixer();
            std::filesystem::path Path;
            bool Dirty = false;
            bool StructureChanged = true;
            bool ValuesChanged = true;
            int32_t EditedSnapshot = -1;
            int32_t ActiveSnapshot = -1;

            std::vector<Slot> Slots;
            std::vector<float> Current;   // blended, what the engine has (dB for faders and sends)
            std::vector<float> BlendFrom;
            float BlendTime = 0.0f;
            float BlendDuration = 0.0f;
            std::vector<float> SentGain;  // per bus: the linear gain sent
            std::vector<float> SentValue; // per slot
            std::vector<uint8_t> SentBypass; // per effect, flattened
            std::unordered_map<uint64_t, float> GameVolume;
            int8_t GameHeadphones = -1; // -1: the mixer's setting
            std::vector<AssetHandle> WaitingImpulses; // rebuild once these are loaded
        };
        MixerState* s_Mixer = nullptr;

        MixerState& M()
        {
            if (!s_Mixer)
                s_Mixer = new MixerState();
            return *s_Mixer;
        }

        const AudioMixerEffect* FindEffect(const AudioMixerData& data, uint64_t bus, uint64_t effect, uint32_t* outIndex = nullptr)
        {
            const AudioMixerBus* found = data.FindBus(bus);
            if (!found)
                return nullptr;
            for (size_t i = 0; i < found->Effects.size(); ++i)
            {
                if (found->Effects[i].Id == effect)
                {
                    if (outIndex)
                        *outIndex = static_cast<uint32_t>(i);
                    return &found->Effects[i];
                }
            }
            return nullptr;
        }

        float BaseValue(const AudioMixerData& data, const AudioMixerValueKey& key)
        {
            const AudioMixerBus* bus = data.FindBus(key.Bus);
            if (!bus)
                return 0.0f;
            switch (key.Kind)
            {
            case AudioMixerValueKind::Volume:
                return bus->VolumeDb;
            case AudioMixerValueKind::Send:
                for (const AudioMixerSend& send : bus->Sends)
                    if (send.Target == key.Item)
                        return send.LevelDb;
                return AudioDSP::kSilenceDb;
            case AudioMixerValueKind::Effect:
                if (const AudioMixerEffect* effect = FindEffect(data, key.Bus, key.Item))
                    return key.Parameter < effect->Values.size() ? effect->Values[key.Parameter] : 0.0f;
                return 0.0f;
            }
            return 0.0f;
        }

        const float* SnapshotValue(const AudioMixerSnapshot& snapshot, const AudioMixerValueKey& key)
        {
            for (const auto& [candidate, value] : snapshot.Values)
                if (candidate == key)
                    return &value;
            return nullptr;
        }

        // Every value of `snapshot` (-1: Default), slot by slot.
        std::vector<float> TargetValues(const MixerState& state, int32_t snapshot)
        {
            std::vector<float> values(state.Slots.size());
            const AudioMixerSnapshot* overrides = snapshot >= 0 && static_cast<size_t>(snapshot) < state.Data.Snapshots.size() ? &state.Data.Snapshots[snapshot] : nullptr;
            for (size_t i = 0; i < state.Slots.size(); ++i)
            {
                const float* value = overrides ? SnapshotValue(*overrides, state.Slots[i].Key) : nullptr;
                values[i] = value ? *value : BaseValue(state.Data, state.Slots[i].Key);
            }
            return values;
        }

        void BuildSlots(MixerState& state)
        {
            state.Slots.clear();
            const AudioMixerData& data = state.Data;
            for (uint32_t b = 0; b < data.Buses.size(); ++b)
            {
                const AudioMixerBus& bus = data.Buses[b];
                state.Slots.push_back({ { bus.Id, AudioMixerValueKind::Volume, 0, 0 }, b, 0, false });
                for (uint32_t s = 0; s < bus.Sends.size(); ++s)
                    state.Slots.push_back({ { bus.Id, AudioMixerValueKind::Send, bus.Sends[s].Target, 0 }, b, s, false });
                for (uint32_t e = 0; e < bus.Effects.size(); ++e)
                {
                    const AudioEffectInfo& info = GetAudioEffectInfo(bus.Effects[e].Type);
                    for (uint32_t p = 0; p < info.Parameters.size(); ++p)
                        state.Slots.push_back({ { bus.Id, AudioMixerValueKind::Effect, bus.Effects[e].Id, p }, b, e, info.Parameters[p].Logarithmic });
                }
            }
        }

        // Mute and solo: with any bus soloed, the ones heard are the soloed, what is under them (their inputs), what is above them
        // (their way to Master) and the buses they send to (a soloed bus keeps its reverb).
        std::vector<bool> AudibleBuses(const AudioMixerData& data)
        {
            std::vector<bool> audible(data.Buses.size(), true);
            bool anySolo = false;
            for (const AudioMixerBus& bus : data.Buses)
                anySolo |= bus.Solo;
            for (size_t i = 0; i < data.Buses.size(); ++i)
            {
                const AudioMixerBus& bus = data.Buses[i];
                if (anySolo && !bus.Solo)
                {
                    bool keep = false;
                    for (const AudioMixerBus& soloed : data.Buses)
                    {
                        if (!soloed.Solo)
                            continue;
                        keep |= data.IsUnder(bus.Id, soloed.Id) || data.IsUnder(soloed.Id, bus.Id);
                        for (const AudioMixerSend& send : soloed.Sends)
                            keep |= send.Target == bus.Id || data.IsUnder(send.Target, bus.Id);
                    }
                    audible[i] = keep;
                }
                if (bus.Mute)
                    audible[i] = false;
            }
            audible[0] = !data.Buses.empty() && !data.Buses[0].Mute;
            return audible;
        }

        float BusGain(const MixerState& state, uint32_t bus, float volumeDb, const std::vector<bool>& audible)
        {
            if (!audible[bus])
                return 0.0f;
            const auto found = state.GameVolume.find(state.Data.Buses[bus].Id);
            return AudioDSP::DbToGain(volumeDb) * (found != state.GameVolume.end() ? found->second : 1.0f);
        }

        void Rebuild(MixerState& state)
        {
            BuildSlots(state);
            const std::vector<float> target = TargetValues(state, state.ActiveSnapshot);
            state.Current = target;
            state.BlendDuration = 0.0f;
            const std::vector<bool> audible = AudibleBuses(state.Data);
            state.WaitingImpulses.clear();

            AudioMixerLayoutDesc desc;
            const AudioMixerData& data = state.Data;
            size_t slot = 0;
            for (uint32_t b = 0; b < data.Buses.size() && b < AudioEngine::MaxBuses; ++b)
            {
                const AudioMixerBus& bus = data.Buses[b];
                AudioMixerLayoutDesc::Bus& out = desc.Buses.emplace_back();
                out.Id = bus.Id;
                out.Parent = b == 0 ? -1 : std::max(data.IndexOf(bus.Parent), 0);
                out.PlaysWhilePaused = bus.PlaysWhilePaused;
                out.Gain = BusGain(state, b, state.Current[slot++], audible);
                for (const AudioMixerSend& send : bus.Sends)
                {
                    AudioMixerLayoutDesc::Send& outSend = out.Sends.emplace_back();
                    outSend.Target = data.IndexOf(send.Target);
                    outSend.Gain = AudioDSP::DbToGain(state.Current[slot++]);
                    outSend.PreFader = send.PreFader;
                }
                for (const AudioMixerEffect& effect : bus.Effects)
                {
                    AudioMixerLayoutDesc::Effect& outEffect = out.Effects.emplace_back();
                    outEffect.Id = effect.Id;
                    outEffect.Type = effect.Type;
                    outEffect.Bypass = effect.Bypass || bus.Bypass;
                    outEffect.Sidechain = effect.Sidechain ? data.IndexOf(effect.Sidechain) : -1;
                    const size_t count = GetAudioEffectInfo(effect.Type).Parameters.size();
                    outEffect.Values.assign(state.Current.begin() + static_cast<std::ptrdiff_t>(slot), state.Current.begin() + static_cast<std::ptrdiff_t>(slot + count));
                    slot += count;
                    if (effect.Impulse && Project::GetActive() && AssetManager::GetAssetType(effect.Impulse) == AssetType::AudioClip)
                    {
                        const AssetState loaded = AssetManager::RequestAsset(effect.Impulse);
                        if (loaded == AssetState::Ready)
                            outEffect.Impulse = AssetManager::GetAsset<AudioClip>(effect.Impulse);
                        else if (loaded != AssetState::Failed)
                            state.WaitingImpulses.push_back(effect.Impulse);
                    }
                }
                out.DuckSource = bus.Duck.Source ? data.IndexOf(bus.Duck.Source) : -1;
                out.DuckDepthDb = bus.Duck.DepthDb;
                out.DuckThresholdDb = bus.Duck.ThresholdDb;
                out.DuckAttack = bus.Duck.Attack;
                out.DuckRelease = bus.Duck.Release;
            }
            AudioEngine::SetMixerLayout(desc);

            // What the engine has now.
            state.SentGain.assign(data.Buses.size(), -1.0f);
            for (uint32_t b = 0; b < desc.Buses.size(); ++b)
                state.SentGain[b] = desc.Buses[b].Gain;
            state.SentValue = state.Current;
            state.SentBypass.clear();
            for (const AudioMixerBus& bus : data.Buses)
                for (const AudioMixerEffect& effect : bus.Effects)
                    state.SentBypass.push_back(effect.Bypass || bus.Bypass);
            state.StructureChanged = false;
            state.ValuesChanged = false;
        }

        // What changed since the last call, to the engine.
        void SendValues(MixerState& state)
        {
            const AudioMixerData& data = state.Data;
            const std::vector<bool> audible = AudibleBuses(data);
            for (size_t i = 0; i < state.Slots.size(); ++i)
            {
                const Slot& slot = state.Slots[i];
                const float value = state.Current[i];
                switch (slot.Key.Kind)
                {
                case AudioMixerValueKind::Volume:
                {
                    const float gain = BusGain(state, slot.Bus, value, audible);
                    if (gain != state.SentGain[slot.Bus])
                    {
                        AudioEngine::SetBusGain(slot.Bus, gain);
                        state.SentGain[slot.Bus] = gain;
                    }
                    break;
                }
                case AudioMixerValueKind::Send:
                    if (value != state.SentValue[i])
                        AudioEngine::SetSendGain(slot.Bus, slot.Item, AudioDSP::DbToGain(value));
                    break;
                case AudioMixerValueKind::Effect:
                    if (value != state.SentValue[i])
                        AudioEngine::SetEffectParameter(slot.Bus, slot.Item, slot.Key.Parameter, value);
                    break;
                }
                state.SentValue[i] = value;
            }
            size_t flat = 0;
            for (uint32_t b = 0; b < data.Buses.size(); ++b)
            {
                for (uint32_t e = 0; e < data.Buses[b].Effects.size(); ++e, ++flat)
                {
                    const bool bypass = data.Buses[b].Effects[e].Bypass || data.Buses[b].Bypass;
                    if (flat < state.SentBypass.size() && state.SentBypass[flat] != static_cast<uint8_t>(bypass))
                    {
                        AudioEngine::SetEffectBypass(b, e, bypass);
                        state.SentBypass[flat] = bypass;
                    }
                }
            }
        }

        int32_t FindSlot(const MixerState& state, const AudioMixerValueKey& key)
        {
            for (size_t i = 0; i < state.Slots.size(); ++i)
                if (state.Slots[i].Key == key)
                    return static_cast<int32_t>(i);
            return -1;
        }

        bool EqualsIgnoringCase(std::string_view a, std::string_view b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
                { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
        }
    }

    void AudioMixer::Init()
    {
        MixerState& state = M();
        state.StructureChanged = true;
        Rebuild(state);
    }

    void AudioMixer::Shutdown()
    {
        delete s_Mixer;
        s_Mixer = nullptr;
    }

    void AudioMixer::LoadProjectMixer(bool create)
    {
        MixerState& state = M();
        Ref<Project> project = Project::GetActive();
        if (!project)
            return;
        std::filesystem::path relative = project->GetConfig().AudioMixer;
        if (relative.empty())
            relative = kDefaultAudioMixerPath;
        const std::filesystem::path path = Project::GetActiveAssetDirectory() / relative;
        AudioMixerData data;
        if (DeserializeAudioMixer(path, data))
        {
            state.Data = std::move(data);
        }
        else
        {
            state.Data = CreateDefaultAudioMixer();
            if (create)
            {
                if (SerializeAudioMixer(path, state.Data))
                {
                    project->GetEditorAssetManager()->RegisterExistingFile(relative, AssetType::AudioMixer);
                    NOX_CORE_INFO("Audio: created the project's mixer {}", relative.generic_string());
                }
            }
            else if (VFS::Exists(path))
                NOX_CORE_ERROR("Audio: the project's mixer {} could not be read; the default buses are used", relative.generic_string());
        }
        state.Path = path;
        state.Dirty = false;
        state.EditedSnapshot = state.ActiveSnapshot = -1;
        state.GameVolume.clear();
        Rebuild(state);
    }

    const std::filesystem::path& AudioMixer::GetPath() { return M().Path; }

    bool AudioMixer::Save()
    {
        MixerState& state = M();
        if (state.Path.empty())
            return false;
        if (!SerializeAudioMixer(state.Path, state.Data))
            return false;
        state.Dirty = false;
        return true;
    }

    bool AudioMixer::IsDirty() { return M().Dirty; }
    AudioMixerData& AudioMixer::GetData() { return M().Data; }

    void AudioMixer::NotifyStructureChanged()
    {
        M().StructureChanged = true;
        M().Dirty = true;
    }

    void AudioMixer::NotifyValuesChanged()
    {
        M().ValuesChanged = true;
        M().Dirty = true;
    }

    void AudioMixer::MarkDirty() { M().Dirty = true; }

    void AudioMixer::SetEditedSnapshot(int32_t index)
    {
        MixerState& state = M();
        if (index >= static_cast<int32_t>(state.Data.Snapshots.size()))
            index = -1;
        state.EditedSnapshot = index;
        state.ActiveSnapshot = index; // heard at once
        state.BlendDuration = 0.0f;
        state.ValuesChanged = true;
    }

    int32_t AudioMixer::GetEditedSnapshot() { return M().EditedSnapshot; }

    float AudioMixer::GetValue(const AudioMixerValueKey& key)
    {
        const MixerState& state = M();
        if (state.EditedSnapshot >= 0 && static_cast<size_t>(state.EditedSnapshot) < state.Data.Snapshots.size())
            if (const float* value = SnapshotValue(state.Data.Snapshots[state.EditedSnapshot], key))
                return *value;
        return BaseValue(state.Data, key);
    }

    void AudioMixer::SetValue(const AudioMixerValueKey& key, float value)
    {
        MixerState& state = M();
        if (state.EditedSnapshot >= 0 && static_cast<size_t>(state.EditedSnapshot) < state.Data.Snapshots.size())
        {
            AudioMixerSnapshot& snapshot = state.Data.Snapshots[state.EditedSnapshot];
            bool found = false;
            for (auto& [candidate, stored] : snapshot.Values)
                if (candidate == key)
                    stored = value, found = true;
            if (!found)
                snapshot.Values.emplace_back(key, value);
        }
        else if (AudioMixerBus* bus = state.Data.FindBus(key.Bus))
        {
            switch (key.Kind)
            {
            case AudioMixerValueKind::Volume:
                bus->VolumeDb = value;
                break;
            case AudioMixerValueKind::Send:
                for (AudioMixerSend& send : bus->Sends)
                    if (send.Target == key.Item)
                        send.LevelDb = value;
                break;
            case AudioMixerValueKind::Effect:
                for (AudioMixerEffect& effect : bus->Effects)
                    if (effect.Id == key.Item && key.Parameter < effect.Values.size())
                        effect.Values[key.Parameter] = value;
                break;
            }
        }
        NotifyValuesChanged();
    }

    bool AudioMixer::IsOverridden(const AudioMixerValueKey& key)
    {
        const MixerState& state = M();
        return state.EditedSnapshot >= 0 && static_cast<size_t>(state.EditedSnapshot) < state.Data.Snapshots.size() &&
               SnapshotValue(state.Data.Snapshots[state.EditedSnapshot], key) != nullptr;
    }

    void AudioMixer::ClearOverride(const AudioMixerValueKey& key)
    {
        MixerState& state = M();
        if (state.EditedSnapshot < 0 || static_cast<size_t>(state.EditedSnapshot) >= state.Data.Snapshots.size())
            return;
        auto& values = state.Data.Snapshots[state.EditedSnapshot].Values;
        std::erase_if(values, [&](const auto& entry) { return entry.first == key; });
        NotifyValuesChanged();
    }

    float AudioMixer::GetLiveValue(const AudioMixerValueKey& key)
    {
        const MixerState& state = M();
        const int32_t slot = FindSlot(state, key);
        return slot >= 0 && static_cast<size_t>(slot) < state.Current.size() ? state.Current[slot] : GetValue(key);
    }

    void AudioMixer::Update(float deltaSeconds)
    {
        if (!AudioEngine::IsAvailable())
            return;
        MixerState& state = M();
        // An impulse response arrived: the layout again, with it.
        if (!state.WaitingImpulses.empty())
        {
            bool ready = true;
            for (AssetHandle handle : state.WaitingImpulses)
            {
                const AssetState loaded = AssetManager::RequestAsset(handle);
                ready &= loaded == AssetState::Ready || loaded == AssetState::Failed;
            }
            if (ready)
                state.StructureChanged = true;
        }
        if (state.StructureChanged)
        {
            Rebuild(state);
            return;
        }
        AudioEngine::SetHeadphones(state.GameHeadphones >= 0 ? state.GameHeadphones != 0 : state.Data.Headphones);
        AudioEngine::SetReflections(state.Data.Reflections);
        AudioEngine::SetMaxRealVoices(state.Data.RealVoices);
        const bool blending = state.BlendDuration > 0.0f;
        if (!state.ValuesChanged && !blending)
            return;
        const std::vector<float> target = TargetValues(state, state.ActiveSnapshot);
        if (blending)
        {
            state.BlendTime += deltaSeconds;
            const float t = std::clamp(state.BlendTime / state.BlendDuration, 0.0f, 1.0f);
            const float s = t * t * (3.0f - 2.0f * t); // smoothstep: no jerk at either end
            for (size_t i = 0; i < target.size(); ++i)
            {
                const float from = i < state.BlendFrom.size() ? state.BlendFrom[i] : target[i];
                if (state.Slots[i].Logarithmic && from > 0.0f && target[i] > 0.0f)
                    state.Current[i] = from * std::pow(target[i] / from, s);
                else
                    state.Current[i] = from + (target[i] - from) * s;
            }
            if (t >= 1.0f)
                state.BlendDuration = 0.0f;
        }
        else
            state.Current = target;
        state.ValuesChanged = false;
        SendValues(state);
    }

    const AudioMixerBus* AudioMixer::FindBus(std::string_view name)
    {
        for (const AudioMixerBus& bus : M().Data.Buses)
            if (EqualsIgnoringCase(bus.Name, name))
                return &bus;
        return nullptr;
    }

    const AudioMixerBus* AudioMixer::FindBus(uint64_t id) { return M().Data.FindBus(id); }

    int32_t AudioMixer::GetBusIndex(uint64_t id) { return M().Data.IndexOf(id); }

    void AudioMixer::SetGameVolume(uint64_t bus, float volume)
    {
        M().GameVolume[bus] = std::max(volume, 0.0f);
        M().ValuesChanged = true;
    }

    float AudioMixer::GetGameVolume(uint64_t bus)
    {
        const auto found = M().GameVolume.find(bus);
        return found != M().GameVolume.end() ? found->second : 1.0f;
    }

    void AudioMixer::SetGameHeadphones(bool headphones) { M().GameHeadphones = headphones ? 1 : 0; }

    bool AudioMixer::GetHeadphones()
    {
        const MixerState& state = M();
        return state.GameHeadphones >= 0 ? state.GameHeadphones != 0 : state.Data.Headphones;
    }

    bool AudioMixer::SetSnapshot(std::string_view name, float seconds)
    {
        MixerState& state = M();
        int32_t index = -1;
        if (!EqualsIgnoringCase(name, "Default"))
        {
            for (size_t i = 0; i < state.Data.Snapshots.size(); ++i)
                if (EqualsIgnoringCase(state.Data.Snapshots[i].Name, name))
                    index = static_cast<int32_t>(i);
            if (index < 0)
                return false;
        }
        state.ActiveSnapshot = index;
        state.BlendFrom = state.Current;
        state.BlendTime = 0.0f;
        state.BlendDuration = std::max(seconds, 0.0f);
        state.ValuesChanged = true;
        if (state.BlendDuration <= 0.0f)
            state.BlendDuration = 0.0f;
        return true;
    }

    std::string AudioMixer::GetSnapshot()
    {
        const MixerState& state = M();
        return state.ActiveSnapshot >= 0 && static_cast<size_t>(state.ActiveSnapshot) < state.Data.Snapshots.size()
                   ? state.Data.Snapshots[state.ActiveSnapshot].Name : std::string("Default");
    }

    void AudioMixer::ResetGameState()
    {
        MixerState& state = M();
        state.GameVolume.clear();
        state.GameHeadphones = -1;
        state.ActiveSnapshot = state.EditedSnapshot;
        state.BlendDuration = 0.0f;
        state.ValuesChanged = true;
    }

    AudioBusMeter AudioMixer::GetMeter(uint64_t bus)
    {
        const int32_t index = M().Data.IndexOf(bus);
        return index >= 0 ? AudioEngine::GetBusMeter(static_cast<uint32_t>(index)) : AudioBusMeter{};
    }

    float AudioMixer::GetEffectGainReduction(uint64_t bus, uint32_t effect)
    {
        const int32_t index = M().Data.IndexOf(bus);
        return index >= 0 ? AudioEngine::GetEffectGainReduction(static_cast<uint32_t>(index), effect) : 0.0f;
    }
}
