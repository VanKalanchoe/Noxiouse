#include "AudioGraph.h"

#include <algorithm>
#include <cstring>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/NodeGraph/GraphCompiler.h"
#include "NoxCore/NodeGraph/NodeType.h"
#include "NoxCore/NodeGraph/NodeGraphAsset.h"

namespace Nox
{
    namespace
    {
        std::unordered_map<std::string, AudioNodeImpl>& Impls()
        {
            static std::unordered_map<std::string, AudioNodeImpl> impls;
            return impls;
        }

        constexpr const char* kAudioClipHint = "AssetType::AudioClip";
        constexpr size_t kMaxStateAlign = 16; // (new std::byte[] gives at least this)

        const NodeGraphValue* FindLiteral(const CompiledNode& node, const std::string& name)
        {
            if (const auto found = node.Properties.find(name); found != node.Properties.end())
                return &found->second;
            for (const auto& [property, value] : node.Type->DefaultProperties)
                if (property == name)
                    return &value;
            // A variable group's pin ("Gain 2"): the group's literal.
            const size_t space = name.rfind(' ');
            if (space != std::string::npos)
                if (const auto literal = node.Type->VariableInputLiterals.find(name.substr(0, space)); literal != node.Type->VariableInputLiterals.end())
                    return &literal->second;
            return nullptr;
        }
    }

    // ---- Registry ------------------------------------------------------------------------------------------------------------------

    void RegisterAudioNodeImpl(const std::string& typeName, const AudioNodeImpl& impl)
    {
        NOX_CORE_ASSERT(impl.StateAlign <= kMaxStateAlign, "audio node state over-aligned");
        Impls()[typeName] = impl;
    }

    const AudioNodeImpl* FindAudioNodeImpl(const std::string& typeName)
    {
        const auto found = Impls().find(typeName);
        return found != Impls().end() ? &found->second : nullptr;
    }

    AudioPinType AudioPinTypeFromName(const std::string& name)
    {
        if (name == "Audio") return AudioPinType::Audio;
        if (name == "Int") return AudioPinType::Int;
        if (name == "Bool") return AudioPinType::Bool;
        if (name == "Trigger") return AudioPinType::Trigger;
        return AudioPinType::Float;
    }

    // ---- Setup / run ---------------------------------------------------------------------------------------------------------------

    float AudioNodeSetup::Float(const std::string& name, float fallback) const
    {
        const NodeGraphValue* value = FindLiteral(*m_Node, name);
        return value ? NodeGraphValueToFloat(*value, fallback) : fallback;
    }

    int32_t AudioNodeSetup::Int(const std::string& name, int32_t fallback) const
    {
        const NodeGraphValue* value = FindLiteral(*m_Node, name);
        if (!value)
            return fallback;
        if (const int32_t* i = std::get_if<int32_t>(value))
            return *i;
        return static_cast<int32_t>(NodeGraphValueToFloat(*value, static_cast<float>(fallback)));
    }

    bool AudioNodeSetup::Bool(const std::string& name, bool fallback) const
    {
        const NodeGraphValue* value = FindLiteral(*m_Node, name);
        return value ? NodeGraphValueToFloat(*value, fallback ? 1.0f : 0.0f) != 0.0f : fallback;
    }

    uint64_t AudioNodeSetup::Handle(const std::string& name) const
    {
        const NodeGraphValue* value = FindLiteral(*m_Node, name);
        const uint64_t* handle = value ? std::get_if<uint64_t>(value) : nullptr;
        return handle ? *handle : 0;
    }

    const NodeGraphCurve* AudioNodeSetup::Curve(const std::string& name) const
    {
        const NodeGraphValue* value = FindLiteral(*m_Node, name);
        return value ? std::get_if<NodeGraphCurve>(value) : nullptr;
    }

    std::string AudioNodeSetup::String(const std::string& name) const
    {
        const NodeGraphValue* value = FindLiteral(*m_Node, name);
        const std::string* text = value ? std::get_if<std::string>(value) : nullptr;
        return text ? *text : std::string();
    }

    const AudioClip* AudioNodeSetup::Clip(uint64_t handle) const
    {
        return m_Program->FindClip(handle);
    }

