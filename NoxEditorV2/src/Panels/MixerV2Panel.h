#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "NoxCore/Audio/AudioMixer.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/UI/Core/UITypes.h"

namespace Nox
{
    namespace UI
    {
        class UIContext;
        class UIElement;
        class UIStackBox;
    }

    // Window > Mixer: the project's audio mixer (.nmixer) -- a strip per bus (name, where it plays into, meter, fader, the live gain
    // reduction, mute / solo / bypass, its effects), the selected bus's details (name, Output To, volume, effects with their
    // parameters, sends, ducking), snapshots, the voice budget, headphones and the acoustics (Steam Audio reflections, baked reverb).
    // Values a snapshot being edited overrides are marked (orange dot); right-click a value: Reset, Remove from Snapshot. Its own
    // history (the whole mixer per finished change, 100 steps): Ctrl+Z / Ctrl+Y while the window has the keyboard.
    class MixerV2Panel
    {
    public:
        const Ref<UI::UIElement>& BuildUI(UI::UIContext* context);
        const Ref<UI::UIElement>& GetRoot() const { return m_Root; }
        // Once a frame while shown: meters, values, the history.
        void Update();

    private:
        uint64_t StructureSignature() const;
        void Rebuild();
        void BuildToolbar(UI::UIStackBox& into);
        void BuildSnapshots(UI::UIStackBox& into);
        void BuildStrips(UI::UIStackBox& into);
        void BuildDetails(UI::UIStackBox& into);
        void ShowAcoustics(const UI::UIRect& anchor);
        // A value a snapshot can override: a field with the override dot, right-click Reset / Remove from Snapshot.
        Ref<UI::UIElement> ValueRow(const std::string& label, const AudioMixerValueKey& key, float min, float max, const char* format, float resetValue,
                                    const std::string& tooltip = {});
        void DeleteBus(uint64_t id);
        void Changed(bool structure);
        void CommitHistory();
        void Undo();
        void Redo();
        bool HandleKey(int keyCode);

        UI::UIContext* m_Context = nullptr;
        Ref<UI::UIElement> m_Root;
        Ref<UI::UIStackBox> m_Body;
        std::vector<std::function<void()>> m_Syncs;
        uint64_t m_Signature = 0;
        bool m_Rebuild = true;
        uint64_t m_SelectedBus = kAudioBusMaster;
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
