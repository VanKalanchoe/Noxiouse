#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/core.h"
#include "NoxCore/NodeGraph/CompiledGraph.h"
#include "NoxCore/NodeGraph/NodeGraph.h"
#include "AudioClip.h"

namespace Nox
{
    // The audio graph (docs/Audio_Architecture_Plan_2026.md §5; Unreal's MetaSounds): a NodeGraph of domain "AudioGraph", authored in
    // the node graph editor and saved as .naudiograph, compiled into an AudioGraphProgram that every playing voice of it runs once per
    // 256-frame block on the mixer thread -- node after node in dependency order, each reading its inputs' pin slots and writing its
    // outputs'. A voice's pin slots and node states live in memory sized when it is created (AudioGraphInstance); running it never
    // allocates or locks.
    //
    // Pins are of five types: Audio (a block of samples), Float, Int and Bool (one value a block) and Trigger (events at frame offsets
    // inside the block: sample-accurate starts and stops).
    inline constexpr const char* kAudioGraphDomain = "AudioGraph";
    inline constexpr uint32_t kAudioGraphBlockFrames = 256;
    inline constexpr uint32_t kMaxTriggersPerBlock = 16;

    enum class AudioPinType : uint8_t
    {
        Audio,
        Float,
        Int,
        Bool,
        Trigger
    };

    // The events on a Trigger pin this block: frame offsets, ascending.
    struct AudioTriggers
    {
        uint32_t Count = 0;
        uint16_t Offsets[kMaxTriggersPerBlock] = {};

        void Add(uint32_t frame)
        {
            if (Count == kMaxTriggersPerBlock)
                return;
            uint32_t i = Count++;
            while (i > 0 && Offsets[i - 1] > frame) // (kept sorted: merged triggers come in any order)
            {
                Offsets[i] = Offsets[i - 1];
                --i;
            }
            Offsets[i] = static_cast<uint16_t>(frame);
        }
        bool Any() const { return Count > 0; }
        void Clear() { Count = 0; }
    };

    class AudioGraphProgram;
    class AudioGraphInstance;

    // A node implementation's view at voice creation (game thread): its properties and literals, the clips the program holds, and
    // streams for clips decoded while they play.
    class AudioNodeSetup
    {
    public:
        float Float(const std::string& name, float fallback = 0.0f) const;
        int32_t Int(const std::string& name, int32_t fallback = 0) const;
        bool Bool(const std::string& name, bool fallback = false) const;
        uint64_t Handle(const std::string& name) const;
        std::string String(const std::string& name) const;
        const AudioClip* Clip(uint64_t handle) const; // null: none, or not an audio clip
        AudioClipStream* CreateStream();              // owned by the voice, filled by the engine's stream thread
        int32_t ParameterIndex(const std::string& name) const; // -1: none
        float SampleRate() const { return m_SampleRate; }
        uint32_t VoiceSeed() const { return m_Seed; }  // differs per voice: random nodes without a seed of their own vary between voices
        uint32_t VariableCount() const { return m_VariableCount; }

    private:
        friend class AudioGraphInstance;
        const CompiledNode* m_Node = nullptr;
        const AudioGraphProgram* m_Program = nullptr;
        AudioGraphInstance* m_Instance = nullptr;
        float m_SampleRate = 48000.0f;
        uint32_t m_Seed = 1;
        uint32_t m_VariableCount = 0;
    };

    // A node implementation's view of one block (mixer thread). Inputs and outputs by their index on the node.
    class AudioNodeRun
    {
    public:
        const float* Audio(uint32_t input) const;
        float Float(uint32_t input) const;
        int32_t Int(uint32_t input) const;
        bool Bool(uint32_t input) const;
        const AudioTriggers& Trigger(uint32_t input) const;
        bool IsConnected(uint32_t input) const;

        float* AudioOut(uint32_t output) const;
        void SetFloat(uint32_t output, float value) const;
        void SetInt(uint32_t output, int32_t value) const;
        void SetBool(uint32_t output, bool value) const;
        AudioTriggers& TriggerOut(uint32_t output) const;

        uint32_t Frames() const { return kAudioGraphBlockFrames; }
        float SampleRate() const;
        // The voice's pitch (the source's Pitch times Doppler): Wave Players and oscillators follow it.
        float VoicePitch() const;
        bool IsFirstBlock() const;
        float Parameter(int32_t index) const;
        const AudioTriggers& ParameterTrigger(int32_t index) const;
        uint32_t VariableCount() const { return m_VariableCount; }
        uint32_t InputCount() const { return m_InputCount; }
        // The Output node: the voice ends at this frame (its sound after it is cut).
        void FinishVoice(uint32_t frame) const;
        void ReportStarvation() const;