    AudioClipStream* AudioNodeSetup::CreateStream()
    {
        // One second of stereo: what the stream thread decodes ahead of a Wave Player (it refills within 10 ms).
        m_Instance->m_Streams.push_back(std::make_shared<AudioClipStream>(static_cast<uint32_t>(m_SampleRate)));
        return m_Instance->m_Streams.back().get();
    }

    int32_t AudioNodeSetup::ParameterIndex(const std::string& name) const
    {
        return m_Program->FindParameter(name);
    }

    bool AudioNodeRun::IsConnected(uint32_t input) const
    {
        const AudioGraphProgram::Node& node = m_Instance->m_Program->m_Nodes[m_NodeIndex];
        return input < 64 && (node.ConnectedMask & (uint64_t(1) << input)) != 0;
    }

    float AudioNodeRun::SampleRate() const { return m_Instance->m_SampleRate; }
    float AudioNodeRun::VoicePitch() const { return m_Instance->m_VoicePitch; }
    bool AudioNodeRun::IsFirstBlock() const { return m_Instance->m_FirstBlock; }

    float AudioNodeRun::Parameter(int32_t index) const
    {
        return index >= 0 && index < static_cast<int32_t>(m_Instance->m_ParameterValues.size()) ? m_Instance->m_ParameterValues[index] : 0.0f;
    }

    const AudioTriggers& AudioNodeRun::ParameterTrigger(int32_t index) const
    {
        static const AudioTriggers kNone{};
        return index >= 0 && index < static_cast<int32_t>(m_Instance->m_ParameterTriggers.size()) ? m_Instance->m_ParameterTriggers[index] : kNone;
    }

    void AudioNodeRun::FinishVoice(uint32_t frame) const
    {
        if (!m_Instance->m_Finished || frame < m_Instance->m_FinishFrame)
        {
            m_Instance->m_Finished = true;
            m_Instance->m_FinishFrame = std::min(frame, kAudioGraphBlockFrames);
        }
    }

    void AudioNodeRun::ReportStarvation() const
    {
        ++m_Instance->m_Starvations;
    }

    // ---- Program -------------------------------------------------------------------------------------------------------------------

    namespace
    {
        // MetaSounds' composition: an Audio Graph node plays another audio graph inside this one. Before compiling, every such node
        // is replaced by the graph it plays (itself inlined first) -- one flat graph, the same program and cost as if built by hand:
        // - its inputs are that graph's parameters: what feeds the node's pin feeds whatever read the parameter (its Get nodes go);
        //   an unconnected pin's literal (or the parameter's default) becomes the readers' literal; an unconnected trigger never fires;
        // - its outputs are that graph's Output node's pins (Out Left, Out Right, On Finished): what fed them feeds what the node fed.
        constexpr const char* kGraphNodeType = "Audio Graph";

        struct InlineContext
        {
            std::vector<AssetHandle>& Missing;
            std::string& Error;
            std::vector<std::pair<AssetHandle, uint32_t>>& Dependencies;
            std::vector<AssetHandle> Stack; // the graphs being inlined (one playing itself is refused)
        };

        uint64_t GraphNodeTarget(const GraphNode& node)
        {
            const auto found = node.Properties.find("Graph");
            const uint64_t* handle = found != node.Properties.end() ? std::get_if<uint64_t>(&found->second) : nullptr;
            return handle ? *handle : 0;
        }

        // The name of input pin `pin` of `node` (its literal's property name).
        std::string InputPinName(const NodeGraph& graph, const GraphNode& node, uint32_t pin)
        {
            const NodeTypeDesc* type = NodeTypeRegistry::Find(graph.Domain, node.TypeName);
            if (!type)
                return {};
            const NodePinList pins = ResolveNodePins(*type, node.Properties);
            return pin < pins.Inputs.size() ? pins.Inputs[pin].Name : std::string();
        }

