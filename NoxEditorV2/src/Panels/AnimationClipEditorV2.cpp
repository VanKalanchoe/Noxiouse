#include "AnimationClipEditorV2.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "Panels/AssetPreviewRenderer.h"
#include "Panels/InspectorWidgetsV2.h"
#include <NoxUI/Rendering/UIFont.h>
#include <NoxUI/Widgets/UIPropertyRows.h>
#include "NoxCore/Animation/AnimPose.h"
#include "NoxCore/Animation/AnimationSequence.h"
#include "NoxCore/Animation/Skeleton.h"
#include "NoxCore/Asset/AnimationImporter.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Audio/AudioClip.h"
#include "NoxCore/Audio/AudioEngine.h"
#include "NoxCore/Audio/AudioGraph.h"
#include "NoxCore/Audio/AudioMixer.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Font.h"
#include <NoxUI/Core/UIBuilder.h>
#include <NoxUI/Core/UIContext.h>
#include "NoxCore/UI/Core/UILayer.h"
#include <NoxUI/Layout/UILayout.h>
#include <NoxUI/Rendering/UIDrawList.h>
#include <NoxUI/Widgets/UIButton.h>
#include <NoxUI/Widgets/UICheckBox.h>
#include <NoxUI/Widgets/UIDragScalar.h>
#include <NoxUI/Widgets/UIImage.h>
#include <NoxUI/Widgets/UILabel.h>
#include <NoxUI/Widgets/UIMenu.h>
#include <NoxUI/Widgets/UIPopup.h>
#include <NoxUI/Widgets/UIScrollView.h>
#include <NoxUI/Widgets/UITextInput.h>

namespace Nox
{
    namespace
    {
        NoxUI::UIContext* Context()
        {
            UILayer* uiLayer = Application::Get().GetUILayer();
            return uiLayer ? &uiLayer->GetContext() : nullptr;
        }

        AnimationSequence* Clip(AssetHandle handle) { return AssetManager::FindLoadedAsset<AnimationSequence>(handle); }

        // A notify's sound, heard as the preview passes it (2D, the Editor group; nothing until the sound is loaded).
        void PlayPreviewSound(AssetHandle sound, float volume)
        {
            if (sound == 0)
                return;
            AudioPlayDesc desc;
            desc.Volume = volume;
            desc.Bus = NoxAudio::kAudioBusMaster;
            desc.Group = NoxAudio::AudioVoiceGroup::Editor;
            if (AssetManager::GetAssetType(sound) == AssetType::AudioGraph)
            {
                desc.Graph = GetAudioGraphProgram(sound);
                if (!desc.Graph)
                    return;
            }
            else
            {
                if (AssetManager::RequestAsset(sound) != AssetState::Ready)
                    return;
                desc.Clip = AssetManager::GetAsset<AudioClip>(sound);
            }
            AudioEngine::Play(desc);
        }

        Ref<NoxUI::UILabel> Muted(const std::string& text, float size = 13.0f)
        {
            Ref<NoxUI::UILabel> label(new NoxUI::UILabel(text, size));
            label->UseColorOverride = true;
            label->ColorOverride = NoxUI::ThemeColor("TextMuted", { 0.60f, 0.64f, 0.72f, 1.0f });
            label->VAlign = NoxUI::UIAlignment::Center;
            return label;
        }

        // The clip's timeline: the playhead (click or drag moves it, and pauses), the loop range shaded, a marker per notify.
        class ClipTimeline final : public NoxUI::UIElement
        {
        public:
            ClipTimeline(AnimationClipEditorV2* editor, AssetHandle clip) : m_Editor(editor), m_Clip(clip)
            {
                HitTestMode = NoxUI::UIHitTestMode::Self;
                WidthMode = NoxUI::UISizeMode::Fill;
                HeightMode = NoxUI::UISizeMode::Fixed;
                FixedSize = { 0.0f, 36.0f };
                Tooltip = "Click or drag: move the playhead. Orange: notifies (events), green: sync markers, blue: the loop range.";
            }
            bool OnPointerDown(const NoxUI::UIPointerEvent& e) override
            {
                if (e.Button != 1)
                    return false;
                m_Dragging = true;
                Seek(e.Position.x);
                return true;
            }
            bool OnPointerMove(const NoxUI::UIPointerEvent& e) override
            {
                if (m_Dragging)
                    Seek(e.Position.x);
                return true;
            }
            bool OnPointerUp(const NoxUI::UIPointerEvent&) override { m_Dragging = false; return true; }

