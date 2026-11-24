#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "NoxCore/Audio/AudioMixer.h"
#include "NoxCore/Core/Ref.h"
#include <NoxUI/Core/UITypes.h>

namespace NoxUI
{
        class UIContext;
        class UIElement;
        class UIStackBox;
    }

namespace Nox
{

    // Window > Mixer: the project's audio mixer (.nmixer) -- a strip per bus (name, where it plays into, meter, fader, the live gain
    // reduction, mute / solo / bypass, its effects), the selected bus's details (name, Output To, volume, effects with their
    // parameters, sends, ducking), snapshots, the voice budget, headphones and the acoustics (Steam Audio reflections, baked reverb).
    // Values a snapshot being edited overrides are marked (orange dot); right-click a value: Reset, Remove from Snapshot. Its own
    // history (the whole mixer per finished change, 100 steps): Ctrl+Z / Ctrl+Y while the window has the keyboard.
    class MixerPanel
    {
    public:
        const Ref<NoxUI::UIElement>& BuildUI(NoxUI::UIContext* context);
        const Ref<NoxUI::UIElement>& GetRoot() const { return m_Root; }
        // Once a frame while shown: meters, values, the history.
        void Update();

    private:
        uint64_t StructureSignature() const;
        void Rebuild();
        void BuildToolbar(NoxUI::UIStackBox& into);
        void BuildSnapshots(NoxUI::UIStackBox& into);
        void BuildStrips(NoxUI::UIStackBox& into);
        void BuildDetails(NoxUI::UIStackBox& into);
        void ShowAcoustics(const NoxUI::UIRect& anchor);
        // A value a snapshot can override: a field with the override dot, right-click Reset / Remove from Snapshot.
        Ref<NoxUI::UIElement> ValueRow(const std::string& label, const AudioMixerValueKey& key, float min, float max, const char* format, float resetValue,
                                    const std::string& tooltip = {});
        void DeleteBus(uint64_t id);
        void Changed(bool structure);
        void CommitHistory();
        void Undo();
        void Redo();
        bool HandleKey(const NoxUI::UIKeyEvent& e);

        NoxUI::UIContext* m_Context = nullptr;
        Ref<NoxUI::UIElement> m_Root;
        Ref<NoxUI::UIStackBox> m_Body;
        uint64_t m_Signature = 0;
        bool m_Rebuild = true;
        uint64_t m_SelectedBus = NoxAudio::kAudioBusMaster;
        std::unordered_map<uint64_t, bool> m_CollapsedEffects;

        // History: the whole mixer before each finished change (it is small).
        std::vector<AudioMixerData> m_UndoStack;
        std::vector<AudioMixerData> m_RedoStack;
        AudioMixerData m_Committed;
        bool m_HaveCommitted = false;
        bool m_PendingChange = false;
        std::filesystem::path m_LoadedPath; // the mixer the history is of
    };
}