        bool InlineGraphNodes(const NodeGraph& input, NodeGraph& out, InlineContext& context)
        {
            out = input;
            bool complete = true;
            for (;;)
            {
                const auto nodeIt = std::find_if(out.Nodes.begin(), out.Nodes.end(), [](const GraphNode& node) { return node.TypeName == kGraphNodeType; });
                if (nodeIt == out.Nodes.end())
                    break;
                const GraphNode graphNode = *nodeIt;
                out.Nodes.erase(nodeIt);

                // The links into and out of the node, taken off.
                std::unordered_map<uint32_t, GraphLink> incoming; // by its input pin
                std::vector<GraphLink> outgoing;
                for (auto link = out.Links.begin(); link != out.Links.end();)
                {
                    if (link->ToNode == graphNode.Id)
                        incoming[link->ToPin] = *link;
                    else if (link->FromNode == graphNode.Id)
                        outgoing.push_back(*link);
                    else
                    {
                        ++link;
                        continue;
                    }
                    link = out.Links.erase(link);
                }

                const AssetHandle handle(GraphNodeTarget(graphNode));
                if (handle == 0 || AssetManager::GetAssetType(handle) != AssetType::AudioGraph)
                    continue; // (none picked, or deleted: plays nothing)
                if (std::find(context.Stack.begin(), context.Stack.end(), handle) != context.Stack.end())
                {
                    context.Error = "an Audio Graph node plays a graph that plays this one (a loop)";
                    return false;
                }
                const AssetState state = AssetManager::RequestAsset(handle);
                if (state == AssetState::Failed)
                    continue;
                const NodeGraphAsset* asset = state == AssetState::Ready ? AssetManager::FindLoadedAsset<NodeGraphAsset>(handle) : nullptr;
                if (!asset)
                {
                    context.Missing.push_back(handle);
                    complete = false;
                    continue;
                }
                context.Dependencies.emplace_back(handle, asset->CompileVersion);
                NodeGraph child;
                context.Stack.push_back(handle);
                const bool inlined = InlineGraphNodes(asset->Graph, child, context);
                context.Stack.pop_back();
                if (!inlined)
                {
                    if (!context.Error.empty())
                        return false;
                    complete = false;
                    continue;
                }

                // The child's Output node (its sink) and its parameter readers stay out; the rest comes in with new ids.
                uint32_t childOutput = child.OutputNode;
                if (std::none_of(child.Nodes.begin(), child.Nodes.end(), [&](const GraphNode& node) { return node.Id == childOutput && node.TypeName == "Output"; }))
                {
                    const auto sink = std::find_if(child.Nodes.begin(), child.Nodes.end(), [](const GraphNode& node) { return node.TypeName == "Output"; });
                    childOutput = sink != child.Nodes.end() ? sink->Id : kInvalidGraphIndex;
                }
                std::unordered_map<uint32_t, std::string> readers; // node id -> the parameter it reads
                std::unordered_map<uint32_t, uint32_t> newId;
                for (const GraphNode& node : child.Nodes)
                {
                    if (node.Id == childOutput)
                        continue;
                    if (const auto reference = node.Properties.find(kParameterReferenceProperty);
                        reference != node.Properties.end() && node.TypeName.rfind("Get ", 0) == 0)
                    {
                        if (const std::string* name = std::get_if<std::string>(&reference->second))
                        {
                            readers[node.Id] = *name;
                            continue;
                        }
                    }
                    GraphNode copy = node;
                    copy.Id = out.NextNodeId++;
                    newId[node.Id] = copy.Id;
                    out.Nodes.push_back(std::move(copy));
                }
                auto findNode = [&](uint32_t id) -> GraphNode*
                {
                    const auto found = std::find_if(out.Nodes.begin(), out.Nodes.end(), [&](const GraphNode& node) { return node.Id == id; });
                    return found != out.Nodes.end() ? &*found : nullptr;
                };
                auto parameterIndex = [&](const std::string& name) -> int32_t
                {
                    for (size_t i = 0; i < child.Parameters.size(); ++i)
                        if (child.Parameters[i].Name == name)
                            return static_cast<int32_t>(i);
                    return -1;
                };

                // What feeds a child pin that a reader fed: the node's input for that parameter -- a link (from, pin), or none.
                struct Source
                {
                    bool Linked = false;
                    uint32_t Node = 0, Pin = 0;
                };
                auto readerSource = [&](uint32_t readerId) -> Source
                {
                    const int32_t index = parameterIndex(readers[readerId]);
                    if (index < 0)
                        return {};
                    const auto link = incoming.find(static_cast<uint32_t>(index));
                    return link != incoming.end() ? Source{ true, link->second.FromNode, link->second.FromPin } : Source{};
                };
                auto readerLiteral = [&](uint32_t readerId) -> const NodeGraphValue*
                {
                    const int32_t index = parameterIndex(readers[readerId]);
                    if (index < 0 || child.Parameters[index].IsTrigger)
                        return nullptr;
                    if (const auto literal = graphNode.Properties.find(child.Parameters[index].Name); literal != graphNode.Properties.end())
                        return &literal->second;
                    return &child.Parameters[index].DefaultValue;
                };

                std::unordered_map<uint32_t, Source> outputFeed; // child Output pin -> its source in `out`
                for (const GraphLink& link : child.Links)
                {
                    Source from;
                    if (readers.contains(link.FromNode))
                        from = readerSource(link.FromNode);
                    else if (const auto mapped = newId.find(link.FromNode); mapped != newId.end())
                        from = { true, mapped->second, link.FromPin };
                    if (link.ToNode == childOutput)
                    {
                        outputFeed[link.ToPin] = from;
                        continue;
                    }
                    const auto to = newId.find(link.ToNode);
                    if (to == newId.end())
                        continue;
                    if (from.Linked)
                        out.Links.push_back({ from.Node, from.Pin, to->second, link.ToPin });
                    else if (readers.contains(link.FromNode))
                    {
                        // An unconnected parameter input: its literal on the reading pin (a trigger: nothing, it never fires).
                        if (const NodeGraphValue* literal = readerLiteral(link.FromNode))
                            if (GraphNode* target = findNode(to->second))
                                target->Properties[InputPinName(child, *target, link.ToPin)] = *literal;
                    }
                }
                for (const GraphLink& link : outgoing)
                {
                    const auto feed = outputFeed.find(link.FromPin);
                    if (feed != outputFeed.end() && feed->second.Linked)
                        out.Links.push_back({ feed->second.Node, feed->second.Pin, link.ToNode, link.ToPin });
                    // (else: nothing fed that output -- silence, or a trigger that never fires)
                }
            }
            return complete;
        }
    }

