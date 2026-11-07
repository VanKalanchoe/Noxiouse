#include "MixerPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <type_traits>

#include <imgui.h>
#include "misc/cpp/imgui_stdlib.h"

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Audio/AudioDSP.h"
#include "NoxCore/Core/UUID.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    namespace
    {
        constexpr float kStripWidth = 132.0f;
        constexpr float kMinDb = -60.0f;  // the meters' floor
        constexpr float kMaxDb = 6.0f;
        constexpr float kFaderMinDb = AudioDSP::kSilenceDb;
        constexpr float kFaderMaxDb = 12.0f;
        const ImVec4 kOverrideColor(0.85f, 0.45f, 0.1f, 0.55f); // a value the edited snapshot changes
        const ImVec4 kOnColor(0.2f, 0.55f, 0.85f, 1.0f);

        float MeterFraction(float linear)
        {
            const float db = AudioDSP::LinearToDb(linear);
            return std::clamp((db - kMinDb) / (kMaxDb - kMinDb), 0.0f, 1.0f);
        }

        ImU32 MeterColor(float fraction)
        {
            const float db = kMinDb + fraction * (kMaxDb - kMinDb);
            if (db > -0.5f)
                return IM_COL32(230, 60, 50, 255);   // at / over full scale
            if (db > -12.0f)
                return IM_COL32(230, 200, 60, 255);  // loud
            return IM_COL32(70, 200, 90, 255);
        }

        // Two bars (left, right): RMS filled, the peak a line; a tick every 12 dB.
        void DrawMeter(const AudioBusMeter& meter, ImVec2 size)
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(20, 20, 24, 255));
            const float barWidth = (size.x - 3.0f) * 0.5f;
            const float values[2][2] = { { meter.RmsLeft, meter.PeakLeft }, { meter.RmsRight, meter.PeakRight } };
            for (int channel = 0; channel < 2; ++channel)
            {
                const float x0 = origin.x + 1.0f + static_cast<float>(channel) * (barWidth + 1.0f);
                const float x1 = x0 + barWidth;
                const float rms = MeterFraction(values[channel][0]);
                const float peak = MeterFraction(values[channel][1]);
                const float bottom = origin.y + size.y;
                draw->AddRectFilled(ImVec2(x0, bottom - rms * size.y), ImVec2(x1, bottom), MeterColor(rms));
                if (peak > 0.0f)
                    draw->AddLine(ImVec2(x0, bottom - peak * size.y), ImVec2(x1, bottom - peak * size.y), MeterColor(peak), 2.0f);
            }
            for (float db = 0.0f; db >= kMinDb; db -= 12.0f)
            {
                const float y = origin.y + size.y * (1.0f - (db - kMinDb) / (kMaxDb - kMinDb));
                draw->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + 3.0f, y), IM_COL32(160, 160, 170, 160));
            }
            ImGui::Dummy(size);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Peak L %.1f dB / R %.1f dB\nRMS L %.1f dB / R %.1f dB\nTicks every 12 dB from 0 dB down",
                                  AudioDSP::LinearToDb(meter.PeakLeft), AudioDSP::LinearToDb(meter.PeakRight),
                                  AudioDSP::LinearToDb(meter.RmsLeft), AudioDSP::LinearToDb(meter.RmsRight));
        }

        bool ToggleButton(const char* label, bool on, const char* tooltip)
        {
            if (on)
                ImGui::PushStyleColor(ImGuiCol_Button, kOnColor);
            const bool clicked = ImGui::SmallButton(label);
            if (on)
                ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", tooltip);
            return clicked;
        }

        // Buses that `bus` may route into (Output To, a send target, a sidechain or ducking source) without closing a loop.
        template <typename Fn>
        bool BusCombo(const char* label, const AudioMixerData& data, uint64_t current, bool allowNone, Fn&& allowed, uint64_t& outChosen)
        {
            const AudioMixerBus* found = data.FindBus(current);
            const char* preview = found ? found->Name.c_str() : "None";
            bool changed = false;
            if (ImGui::BeginCombo(label, preview))
            {
                if (allowNone && ImGui::Selectable("None", current == 0))
                {
                    outChosen = 0;
                    changed = true;
                }
                for (const AudioMixerBus& candidate : data.Buses)
                {
                    if (!allowed(candidate))
                        continue;
                    ImGui::PushID(static_cast<int>(candidate.Id & 0x7fffffff));
                    if (ImGui::Selectable(candidate.Name.c_str(), candidate.Id == current))
                    {
                        outChosen = candidate.Id;
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            return changed;
        }
    }

    void MixerPanel::Changed(bool structure)
    {
        if (structure)
            AudioMixer::NotifyStructureChanged();
        else
            AudioMixer::NotifyValuesChanged();
        m_PendingChange = true;
    }

    bool MixerPanel::ValueSlider(const char* label, const AudioMixerValueKey& key, float min, float max, const char* format, bool logarithmic, float resetValue)
    {
        float value = AudioMixer::GetValue(key);
        const bool overridden = AudioMixer::IsOverridden(key);
        if (overridden)
        {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, kOverrideColor);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kOverrideColor);
        }
        const bool changed = ImGui::SliderFloat(label, &value, min, max, format, logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
        if (overridden)
            ImGui::PopStyleColor(2);
        if (changed)
        {
            AudioMixer::SetValue(key, value);
            m_PendingChange = true;
        }
        if (ImGui::BeginPopupContextItem())
        {
            char reset[64];
            std::snprintf(reset, sizeof(reset), "Reset (%g)", resetValue);
            if (ImGui::MenuItem(reset))
            {
                AudioMixer::SetValue(key, resetValue);
                m_PendingChange = true;
            }
            if (ImGui::MenuItem("Remove from Snapshot", nullptr, false, overridden))
            {
                AudioMixer::ClearOverride(key);
                m_PendingChange = true;
            }
            ImGui::EndPopup();
        }
        return changed;
    }

    void MixerPanel::OnImGuiRender()
    {
        if (!m_Open)
            return;
        if (m_FocusRequested)
        {
            ImGui::SetNextWindowFocus();
            m_FocusRequested = false;
        }
        ImGui::SetNextWindowSize(ImVec2(900.0f, 620.0f), ImGuiCond_FirstUseEver);
        const std::string title = std::string("Mixer") + (AudioMixer::IsDirty() ? " *" : "") + "###Mixer";
        if (!ImGui::Begin(title.c_str(), &m_Open))
        {
            ImGui::End();
            return;
        }
        AudioMixerData& data = AudioMixer::GetData();
        if (AudioMixer::GetPath() != m_LoadedPath)
        {
            // Another project's mixer: its own history.
            m_LoadedPath = AudioMixer::GetPath();
            m_UndoStack.clear();
            m_RedoStack.clear();
            m_HaveCommitted = false;
            m_SelectedBus = kAudioBusMaster;
        }
        if (!data.FindBus(m_SelectedBus))
            m_SelectedBus = kAudioBusMaster;

        // Shortcuts while the panel has focus.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive())
        {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
                io.KeyShift ? Redo(data) : Undo(data);
            else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
                Redo(data);
        }

        DrawToolbar(data);
        DrawSnapshots(data);
        ImGui::Separator();
        const float stripsHeight = std::max(260.0f, ImGui::GetContentRegionAvail().y * 0.5f);
        if (ImGui::BeginChild("Strips", ImVec2(0.0f, stripsHeight), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
            DrawStrips(data);
        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::BeginChild("Details"))
            DrawDetails(data);
        ImGui::EndChild();

        UpdateUndo(data);
        ImGui::End();
    }

    void MixerPanel::DrawToolbar(AudioMixerData& data)
    {
        ImGui::BeginDisabled(!AudioMixer::IsDirty() || AudioMixer::GetPath().empty());
        if (ImGui::Button("Save"))
            AudioMixer::Save();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Writes the mixer to its file (File > Save Scene saves it too).");
        ImGui::SameLine();
        ImGui::BeginDisabled(data.Buses.size() >= AudioEngine::MaxBuses);
        if (ImGui::Button("Add Bus"))
        {
            AudioMixerBus bus;
            bus.Id = UUID();
            int number = static_cast<int>(data.Buses.size());
            do
                bus.Name = "Bus " + std::to_string(number++);
            while (AudioMixer::FindBus(bus.Name));
            bus.Parent = kAudioBusMaster;
            data.Buses.push_back(bus);
            m_SelectedBus = bus.Id;
            Changed(true);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("A new bus into Master (up to %u). Set its Output To below to put it under another bus.", AudioEngine::MaxBuses);
        ImGui::SameLine();
        ImGui::BeginDisabled(m_UndoStack.empty());
        if (ImGui::Button("Undo"))
            Undo(data);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_RedoStack.empty());
        if (ImGui::Button("Redo"))
            Redo(data);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", AudioMixer::GetPath().empty() ? "(no project: the default buses)" : AudioMixer::GetPath().filename().string().c_str());
    }

    void MixerPanel::DrawSnapshots(AudioMixerData& data)
    {
        ImGui::TextUnformatted("Snapshot:");
        const int32_t edited = AudioMixer::GetEditedSnapshot();
        ImGui::SameLine();
        if (ImGui::RadioButton("Default", edited < 0))
            AudioMixer::SetEditedSnapshot(-1);
        ImGui::SetItemTooltip("The mix itself: every value as set here. Snapshots change some of these values.");
        int32_t remove = -1;
        int32_t duplicate = -1;
        for (int32_t i = 0; i < static_cast<int32_t>(data.Snapshots.size()); ++i)
        {
            ImGui::SameLine();
            ImGui::PushID(i);
            if (m_RenamingSnapshot == i)
            {
                ImGui::SetNextItemWidth(120.0f);
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##rename", m_RenameBuffer, sizeof(m_RenameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
                {
                    if (m_RenameBuffer[0] != '\0')
                    {
                        data.Snapshots[i].Name = m_RenameBuffer;
                        Changed(false);
                    }
                    m_RenamingSnapshot = -1;
                }
                if (ImGui::IsItemDeactivated())
                    m_RenamingSnapshot = -1;
            }
            else
            {
                if (ImGui::RadioButton(data.Snapshots[i].Name.c_str(), edited == i))
                    AudioMixer::SetEditedSnapshot(i);
                ImGui::SetItemTooltip("Pick to hear it and to change it: what you change is stored in this snapshot (shown orange). Right-click: rename, duplicate, delete.");
                if (ImGui::BeginPopupContextItem())
                {
                    if (ImGui::MenuItem("Rename"))
                    {
                        m_RenamingSnapshot = i;
                        std::snprintf(m_RenameBuffer, sizeof(m_RenameBuffer), "%s", data.Snapshots[i].Name.c_str());
                    }
                    if (ImGui::MenuItem("Duplicate"))
                        duplicate = i;
                    if (ImGui::MenuItem("Delete"))
                        remove = i;
                    ImGui::EndPopup();
                }
            }
            ImGui::PopID();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+"))
        {
            AudioMixerSnapshot snapshot;
            int number = static_cast<int>(data.Snapshots.size()) + 1;
            auto taken = [&](const std::string& name) { return std::any_of(data.Snapshots.begin(), data.Snapshots.end(), [&](const AudioMixerSnapshot& s) { return s.Name == name; }); };
            do
                snapshot.Name = "Snapshot " + std::to_string(number++);
            while (taken(snapshot.Name));
            data.Snapshots.push_back(snapshot);
            AudioMixer::SetEditedSnapshot(static_cast<int32_t>(data.Snapshots.size()) - 1);
            Changed(false);
        }
        ImGui::SetItemTooltip("A new snapshot (it starts as the Default mix; change faders, sends and effect values to store them in it).");
        if (duplicate >= 0)
        {
            AudioMixerSnapshot copy = data.Snapshots[duplicate];
            copy.Name += " Copy";
            data.Snapshots.push_back(copy);
            AudioMixer::SetEditedSnapshot(static_cast<int32_t>(data.Snapshots.size()) - 1);
            Changed(false);
        }
        if (remove >= 0)
        {
            data.Snapshots.erase(data.Snapshots.begin() + remove);
            AudioMixer::SetEditedSnapshot(-1);
            Changed(false);
        }
        const std::string playing = AudioMixer::GetSnapshot();
        const bool editingSnapshot = edited >= 0 && static_cast<size_t>(edited) < data.Snapshots.size();
        if (editingSnapshot || playing != "Default")
        {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.3f, 1.0f), "%s%s",
                editingSnapshot ? ("Editing '" + data.Snapshots[edited].Name + "': fader, send and effect values you change are stored in it (orange). Right-click one: Remove from Snapshot.  ").c_str() : "",
                playing != (editingSnapshot ? data.Snapshots[edited].Name : std::string("Default")) ? ("Heard now (set by the game): " + playing).c_str() : "");
        }
    }

    void MixerPanel::DrawStrips(AudioMixerData& data)
    {
        const float height = ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ScrollbarSize;
        for (AudioMixerBus& bus : data.Buses)
        {
            ImGui::PushID(static_cast<int>(bus.Id & 0x7fffffff));
            DrawStrip(data, bus, height);
            ImGui::PopID();
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }

    void MixerPanel::DrawStrip(AudioMixerData& data, AudioMixerBus& bus, float height)
    {
        const bool selected = bus.Id == m_SelectedBus;
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.22f, 0.26f, 0.32f, 1.0f));
        ImGui::BeginChild("Strip", ImVec2(kStripWidth, std::max(height, 200.0f)), ImGuiChildFlags_Borders);
        if (selected)
            ImGui::PopStyleColor();

        // Name (click: select), the parent.
        if (ImGui::Selectable(bus.Name.c_str(), selected, 0, ImVec2(0.0f, 0.0f)))
            m_SelectedBus = bus.Id;
        ImGui::SetItemTooltip("Select to edit its effects, sends and ducking below.");
        const AudioMixerBus* parent = data.FindBus(bus.Parent);
        ImGui::TextDisabled("-> %s", bus.Id == kAudioBusMaster ? "Speakers" : (parent ? parent->Name.c_str() : "Master"));

        // Meter and fader side by side.
        const AudioBusMeter meter = AudioMixer::GetMeter(bus.Id);
        const float faderHeight = std::max(80.0f, ImGui::GetContentRegionAvail().y - 150.0f);
        DrawMeter(meter, ImVec2(22.0f, faderHeight));
        ImGui::SameLine();
        const AudioMixerValueKey volume{ bus.Id, AudioMixerValueKind::Volume, 0, 0 };
        float db = AudioMixer::GetValue(volume);
        const bool overridden = AudioMixer::IsOverridden(volume);
        if (overridden)
            ImGui::PushStyleColor(ImGuiCol_FrameBg, kOverrideColor);
        if (ImGui::VSliderFloat("##Fader", ImVec2(28.0f, faderHeight), &db, kFaderMinDb, kFaderMaxDb, ""))
        {
            AudioMixer::SetValue(volume, db);
            m_PendingChange = true;
        }
        if (overridden)
            ImGui::PopStyleColor();
        if (ImGui::IsItemActive() || ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%.1f dB (right-click: 0 dB / remove from snapshot)", db);
        if (ImGui::BeginPopupContextItem("FaderMenu"))
        {
            if (ImGui::MenuItem("0 dB"))
            {
                AudioMixer::SetValue(volume, 0.0f);
                m_PendingChange = true;
            }
            if (ImGui::MenuItem("Remove from Snapshot", nullptr, false, overridden))
            {
                AudioMixer::ClearOverride(volume);
                m_PendingChange = true;
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        // The live gain reduction (compressors, limiters, gates, ducking).
        ImGui::BeginGroup();
        if (meter.GainReductionDb > 0.05f)
            ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1.0f), "-%.1f", meter.GainReductionDb);
        else
            ImGui::TextDisabled("GR");
        ImGui::SetItemTooltip("Gain reduction now (dB): how far its compressor, limiter, gate or ducking turns it down.");
        ImGui::EndGroup();

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat("##dB", &db, 0.1f, kFaderMinDb, kFaderMaxDb, db <= kFaderMinDb ? "-inf dB" : "%.1f dB"))
        {
            AudioMixer::SetValue(volume, db);
            m_PendingChange = true;
        }

        // Mute / solo / bypass.
        if (ToggleButton("M", bus.Mute, "Mute"))
        {
            bus.Mute = !bus.Mute;
            Changed(false);
        }
        ImGui::SameLine();
        if (ToggleButton("S", bus.Solo, "Solo: only the soloed buses (with what feeds them, where they go and the buses they send to) are heard"))
        {
            bus.Solo = !bus.Solo;
            Changed(false);
        }
        ImGui::SameLine();
        if (ToggleButton("B", bus.Bypass, "Bypass every effect of this bus"))
        {
            bus.Bypass = !bus.Bypass;
            Changed(false);
        }

        // Its effects, in order (click: edit below).
        for (size_t i = 0; i < bus.Effects.size(); ++i)
        {
            const AudioMixerEffect& effect = bus.Effects[i];
            ImGui::PushID(static_cast<int>(i));
            if (effect.Bypass || bus.Bypass)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(GetAudioEffectInfo(effect.Type).Name.c_str(), false))
                m_SelectedBus = bus.Id;
            if (effect.Bypass || bus.Bypass)
                ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (bus.Effects.empty())
            ImGui::TextDisabled("(no effects)");
        ImGui::EndChild();
    }

    void MixerPanel::DrawDetails(AudioMixerData& data)
    {
        AudioMixerBus* bus = data.FindBus(m_SelectedBus);
        if (!bus)
            return;
        const bool isMaster = bus->Id == kAudioBusMaster;
        ImGui::PushID(static_cast<int>(bus->Id & 0x7fffffff));

        ImGui::SeparatorText("Bus");
        if (isMaster)
            ImGui::Text("Master -- everything ends here, then the speakers.");
        else
        {
            std::string name = bus->Name;
            if (ImGui::InputText("Name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty())
            {
                const AudioMixerBus* other = AudioMixer::FindBus(name);
                if (!other || other == bus)
                {
                    bus->Name = name;
                    Changed(false);
                }
            }
            ImGui::SetItemTooltip("Scripts name buses by this (Audio.SetBusVolume(\"Music\", 0.5f)); press Enter to rename.");
            uint64_t parent = bus->Parent;
            if (BusCombo("Output To", data, bus->Parent, false,
                    [&](const AudioMixerBus& candidate) { return candidate.Id != bus->Id && !data.WouldLoop(bus->Id, candidate.Id) && !data.IsUnder(candidate.Id, bus->Id); }, parent))
            {
                bus->Parent = parent;
                Changed(true);
            }
            ImGui::SetItemTooltip("The bus this one plays into (its parent): Music -> Master, Footsteps -> SFX.");
        }
        if (ImGui::Checkbox("Plays While Paused", &bus->PlaysWhilePaused))
            Changed(true);
        ImGui::SetItemTooltip("Its sounds keep playing while the game is paused (the UI bus: a pause menu's clicks).");
        ValueSlider("Volume", { bus->Id, AudioMixerValueKind::Volume, 0, 0 }, kFaderMinDb, kFaderMaxDb, "%.1f dB", false, 0.0f);
        if (!isMaster)
        {
            ImGui::SameLine();
            if (ImGui::Button("Delete Bus"))
                ImGui::OpenPopup("DeleteBus");
            if (ImGui::BeginPopup("DeleteBus"))
            {
                ImGui::Text("Delete '%s'? Its children go to its Output To; sounds on it play into Master.", bus->Name.c_str());
                if (ImGui::Button("Delete"))
                {
                    const uint64_t id = bus->Id;
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    ImGui::PopID();
                    DeleteBus(data, id);
                    return;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }

        ImGui::SeparatorText("Effects");
        ImGui::TextDisabled("Run top to bottom, before the fader.");
        int moveFrom = -1, moveTo = -1, removed = -1;
        for (size_t i = 0; i < bus->Effects.size(); ++i)
            DrawEffect(data, *bus, i, moveFrom, moveTo, removed);
        if (moveFrom >= 0 && moveTo >= 0 && moveTo < static_cast<int>(bus->Effects.size()))
        {
            std::swap(bus->Effects[moveFrom], bus->Effects[moveTo]);
            Changed(true);
        }
        if (removed >= 0)
        {
            const uint64_t effectId = bus->Effects[removed].Id;
            bus->Effects.erase(bus->Effects.begin() + removed);
            for (AudioMixerSnapshot& snapshot : data.Snapshots)
                std::erase_if(snapshot.Values, [&](const auto& entry) { return entry.first.Bus == bus->Id && entry.first.Kind == AudioMixerValueKind::Effect && entry.first.Item == effectId; });
            Changed(true);
        }
        if (ImGui::BeginCombo("##AddEffect", "Add Effect..."))
        {
            for (uint32_t t = 0; t < static_cast<uint32_t>(AudioEffectType::Count); ++t)
            {
                const AudioEffectInfo& info = GetAudioEffectInfo(static_cast<AudioEffectType>(t));
                if (ImGui::Selectable(info.Name.c_str()))
                {
                    AudioMixerEffect effect;
                    effect.Id = UUID();
                    effect.Type = info.Type;
                    effect.Values = DefaultEffectValues(info.Type);
                    bus->Effects.push_back(std::move(effect));
                    Changed(true);
                }
                ImGui::SetItemTooltip("%s", info.Description.c_str());
            }
            ImGui::EndCombo();
        }

        ImGui::SeparatorText("Sends");
        DrawSends(data, *bus);
        if (!isMaster)
        {
            ImGui::SeparatorText("Ducking");
            DrawDucking(data, *bus);
        }
        ImGui::PopID();
    }

    void MixerPanel::DrawEffect(AudioMixerData& data, AudioMixerBus& bus, size_t index, int& moveFrom, int& moveTo, int& removed)
    {
        AudioMixerEffect& effect = bus.Effects[index];
        const AudioEffectInfo& info = GetAudioEffectInfo(effect.Type);
        ImGui::PushID(static_cast<int>(effect.Id & 0x7fffffff));
        const std::string header = std::to_string(index + 1) + ". " + info.Name + (effect.Bypass ? "  (bypassed)" : "") + "###Effect";
        const bool open = ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::SetItemTooltip("%s", info.Description.c_str());
        // Buttons on the header's line.
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150.0f);
        if (ImGui::SmallButton(effect.Bypass ? "On" : "Off"))
        {
            effect.Bypass = !effect.Bypass;
            Changed(false);
        }
        ImGui::SetItemTooltip(effect.Bypass ? "Turn the effect back on" : "Bypass: the sound passes it untouched");
        ImGui::SameLine();
        ImGui::BeginDisabled(index == 0);
        if (ImGui::ArrowButton("Up", ImGuiDir_Up))
            moveFrom = static_cast<int>(index), moveTo = static_cast<int>(index) - 1;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(index + 1 >= bus.Effects.size());
        if (ImGui::ArrowButton("Down", ImGuiDir_Down))
            moveFrom = static_cast<int>(index), moveTo = static_cast<int>(index) + 1;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
            removed = static_cast<int>(index);
        ImGui::SetItemTooltip("Remove the effect");

        if (open)
        {
            ImGui::Indent();
            if (info.ReportsGainReduction)
            {
                const float reduction = AudioMixer::GetEffectGainReduction(bus.Id, static_cast<uint32_t>(index));
                char overlay[32];
                std::snprintf(overlay, sizeof(overlay), "-%.1f dB", reduction);
                ImGui::ProgressBar(std::clamp(reduction / 24.0f, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), overlay);
                ImGui::SetItemTooltip("Gain reduction now (up to 24 dB shown).");
            }
            if (info.Sidechain)
            {
                uint64_t key = effect.Sidechain;
                if (BusCombo("Sidechain", data, effect.Sidechain, true,
                        [&](const AudioMixerBus& candidate) { return candidate.Id != bus.Id && !data.WouldLoop(candidate.Id, bus.Id); }, key))
                {
                    effect.Sidechain = key;
                    Changed(true);
                }
                ImGui::SetItemTooltip("Listen to another bus instead of this one (None): a compressor keyed by Voice turns this bus down while someone speaks.");
            }
            if (info.ImpulseResponse)
            {
                static const std::remove_cvref_t<decltype(Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry())> kNoAssets;
                const auto& registry = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry() : kNoAssets;
                const std::string current = effect.Impulse != 0 && registry.contains(effect.Impulse) ? registry.at(effect.Impulse).FilePath.filename().string() : std::string("None");
                if (ImGui::BeginCombo("Impulse Response", current.c_str()))
                {
                    if (ImGui::Selectable("None", effect.Impulse == 0))
                    {
                        effect.Impulse = 0;
                        Changed(true);
                    }
                    for (const auto& [handle, metadata] : registry)
                    {
                        if (metadata.Type != AssetType::AudioClip)
                            continue;
                        ImGui::PushID(static_cast<int>(static_cast<uint64_t>(handle) & 0x7fffffff));
                        if (ImGui::Selectable(metadata.FilePath.filename().string().c_str(), effect.Impulse == handle))
                        {
                            effect.Impulse = handle;
                            Changed(true);
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("A recording of a space's response to a click (a .wav of a room, a hall, a cave): mono or stereo, up to 10 s. Drop a clip here.");
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                    {
                        const AssetHandle handle = *static_cast<const AssetHandle*>(payload->Data);
                        if (AssetManager::GetAssetType(handle) == AssetType::AudioClip)
                        {
                            effect.Impulse = handle;
                            Changed(true);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
            }
            for (uint32_t p = 0; p < info.Parameters.size(); ++p)
            {
                const AudioEffectParameterInfo& parameter = info.Parameters[p];
                ImGui::PushID(static_cast<int>(p));
                if (!parameter.Options.empty())
                {
                    // Choices are the mix's (snapshots blend numbers only).
                    int choice = static_cast<int>(effect.Values[p]);
                    const std::string preview = choice >= 0 && choice < static_cast<int>(parameter.Options.size()) ? parameter.Options[choice] : std::string("?");
                    if (ImGui::BeginCombo(parameter.Name.c_str(), preview.c_str()))
                    {
                        for (int o = 0; o < static_cast<int>(parameter.Options.size()); ++o)
                        {
                            if (ImGui::Selectable(parameter.Options[o].c_str(), o == choice))
                            {
                                effect.Values[p] = static_cast<float>(o);
                                Changed(false);
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
                else if (parameter.Toggle)
                {
                    bool on = effect.Values[p] >= 0.5f;
                    if (ImGui::Checkbox(parameter.Name.c_str(), &on))
                    {
                        effect.Values[p] = on ? 1.0f : 0.0f;
                        Changed(false);
                    }
                }
                else
                {
                    char format[32];
                    std::snprintf(format, sizeof(format), parameter.Unit.empty() ? "%%.2f" : "%%.2f %s", parameter.Unit.c_str());
                    ValueSlider(parameter.Name.c_str(), { bus.Id, AudioMixerValueKind::Effect, effect.Id, p }, parameter.Min, parameter.Max, format,
                                parameter.Logarithmic && parameter.Min > 0.0f, parameter.Default);
                }
                ImGui::PopID();
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }

    void MixerPanel::DrawSends(AudioMixerData& data, AudioMixerBus& bus)
    {
        int removed = -1;
        for (size_t i = 0; i < bus.Sends.size(); ++i)
        {
            AudioMixerSend& send = bus.Sends[i];
            const AudioMixerBus* target = data.FindBus(send.Target);
            ImGui::PushID(static_cast<int>(i));
            const std::string label = "To " + (target ? target->Name : std::string("(missing)"));
            ValueSlider(label.c_str(), { bus.Id, AudioMixerValueKind::Send, send.Target, 0 }, kFaderMinDb, kFaderMaxDb, "%.1f dB", false, 0.0f);
            ImGui::SameLine();
            if (ImGui::Checkbox("Pre-Fader", &send.PreFader))
                Changed(true);
            ImGui::SetItemTooltip("Taken before this bus's fader: it still sends when the bus is turned down.");
            ImGui::SameLine();
            if (ImGui::SmallButton("X"))
                removed = static_cast<int>(i);
            ImGui::PopID();
        }
        if (removed >= 0)
        {
            const uint64_t target = bus.Sends[removed].Target;
            bus.Sends.erase(bus.Sends.begin() + removed);
            for (AudioMixerSnapshot& snapshot : data.Snapshots)
                std::erase_if(snapshot.Values, [&](const auto& entry) { return entry.first.Bus == bus.Id && entry.first.Kind == AudioMixerValueKind::Send && entry.first.Item == target; });
            Changed(true);
        }
        uint64_t chosen = 0;
        if (BusCombo("##AddSend", data, 0, false, [&](const AudioMixerBus& candidate)
            {
                const bool exists = std::any_of(bus.Sends.begin(), bus.Sends.end(), [&](const AudioMixerSend& send) { return send.Target == candidate.Id; });
                return candidate.Id != bus.Id && !exists && !data.WouldLoop(bus.Id, candidate.Id) && !data.IsUnder(candidate.Id, bus.Id);
            }, chosen) && chosen != 0)
        {
            AudioMixerSend send;
            send.Target = chosen;
            send.LevelDb = -6.0f;
            bus.Sends.push_back(send);
            Changed(true);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Add Send (a copy of this bus's sound into another bus: a shared Reverb)");
    }

    void MixerPanel::DrawDucking(AudioMixerData& data, AudioMixerBus& bus)
    {
        uint64_t source = bus.Duck.Source;
        if (BusCombo("Duck When", data, bus.Duck.Source, true,
                [&](const AudioMixerBus& candidate) { return candidate.Id != bus.Id && !data.WouldLoop(candidate.Id, bus.Id); }, source))
        {
            bus.Duck.Source = source;
            Changed(true);
        }
        ImGui::SetItemTooltip("This bus is turned down while that bus plays (Music ducked by Voice: dialogue stays clear).");
        if (bus.Duck.Source == 0)
            return;
        bool changed = false;
        changed |= ImGui::SliderFloat("Depth", &bus.Duck.DepthDb, -40.0f, 0.0f, "%.1f dB");
        ImGui::SetItemTooltip("How far it goes down.");
        changed |= ImGui::SliderFloat("Threshold", &bus.Duck.ThresholdDb, -80.0f, 0.0f, "%.1f dB");
        ImGui::SetItemTooltip("The other bus counts as playing above this level.");
        changed |= ImGui::SliderFloat("Attack", &bus.Duck.Attack, 0.005f, 2.0f, "%.3f s", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("How fast it goes down.");
        changed |= ImGui::SliderFloat("Release", &bus.Duck.Release, 0.02f, 5.0f, "%.2f s", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("How fast it comes back up once the other bus is quiet.");
        if (changed)
            Changed(true);
    }

    void MixerPanel::DeleteBus(AudioMixerData& data, uint64_t id)
    {
        const AudioMixerBus* bus = data.FindBus(id);
        if (!bus || id == kAudioBusMaster)
            return;
        const uint64_t parent = bus->Parent;
        for (AudioMixerBus& other : data.Buses)
        {
            if (other.Parent == id)
                other.Parent = parent;
            std::erase_if(other.Sends, [&](const AudioMixerSend& send) { return send.Target == id; });
            for (AudioMixerEffect& effect : other.Effects)
                if (effect.Sidechain == id)
                    effect.Sidechain = 0;
            if (other.Duck.Source == id)
                other.Duck.Source = 0;
        }
        for (AudioMixerSnapshot& snapshot : data.Snapshots)
            std::erase_if(snapshot.Values, [&](const auto& entry) { return entry.first.Bus == id || (entry.first.Kind == AudioMixerValueKind::Send && entry.first.Item == id); });
        std::erase_if(data.Buses, [&](const AudioMixerBus& candidate) { return candidate.Id == id; });
        m_SelectedBus = parent;
        Changed(true);
    }

    void MixerPanel::UpdateUndo(AudioMixerData& data)
    {
        if (!m_HaveCommitted)
        {
            m_Committed = data;
            m_HaveCommitted = true;
            m_PendingChange = false;
            return;
        }
        // One step per interaction: taken once nothing is held any more (a fader drag is one step).
        if (m_PendingChange && !ImGui::IsAnyItemActive())
        {
            m_UndoStack.push_back(std::move(m_Committed));
            if (m_UndoStack.size() > 100)
                m_UndoStack.erase(m_UndoStack.begin());
            m_RedoStack.clear();
            m_Committed = data;
            m_PendingChange = false;
        }
    }

    void MixerPanel::Undo(AudioMixerData& data)
    {
        if (m_UndoStack.empty())
            return;
        m_RedoStack.push_back(data);
        data = std::move(m_UndoStack.back());
        m_UndoStack.pop_back();
        m_Committed = data;
        m_PendingChange = false;
        AudioMixer::SetEditedSnapshot(AudioMixer::GetEditedSnapshot()); // (an index past the snapshots there are now: Default)
        AudioMixer::NotifyStructureChanged();
    }

    void MixerPanel::Redo(AudioMixerData& data)
    {
        if (m_RedoStack.empty())
            return;
        m_UndoStack.push_back(data);
        data = std::move(m_RedoStack.back());
        m_RedoStack.pop_back();
        m_Committed = data;
        m_PendingChange = false;
        AudioMixer::SetEditedSnapshot(AudioMixer::GetEditedSnapshot());
        AudioMixer::NotifyStructureChanged();
    }
}
