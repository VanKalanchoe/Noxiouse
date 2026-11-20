#include "MixerV2Panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include "Panels/InspectorWidgetsV2.h"
#include "NoxCore/UI/Widgets/UIPropertyRows.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Audio/AudioDSP.h"
#include "NoxCore/Audio/AudioEffects.h"
#include "NoxCore/Audio/AudioEngine.h"
#include "NoxCore/Audio/SpatialAudio.h"
#include "NoxCore/Core/UUID.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Widgets/UILevelMeter.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UIDragScalar.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIMenu.h"
#include "NoxCore/UI/Widgets/UIModal.h"
#include "NoxCore/UI/Widgets/UIPopup.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"
#include "NoxCore/UI/Widgets/UITextInput.h"

namespace Nox
{
    namespace
    {
        constexpr float kStripWidth = 136.0f;
        constexpr float kMinDb = -60.0f; // the meters' floor
        constexpr float kMaxDb = 6.0f;
        constexpr float kFaderMinDb = AudioDSP::kSilenceDb;
        constexpr float kFaderMaxDb = 12.0f;
        constexpr UI::UIThemeToken kOverride{ "MixerOverride", { 0.95f, 0.55f, 0.15f, 1.0f } };

        glm::vec4 Token(const UI::UITheme& theme, const char* name, const UI::UIColor& fallback)
        {
            const UI::UIColor c = theme.GetToken(name, fallback);
            return { c.r, c.g, c.b, c.a };
        }

        std::string DbText(float db) { char t[32]; std::snprintf(t, sizeof(t), db <= kFaderMinDb ? "-inf dB" : "%.1f dB", db); return t; }

        // A vertical fader in dB: drag up / down (Shift: finer), double-click 0 dB, right-click its menu.
        class MixerFader final : public UI::UIElement
        {
        public:
            std::function<float()> Get;
            std::function<void(float)> Set;
            std::function<bool()> Overridden;
            std::function<void(const UI::UIVec2&)> OnContext;
            bool Dragging() const { return m_Dragging; }

            MixerFader() { HitTestMode = UI::UIHitTestMode::Self; }

            bool OnPointerDown(const UI::UIPointerEvent& e) override
            {
                if (e.Button == 3 && OnContext)
                {
                    OnContext(e.Position);
                    return true;
                }
                if (e.Button != 1)
                    return false;
                const auto now = std::chrono::steady_clock::now();
                if (now - m_LastClick < std::chrono::milliseconds(350) && Set)
                    Set(0.0f);
                m_LastClick = now;
                m_Dragging = true;
                m_LastY = e.Position.y;
                return true;
            }
            bool OnPointerMove(const UI::UIPointerEvent& e) override
            {
                if (!m_Dragging || !Get || !Set)
                    return false;
                const float perPixel = (kFaderMaxDb - kFaderMinDb) / std::max(GetBounds().Height(), 40.0f) * ((e.Modifiers & UI::UIMod::Shift) ? 0.2f : 1.0f);
                Set(std::clamp(Get() - (e.Position.y - m_LastY) * perPixel, kFaderMinDb, kFaderMaxDb));
                m_LastY = e.Position.y;
                return true;
            }
            bool OnPointerUp(const UI::UIPointerEvent& e) override
            {
                if (e.Button == 1)
                    m_Dragging = false;
                return true;
            }

        protected:
            UI::UIVec2 ComputeDesiredSize() override { return { 28.0f, 120.0f }; }
            void OnUpdate(float) override
            {
                if (Get)
                    Tooltip = DbText(Get()) + "\nDrag (Shift: finer); double-click: 0 dB; right-click: its menu";
            }
            void OnPaint(UI::UIDrawList& d, const UI::UITheme& theme) override
            {
                const UI::UIRect b = GetBounds();
                const float cx = (b.Min.x + b.Max.x) * 0.5f;
                d.AddRoundedRect({ cx - 2.0f, b.Min.y }, { cx + 2.0f, b.Max.y }, Token(theme, "FaderTrack", { 0.16f, 0.16f, 0.18f, 1.0f }), glm::vec4(2.0f));
                const float zero = b.Min.y + b.Height() * (1.0f - (0.0f - kFaderMinDb) / (kFaderMaxDb - kFaderMinDb));
                d.AddRoundedRect({ b.Min.x + 4.0f, zero }, { b.Max.x - 4.0f, zero + 1.0f }, UI::ThemeColor("FaderZeroLine", { 0.6f, 0.6f, 0.65f, 0.7f }));
                const float value = Get ? Get() : 0.0f;
                const float y = b.Min.y + b.Height() * (1.0f - (value - kFaderMinDb) / (kFaderMaxDb - kFaderMinDb));
                const bool overridden = Overridden && Overridden();
                const glm::vec4 knob = overridden ? kOverride : Token(theme, "FaderKnob", { 0.78f, 0.80f, 0.85f, 1.0f });
                d.AddRoundedRect({ b.Min.x + 2.0f, y - 6.0f }, { b.Max.x - 2.0f, y + 6.0f }, knob, glm::vec4(3.0f));
            }

        private:
            bool m_Dragging = false;
            float m_LastY = 0.0f;
            std::chrono::steady_clock::time_point m_LastClick{};
        };

        // A row that opens a menu on right-click (a value's Reset / Remove from Snapshot, a snapshot's Rename / Duplicate / Delete).
        class ContextBox final : public UI::UIStackBox
        {
        public:
            explicit ContextBox(UI::UIOrientation orientation) : UI::UIStackBox(orientation, 6.0f) {}
            std::function<void(const UI::UIVec2&)> OnContext;
            bool OnPointerDown(const UI::UIPointerEvent& e) override
            {
                if (e.Button == 3 && OnContext)
                {
                    OnContext(e.Position);
                    return true;
                }
                return UI::UIStackBox::OnPointerDown(e);
            }
        };

        // A horizontal row of fixed-width strips that scrolls sideways with the wheel (the strips can be wider than the window).
        class StripRow final : public UI::UIElement
        {
        public:
            float Spacing = 6.0f;
            StripRow() { ClipChildren = true; HitTestMode = UI::UIHitTestMode::Self; WidthMode = UI::UISizeMode::Fill; }
            bool OnMouseWheel(const UI::UIPointerEvent& e) override
            {
                m_Scroll = std::clamp(m_Scroll - (e.ScrollDelta.y + e.ScrollDelta.x) * 48.0f, 0.0f, std::max(0.0f, m_ContentWidth - GetBounds().Width()));
                return true;
            }

        protected:
            UI::UIVec2 ComputeDesiredSize() override { return { 300.0f, 330.0f }; }
            void ArrangeChildren(const UI::UIRect& content) override
            {
                float x = content.Min.x - m_Scroll;
                m_ContentWidth = 0.0f;
                for (const auto& child : GetChildren())
                {
                    if (!child->Visible)
                        continue;
                    child->Arrange({ { x, content.Min.y }, { x + kStripWidth, content.Max.y } });
                    x += kStripWidth + Spacing;
                    m_ContentWidth += kStripWidth + Spacing;
                }
                m_Scroll = std::clamp(m_Scroll, 0.0f, std::max(0.0f, m_ContentWidth - content.Width()));
            }