    void AudioGraphProgram::CollectClips(const NodeGraph& graph, std::vector<AssetHandle>& outClips)
    {
        for (const GraphNode& node : graph.Nodes)
        {
            if (node.TypeName == kGraphNodeType)
            {
                // The graph it plays, and that graph's clips (when it is loaded).
                const AssetHandle handle(GraphNodeTarget(node));
                if (handle == 0 || std::find(outClips.begin(), outClips.end(), handle) != outClips.end())
                    continue;
                outClips.push_back(handle);
                if (const NodeGraphAsset* asset = AssetManager::GetAssetType(handle) == AssetType::AudioGraph
                        ? AssetManager::FindLoadedAsset<NodeGraphAsset>(handle) : nullptr)
                    CollectClips(asset->Graph, outClips);
                continue;
            }
            const NodeTypeDesc* type = NodeTypeRegistry::Find(graph.Domain, node.TypeName);
            if (!type)
                continue;
            for (const auto& [name, hint] : type->PropertyAssetTypeHints)
            {
                if (hint != kAudioClipHint)
                    continue;
                const auto found = node.Properties.find(name);
                const uint64_t* handle = found != node.Properties.end() ? std::get_if<uint64_t>(&found->second) : nullptr;
                if (handle && *handle != 0 && std::find(outClips.begin(), outClips.end(), AssetHandle(*handle)) == outClips.end())
                    outClips.push_back(AssetHandle(*handle));
            }
        }
    }