        protected:
            void OnPaint(NoxUI::UIDrawList& d, const NoxUI::UITheme&) override
            {
                const NoxUI::UIRect b = GetBounds();
                d.AddRoundedRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Max.y }, NoxUI::ThemeColor("ClipTimelineBackground", { 0.10f, 0.10f, 0.12f, 1.0f }), glm::vec4(3.0f));
                const AnimationSequence* clip = Clip(m_Clip);
                if (!clip)
                    return;
                const float duration = std::max(clip->Duration, 1e-3f);
                const auto toX = [&](float t) { return b.Min.x + b.Width() * std::clamp(t / duration, 0.0f, 1.0f); };
                const float rangeStart = std::clamp(m_Editor->RangeStart, 0.0f, duration);
                const float rangeEnd = m_Editor->RangeEnd > rangeStart ? std::min(m_Editor->RangeEnd, duration) : duration;
                if (rangeStart > 0.0f || rangeEnd < duration)
                {
                    d.AddRoundedRect({ toX(rangeStart), b.Min.y }, { toX(rangeEnd), b.Max.y }, NoxUI::ThemeColor("ClipRange", { 0.31f, 0.63f, 1.0f, 0.2f }));
                    d.AddRoundedRect({ toX(rangeStart) - 1.0f, b.Min.y }, { toX(rangeStart) + 1.0f, b.Max.y }, NoxUI::ThemeColor("ClipRangeEdge", { 0.31f, 0.63f, 1.0f, 1.0f }));
                    d.AddRoundedRect({ toX(rangeEnd) - 1.0f, b.Min.y }, { toX(rangeEnd) + 1.0f, b.Max.y }, NoxUI::ThemeColor("ClipRangeEdge", { 0.31f, 0.63f, 1.0f, 1.0f }));
                }
                for (const AnimationNotify& notify : clip->Notifies)
                {
                    const float x = toX(notify.Time);
                    d.AddRoundedRect({ x - 1.0f, b.Min.y + 4.0f }, { x + 1.0f, b.Max.y - 4.0f },
                                     notify.SyncMarker ? NoxUI::ThemeColor("NotifySyncMarker", { 0.35f, 0.86f, 0.55f, 1.0f }) : NoxUI::ThemeColor("Notify", { 1.0f, 0.75f, 0.24f, 1.0f }));
                }
                const float x = toX(m_Editor->Cursor);
                d.AddRoundedRect({ x - 1.5f, b.Min.y }, { x + 1.5f, b.Max.y }, NoxUI::ThemeColor("ClipCursor", { 1.0f, 1.0f, 1.0f, 0.95f }));
                d.AddText(std::format("{:.3f} s", m_Editor->Cursor), nullptr, { b.Min.x + 6.0f, b.Min.y + 10.0f }, 12.0f, NoxUI::ThemeColor("ClipCursorText", { 0.85f, 0.87f, 0.92f, 0.8f }));
            }

