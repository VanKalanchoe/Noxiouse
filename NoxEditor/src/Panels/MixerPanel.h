#pragma once

#include <cstdint>
#include <vector>

#include "NoxCore/Audio/AudioMixer.h"

namespace Nox
{
    // The Mixer panel (docs/Audio_Architecture_Plan_2026.md §7.1; Unity's Audio Mixer window, Godot's Audio bottom panel): every bus of
    // the project's mixer side by side as a channel strip -- fader, live level meter, mute / solo / bypass, its effects -- and below,
    // the selected bus in full: Output To, its effect chain with every parameter, its sends and its ducking. Snapshots across the top:
    // pick one to hear it and to store changes in it. Edits are heard at once (also in Play), saved with File > Save (Ctrl+S) or the
    // panel's Save, undone with Ctrl+Z / Ctrl+Y while the panel has focus. Window > Mixer, or double-click the .nmixer asset.
    class MixerPanel
    {
    public:
        void OnImGuiRender();

        bool IsOpen() const { return m_Open; }
        void SetOpen(bool open) { m_Open = open; }
        // Opens and brings it to the front.
        void Focus() { m_Open = true; m_FocusRequested = true; }

    private:
        void DrawToolbar(AudioMixerData& data);
        void DrawSnapshots(AudioMixerData& data);
        void DrawStrips(AudioMixerData& data);
        void DrawStrip(AudioMixerData& data, AudioMixerBus& bus, float height);
        void DrawDetails(AudioMixerData& data);
        void DrawEffect(AudioMixerData& data, AudioMixerBus& bus, size_t index, int& moveFrom, int& moveTo, int& removed);
        void DrawSends(AudioMixerData& data, AudioMixerBus& bus);
        void DrawDucking(AudioMixerData& data, AudioMixerBus& bus);

        // A snapshot-aware number: shown orange when the edited snapshot overrides it; right-click to reset or take it out.
        bool ValueSlider(const char* label, const AudioMixerValueKey& key, float min, float max, const char* format, bool logarithmic, float resetValue);
        void DeleteBus(AudioMixerData& data, uint64_t id);
        void Changed(bool structure);
        void UpdateUndo(AudioMixerData& data);
        void Undo(AudioMixerData& data);
        void Redo(AudioMixerData& data);

    private:
        bool m_Open = false;
        bool m_FocusRequested = false;
        uint64_t m_SelectedBus = kAudioBusMaster;
        int32_t m_RenamingSnapshot = -1;
        char m_RenameBuffer[64] = {};

        // Undo / redo: the whole mixer before each change (it is small).
        std::vector<AudioMixerData> m_UndoStack;
        std::vector<AudioMixerData> m_RedoStack;
        AudioMixerData m_Committed;
        bool m_HaveCommitted = false;
        bool m_PendingChange = false;
        std::filesystem::path m_LoadedPath; // the mixer the history is of
    };
}