    std::shared_ptr<AudioGraphProgram> AudioGraphProgram::Build(const NodeGraph& sourceGraph, std::vector<AssetHandle>& outMissing, std::string& outError,
                                                                std::vector<std::pair<AssetHandle, uint32_t>>* outDependencies)
    {
        auto program = std::make_shared<AudioGraphProgram>();

        // The graphs its Audio Graph nodes play, inlined (loaded first).
        NodeGraph graph;
        std::vector<std::pair<AssetHandle, uint32_t>> dependencies;
        InlineContext context{ outMissing, outError, dependencies, {} };
        if (!InlineGraphNodes(sourceGraph, graph, context))
        {
            if (outError.empty())
                outError = "loading the graphs it plays";
            return nullptr;
        }
        if (outDependencies)
            *outDependencies = std::move(dependencies);

        // Its clips, loaded.
        std::vector<AssetHandle> clips;
        CollectClips(graph, clips);
        for (AssetHandle handle : clips)
        {
            if (AssetManager::GetAssetType(handle) != AssetType::AudioClip)
                continue; // (a deleted clip: the node plays nothing)
            const AssetState state = AssetManager::RequestAsset(handle);
            if (state == AssetState::Ready)
                program->m_Clips[handle] = AssetManager::GetAsset<AudioClip>(handle);
            else if (state != AssetState::Failed)
                outMissing.push_back(handle);
        }
        if (!outMissing.empty())
        {
            outError = "loading its clips";
            return nullptr;
        }

        program->m_Compiled = GraphCompiler::Compile(graph);
        const CompiledGraph& compiled = program->m_Compiled;
        if (!compiled.IsValid())
        {
            outError = "the graph does not compile (see the log)";
            return nullptr;
        }

        program->m_AudioSlots = 1;   // 0: silence
        program->m_TriggerSlots = 1; // 0: never fires
        std::vector<uint32_t> outputSlotBase(compiled.Nodes.size(), 0); // per compiled node: where its outputs' slots are in m_PinSlots
        for (uint32_t index = 0; index < compiled.Nodes.size(); ++index)
        {
            const CompiledNode& source = compiled.Nodes[index];
            const AudioNodeImpl* impl = FindAudioNodeImpl(source.Type->TypeName);
            if (!impl)
            {
                outError = "no implementation for the node type '" + source.Type->TypeName + "'";
                return nullptr;
            }
            Node node;
            node.Impl = impl;
            node.CompiledIndex = index;
            node.VariableCount = VariableInputCount(*source.Type, source.Properties);
            node.InputCount = static_cast<uint32_t>(source.Inputs.size());
            node.OutputCount = static_cast<uint32_t>(source.Type->Outputs.size());

            node.FirstInput = static_cast<uint32_t>(program->m_PinSlots.size());
            for (uint32_t input = 0; input < node.InputCount; ++input)
            {
                const NodePinDesc pin = InputPin(*source.Type, input);
                const AudioPinType type = AudioPinTypeFromName(pin.Type);
                const CompiledPin& link = source.Inputs[input];
                if (link.Node != kInvalidGraphIndex)
                {
                    program->m_PinSlots.push_back(program->m_PinSlots[outputSlotBase[link.Node] + link.OutputSlot]);
                    if (input < 64)
                        node.ConnectedMask |= uint64_t(1) << input;
                    continue;
                }
                if (type == AudioPinType::Audio || type == AudioPinType::Trigger)
                {
                    program->m_PinSlots.push_back(0);
                    continue;
                }
                // A literal: its own value slot.
                const NodeGraphValue* literal = FindLiteral(source, pin.Name);
                program->m_Constants.push_back({ program->m_ValueSlots, literal ? NodeGraphValueToFloat(*literal) : 0.0f,
                                                 static_cast<uint32_t>(program->m_Nodes.size()), input });
                program->m_PinSlots.push_back(program->m_ValueSlots++);
            }

            node.FirstOutput = static_cast<uint32_t>(program->m_PinSlots.size());
            outputSlotBase[index] = node.FirstOutput;
            for (const NodePinDesc& pin : source.Type->Outputs)
            {
                switch (AudioPinTypeFromName(pin.Type))
                {
                case AudioPinType::Audio:
                    program->m_PinSlots.push_back(program->m_AudioSlots++);
                    break;
                case AudioPinType::Trigger:
                    program->m_OutputTriggerSlots.push_back(program->m_TriggerSlots);
                    program->m_PinSlots.push_back(program->m_TriggerSlots++);
                    break;
                default:
                    program->m_PinSlots.push_back(program->m_ValueSlots++);
                    break;
                }
            }

            program->m_StateBytes = (program->m_StateBytes + impl->StateAlign - 1) / impl->StateAlign * impl->StateAlign;
            node.StateOffset = program->m_StateBytes;
            program->m_StateBytes += impl->StateSize;

            if (index == compiled.OutputNodeIndex)
            {
                program->m_OutputNode = static_cast<int32_t>(program->m_Nodes.size());
                program->m_StereoOutput = (node.ConnectedMask & 2) != 0;
            }
            program->m_Nodes.push_back(node);
        }
        if (program->m_OutputNode < 0 || compiled.Nodes[compiled.OutputNodeIndex].Type->TypeName != "Output")
        {
            outError = "it has no Output node";
            return nullptr;
        }

        for (const GraphParameter& parameter : graph.Parameters)
        {
            Parameter entry;
            entry.Name = parameter.Name;
            entry.Type = AudioPinTypeFromName(GraphParameterTypeName(parameter));
            entry.Default = parameter.IsTrigger ? 0.0f : NodeGraphValueToFloat(parameter.DefaultValue);
            program->m_Parameters.push_back(std::move(entry));
        }
        return program;
    }