        private:
            float m_Scroll = 0.0f;
            float m_ContentWidth = 0.0f;
        };

        Ref<UI::UILabel> Muted(const std::string& text, float size = 13.0f)
        {
            Ref<UI::UILabel> label(new UI::UILabel(text, size));
            label->UseColorOverride = true;
            label->ColorOverride = UI::ThemeColor("TextMuted", { 0.60f, 0.64f, 0.72f, 1.0f });
            label->VAlign = UI::UIAlignment::Center;
            return label;
        }

        // Buses `bus` may route into (Output To, a send, a sidechain or ducking source) without closing a loop.
        std::vector<UI::UIMenuItem> BusMenu(const AudioMixerData& data, uint64_t current, bool allowNone, const std::function<bool(const AudioMixerBus&)>& allowed,
                                            const std::function<void(uint64_t)>& pick)
        {
            std::vector<UI::UIMenuItem> items;
            if (allowNone)
                items.push_back({ "None", "", [pick]() { pick(0); }, false, true, current == 0 });
            for (const AudioMixerBus& candidate : data.Buses)
                if (allowed(candidate))
                {
                    const uint64_t id = candidate.Id;
                    items.push_back({ candidate.Name, "", [pick, id]() { pick(id); }, false, true, id == current });
                }
            if (items.empty())
                items.push_back({ "No bus fits (it would loop)", "", {}, false, false, false, true });
            return items;
        }

        std::string BusName(const AudioMixerData& data, uint64_t id, const char* none = "None")
        {
            const AudioMixerBus* bus = data.FindBus(id);
            return bus ? bus->Name : std::string(none);
        }
    }

    void MixerV2Panel::Changed(bool structure)
    {
        if (structure)
            AudioMixer::NotifyStructureChanged();
        else
            AudioMixer::NotifyValuesChanged();
        m_PendingChange = true;
    }

    const Ref<UI::UIElement>& MixerV2Panel::BuildUI(UI::UIContext* context)
    {
        m_Context = context;
        // Keys typed in the panel that its children leave (Ctrl+Z, Ctrl+Y) come to its root.
        Ref<UI::UIStackBox> root(new UI::UIStackBox(UI::UIOrientation::Vertical, 6.0f));
        root->OnKey = [this](const UI::UIKeyEvent& e) { return HandleKey(e); };
        root->WidthMode = UI::UISizeMode::Fill;
        root->HeightMode = UI::UISizeMode::Fill;
        root->StyleClass = "Surface";
        root->DrawBackground = true;
        root->Padding = UI::UIThickness::All(8.0f);
        root->Focusable = true;
        m_Body = root;
        m_Root = root;
        m_Rebuild = true;
        Update();
        return m_Root;
    }

    bool MixerV2Panel::HandleKey(const UI::UIKeyEvent& e)
    {
        if ((e.Modifiers & UI::UIMod::Ctrl) == 0)
            return false;
        if (e.KeyCode == SDL_SCANCODE_Z)
        {
            (e.Modifiers & UI::UIMod::Shift) ? Redo() : Undo();
            return true;
        }
        if (e.KeyCode == SDL_SCANCODE_Y)
        {
            Redo();
            return true;
        }
        return false;
    }

    uint64_t MixerV2Panel::StructureSignature() const
    {
        // What changes the rows themselves (not the values they show).
        const AudioMixerData& data = AudioMixer::GetData();
        uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        const auto mixText = [&mix](const std::string& s) { for (char c : s) mix(static_cast<unsigned char>(c)); mix(0xFF); };
        mix(data.Buses.size());
        for (const AudioMixerBus& bus : data.Buses)
        {
            mix(bus.Id);
            mixText(bus.Name);
            mix(bus.Parent);
            mix(bus.Duck.Source);
            for (const AudioMixerEffect& effect : bus.Effects)
            {
                mix(effect.Id);
                mix(static_cast<uint64_t>(effect.Type));
                mix(effect.Sidechain);
                mix(static_cast<uint64_t>(effect.Impulse));
            }
            for (const AudioMixerSend& send : bus.Sends)
                mix(send.Target);
        }
        for (const AudioMixerSnapshot& snapshot : data.Snapshots)
            mixText(snapshot.Name);
        mix(static_cast<uint64_t>(AudioMixer::GetEditedSnapshot() + 2));
        mix(m_SelectedBus);
        for (const auto& [effect, collapsed] : m_CollapsedEffects)
            mix(effect ^ (collapsed ? 0x5555ull : 0xAAAAull));
        return h;
    }

    void MixerV2Panel::Update()
    {
        if (!m_Body)
            return;
        AudioMixerData& data = AudioMixer::GetData();
        if (AudioMixer::GetPath() != m_LoadedPath)
        {
            // Another project's mixer: its own history.
            m_LoadedPath = AudioMixer::GetPath();
            m_UndoStack.clear();
            m_RedoStack.clear();
            m_HaveCommitted = false;
            m_SelectedBus = kAudioBusMaster;
            m_Rebuild = true;
        }
        if (!data.FindBus(m_SelectedBus))
            m_SelectedBus = kAudioBusMaster;
        const uint64_t signature = StructureSignature();
        if (m_Rebuild || signature != m_Signature)
        {
            m_Signature = signature;
            m_Rebuild = false;
            Rebuild();
        }
        // One history step per finished change: taken once no mouse button is held (a fader drag is one step).
        if (!m_HaveCommitted)
        {
            m_Committed = data;
            m_HaveCommitted = true;
            m_PendingChange = false;
        }
        else if (m_PendingChange && !UI::UIContext::IsMouseButtonHeld())
            CommitHistory();
    }

    void MixerV2Panel::CommitHistory()
    {
        m_UndoStack.push_back(std::move(m_Committed));
        if (m_UndoStack.size() > 100)
            m_UndoStack.erase(m_UndoStack.begin());
        m_RedoStack.clear();
        m_Committed = AudioMixer::GetData();
        m_PendingChange = false;
    }

    void MixerV2Panel::Undo()
    {
        if (m_PendingChange)
            CommitHistory();
        if (m_UndoStack.empty())
            return;
        AudioMixerData& data = AudioMixer::GetData();
        m_RedoStack.push_back(data);
        data = std::move(m_UndoStack.back());
        m_UndoStack.pop_back();
        m_Committed = data;
        AudioMixer::SetEditedSnapshot(AudioMixer::GetEditedSnapshot()); // (an index past the snapshots there are now: Default)
        AudioMixer::NotifyStructureChanged();
        AudioMixer::MarkDirty();
        m_Rebuild = true;
    }

    void MixerV2Panel::Redo()
    {
        if (m_RedoStack.empty() || m_PendingChange)
            return;
        AudioMixerData& data = AudioMixer::GetData();
        m_UndoStack.push_back(data);
        data = std::move(m_RedoStack.back());
        m_RedoStack.pop_back();
        m_Committed = data;
        AudioMixer::SetEditedSnapshot(AudioMixer::GetEditedSnapshot());
        AudioMixer::NotifyStructureChanged();
        AudioMixer::MarkDirty();
        m_Rebuild = true;
    }