        void* State = nullptr;

    private:
        friend class AudioGraphInstance;
        AudioGraphInstance* m_Instance = nullptr;
        const uint32_t* m_Inputs = nullptr;
        const uint32_t* m_Outputs = nullptr;
        uint32_t m_InputCount = 0;
        uint32_t m_VariableCount = 0;
        uint32_t m_NodeIndex = 0;
    };

    // What runs a node type (registered beside its NodeTypeDesc by RegisterAudioGraphNodeTypes).
    struct AudioNodeImpl
    {
        size_t StateSize = 0;
        size_t StateAlign = alignof(std::max_align_t);
        void (*Construct)(void* state) = nullptr;                     // placement-new of the state
        void (*Destroy)(void* state) = nullptr;                       // its destructor (game thread)
        void (*Init)(void* state, AudioNodeSetup& setup) = nullptr;   // game thread, at voice creation
        void (*Process)(AudioNodeRun& run) = nullptr;                 // mixer thread, every block
    };

    void RegisterAudioNodeImpl(const std::string& typeName, const AudioNodeImpl& impl);
    const AudioNodeImpl* FindAudioNodeImpl(const std::string& typeName);
    AudioPinType AudioPinTypeFromName(const std::string& name);

    // The audio graph as it runs: compiled from the asset's NodeGraph (GraphCompiler's checks and order), its pins laid out as slots,
    // its clips held. Shared, immutable, by every voice playing it; a live edit builds a new one.
    class AudioGraphProgram
    {
    public:
        struct Node
        {
            const AudioNodeImpl* Impl = nullptr;
            uint32_t CompiledIndex = 0;   // in Compiled.Nodes
            uint32_t FirstInput = 0;      // into Slots
            uint32_t InputCount = 0;
            uint32_t FirstOutput = 0;
            uint32_t OutputCount = 0;
            size_t StateOffset = 0;
            uint32_t VariableCount = 0;
            uint64_t ConnectedMask = 0;   // inputs fed by a link (the first 64)
        };
        struct Constant
        {
            uint32_t Slot = 0;
            float Value = 0.0f;
            uint32_t Node = 0;   // in m_Nodes
            uint32_t Input = 0;  // its input pin
        };
        struct Parameter
        {
            std::string Name;
            AudioPinType Type = AudioPinType::Float;
            float Default = 0.0f;
        };

        // Compiles `graph`; the clips its nodes name must be loaded (the ones that are not are put in outMissing and the build fails --
        // ask for them and build again). Null with a reason in outError when the graph does not compile.
        static std::shared_ptr<AudioGraphProgram> Build(const NodeGraph& graph, std::vector<AssetHandle>& outMissing, std::string& outError);
        // The clips `graph` names (its nodes' audio clip properties).
        static void CollectClips(const NodeGraph& graph, std::vector<AssetHandle>& outClips);

        int32_t FindParameter(const std::string& name) const;
        // A live edit that changed only unconnected inputs' literals (MetaSounds updates those in the playing sound): true with the
        // value slots to change when `newer` has this program's nodes, links and node settings; false when it must restart.
        bool LiteralChanges(const AudioGraphProgram& newer, std::vector<std::pair<uint32_t, float>>& outChanges) const;
        const std::vector<Parameter>& GetParameters() const { return m_Parameters; }
        const AudioClip* FindClip(uint64_t handle) const;

    private:
        friend class AudioGraphInstance;
        friend class AudioNodeSetup;
        friend class AudioNodeRun;
        CompiledGraph m_Compiled;
        std::vector<Node> m_Nodes;           // in evaluation order
        std::vector<uint32_t> m_PinSlots;    // per node: its inputs' slots, then its outputs'
        std::vector<Constant> m_Constants;   // value slots holding unconnected inputs' literals
        std::vector<uint32_t> m_OutputTriggerSlots; // cleared before every block
        uint32_t m_AudioSlots = 0;           // slot 0: silence (unconnected audio inputs)
        uint32_t m_ValueSlots = 0;
        uint32_t m_TriggerSlots = 0;         // slot 0: never fires (unconnected trigger inputs)
        size_t m_StateBytes = 0;
        std::vector<Parameter> m_Parameters;
        int32_t m_OutputNode = -1;           // in m_Nodes
        bool m_StereoOutput = false;         // the Output node's right channel is connected (else it is the left one)
        std::unordered_map<uint64_t, Ref<AudioClip>> m_Clips;
    };