    bool AudioGraphProgram::LiteralChanges(const AudioGraphProgram& newer, std::vector<std::pair<uint32_t, float>>& outChanges) const
    {
        outChanges.clear();
        if (m_Nodes.size() != newer.m_Nodes.size() || m_PinSlots != newer.m_PinSlots || m_Constants.size() != newer.m_Constants.size() ||
            m_Parameters.size() != newer.m_Parameters.size() || m_OutputNode != newer.m_OutputNode)
            return false;
        for (size_t i = 0; i < m_Parameters.size(); ++i)
            if (m_Parameters[i].Name != newer.m_Parameters[i].Name || m_Parameters[i].Type != newer.m_Parameters[i].Type)
                return false;
        for (size_t i = 0; i < m_Nodes.size(); ++i)
        {
            const Node& a = m_Nodes[i];
            const Node& b = newer.m_Nodes[i];
            if (a.Impl != b.Impl || a.ConnectedMask != b.ConnectedMask || a.VariableCount != b.VariableCount)
                return false;
            // Its settings read when a voice starts (a waveform, a clip, a seed): the same, apart from its unconnected inputs' literals.
            const CompiledNode& oldNode = m_Compiled.Nodes[a.CompiledIndex];
            const CompiledNode& newNode = newer.m_Compiled.Nodes[b.CompiledIndex];
            auto isLiteral = [&](const std::string& name)
            {
                for (uint32_t input = 0; input < a.InputCount; ++input)
                    if (InputPin(*oldNode.Type, input).Name == name)
                        return true;
                return false;
            };
            for (const auto& [name, value] : oldNode.Properties)
            {
                if (isLiteral(name))
                    continue;
                const auto other = newNode.Properties.find(name);
                if (other == newNode.Properties.end() || other->second != value)
                    return false;
            }
            for (const auto& [name, value] : newNode.Properties)
                if (!isLiteral(name) && !oldNode.Properties.contains(name))
                    return false;
        }
        for (size_t i = 0; i < m_Constants.size(); ++i)
        {
            if (m_Constants[i].Slot != newer.m_Constants[i].Slot)
                return false;
            if (m_Constants[i].Value != newer.m_Constants[i].Value)
                outChanges.emplace_back(newer.m_Constants[i].Slot, newer.m_Constants[i].Value);
        }
        return true;
    }

    int32_t AudioGraphProgram::FindParameter(const std::string& name) const
    {
        for (size_t i = 0; i < m_Parameters.size(); ++i)
            if (m_Parameters[i].Name == name)
                return static_cast<int32_t>(i);
        return -1;
    }

    const AudioClip* AudioGraphProgram::FindClip(uint64_t handle) const
    {
        const auto found = m_Clips.find(handle);
        return found != m_Clips.end() ? found->second.get() : nullptr;
    }

    // ---- Instance ------------------------------------------------------------------------------------------------------------------