        private:
            void Seek(float x)
            {
                const AnimationSequence* clip = Clip(m_Clip);
                if (!clip)
                    return;
                const NoxUI::UIRect b = GetBounds();
                m_Editor->Cursor = std::clamp((x - b.Min.x) / std::max(b.Width(), 1.0f), 0.0f, 1.0f) * std::max(clip->Duration, 1e-3f);
                m_Editor->Playing = false; // (scrubbing)
            }
            AnimationClipEditorV2* m_Editor = nullptr;
            AssetHandle m_Clip = 0;
            bool m_Dragging = false;
        };
    }

    AnimationClipEditorV2::AnimationClipEditorV2(AssetHandle clip)
        : m_Clip(clip), m_Name(AssetDisplayName(clip))
    {
    }

    void AnimationClipEditorV2::Edited()
    {
        m_Dirty = true;
    }

    void AnimationClipEditorV2::Save()
    {
        AnimationSequence* clip = Clip(m_Clip);
        if (!clip || !m_Dirty)
            return;
        // Notifies without a name are dropped and the rest put in time order when saved (not while typing: rows stayed put).
        std::erase_if(clip->Notifies, [](const AnimationNotify& notify) { return notify.Name.empty(); });
        std::sort(clip->Notifies.begin(), clip->Notifies.end(), [](const AnimationNotify& a, const AnimationNotify& b) { return a.Time < b.Time; });
        const std::filesystem::path path = Project::GetActiveAssetDirectory() / AssetManager::GetMetadata(m_Clip).FilePath;
        if (!AnimationImporter::SaveNotifies(path, clip->Notifies))
        {
            NOX_CORE_WARN("Could not save the notifies of {}", path.string());
            if (Notify)
                Notify({ NoxUI::UINotificationSeverity::Error, "Notifies not saved", path.filename().string() + " could not be written." });
            return;
        }
        m_Dirty = false;
        m_NotifyCount = ~size_t(0); // (rows in the new order)
        if (Notify)
            Notify({ NoxUI::UINotificationSeverity::Success, "Animation clip saved", m_Name });
    }

    bool AnimationClipEditorV2::RevertToDisk()
    {
        AnimationSequence* clip = Clip(m_Clip);
        if (!clip)
            return true;
        const std::filesystem::path path = Project::GetActiveAssetDirectory() / AssetManager::GetMetadata(m_Clip).FilePath;
        clip->Notifies = AnimationImporter::LoadNotifies(path);
        m_Dirty = false;
        m_NotifyCount = ~size_t(0);
        return true;
    }

    void AnimationClipEditorV2::FindPreviewMesh()
    {
        m_PreviewSearched = true;
        auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr;
        if (!assets)
            return;
        const AssetMetadata& clipMetadata = AssetManager::GetMetadata(m_Clip);
        // A skeletal mesh cooked from the same model, else the first one in the clip's folder.
        AssetHandle sameSource = 0, sameFolder = 0;
        for (const auto& [handle, metadata] : assets->GetAssetRegistry())
        {
            if (metadata.Type != AssetType::Mesh)
                continue;
            if (!clipMetadata.SourceFilePath.empty() && metadata.SourceFilePath == clipMetadata.SourceFilePath && sameSource == 0)
                sameSource = handle;
            if (metadata.FilePath.parent_path() == clipMetadata.FilePath.parent_path() && sameFolder == 0)
                sameFolder = handle;
        }
        SetPreviewMesh(sameSource != 0 ? sameSource : sameFolder);
    }

    void AnimationClipEditorV2::SetPreviewMesh(AssetHandle mesh)
    {
        m_PreviewMesh = mesh;
        m_PreviewSkeleton = 0;
        if (mesh != 0)
            if (auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr)
                m_PreviewSkeleton = assets->FindSkeletonOf(mesh);
    }

    Ref<NoxUI::UIElement> AnimationClipEditorV2::BuildUI()
    {
        auto root = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 6.0f);
        root->WidthMode = NoxUI::UISizeMode::Fill;
        root->HeightMode = NoxUI::UISizeMode::Fill;
        root->StyleClass = "Surface";
        root->DrawBackground = true;
        root->Padding = NoxUI::UIThickness::All(10.0f);
        m_Root = root;

        // Toolbar: Save, the clip's file and figures.
        auto bar = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 8.0f);
        bar->WidthMode = NoxUI::UISizeMode::Fill;
        Ref<NoxUI::UIButton> save(new NoxUI::UIButton("Save", [this]() { Save(); }));
        bar->AddChild(save);
        auto info = Muted("");
        bar->AddChild(info);
        root->AddChild(bar);
        NoxUI::AppendSync(*bar, [this, save, info]()
        {
            save->SetEnabled(m_Dirty);
            save->StyleVariant = m_Dirty ? "primary" : "";
            const AnimationSequence* clip = Clip(m_Clip);
            info->Text = clip ? std::format("{}   |   {:.3f} s, {} channels, {:.0f} ticks per second{}", AssetManager::GetMetadata(m_Clip).FilePath.generic_string(),
                                            clip->Duration, clip->Channels.size(), clip->TicksPerSecond, m_Dirty ? "   |   unsaved" : "")
                              : std::string("Loading...");
        });

        // Preview and its controls.
        auto previewRow = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 10.0f);
        previewRow->WidthMode = NoxUI::UISizeMode::Fill;
        Ref<NoxUI::UIImage> image(new NoxUI::UIImage(~0u, { 256.0f, 256.0f }));
        image->WidthMode = NoxUI::UISizeMode::Fixed;
        image->HeightMode = NoxUI::UISizeMode::Fixed;
        image->FixedSize = { 256.0f, 256.0f };
        previewRow->AddChild(image);
        auto controls = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 6.0f);
        Ref<NoxUI::UIButton> play(new NoxUI::UIButton("Pause", [this]()
        {
            const AnimationSequence* clip = Clip(m_Clip);
            const float duration = clip ? std::max(clip->Duration, 1e-3f) : 1.0f;
            const float end = RangeEnd > RangeStart ? std::min(RangeEnd, duration) : duration;
            if (!Playing && !m_Loop && Cursor >= end)
                Cursor = std::clamp(RangeStart, 0.0f, duration);
            Playing = !Playing;
        }));
        controls->AddChild(play);
        Ref<NoxUI::UICheckBox> loop(new NoxUI::UICheckBox("Loop", m_Loop, [this](bool v) { m_Loop = v; }));
        controls->AddChild(loop);
        controls->AddChild(Muted("Speed"));
        Ref<NoxUI::UIDragScalar> speed = CreateSliderField(m_Speed, -2.0f, 2.0f, [this](float v) { m_Speed = v; });
        speed->Format = "{:.2f}x";
        speed->WidthMode = NoxUI::UISizeMode::Fixed;
        speed->FixedSize = { 160.0f, 24.0f };
        controls->AddChild(speed);
        controls->AddChild(Muted("Preview Mesh"));
        Ref<NoxUI::UIButton> mesh(new NoxUI::UIButton("(none)"));
        NoxUI::UIButton* meshRaw = mesh.get();
        mesh->OnClick = [this, meshRaw]()
        {
            NoxUI::UIContext* context = Context();
            auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr;
            if (!context || !assets)
                return;
            std::vector<NoxUI::UIMenuItem> items;
            for (const auto& [handle, metadata] : assets->GetAssetRegistry())
                if (metadata.Type == AssetType::Mesh)
                {
                    const AssetHandle h = handle;
                    items.push_back({ metadata.FilePath.stem().string(), "", [this, h]() { SetPreviewMesh(h); }, false, true, h == m_PreviewMesh, false,
                                      metadata.FilePath.generic_string() });
                }
            if (items.empty())
                items.push_back({ "No skeletal meshes in the project", "", {}, false, false, false, true });
            NoxUI::UIMenuPopup::ShowPopupMenu(*context, meshRaw->GetBounds(), std::move(items));
        };
        mesh->WidthMode = NoxUI::UISizeMode::Fixed;
        mesh->FixedSize = { 200.0f, 24.0f };
        controls->AddChild(mesh);
        auto meshNote = Muted("", 12.5f);
        meshNote->UseColorOverride = true;
        meshNote->ColorOverride = NoxUI::ThemeColor("StatusWarning", { 1.0f, 0.7f, 0.3f, 1.0f });
        controls->AddChild(meshNote);
        previewRow->AddChild(controls);
        root->AddChild(previewRow);
        NoxUI::AppendSync(*previewRow, [this, image, play, loop, speed, mesh, meshNote]()
        {
            Texture2D* frame = m_PreviewMesh != 0 ? AssetPreviewRenderer::Get().GetAnimationPreview(m_PreviewMesh, m_PreviewSkeleton, m_Clip, Cursor, 256) : nullptr;
            image->TextureId = frame ? frame->GetDescriptorIndexSlot() : ~0u;
            image->Tooltip = frame ? (m_LastNotifyAge < 0.6f && !m_LastNotify.empty() ? "Notify: " + m_LastNotify : std::string())
                                   : (m_PreviewMesh != 0 ? "Loading..." : "No skeletal mesh to preview with");
            play->SetText(Playing ? "Pause" : "Play");
            loop->Checked = m_Loop;
            if (!speed->IsInteracting())
                speed->Value = m_Speed;
            mesh->SetText(m_PreviewMesh != 0 ? AssetManager::GetMetadata(m_PreviewMesh).FilePath.stem().string() : std::string("(none)"));
            meshNote->Text = m_PreviewMesh != 0 && m_PreviewSkeleton == 0 ? "No skeleton from its model: shown in its rest pose" : "";
        });

        // Timeline and the loop range.
        root->AddChild(Ref<ClipTimeline>(new ClipTimeline(this, m_Clip)));
        auto rangeRow = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 6.0f);
        rangeRow->WidthMode = NoxUI::UISizeMode::Fill;
        rangeRow->AddChild(Ref<NoxUI::UIButton>(new NoxUI::UIButton("Range Start Here", [this]() { RangeStart = Cursor; })));
        rangeRow->AddChild(Ref<NoxUI::UIButton>(new NoxUI::UIButton("Range End Here", [this]() { RangeEnd = Cursor; })));
        rangeRow->AddChild(Ref<NoxUI::UIButton>(new NoxUI::UIButton("Whole Clip", [this]() { RangeStart = RangeEnd = 0.0f; })));
        auto rangeText = Muted("");
        rangeRow->AddChild(rangeText);
        root->AddChild(rangeRow);
        NoxUI::AppendSync(*rangeRow, [this, rangeText]()
        {
            const AnimationSequence* clip = Clip(m_Clip);
            const float duration = clip ? clip->Duration : 0.0f;
            rangeText->Text = std::format("Loop range: {:.3f} - {:.3f} s", RangeStart, RangeEnd > 0.0f ? RangeEnd : duration);
        });

        // Notifies.
        auto addRow = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 6.0f);
        addRow->WidthMode = NoxUI::UISizeMode::Fill;
        addRow->AddChild(Muted("Notifies"));
        auto newName = NoxUI::MakeUI<NoxUI::UITextInput>();
        newName->Text = m_NewName;
        newName->WidthMode = NoxUI::UISizeMode::Fixed;
        newName->FixedSize = { 180.0f, 24.0f };
        newName->OnChanged = [this](const std::string& text) { m_NewName = text; };
        addRow->AddChild(newName);
        Ref<NoxUI::UIButton> add(new NoxUI::UIButton("Add at Playhead", [this]()
        {
            AnimationSequence* clip = Clip(m_Clip);
            if (!clip || m_NewName.empty())
                return;
            AnimationNotify notify;
            notify.Time = Cursor;
            notify.Name = m_NewName;
            clip->Notifies.push_back(notify);
            Edited();
        }));
        add->Tooltip = "Scripts on the animated entity receive OnAnimationEvent(name) when a playhead crosses it.";
        addRow->AddChild(add);
        root->AddChild(addRow);
        auto scroll = NoxUI::MakeUI<NoxUI::UIScrollView>();
        scroll->WidthMode = NoxUI::UISizeMode::Fill;
        scroll->HeightMode = NoxUI::UISizeMode::Fill;
        m_Notifies = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 3.0f);
        m_Notifies->WidthMode = NoxUI::UISizeMode::Fill;
        scroll->AddChild(m_Notifies);
        root->AddChild(scroll);
        m_NotifyCount = ~size_t(0);
        return m_Root;
    }

    void AnimationClipEditorV2::RebuildNotifies()
    {
        m_Notifies->ClearChildren();
        AnimationSequence* clip = Clip(m_Clip);
        if (!clip)
            return;
        m_NotifyCount = clip->Notifies.size();
        const float duration = std::max(clip->Duration, 1e-3f);
        auto header = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 6.0f);
        for (const auto& [title, width] : std::initializer_list<std::pair<const char*, float>>{ { "Time", 110.0f }, { "Name", 200.0f }, { "Sync", 44.0f }, { "Sound", 160.0f } })
        {
            auto label = Muted(title, 12.5f);
            label->WidthMode = NoxUI::UISizeMode::Fixed;
            label->FixedSize = { width, 18.0f };
            header->AddChild(label);
        }
        m_Notifies->AddChild(header);
        for (size_t i = 0; i < clip->Notifies.size(); ++i)
        {
            auto row = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 6.0f);
            row->WidthMode = NoxUI::UISizeMode::Fill;
            const auto notify = [this, i]() -> AnimationNotify* { AnimationSequence* c = Clip(m_Clip); return c && i < c->Notifies.size() ? &c->Notifies[i] : nullptr; };
            auto time = NoxUI::MakeUI<NoxUI::UIDragScalar>(clip->Notifies[i].Time, 0.005f, [this, notify](float v) { if (AnimationNotify* n = notify()) { n->Time = v; Edited(); } });
            time->Min = 0.0f;
            time->Max = duration;
            time->Format = "{:.3f} s";
            time->WidthMode = NoxUI::UISizeMode::Fixed;
            time->FixedSize = { 110.0f, 24.0f };
            row->AddChild(time);
            auto name = NoxUI::MakeUI<NoxUI::UITextInput>();
            name->Text = clip->Notifies[i].Name;
            name->WidthMode = NoxUI::UISizeMode::Fixed;
            name->FixedSize = { 200.0f, 24.0f };
            // Renamed when the edit ends (Enter or click away); an empty name puts the old one back.
            NoxUI::UITextInput* nameRaw = name.get();
            const auto applyName = [this, notify, nameRaw]()
            {
                AnimationNotify* n = notify();
                if (!n || nameRaw->Text == n->Name)
                    return;
                if (nameRaw->Text.empty())
                {
                    nameRaw->Text = n->Name;
                    return;
                }
                n->Name = nameRaw->Text;
                Edited();
            };
            name->OnSubmit = [applyName](const std::string&) { applyName(); };
            row->AddChild(name);
            Ref<NoxUI::UICheckBox> sync(new NoxUI::UICheckBox("", clip->Notifies[i].SyncMarker, [this, notify](bool v) { if (AnimationNotify* n = notify()) { n->SyncMarker = v; Edited(); } }));
            sync->Tooltip = "Sync marker: not raised as an event; blend spaces line their clips up at markers of the same name (e.g. L and R where each foot plants)";
            sync->WidthMode = NoxUI::UISizeMode::Fixed;
            sync->FixedSize = { 44.0f, 24.0f };
            row->AddChild(sync);
            Ref<NoxUI::UIButton> sound(new NoxUI::UIButton("Add Sound..."));
            NoxUI::UIButton* soundRaw = sound.get();
            sound->OnClick = [this, i, soundRaw]() { ShowSoundSettings(i, soundRaw->GetBounds()); };
            sound->Tooltip = "Play Sound: a sound this notify plays where the character is (or at a bone), optionally picked by the physical surface under it.";
            sound->WidthMode = NoxUI::UISizeMode::Fixed;
            sound->FixedSize = { 160.0f, 24.0f };
            row->AddChild(sound);
            row->AddChild(Ref<NoxUI::UIButton>(new NoxUI::UIButton("Go to", [this, notify]() { if (AnimationNotify* n = notify()) { Cursor = n->Time; Playing = false; } })));
            row->AddChild(Ref<NoxUI::UIButton>(new NoxUI::UIButton("Delete", [this, i]()
            {
                AnimationSequence* c = Clip(m_Clip);
                if (c && i < c->Notifies.size())
                {
                    c->Notifies.erase(c->Notifies.begin() + static_cast<std::ptrdiff_t>(i));
                    Edited();
                }
            })));
            m_Notifies->AddChild(row);
            NoxUI::AppendSync(*row, [notify, time, name, applyName, sync, sound]()
            {
                AnimationNotify* n = notify();
                if (!n)
                    return;
                if (!time->IsInteracting())
                    time->Value = n->Time;
                if (!NoxUI::HasState(name->GetState(), NoxUI::UIState::Focused))
                    applyName();
                sync->Checked = n->SyncMarker;
                std::string label = "Add Sound...";
                if (n->Sound != 0)
                    label = AssetManager::GetMetadata(n->Sound).FilePath.stem().string();
                else if (!n->SurfaceSounds.empty())
                    label = std::to_string(n->SurfaceSounds.size()) + " surface sounds";
                if (!n->SurfaceSounds.empty() && n->Sound != 0)
                    label += " +" + std::to_string(n->SurfaceSounds.size());
                if (sound->GetText() != label)
                    sound->SetText(label);
            });
        }
    }

    void AnimationClipEditorV2::ShowSoundSettings(size_t index, const NoxUI::UIRect& anchor)
    {
        NoxUI::UIContext* context = Context();
        AnimationSequence* clip = Clip(m_Clip);
        if (!context || !clip || index >= clip->Notifies.size())
            return;
        AnimationNotify* n = &clip->Notifies[index];
        auto box = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 4.0f);
        box->Padding = NoxUI::UIThickness::All(10.0f);
        box->FixedSize = { 460.0f, 0.0f };
        NoxUI::UIPropertyRows rows(box, context);
        rows.Header("Play Sound: " + n->Name);
        const auto changed = [this]() { Edited(); };
        InspectorCardBuilder slots{ box, {}, context };
        slots.AddAssetSlot("Sound", n->Sound, { AssetType::AudioClip, AssetType::AudioGraph }, [n, changed](AssetHandle sound) { n->Sound = sound; changed(); });
        // The bone it plays at: the preview skeleton's bones, or typed.
        std::vector<std::string> bones{ "(the entity)" };
        if (const Skeleton* skeleton = m_PreviewSkeleton != 0 ? AssetManager::FindLoadedAsset<Skeleton>(m_PreviewSkeleton) : nullptr)
        {
            for (const Node* node : skeleton->AllNodes)
                if (node && !node->Name.empty())
                    bones.push_back(node->Name);
        }
        else if (m_PreviewSkeleton != 0)
            AssetManager::RequestAsset(m_PreviewSkeleton);
        if (bones.size() > 1)
            rows.Choice("Bone", bones,
                [n, bones]() { const auto found = std::find(bones.begin() + 1, bones.end(), n->Bone); return found == bones.end() ? 0 : static_cast<int>(found - bones.begin()); },
                [n, bones, changed](int i) { n->Bone = i == 0 ? std::string() : bones[static_cast<size_t>(i)]; changed(); },
                "Where the sound plays (a foot for a footstep). The entity: its origin.");
        else
            rows.String("Bone", &n->Bone, "(the entity)", changed, "No preview skeleton: type the bone's name.");
        rows.Check("Trace Surface", [n]() { return n->TraceSurface; }, [n, changed](bool v) { n->TraceSurface = v; changed(); },
                   "A ray goes down from the bone; the physical surface it hits picks the sound from the list below. No hit, or no sound for that surface: Sound.");
        NoxUI::UIPropertyRows trace = rows.Group();
        rows.ShowWhen(trace.GetBody(), [n]() { return n->TraceSurface; });
        trace.Drag("Trace Distance (m)", [n]() { return n->TraceDistance; }, [n, changed](float v) { n->TraceDistance = v; changed(); }, 0.01f, 0.0f, 10.0f, "{:.2f}",
                   "How far below the bone the ground may be (the ray is this + 0.2 m long).");
        const std::vector<std::string> surfaces = Project::GetActive() ? Project::GetActive()->GetConfig().PhysicalSurfaces : std::vector<std::string>{ "Default" };
        for (size_t s = 0; s < n->SurfaceSounds.size(); ++s)
        {
            trace.Choice("Surface " + std::to_string(s + 1), surfaces,
                [n, s, surfaces]() { if (s >= n->SurfaceSounds.size()) return 0; const auto f = std::find(surfaces.begin(), surfaces.end(), n->SurfaceSounds[s].Surface); return f == surfaces.end() ? 0 : static_cast<int>(f - surfaces.begin()); },
                [n, s, surfaces, changed](int i) { if (s < n->SurfaceSounds.size()) { n->SurfaceSounds[s].Surface = surfaces[static_cast<size_t>(i)]; changed(); } });
            InspectorCardBuilder sound{ trace.GetBody(), {}, context };
            sound.AddAssetSlot("  Sound", n->SurfaceSounds[s].Sound, { AssetType::AudioClip, AssetType::AudioGraph },
                [n, s, changed](AssetHandle h) { if (s < n->SurfaceSounds.size()) { n->SurfaceSounds[s].Sound = h; changed(); } });
        }
        trace.Button("Add Surface Sound", [this, n, index, anchor, changed]() { n->SurfaceSounds.push_back({ "Grass", 0 }); changed(); ShowSoundSettings(index, anchor); });
        rows.Slider("Volume", [n]() { return n->Volume; }, [n, changed](float v) { n->Volume = v; changed(); }, 0.0f, 1.0f, "{:.2f}");
        rows.Drag("Pitch Variation", [n]() { return n->PitchVariation; }, [n, changed](float v) { n->PitchVariation = v; changed(); }, 0.005f, 0.0f, 0.5f, "+/- {:.3f}",
                  "Each play's pitch is random within this (0.05: +/- 5 %): no two footsteps alike.");
        rows.Button("Remove Sound", [n, changed, context]() { n->Sound = 0; n->SurfaceSounds.clear(); changed(); context->ClearOverlay(); });
        // (its rows follow the notify while the popup is up, and stop with it: their syncs are part of the overlay)
        NoxUI::UIPopup::Show(*context, anchor, box);
    }

    void AnimationClipEditorV2::Update()
    {
        AnimationSequence* clip = Clip(m_Clip);
        if (!clip)
        {
            AssetManager::RequestAsset(m_Clip);
            return;
        }
        if (!m_PreviewSearched)
            FindPreviewMesh();
        const auto now = std::chrono::steady_clock::now();
        const float dt = m_LastUpdate.time_since_epoch().count() == 0 ? 0.0f : std::min(std::chrono::duration<float>(now - m_LastUpdate).count(), 0.1f);
        m_LastUpdate = now;

        // Playing moves the playhead inside the loop range (looping or stopping at its end) and reports the notifies it passes.
        const float duration = std::max(clip->Duration, 1e-3f);
        const float rangeStart = std::clamp(RangeStart, 0.0f, duration);
        const float rangeEnd = RangeEnd > rangeStart ? std::min(RangeEnd, duration) : duration;
        const float rangeLength = std::max(rangeEnd - rangeStart, 1e-4f);
        if (Playing && (Cursor < rangeStart || Cursor > rangeEnd))
            Cursor = m_Speed >= 0.0f ? rangeStart : rangeEnd;
        const float previous = Cursor;
        bool wrapped = false;
        if (Playing)
        {
            Cursor += dt * m_Speed;
            if (Cursor > rangeEnd || Cursor < rangeStart)
            {
                if (m_Loop)
                {
                    Cursor = rangeStart + std::fmod(std::fmod(Cursor - rangeStart, rangeLength) + rangeLength, rangeLength);
                    wrapped = true;
                }
                else
                {
                    Cursor = std::clamp(Cursor, rangeStart, rangeEnd);
                    Playing = false;
                }
            }
            std::vector<AnimationNotify> crossed;
            if (wrapped && (rangeStart > 0.0f || rangeEnd < duration))
            {
                if (m_Speed >= 0.0f)
                {
                    CollectCrossedNotifies(*clip, previous, rangeEnd, false, crossed);
                    CollectCrossedNotifies(*clip, rangeStart - 1e-5f, Cursor, false, crossed);
                }
                else
                {
                    CollectCrossedNotifies(*clip, previous, rangeStart, false, crossed);
                    CollectCrossedNotifies(*clip, rangeEnd + 1e-5f, Cursor, false, crossed);
                }
            }
            else
                CollectCrossedNotifies(*clip, previous, Cursor, wrapped, crossed);
            if (!crossed.empty())
            {
                m_LastNotify = crossed.back().Name;
                m_LastNotifyAge = 0.0f;
            }
            for (const AnimationNotify& notify : crossed)
            {
                AssetHandle sound = notify.Sound;
                if (sound == 0 && !notify.SurfaceSounds.empty())
                    sound = notify.SurfaceSounds.front().Sound;
                PlayPreviewSound(sound, notify.Volume);
            }
        }
        m_LastNotifyAge += dt;

        if (m_Notifies && clip->Notifies.size() != m_NotifyCount)
            RebuildNotifies();
    }
}