    void MixerV2Panel::Rebuild()
    {
        m_Body->ClearChildren();
        BuildToolbar(*m_Body);
        BuildSnapshots(*m_Body);
        BuildStrips(*m_Body);
        auto scroll = UI::MakeUI<UI::UIScrollView>();
        scroll->WidthMode = UI::UISizeMode::Fill;
        scroll->HeightMode = UI::UISizeMode::Fill;
        m_Body->AddChild(scroll);
        auto details = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 4.0f);
        details->WidthMode = UI::UISizeMode::Fill;
        scroll->AddChild(details);
        BuildDetails(*details);
    }

    Ref<UI::UIElement> MixerV2Panel::ValueRow(const std::string& label, const AudioMixerValueKey& key, float min, float max, const char* format,
                                              float resetValue, const std::string& tooltip)
    {
        Ref<ContextBox> row(new ContextBox(UI::UIOrientation::Horizontal));
        row->WidthMode = UI::UISizeMode::Fill;
        auto name = Muted(label, 13.5f);
        name->WidthMode = UI::UISizeMode::Fixed;
        name->FixedSize = { 140.0f, 24.0f };
        name->Tooltip = tooltip.empty() ? label : label + "\n" + tooltip;
        row->AddChild(name);
        Ref<UI::UIDragScalar> field = CreateSliderField(AudioMixer::GetValue(key), min, max, [this, key](float v)
        {
            AudioMixer::SetValue(key, v);
            m_PendingChange = true;
        });
        field->Format = format;
        field->WidthMode = UI::UISizeMode::Fill;
        row->AddChild(field);
        auto dot = Muted("in snapshot", 12.0f);
        dot->UseColorOverride = true;
        dot->ColorOverride = kOverride.Get();
        dot->Tooltip = "The snapshot being edited sets this value (right-click: Remove from Snapshot)";
        row->AddChild(dot);
        row->OnContext = [this, key, resetValue](const UI::UIVec2& at)
        {
            if (!m_Context)
                return;
            char reset[48];
            std::snprintf(reset, sizeof(reset), "Reset (%g)", resetValue);
            UI::UIMenuPopup::ShowContextMenu(*m_Context, at, {
                { reset, "", [this, key, resetValue]() { AudioMixer::SetValue(key, resetValue); m_PendingChange = true; } },
                { "Remove from Snapshot", "", [this, key]() { AudioMixer::ClearOverride(key); m_PendingChange = true; }, false, AudioMixer::IsOverridden(key) },
            });
        };
        UI::AppendSync(*row, [field, dot, key]() // (on the row: it holds both)
        {
            if (!field->IsInteracting())
                field->Value = AudioMixer::GetValue(key);
            dot->Visible = AudioMixer::IsOverridden(key);
        });
        return row;
    }

    void MixerV2Panel::BuildToolbar(UI::UIStackBox& into)
    {
        auto bar = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 6.0f);
        bar->WidthMode = UI::UISizeMode::Fill;
        Ref<UI::UIButton> save(new UI::UIButton("Save", []() { AudioMixer::Save(); }));
        save->Tooltip = "Writes the mixer to its file (File > Save Scene saves it too).";
        bar->AddChild(save);
        Ref<UI::UIButton> addBus(new UI::UIButton("Add Bus", [this]()
        {
            AudioMixerData& data = AudioMixer::GetData();
            if (data.Buses.size() >= AudioEngine::MaxBuses)
                return;
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
        }));
        addBus->Tooltip = "A new bus into Master (up to " + std::to_string(AudioEngine::MaxBuses) + "). Set its Output To below to put it under another bus.";
        bar->AddChild(addBus);
        Ref<UI::UIButton> undo(new UI::UIButton("Undo", [this]() { Undo(); }));
        Ref<UI::UIButton> redo(new UI::UIButton("Redo", [this]() { Redo(); }));
        undo->Tooltip = "Ctrl+Z (while the Mixer has the keyboard)";
        redo->Tooltip = "Ctrl+Y / Ctrl+Shift+Z";
        bar->AddChild(undo);
        bar->AddChild(redo);
        Ref<UI::UICheckBox> headphones(new UI::UICheckBox("Headphones (HRTF)", AudioMixer::GetData().Headphones, [this](bool v)
        {
            AudioMixer::GetData().Headphones = v;
            Changed(false);
        }));
        headphones->Tooltip = "3D sounds set to Default Spatialization: binaural (Steam Audio's HRTF -- for headphones) or panned left / right "
                              "(for speakers). A game's options menu changes it: Audio.SetHeadphones.";
        bar->AddChild(headphones);
        bar->AddChild(Muted("Real Voices"));
        auto voices = UI::MakeUI<UI::UIDragScalar>(static_cast<float>(AudioMixer::GetData().RealVoices), 0.25f, [this](float v)
        {
            AudioMixer::GetData().RealVoices = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(v)), 1, static_cast<int>(AudioEngine::MaxVoices)));
            Changed(false);
        });
        voices->Min = 1.0f;
        voices->Max = static_cast<float>(AudioEngine::MaxVoices);
        voices->Step = 1.0f;
        voices->Format = "{:.0f}";
        voices->WidthMode = UI::UISizeMode::Fixed;
        voices->FixedSize = { 64.0f, 24.0f };
        bar->AddChild(voices);
        Ref<UI::UIButton> acoustics(new UI::UIButton("Acoustics..."));
        UI::UIButton* acousticsRaw = acoustics.get();
        acoustics->OnClick = [this, acousticsRaw]() { ShowAcoustics(acousticsRaw->GetBounds()); };
        acoustics->Tooltip = "Steam Audio's real-time reflections: rays bounced off the level give the nearest sounds their echoes and reverb.";
        bar->AddChild(acoustics);
        auto file = Muted("");
        bar->AddChild(file);
        into.AddChild(bar);
        UI::AppendSync(*bar, [this, save, undo, redo, headphones, voices, file]() // (on the bar: it holds them)
        {
            save->SetEnabled(AudioMixer::IsDirty() && !AudioMixer::GetPath().empty());
            save->StyleVariant = AudioMixer::IsDirty() ? "primary" : "";
            undo->SetEnabled(!m_UndoStack.empty() || m_PendingChange);
            redo->SetEnabled(!m_RedoStack.empty() && !m_PendingChange);
            headphones->Checked = AudioMixer::GetData().Headphones;
            if (!voices->IsInteracting())
                voices->Value = static_cast<float>(AudioMixer::GetData().RealVoices);
            const AudioStats stats = AudioEngine::GetStats();
            voices->Tooltip = "How many sounds are mixed at most (of " + std::to_string(AudioEngine::MaxVoices) + " voices). Beyond it the quietest x "
                              "lowest-priority ones are virtual: silent, their playhead going on. Now: " + std::to_string(stats.Voices) + " playing, " +
                              std::to_string(stats.RealVoices) + " real, " + std::to_string(stats.VirtualVoices) + " virtual.";
            std::string text = AudioMixer::GetPath().empty() ? std::string("(no project: the default buses)") : AudioMixer::GetPath().filename().string();
            if (AudioMixer::IsDirty())
                text += " *";
            const SpatialAudio::SceneStats acoustic = SpatialAudio::GetSceneStats();
            if (acoustic.Instances > 0 || acoustic.Building > 0)
            {
                char scene[128];
                std::snprintf(scene, sizeof(scene), "  |  acoustic scene: %u meshes, %.1fk triangles", acoustic.Instances, static_cast<double>(acoustic.Triangles) / 1000.0);
                text += scene;
                if (acoustic.Building > 0)
                    text += " (" + std::to_string(acoustic.Building) + " building)";
            }
            if (file->Text != text)
                file->Text = text;
        });
    }

    void MixerV2Panel::ShowAcoustics(const UI::UIRect& anchor)
    {
        if (!m_Context)
            return;
        auto box = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 4.0f);
        box->Padding = UI::UIThickness::All(10.0f);
        box->FixedSize = { 440.0f, 0.0f };
        // (its rows follow the settings while the popup is up: their syncs are part of the overlay, and stop with it)
        UI::UIPropertyRows rows(box, m_Context);
        AudioReflectionSettings* r = &AudioMixer::GetData().Reflections;
        const auto changed = [this]() { Changed(false); };
        rows.Check("Reflections", [r]() { return r->Enabled; }, [r, changed](bool v) { r->Enabled = v; changed(); },
                   "Real-time reflections from the level's geometry (its static meshes and their acoustic materials).");
        rows.Int("Voices", [r]() { return r->MaxSources; }, [r, changed](int v) { r->MaxSources = v; changed(); }, 0, SpatialAudio::MaxReflectionSources,
                 "How many sounds get reflections at once: the nearest ones (each costs mixer time).");
        rows.Int("Rays", [r]() { return r->Rays; }, [r, changed](int v) { r->Rays = v; changed(); }, 256, 16384,
                 "Rays traced from the listener each run: more is smoother and costs more on the reflection thread.");
        rows.Int("Bounces", [r]() { return r->Bounces; }, [r, changed](int v) { r->Bounces = v; changed(); }, 1, 16);
        rows.Slider("Duration (s)", [r]() { return r->Duration; }, [r, changed](float v) { r->Duration = v; changed(); }, 0.1f, SpatialAudio::MaxReflectionDuration, "{:.2f}",
                    "How long each sound's response (echoes and reverb tail) is simulated and convolved.");
        std::vector<std::string> busNames;
        std::vector<uint64_t> busIds;
        for (const AudioMixerBus& bus : AudioMixer::GetData().Buses)
        {
            busNames.push_back(bus.Name);
            busIds.push_back(bus.Id);
        }
        rows.Choice("Output Bus", busNames,
            [r, busIds]() { const auto found = std::find(busIds.begin(), busIds.end(), r->Bus); return found == busIds.end() ? 0 : static_cast<int>(found - busIds.begin()); },
            [r, busIds, changed](int i) { r->Bus = busIds[static_cast<size_t>(i)]; changed(); }, "The bus the reflections are heard through (Master by default).");
        rows.Slider("Level (dB)", [r]() { return r->LevelDb; }, [r, changed](float v) { r->LevelDb = v; changed(); }, kFaderMinDb, 12.0f, "{:.1f}");
        rows.Header("Baked acoustics");
        rows.Choice("Listener Reverb", { "Off", "Real time", "Baked" }, [r]() { return static_cast<int>(r->ListenerReverb); },
            [r, changed](int v) { r->ListenerReverb = static_cast<AudioListenerReverb>(v); changed(); },
            "The reverb an Acoustic Reverb effect applies: Baked looks it up in the Acoustic Probe Volume the listener is in; Real time traces it "
            "from the listener; Off: none.");
        rows.Int("Pathing Voices", [r]() { return r->PathingSources; }, [r, changed](int v) { r->PathingSources = v; changed(); }, 0, SpatialAudio::MaxPathingSources,
                 "How many sounds with Pathing find their way around corners at once (the nearest).");
        rows.Text([]()
        {
            const AudioEngine::ReflectionStats stats = AudioEngine::GetReflectionStats();
            const auto toDb = [](float gain) { return gain > 1e-5f ? 20.0f * std::log10(gain) : -100.0f; };
            const SpatialAudio::ProbeStats probes = SpatialAudio::GetProbeStats();
            char text[400];
            std::snprintf(text, sizeof(text),
                          "Reflections out: RMS %.1f dB, peak %.1f dB\n%d voice(s) with reflections | last run %.1f ms on the reflection thread\n"
                          "Probe volumes: %u baked in, %u active (%u probes) | listener %s | reverb at listener: %s | %d voice(s) with paths",
                          toDb(stats.Rms), toDb(stats.Peak), stats.Voices, stats.Milliseconds, probes.Batches, probes.Active, probes.Probes,
                          probes.ListenerInside ? "inside one" : "outside them", stats.ListenerReverb ? "yes" : "no", stats.PathingVoices);
            return std::string(text);
        });
        UI::UIPopup::Show(*m_Context, anchor, box);
    }

    void MixerV2Panel::BuildSnapshots(UI::UIStackBox& into)
    {
        AudioMixerData& data = AudioMixer::GetData();
        auto row = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
        row->WidthMode = UI::UISizeMode::Fill;
        row->AddChild(Muted("Snapshot:"));
        const int32_t edited = AudioMixer::GetEditedSnapshot();
        Ref<UI::UIButton> defaultButton(new UI::UIButton("Default", []() { AudioMixer::SetEditedSnapshot(-1); }));
        defaultButton->StyleVariant = edited < 0 ? "primary" : "";
        defaultButton->Tooltip = "The mix itself: every value as set here. Snapshots change some of these values.";
        row->AddChild(defaultButton);
        for (int32_t i = 0; i < static_cast<int32_t>(data.Snapshots.size()); ++i)
        {
            Ref<ContextBox> holder(new ContextBox(UI::UIOrientation::Horizontal));
            Ref<UI::UIButton> button(new UI::UIButton(data.Snapshots[static_cast<size_t>(i)].Name, [i]() { AudioMixer::SetEditedSnapshot(i); }));
            button->StyleVariant = edited == i ? "primary" : "";
            button->Tooltip = "Pick to hear it and to change it: what you change is stored in this snapshot. Right-click: Rename, Duplicate, Delete.";
            holder->AddChild(button);
            holder->OnContext = [this, i](const UI::UIVec2& at)
            {
                if (!m_Context)
                    return;
                UI::UIMenuPopup::ShowContextMenu(*m_Context, at, {
                    { "Rename...", "", [this, i]()
                    {
                        AudioMixerData& d = AudioMixer::GetData();
                        if (i >= static_cast<int32_t>(d.Snapshots.size()) || !m_Context)
                            return;
                        auto content = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 6.0f);
                        content->WidthMode = UI::UISizeMode::Fill;
                        auto field = UI::MakeUI<UI::UITextInput>();
                        field->Text = d.Snapshots[static_cast<size_t>(i)].Name;
                        field->WidthMode = UI::UISizeMode::Fill;
                        content->AddChild(field);
                        UI::UITextInput* raw = field.get();
                        const auto apply = [this, i, raw]()
                        {
                            AudioMixerData& now = AudioMixer::GetData();
                            if (i < static_cast<int32_t>(now.Snapshots.size()) && !raw->Text.empty())
                            {
                                now.Snapshots[static_cast<size_t>(i)].Name = raw->Text;
                                Changed(false);
                            }
                        };
                        field->OnSubmit = [this, apply](const std::string&) { apply(); if (m_Context) m_Context->ClearOverlay(); };
                        UI::UIModal::ShowCustom(*m_Context, "Rename Snapshot", content, { { "Rename", apply, true }, { "Cancel", {} } }, 380.0f);
                        m_Context->SetFocus(raw);
                    } },
                    { "Duplicate", "", [this, i]()
                    {
                        AudioMixerData& d = AudioMixer::GetData();
                        if (i >= static_cast<int32_t>(d.Snapshots.size()))
                            return;
                        AudioMixerSnapshot copy = d.Snapshots[static_cast<size_t>(i)];
                        copy.Name += " Copy";
                        d.Snapshots.push_back(copy);
                        AudioMixer::SetEditedSnapshot(static_cast<int32_t>(d.Snapshots.size()) - 1);
                        Changed(false);
                    } },
                    { "Delete", "", [this, i]()
                    {
                        AudioMixerData& d = AudioMixer::GetData();
                        if (i >= static_cast<int32_t>(d.Snapshots.size()))
                            return;
                        d.Snapshots.erase(d.Snapshots.begin() + i);
                        AudioMixer::SetEditedSnapshot(-1);
                        Changed(false);
                    } },
                });
            };
            row->AddChild(holder);
        }
        Ref<UI::UIButton> add(new UI::UIButton("+", [this]()
        {
            AudioMixerData& d = AudioMixer::GetData();
            AudioMixerSnapshot snapshot;
            int number = static_cast<int>(d.Snapshots.size()) + 1;
            const auto taken = [&](const std::string& name) { return std::any_of(d.Snapshots.begin(), d.Snapshots.end(), [&](const AudioMixerSnapshot& s) { return s.Name == name; }); };
            do
                snapshot.Name = "Snapshot " + std::to_string(number++);
            while (taken(snapshot.Name));
            d.Snapshots.push_back(snapshot);
            AudioMixer::SetEditedSnapshot(static_cast<int32_t>(d.Snapshots.size()) - 1);
            Changed(false);
        }));
        add->Tooltip = "A new snapshot (it starts as the Default mix; change faders, sends and effect values to store them in it).";
        row->AddChild(add);
        into.AddChild(row);

        auto status = Muted("");
        status->UseColorOverride = true;
        status->ColorOverride = UI::ThemeColor("StatusWarning", { 0.95f, 0.65f, 0.3f, 1.0f });
        status->WordWrap = true;
        status->WidthMode = UI::UISizeMode::Fill;
        into.AddChild(status);
        UI::AppendSync(*Raw(status), [status = Raw(status)]()
        {
            const AudioMixerData& d = AudioMixer::GetData();
            const int32_t snapshot = AudioMixer::GetEditedSnapshot();
            const bool editingSnapshot = snapshot >= 0 && static_cast<size_t>(snapshot) < d.Snapshots.size();
            const std::string playing = AudioMixer::GetSnapshot();
            std::string text;
            if (editingSnapshot)
                text = "Editing '" + d.Snapshots[static_cast<size_t>(snapshot)].Name + "': fader, send and effect values you change are stored in it "
                       "(marked 'in snapshot'; right-click one: Remove from Snapshot).  ";
            if (playing != (editingSnapshot ? d.Snapshots[static_cast<size_t>(snapshot)].Name : std::string("Default")))
                text += "Heard now (set by the game): " + playing;
            status->Visible = !text.empty();
            if (status->Text != text)
                status->Text = text;
        });
    }

    void MixerV2Panel::BuildStrips(UI::UIStackBox& into)
    {
        AudioMixerData& data = AudioMixer::GetData();
        Ref<StripRow> strips(new StripRow());
        strips->HeightMode = UI::UISizeMode::Fixed;
        strips->FixedSize = { 0.0f, 330.0f };
        for (const AudioMixerBus& bus : data.Buses)
        {
            const uint64_t id = bus.Id;
            const bool selected = id == m_SelectedBus;
            auto strip = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 4.0f);
            strip->StyleClass = selected ? "CardSelected" : "Card";
            strip->DrawBackground = true;
            strip->Padding = UI::UIThickness::All(6.0f);
            strip->HeightMode = UI::UISizeMode::Fill;

            Ref<UI::UIButton> name(new UI::UIButton(bus.Name, [this, id]() { m_SelectedBus = id; }));
            name->StyleVariant = selected ? "primary" : "";
            name->Tooltip = "Select to edit its effects, sends and ducking below.";
            name->WidthMode = UI::UISizeMode::Fill;
            strip->AddChild(name);
            strip->AddChild(Muted("-> " + (id == kAudioBusMaster ? std::string("Speakers") : BusName(data, bus.Parent, "Master")), 12.0f));

            const AudioMixerValueKey volume{ id, AudioMixerValueKind::Volume, 0, 0 };
            auto meterRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
            meterRow->WidthMode = UI::UISizeMode::Fill;
            meterRow->HeightMode = UI::UISizeMode::Fill;
            // The bus's level: RMS filled, the peak a line, a tick every 12 dB (UI::UILevelMeter).
            Ref<UI::UILevelMeter> meter(new UI::UILevelMeter(UI::UIOrientation::Vertical));
            meter->MinDb = kMinDb;
            meter->MaxDb = kMaxDb;
            meter->Source = [id]()
            {
                const AudioBusMeter m = AudioMixer::GetMeter(id);
                return UI::UILevelMeter::Values{ m.RmsLeft, m.RmsRight, m.PeakLeft, m.PeakRight };
            };
            meter->HeightMode = UI::UISizeMode::Fill;
            meterRow->AddChild(meter);
            Ref<MixerFader> fader(new MixerFader());
            fader->HeightMode = UI::UISizeMode::Fill;
            fader->Get = [volume]() { return AudioMixer::GetValue(volume); };
            fader->Set = [this, volume](float db) { AudioMixer::SetValue(volume, db); m_PendingChange = true; };
            fader->Overridden = [volume]() { return AudioMixer::IsOverridden(volume); };
            fader->OnContext = [this, volume](const UI::UIVec2& at)
            {
                if (m_Context)
                    UI::UIMenuPopup::ShowContextMenu(*m_Context, at, {
                        { "0 dB", "", [this, volume]() { AudioMixer::SetValue(volume, 0.0f); m_PendingChange = true; } },
                        { "Remove from Snapshot", "", [this, volume]() { AudioMixer::ClearOverride(volume); m_PendingChange = true; }, false, AudioMixer::IsOverridden(volume) },
                    });
            };
            meterRow->AddChild(fader);
            auto reduction = Muted("GR", 12.0f);
            reduction->Tooltip = "Gain reduction now (dB): how far its compressor, limiter, gate or ducking turns it down.";
            meterRow->AddChild(reduction);
            strip->AddChild(meterRow);

            auto db = UI::MakeUI<UI::UIDragScalar>(AudioMixer::GetValue(volume), 0.1f, [this, volume](float v) { AudioMixer::SetValue(volume, v); m_PendingChange = true; });
            db->Min = kFaderMinDb;
            db->Max = kFaderMaxDb;
            db->Format = "{:.1f} dB";
            db->WidthMode = UI::UISizeMode::Fill;
            strip->AddChild(db);

            auto toggles = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 3.0f);
            const auto toggle = [this, id, &toggles](const char* label, bool AudioMixerBus::*field, const char* tooltip)
            {
                Ref<UI::UIButton> button(new UI::UIButton(label, [this, id, field]()
                {
                    if (AudioMixerBus* b = AudioMixer::GetData().FindBus(id))
                    {
                        b->*field = !(b->*field);
                        Changed(false);
                    }
                }));
                button->Tooltip = tooltip;
                button->WidthMode = UI::UISizeMode::Fixed;
                button->FixedSize = { 34.0f, 24.0f };
                toggles->AddChild(button);
                return button;
            };
            Ref<UI::UIButton> mute = toggle("M", &AudioMixerBus::Mute, "Mute");
            Ref<UI::UIButton> solo = toggle("S", &AudioMixerBus::Solo, "Solo: only the soloed buses (with what feeds them, where they go and the buses they send to) are heard");
            Ref<UI::UIButton> bypass = toggle("B", &AudioMixerBus::Bypass, "Bypass every effect of this bus");
            strip->AddChild(toggles);

            for (const AudioMixerEffect& effect : bus.Effects)
            {
                auto effectLabel = Muted(GetAudioEffectInfo(effect.Type).Name, 12.5f);
                effectLabel->Tooltip = GetAudioEffectInfo(effect.Type).Description;
                strip->AddChild(effectLabel);
            }
            if (bus.Effects.empty())
                strip->AddChild(Muted("(no effects)", 12.0f));
            strips->AddChild(strip);

            UI::AppendSync(*strip, [id, volume, db, reduction, mute, solo, bypass]() // (on the strip: it holds them)
            {
                const AudioMixerBus* b = AudioMixer::GetData().FindBus(id);
                if (!b)
                    return;
                if (!db->IsInteracting())
                    db->Value = AudioMixer::GetValue(volume);
                const AudioBusMeter now = AudioMixer::GetMeter(id);
                char text[16];
                if (now.GainReductionDb > 0.05f)
                    std::snprintf(text, sizeof(text), "-%.1f", now.GainReductionDb);
                else
                    std::snprintf(text, sizeof(text), "GR");
                reduction->Text = text;
                mute->StyleVariant = b->Mute ? "primary" : "";
                solo->StyleVariant = b->Solo ? "primary" : "";
                bypass->StyleVariant = b->Bypass ? "primary" : "";
            });
        }
        into.AddChild(strips);
    }

    void MixerV2Panel::BuildDetails(UI::UIStackBox& into)
    {
        AudioMixerData& data = AudioMixer::GetData();
        AudioMixerBus* bus = data.FindBus(m_SelectedBus);
        if (!bus)
            return;
        const uint64_t id = bus->Id;
        const bool isMaster = id == kAudioBusMaster;
        UI::UIPropertyRows rows(Ref<UI::UIStackBox>(&into), m_Context);
        const auto findBus = [id]() { return AudioMixer::GetData().FindBus(id); };

        rows.Header("Bus");
        if (isMaster)
            rows.Note("Master -- everything ends here, then the speakers.");
        else
        {
            // Renamed when the edit ends (Enter or click away), if the name is free; scripts name buses by it.
            auto field = UI::MakeUI<UI::UITextInput>();
            field->Text = bus->Name;
            field->WidthMode = UI::UISizeMode::Fill;
            UI::UITextInput* raw = field.get();
            const auto apply = [this, findBus, raw]()
            {
                AudioMixerBus* b = findBus();
                if (!b || raw->Text.empty() || raw->Text == b->Name)
                    return;
                const AudioMixerBus* other = AudioMixer::FindBus(raw->Text);
                if (other && other != b)
                {
                    raw->Text = b->Name; // (taken: back)
                    return;
                }
                b->Name = raw->Text;
                Changed(false);
            };
            field->OnSubmit = [apply](const std::string&) { apply(); };
            InspectorCardBuilder builder{ Ref<UI::UIStackBox>(&into), {}, m_Context };
            builder.AddRow("Name", field, "Scripts name buses by this (Audio.SetBusVolume(\"Music\", 0.5f)).");
            UI::AppendSync(*field, [field = field.get(), apply]()
            {
                if (!UI::HasState(field->GetState(), UI::UIState::Focused))
                    apply();
            });

            Ref<UI::UIButton> output(new UI::UIButton(BusName(data, bus->Parent, "Master")));
            UI::UIButton* outputRaw = output.get();
            output->OnClick = [this, id, outputRaw]()
            {
                if (!m_Context)
                    return;
                const AudioMixerData& d = AudioMixer::GetData();
                const AudioMixerBus* b = d.FindBus(id);
                UI::UIMenuPopup::ShowPopupMenu(*m_Context, outputRaw->GetBounds(), BusMenu(d, b ? b->Parent : 0, false,
                    [&d, id](const AudioMixerBus& candidate) { return candidate.Id != id && !d.WouldLoop(id, candidate.Id) && !d.IsUnder(candidate.Id, id); },
                    [this, id](uint64_t chosen) { if (AudioMixerBus* target = AudioMixer::GetData().FindBus(id)) { target->Parent = chosen; Changed(true); } }));
            };
            builder.AddRow("Output To", output, "The bus this one plays into (its parent): Music -> Master, Footsteps -> SFX.");
        }
        rows.Check("Plays While Paused", [findBus]() { AudioMixerBus* b = findBus(); return b && b->PlaysWhilePaused; },
                   [this, findBus](bool v) { if (AudioMixerBus* b = findBus()) { b->PlaysWhilePaused = v; Changed(true); } },
                   "Its sounds keep playing while the game is paused (the UI bus: a pause menu's clicks).");
        into.AddChild(ValueRow("Volume", { id, AudioMixerValueKind::Volume, 0, 0 }, kFaderMinDb, kFaderMaxDb, "{:.1f} dB", 0.0f));
        if (!isMaster)
        {
            rows.Button("Delete Bus...", [this, id]()
            {
                const AudioMixerBus* b = AudioMixer::GetData().FindBus(id);
                if (!b || !m_Context)
                    return;
                auto text = UI::Label("Delete '" + b->Name + "'? Its children go to its Output To; sounds on it play into Master. (Undo brings it back.)", 14.0f);
                text->WordWrap = true;
                text->WidthMode = UI::UISizeMode::Fill;
                UI::UIModal::ShowCustom(*m_Context, "Delete Bus", text, { { "Delete", [this, id]() { DeleteBus(id); }, true }, { "Cancel", {} } }, 420.0f);
            });
        }

        // ---- Effects ----
        rows.Header("Effects", "Run top to bottom, before the fader.");
        for (size_t index = 0; index < bus->Effects.size(); ++index)
        {
            const AudioMixerEffect& effect = bus->Effects[index];
            const uint64_t effectId = effect.Id;
            const AudioEffectInfo& info = GetAudioEffectInfo(effect.Type);
            const auto findEffect = [id, effectId]() -> AudioMixerEffect*
            {
                AudioMixerBus* b = AudioMixer::GetData().FindBus(id);
                if (!b)
                    return nullptr;
                for (AudioMixerEffect& e : b->Effects)
                    if (e.Id == effectId)
                        return &e;
                return nullptr;
            };
            const bool collapsed = m_CollapsedEffects[effectId];
            InspectorCardResult card = CreateCollapsibleCard(std::to_string(index + 1) + ". " + info.Name, !collapsed,
                [this, effectId](bool open) { m_CollapsedEffects[effectId] = !open; }, true, {},
                [this, id, effectId]()
                {
                    AudioMixerBus* b = AudioMixer::GetData().FindBus(id);
                    if (!b)
                        return;
                    std::erase_if(b->Effects, [&](const AudioMixerEffect& e) { return e.Id == effectId; });
                    for (AudioMixerSnapshot& snapshot : AudioMixer::GetData().Snapshots)
                        std::erase_if(snapshot.Values, [&](const auto& entry) { return entry.first.Bus == id && entry.first.Kind == AudioMixerValueKind::Effect && entry.first.Item == effectId; });
                    Changed(true);
                });
            card.Card->Tooltip = info.Description;
            into.AddChild(card.Card);
            UI::UIPropertyRows effectRows(card.Body, m_Context);

            auto actions = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
            Ref<UI::UIButton> onOff(new UI::UIButton("Bypass", [this, findEffect]() { if (AudioMixerEffect* e = findEffect()) { e->Bypass = !e->Bypass; Changed(false); } }));
            Ref<UI::UIButton> up(new UI::UIButton("Up", [this, id, index]()
            {
                AudioMixerBus* b = AudioMixer::GetData().FindBus(id);
                if (b && index > 0 && index < b->Effects.size()) { std::swap(b->Effects[index], b->Effects[index - 1]); Changed(true); }
            }));
            Ref<UI::UIButton> down(new UI::UIButton("Down", [this, id, index]()
            {
                AudioMixerBus* b = AudioMixer::GetData().FindBus(id);
                if (b && index + 1 < b->Effects.size()) { std::swap(b->Effects[index], b->Effects[index + 1]); Changed(true); }
            }));
            up->SetEnabled(index > 0);
            down->SetEnabled(index + 1 < bus->Effects.size());
            actions->AddChild(onOff);
            actions->AddChild(up);
            actions->AddChild(down);
            card.Body->AddChild(actions);
            UI::AppendSync(*actions, [onOff, findEffect]() // (on its row: it holds the button)
            {
                const AudioMixerEffect* e = findEffect();
                onOff->StyleVariant = e && e->Bypass ? "primary" : "";
                onOff->Tooltip = e && e->Bypass ? "Bypassed: the sound passes it untouched (click: on again)" : "Bypass: the sound passes it untouched";
            });

            if (info.ReportsGainReduction)
                effectRows.Text([id, index]()
                {
                    char text[48];
                    std::snprintf(text, sizeof(text), "Gain reduction now: -%.1f dB", AudioMixer::GetEffectGainReduction(id, static_cast<uint32_t>(index)));
                    return std::string(text);
                }, UI::ThemeColor("StatusWarning", { 0.95f, 0.7f, 0.3f, 1.0f }));
            if (info.Sidechain)
            {
                Ref<UI::UIButton> key(new UI::UIButton(BusName(data, effect.Sidechain)));
                UI::UIButton* keyRaw = key.get();
                key->OnClick = [this, id, findEffect, keyRaw]()
                {
                    const AudioMixerEffect* e = findEffect();
                    if (!m_Context || !e)
                        return;
                    const AudioMixerData& d = AudioMixer::GetData();
                    UI::UIMenuPopup::ShowPopupMenu(*m_Context, keyRaw->GetBounds(), BusMenu(d, e->Sidechain, true,
                        [&d, id](const AudioMixerBus& candidate) { return candidate.Id != id && !d.WouldLoop(candidate.Id, id); },
                        [this, findEffect](uint64_t chosen) { if (AudioMixerEffect* target = findEffect()) { target->Sidechain = chosen; Changed(true); } }));
                };
                InspectorCardBuilder builder{ card.Body, {}, m_Context };
                builder.AddRow("Sidechain", key, "Listen to another bus instead of this one (None): a compressor keyed by Voice turns this bus down while someone speaks.");
            }
            if (info.ImpulseResponse)
            {
                InspectorCardBuilder builder{ card.Body, {}, m_Context };
                builder.AddAssetSlot("Impulse Response", effect.Impulse, { AssetType::AudioClip },
                    [this, findEffect](AssetHandle clip) { if (AudioMixerEffect* e = findEffect()) { e->Impulse = clip; Changed(true); } });
            }
            for (uint32_t p = 0; p < info.Parameters.size(); ++p)
            {
                const AudioEffectParameterInfo& parameter = info.Parameters[p];
                if (!parameter.Options.empty())
                {
                    // Choices are the mix's (snapshots blend numbers only).
                    effectRows.Choice(parameter.Name, parameter.Options,
                        [findEffect, p]() { const AudioMixerEffect* e = findEffect(); return e && p < e->Values.size() ? static_cast<int>(e->Values[p]) : 0; },
                        [this, findEffect, p](int v) { if (AudioMixerEffect* e = findEffect(); e && p < e->Values.size()) { e->Values[p] = static_cast<float>(v); Changed(false); } });
                }
                else if (parameter.Toggle)
                {
                    effectRows.Check(parameter.Name,
                        [findEffect, p]() { const AudioMixerEffect* e = findEffect(); return e && p < e->Values.size() && e->Values[p] >= 0.5f; },
                        [this, findEffect, p](bool v) { if (AudioMixerEffect* e = findEffect(); e && p < e->Values.size()) { e->Values[p] = v ? 1.0f : 0.0f; Changed(false); } });
                }
                else
                {
                    const std::string format = parameter.Unit.empty() ? std::string("{:.2f}") : "{:.2f} " + parameter.Unit;
                    // (the format string lives with the field: it is copied into it)
                    card.Body->AddChild(ValueRow(parameter.Name, { id, AudioMixerValueKind::Effect, effectId, p }, parameter.Min, parameter.Max, format.c_str(), parameter.Default));
                }
            }
        }
        Ref<UI::UIButton> addEffect(new UI::UIButton("Add Effect..."));
        UI::UIButton* addRaw = addEffect.get();
        addEffect->OnClick = [this, id, addRaw]()
        {
            if (!m_Context)
                return;
            std::vector<UI::UIMenuItem> items;
            for (uint32_t t = 0; t < static_cast<uint32_t>(AudioEffectType::Count); ++t)
            {
                const AudioEffectInfo& info = GetAudioEffectInfo(static_cast<AudioEffectType>(t));
                const AudioEffectType type = info.Type;
                items.push_back({ info.Name, "", [this, id, type]()
                {
                    AudioMixerBus* b = AudioMixer::GetData().FindBus(id);
                    if (!b)
                        return;
                    AudioMixerEffect effect;
                    effect.Id = UUID();
                    effect.Type = type;
                    effect.Values = DefaultEffectValues(type);
                    b->Effects.push_back(std::move(effect));
                    Changed(true);
                }, false, true, false, false, info.Description });
            }
            UI::UIMenuPopup::ShowPopupMenu(*m_Context, addRaw->GetBounds(), std::move(items));
        };
        addEffect->HAlign = UI::UIAlignment::Start;
        into.AddChild(addEffect);

        // ---- Sends ----
        rows.Header("Sends", "A copy of this bus's sound into another bus (a shared Reverb).");
        for (const AudioMixerSend& send : bus->Sends)
        {
            const uint64_t target = send.Target;
            into.AddChild(ValueRow("To " + BusName(data, target, "(missing)"), { id, AudioMixerValueKind::Send, target, 0 }, kFaderMinDb, kFaderMaxDb, "{:.1f} dB", 0.0f));
            auto line = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 6.0f);
            line->Padding = UI::UIThickness{ 146.0f, 0.0f, 0.0f, 0.0f };
            const auto findSend = [id, target]() -> AudioMixerSend*
            {
                AudioMixerBus* b = AudioMixer::GetData().FindBus(id);
                if (!b)
                    return nullptr;
                for (AudioMixerSend& s : b->Sends)
                    if (s.Target == target)
                        return &s;
                return nullptr;
            };
            Ref<UI::UICheckBox> pre(new UI::UICheckBox("Pre-Fader", send.PreFader, [this, findSend](bool v) { if (AudioMixerSend* s = findSend()) { s->PreFader = v; Changed(true); } }));
            pre->Tooltip = "Taken before this bus's fader: it still sends when the bus is turned down.";
            line->AddChild(pre);
            Ref<UI::UIButton> remove(new UI::UIButton("Remove", [this, id, target]()
            {
                AudioMixerBus* b = AudioMixer::GetData().FindBus(id);
                if (!b)
                    return;
                std::erase_if(b->Sends, [&](const AudioMixerSend& s) { return s.Target == target; });
                for (AudioMixerSnapshot& snapshot : AudioMixer::GetData().Snapshots)
                    std::erase_if(snapshot.Values, [&](const auto& entry) { return entry.first.Bus == id && entry.first.Kind == AudioMixerValueKind::Send && entry.first.Item == target; });
                Changed(true);
            }));
            line->AddChild(remove);
            into.AddChild(line);
        }
        Ref<UI::UIButton> addSend(new UI::UIButton("Add Send..."));
        UI::UIButton* sendRaw = addSend.get();
        addSend->OnClick = [this, id, sendRaw]()
        {
            if (!m_Context)
                return;
            const AudioMixerData& d = AudioMixer::GetData();
            const AudioMixerBus* b = d.FindBus(id);
            if (!b)
                return;
            UI::UIMenuPopup::ShowPopupMenu(*m_Context, sendRaw->GetBounds(), BusMenu(d, 0, false,
                [&d, b, id](const AudioMixerBus& candidate)
                {
                    const bool exists = std::any_of(b->Sends.begin(), b->Sends.end(), [&](const AudioMixerSend& s) { return s.Target == candidate.Id; });
                    return candidate.Id != id && !exists && !d.WouldLoop(id, candidate.Id) && !d.IsUnder(candidate.Id, id);
                },
                [this, id](uint64_t chosen)
                {
                    AudioMixerBus* target = AudioMixer::GetData().FindBus(id);
                    if (!target || chosen == 0)
                        return;
                    AudioMixerSend send;
                    send.Target = chosen;
                    send.LevelDb = -6.0f;
                    target->Sends.push_back(send);
                    Changed(true);
                }));
        };
        addSend->HAlign = UI::UIAlignment::Start;
        into.AddChild(addSend);

        // ---- Ducking ----
        if (!isMaster)
        {
            rows.Header("Ducking", "This bus is turned down while another bus plays (Music ducked by Voice: dialogue stays clear).");
            Ref<UI::UIButton> source(new UI::UIButton(BusName(data, bus->Duck.Source)));
            UI::UIButton* sourceRaw = source.get();
            source->OnClick = [this, id, sourceRaw]()
            {
                if (!m_Context)
                    return;
                const AudioMixerData& d = AudioMixer::GetData();
                const AudioMixerBus* b = d.FindBus(id);
                UI::UIMenuPopup::ShowPopupMenu(*m_Context, sourceRaw->GetBounds(), BusMenu(d, b ? b->Duck.Source : 0, true,
                    [&d, id](const AudioMixerBus& candidate) { return candidate.Id != id && !d.WouldLoop(candidate.Id, id); },
                    [this, id](uint64_t chosen) { if (AudioMixerBus* target = AudioMixer::GetData().FindBus(id)) { target->Duck.Source = chosen; Changed(true); } }));
            };
            InspectorCardBuilder builder{ Ref<UI::UIStackBox>(&into), {}, m_Context };
            builder.AddRow("Duck When", source);
            if (bus->Duck.Source != 0)
            {
                const auto duck = [&rows, this, findBus](const std::string& label, float AudioMixerDuck::*field, float min, float max, const char* format, const std::string& tooltip)
                {
                    rows.Slider(label, [findBus, field]() { AudioMixerBus* b = findBus(); return b ? b->Duck.*field : 0.0f; },
                                [this, findBus, field](float v) { if (AudioMixerBus* b = findBus()) { b->Duck.*field = v; Changed(true); } }, min, max, format, tooltip);
                };
                duck("Depth (dB)", &AudioMixerDuck::DepthDb, -40.0f, 0.0f, "{:.1f}", "How far it goes down.");
                duck("Threshold (dB)", &AudioMixerDuck::ThresholdDb, -80.0f, 0.0f, "{:.1f}", "The other bus counts as playing above this level.");
                duck("Attack (s)", &AudioMixerDuck::Attack, 0.005f, 2.0f, "{:.3f}", "How fast it goes down.");
                duck("Release (s)", &AudioMixerDuck::Release, 0.02f, 5.0f, "{:.2f}", "How fast it comes back up once the other bus is quiet.");
            }
        }
    }

    void MixerV2Panel::DeleteBus(uint64_t id)
    {
        AudioMixerData& data = AudioMixer::GetData();
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
        if (data.Reflections.Bus == id)
            data.Reflections.Bus = kAudioBusMaster; // (the old panel left the reflections on a bus that was gone)
        for (AudioMixerSnapshot& snapshot : data.Snapshots)
            std::erase_if(snapshot.Values, [&](const auto& entry) { return entry.first.Bus == id || (entry.first.Kind == AudioMixerValueKind::Send && entry.first.Item == id); });
        std::erase_if(data.Buses, [&](const AudioMixerBus& candidate) { return candidate.Id == id; });
        m_SelectedBus = parent;
        Changed(true);
    }
}