    AudioGraphInstance::AudioGraphInstance(std::shared_ptr<const AudioGraphProgram> program, float sampleRate, uint32_t seed,
                                           const std::unordered_map<std::string, NodeGraphValue>* parameterOverrides)
        : m_Program(std::move(program)), m_SampleRate(sampleRate)
    {
        const AudioGraphProgram& p = *m_Program;
        m_Audio.assign(static_cast<size_t>(p.m_AudioSlots) * kAudioGraphBlockFrames, 0.0f);
        m_Values.assign(p.m_ValueSlots, 0.0f);
        for (const AudioGraphProgram::Constant& constant : p.m_Constants)
            m_Values[constant.Slot] = constant.Value;
        m_Triggers.assign(p.m_TriggerSlots, AudioTriggers{});
        m_Left.assign(kAudioGraphBlockFrames, 0.0f);
        m_Right.assign(kAudioGraphBlockFrames, 0.0f);

        m_ParameterValues.resize(p.m_Parameters.size());
        m_ParameterFired.assign(p.m_Parameters.size(), 0);
        m_ParameterTriggers.assign(p.m_Parameters.size(), AudioTriggers{});
        for (size_t i = 0; i < p.m_Parameters.size(); ++i)
        {
            m_ParameterValues[i] = p.m_Parameters[i].Default;
            if (parameterOverrides)
                if (const auto found = parameterOverrides->find(p.m_Parameters[i].Name); found != parameterOverrides->end())
                    m_ParameterValues[i] = NodeGraphValueToFloat(found->second, m_ParameterValues[i]);
        }

        m_State.reset(new std::byte[std::max<size_t>(p.m_StateBytes, 1)]);
        AudioNodeSetup setup;
        setup.m_Program = &p;
        setup.m_Instance = this;
        setup.m_SampleRate = sampleRate;
        for (size_t i = 0; i < p.m_Nodes.size(); ++i)
        {
            const AudioGraphProgram::Node& node = p.m_Nodes[i];
            void* state = m_State.get() + node.StateOffset;
            if (node.Impl->Construct)
                node.Impl->Construct(state);
            setup.m_Node = &p.m_Compiled.Nodes[node.CompiledIndex];
            setup.m_Seed = seed * 2654435761u + static_cast<uint32_t>(i) * 40503u + 1u;
            setup.m_VariableCount = node.VariableCount;
            if (node.Impl->Init)
                node.Impl->Init(state, setup);
        }
    }

    AudioGraphInstance::~AudioGraphInstance()
    {
        const AudioGraphProgram& p = *m_Program;
        for (const AudioGraphProgram::Node& node : p.m_Nodes)
            if (node.Impl->Destroy)
                node.Impl->Destroy(m_State.get() + node.StateOffset);
    }

    void AudioGraphInstance::SetParameter(int32_t index, float value)
    {
        if (index >= 0 && index < static_cast<int32_t>(m_ParameterValues.size()))
            m_ParameterValues[index] = value;
    }

    void AudioGraphInstance::FireParameter(int32_t index)
    {
        if (index >= 0 && index < static_cast<int32_t>(m_ParameterFired.size()))
            m_ParameterFired[index] = 1;
    }

    bool AudioGraphInstance::Process()
    {
        const AudioGraphProgram& p = *m_Program;
        for (uint32_t slot : p.m_OutputTriggerSlots)
            m_Triggers[slot].Clear();
        for (size_t i = 0; i < m_ParameterTriggers.size(); ++i)
        {
            m_ParameterTriggers[i].Clear();
            if (m_ParameterFired[i])
            {
                m_ParameterTriggers[i].Add(0);
                m_ParameterFired[i] = 0;
            }
        }

        AudioNodeRun run;
        run.m_Instance = this;
        for (uint32_t i = 0; i < p.m_Nodes.size(); ++i)
        {
            const AudioGraphProgram::Node& node = p.m_Nodes[i];
            run.State = m_State.get() + node.StateOffset;
            run.m_Inputs = p.m_PinSlots.data() + node.FirstInput;
            run.m_Outputs = p.m_PinSlots.data() + node.FirstOutput;
            run.m_InputCount = node.InputCount;
            run.m_VariableCount = node.VariableCount;
            run.m_NodeIndex = i;
            node.Impl->Process(run);
        }

        // The Output node's channels (its right one is the left one when only that is connected: a mono graph).
        const AudioGraphProgram::Node& output = p.m_Nodes[p.m_OutputNode];
        const float* left = AudioSlot(p.m_PinSlots[output.FirstInput]);
        const float* right = p.m_StereoOutput ? AudioSlot(p.m_PinSlots[output.FirstInput + 1]) : left;
        std::memcpy(m_Left.data(), left, sizeof(float) * kAudioGraphBlockFrames);
        std::memcpy(m_Right.data(), right, sizeof(float) * kAudioGraphBlockFrames);
        if (m_Finished && m_FinishFrame < kAudioGraphBlockFrames)
        {
            std::fill(m_Left.begin() + m_FinishFrame, m_Left.end(), 0.0f);
            std::fill(m_Right.begin() + m_FinishFrame, m_Right.end(), 0.0f);
        }
        m_FirstBlock = false;
        return !m_Finished;
    }
}