    // One playing voice of a program: its pin slots, node states, parameter values and streams. Created and destroyed on the game
    // thread, run on the mixer thread.
    class AudioGraphInstance
    {
    public:
        AudioGraphInstance(std::shared_ptr<const AudioGraphProgram> program, float sampleRate, uint32_t seed,
                           const std::unordered_map<std::string, NodeGraphValue>* parameterOverrides);
        ~AudioGraphInstance();
        AudioGraphInstance(const AudioGraphInstance&) = delete;
        AudioGraphInstance& operator=(const AudioGraphInstance&) = delete;

        // Mixer thread.
        void SetParameter(int32_t index, float value);
        void FireParameter(int32_t index);
        void SetConstant(uint32_t slot, float value) { if (slot < m_Values.size()) m_Values[slot] = value; } // (a live literal edit)
        void SetVoicePitch(float pitch) { m_VoicePitch = pitch; }
        // One block. False once the graph has finished (the Output node's On Finished fired): its last samples are in Left / Right.
        bool Process();
        const float* Left() const { return m_Left.data(); }
        const float* Right() const { return m_Right.data(); }
        uint64_t TakeStarvations() { const uint64_t count = m_Starvations; m_Starvations = 0; return count; }

        const std::vector<std::shared_ptr<AudioClipStream>>& GetStreams() const { return m_Streams; }
        const AudioGraphProgram& GetProgram() const { return *m_Program; }

        // Slots (for AudioNodeRun).
        float* AudioSlot(uint32_t slot) { return m_Audio.data() + static_cast<size_t>(slot) * kAudioGraphBlockFrames; }
        float& ValueSlot(uint32_t slot) { return m_Values[slot]; }
        AudioTriggers& TriggerSlot(uint32_t slot) { return m_Triggers[slot]; }

    private:
        friend class AudioNodeRun;
        friend class AudioNodeSetup;
        std::shared_ptr<const AudioGraphProgram> m_Program;
        float m_SampleRate = 48000.0f;
        float m_VoicePitch = 1.0f;
        bool m_FirstBlock = true;
        bool m_Finished = false;
        uint32_t m_FinishFrame = kAudioGraphBlockFrames;
        uint64_t m_Starvations = 0;
        std::vector<float> m_Audio;
        std::vector<float> m_Values;
        std::vector<AudioTriggers> m_Triggers;
        std::vector<float> m_ParameterValues;
        std::vector<uint8_t> m_ParameterFired;
        std::vector<AudioTriggers> m_ParameterTriggers;
        std::vector<float> m_Left, m_Right;
        std::unique_ptr<std::byte[]> m_State;
        std::vector<std::shared_ptr<AudioClipStream>> m_Streams;
    };

    inline const float* AudioNodeRun::Audio(uint32_t input) const { return m_Instance->AudioSlot(m_Inputs[input]); }
    inline float AudioNodeRun::Float(uint32_t input) const { return m_Instance->ValueSlot(m_Inputs[input]); }
    inline int32_t AudioNodeRun::Int(uint32_t input) const { return static_cast<int32_t>(m_Instance->ValueSlot(m_Inputs[input])); }
    inline bool AudioNodeRun::Bool(uint32_t input) const { return m_Instance->ValueSlot(m_Inputs[input]) != 0.0f; }
    inline const AudioTriggers& AudioNodeRun::Trigger(uint32_t input) const { return m_Instance->TriggerSlot(m_Inputs[input]); }
    inline float* AudioNodeRun::AudioOut(uint32_t output) const { return m_Instance->AudioSlot(m_Outputs[output]); }
    inline void AudioNodeRun::SetFloat(uint32_t output, float value) const { m_Instance->ValueSlot(m_Outputs[output]) = value; }
    inline void AudioNodeRun::SetInt(uint32_t output, int32_t value) const { m_Instance->ValueSlot(m_Outputs[output]) = static_cast<float>(value); }
    inline void AudioNodeRun::SetBool(uint32_t output, bool value) const { m_Instance->ValueSlot(m_Outputs[output]) = value ? 1.0f : 0.0f; }
    inline AudioTriggers& AudioNodeRun::TriggerOut(uint32_t output) const { return m_Instance->TriggerSlot(m_Outputs[output]); }

    // An audio graph asset's program as the asset is now (main thread): built once the asset and its clips are loaded (asked for
    // here), rebuilt after an edit (its CompileVersion). Null while they load, or -- with the reason in outError -- when it does not
    // compile. Shared by the editor's previews and every Audio Source playing it.
    std::shared_ptr<const AudioGraphProgram> GetAudioGraphProgram(AssetHandle graph, std::string* outError = nullptr);

    // Registers the audio graph's node types (NodeTypeRegistry, domain kAudioGraphDomain) and their implementations. Once, at startup.
    void RegisterAudioGraphNodeTypes();
    // A new audio graph as the editor's create action starts it: the Output node.
    NodeGraph CreateEmptyAudioGraph();
}
