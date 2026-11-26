#include "Panels/VfxEditor.h"

#include <algorithm>
#include <cstdio>

#include "Panels/InspectorWidgets.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Scene.h"

#include <NoxUI/Core/UIBuilder.h>
#include <NoxUI/Core/UITheme.h>
#include <NoxUI/Widgets/UIBadge.h>
#include <NoxUI/Widgets/UIButton.h>
#include <NoxUI/Widgets/UICheckBox.h>
#include <NoxUI/Widgets/UIColorPicker.h>
#include <NoxUI/Widgets/UICurveEditor.h>
#include <NoxUI/Widgets/UIDragScalar.h>
#include <NoxUI/Widgets/UIDropdown.h>
#include <NoxUI/Widgets/UILabel.h>
#include <NoxUI/Widgets/UIPropertyRows.h>
#include <NoxUI/Widgets/UIScrollView.h>
#include <NoxUI/Widgets/UISeparator.h>
#include <NoxUI/Widgets/UITextInput.h>
#include <NoxUI/Widgets/UITimeline.h>
#include <NoxUI/Widgets/UIVectorEdit.h>

namespace Nox
{
    namespace
    {
        const char* LayerTypeName(VfxLayerType t)
        {
            switch (t)
            {
            case VfxLayerType::Particles:  return "Particles";
            case VfxLayerType::Trail:      return "Trail";
            case VfxLayerType::Beam:       return "Beam";
            case VfxLayerType::Decal:      return "Decal";
            case VfxLayerType::Distortion: return "Distortion";
            case VfxLayerType::Volume:     return "Volume";
            case VfxLayerType::LocalFog:   return "LocalFog";
            case VfxLayerType::PointLight: return "Light";
            }
            return "Layer";
        }

        NoxUI::UIColor LayerTypeColor(VfxLayerType t)
        {
            switch (t)
            {
            case VfxLayerType::Particles:  return { 1.00f, 0.52f, 0.24f, 1.0f };
            case VfxLayerType::Trail:      return { 0.28f, 0.84f, 0.92f, 1.0f };
            case VfxLayerType::Beam:       return { 0.42f, 0.72f, 1.00f, 1.0f };
            case VfxLayerType::Decal:      return { 0.52f, 0.86f, 0.42f, 1.0f };
            case VfxLayerType::Distortion: return { 0.94f, 0.44f, 0.78f, 1.0f };
            case VfxLayerType::Volume:     return { 0.74f, 0.48f, 0.98f, 1.0f };
            case VfxLayerType::LocalFog:   return { 0.46f, 0.76f, 0.96f, 1.0f };
            case VfxLayerType::PointLight: return { 1.00f, 0.84f, 0.30f, 1.0f };
            }
            return { 0.7f, 0.7f, 0.75f, 1.0f };
        }

        void AddPropRow(const Ref<NoxUI::UIStackBox>& parent, const std::string& label, const Ref<NoxUI::UIElement>& control)
        {
            auto row = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 8.0f);
            row->WidthMode = NoxUI::UISizeMode::Fill;
            auto lbl = NoxUI::Label(label, 12.5f);
            lbl->WidthMode = NoxUI::UISizeMode::Fixed;
            lbl->FixedSize = { 132.0f, 22.0f };
            row->AddChild(lbl);
            control->WidthMode = NoxUI::UISizeMode::Fill;
            control->FlexWeight = 1.0f;
            row->AddChild(control);
            parent->AddChild(row);
        }

        void AddFloatRow(const Ref<NoxUI::UIStackBox>& parent, const std::string& label, float* value,
                         float speed, float minVal, float maxVal, std::function<void()> onChange)
        {
            auto drag = NoxUI::MakeUI<NoxUI::UIDragScalar>(*value, speed);
            drag->Min = minVal;
            drag->Max = maxVal;
            drag->OnChanged = [value, onChange](float v) { *value = v; if (onChange) onChange(); };
            AddPropRow(parent, label, drag);
        }

        void AddBoolRow(const Ref<NoxUI::UIStackBox>& parent, const std::string& label, bool* value,
                        std::function<void()> onChange)
        {
            auto box = NoxUI::MakeUI<NoxUI::UICheckBox>("", *value);
            box->OnChanged = [value, onChange](bool v) { *value = v; if (onChange) onChange(); };
            AddPropRow(parent, label, box);
        }

        void AddVec3Row(const Ref<NoxUI::UIStackBox>& parent, const std::string& label, glm::vec3* value,
                        float speed, std::function<void()> onChange)
        {
            auto edit = NoxUI::MakeUI<NoxUI::UIVectorEdit>(3, glm::vec4(*value, 0.0f), speed);
            edit->OnChanged = [value, onChange](const glm::vec4& v) { *value = glm::vec3(v); if (onChange) onChange(); };
            AddPropRow(parent, label, edit);
        }

        void AddColor4Row(const Ref<NoxUI::UIStackBox>& parent, const std::string& label, glm::vec4* value,
                          std::function<void()> onChange)
        {
            auto picker = NoxUI::MakeUI<NoxUI::UIColorPicker>(*value);
            picker->HeightMode = NoxUI::UISizeMode::Fixed;
            picker->FixedSize = { 0.0f, 110.0f };
            picker->OnColorChanged = [value, onChange](const NoxUI::UIColor& c)
            {
                *value = { c.r, c.g, c.b, c.a };
                if (onChange) onChange();
            };
            AddPropRow(parent, label, picker);
        }

        void AddColor3Row(const Ref<NoxUI::UIStackBox>& parent, const std::string& label, glm::vec3* value,
                          std::function<void()> onChange)
        {
            auto picker = NoxUI::MakeUI<NoxUI::UIColorPicker>(NoxUI::UIColor{ value->r, value->g, value->b, 1.0f });
            picker->ShowAlpha = false;
            picker->HeightMode = NoxUI::UISizeMode::Fixed;
            picker->FixedSize = { 0.0f, 100.0f };
            picker->OnColorChanged = [value, onChange](const NoxUI::UIColor& c)
            {
                *value = { c.r, c.g, c.b };
                if (onChange) onChange();
            };
            AddPropRow(parent, label, picker);
        }
    }

    VfxEditor::VfxEditor(AssetHandle vfxHandle, Hooks hooks)
        : m_Handle(vfxHandle)
        , m_Name(AssetDisplayName(vfxHandle))
        , m_Hooks(std::move(hooks))
        , m_LastTick(std::chrono::steady_clock::now())
    {
        // Trigger synchronous load if not already in memory.
        AssetManager::GetAsset<VfxAsset>(m_Handle);
    }

    VfxEditor::~VfxEditor() = default;

    VfxAsset* VfxEditor::GetAsset() const
    {
        return const_cast<VfxAsset*>(AssetManager::FindLoadedAsset<VfxAsset>(m_Handle));
    }

    void VfxEditor::MarkDirty(bool restartPreview)
    {
        m_Dirty = true;
        if (m_StatusLabel)
            m_StatusLabel->Text = "Unsaved changes *";
        SyncScenePreview(restartPreview);
        SyncTimelineMarkers();
    }

    void VfxEditor::SyncScenePreview(bool restart, float* scrubTime)
    {
        if (!m_Hooks.ActiveScene)
            return;
        Ref<Scene> scene = m_Hooks.ActiveScene();
        if (!scene)
            return;
        auto view = scene->GetRegistry().view<VfxComponent>();
        for (auto entity : view)
        {
            auto& comp = view.get<VfxComponent>(entity);
            if (comp.Vfx != m_Handle)
                continue;
            comp.Playing = m_Playing;
            if (restart)
                comp.Runtime.NeedsRestart = true;
            if (scrubTime)
                comp.Runtime.Time = *scrubTime;
        }
    }

    void VfxEditor::Save()
    {
        VfxAsset* asset = GetAsset();
        if (!asset)
            return;
        std::filesystem::path savePath = asset->FilePath;
        if (savePath.empty() && Project::GetActive())
        {
            if (auto mgr = Project::GetActive()->GetEditorAssetManager())
            {
                const AssetMetadata& meta = mgr->GetMetadata(m_Handle);
                if (!meta.FilePath.empty())
                    savePath = Project::GetAssetFileSystemPath(meta.FilePath);
            }
        }
        if (savePath.empty() || !asset->Save(savePath))
        {
            if (Notify)
                Notify({ NoxUI::UINotificationSeverity::Error, "Save failed", "Could not write .nvfx file." });
            return;
        }
        asset->FilePath = savePath;
        m_Dirty = false;
        if (m_StatusLabel)
            m_StatusLabel->Text = "Saved";
        if (Notify)
            Notify({ NoxUI::UINotificationSeverity::Success, "VFX saved", savePath.filename().string() });
    }

    bool VfxEditor::RevertToDisk()
    {
        VfxAsset* asset = GetAsset();
        if (!asset || asset->FilePath.empty())
            return false;
        if (!asset->Load(asset->FilePath))
            return false;
        m_Dirty = false;
        if (m_StatusLabel)
            m_StatusLabel->Text = "Reverted";
        m_SelectedLayer = asset->Layers.empty() ? -1 : 0;
        RebuildLeftPane();
        RebuildDetailsPane();
        SyncScenePreview(true);
        return true;
    }

    void VfxEditor::SyncTimelineMarkers()
    {
        VfxAsset* asset = GetAsset();
        if (!asset || !m_Timeline)
            return;
        m_Timeline->SetDuration(std::max(asset->Duration, 0.5f));
        std::vector<float> markers;
        markers.reserve(asset->Layers.size());
        for (const auto& layer : asset->Layers)
            markers.push_back(layer.StartDelay);
        m_Timeline->SetKeyframes(std::move(markers));
    }

    Ref<NoxUI::UIElement> VfxEditor::BuildUI()
    {
        auto root = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 6.0f);
        root->WidthMode = NoxUI::UISizeMode::Fill;
        root->HeightMode = NoxUI::UISizeMode::Fill;
        root->Padding = NoxUI::UIThickness::All(8.0f);

        // 1. Top Toolbar
        auto toolbar = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 6.0f);
        toolbar->WidthMode = NoxUI::UISizeMode::Fill;

        auto saveBtn = NoxUI::Button("Save .nvfx", [this]() { Save(); });
        saveBtn->Padding = NoxUI::UIThickness::Symmetric(10.0f, 4.0f);
        toolbar->AddChild(saveBtn);

        auto revertBtn = NoxUI::Button("Revert", [this]() { RevertToDisk(); });
        revertBtn->Padding = NoxUI::UIThickness::Symmetric(8.0f, 4.0f);
        toolbar->AddChild(revertBtn);

        toolbar->AddChild(NoxUI::Label("| Presets:", 12.0f));
        for (const char* preset : { "Teleport", "Fireball", "Lightning" })
        {
            auto pBtn = NoxUI::Button(preset, [this, name = std::string(preset)]()
            {
                if (VfxAsset* a = GetAsset())
                {
                    a->LoadPreset(name);
                    m_SelectedLayer = a->Layers.empty() ? -1 : 0;
                    MarkDirty(true);
                    RebuildLeftPane();
                    RebuildDetailsPane();
                }
            });
            pBtn->Padding = NoxUI::UIThickness::Symmetric(8.0f, 4.0f);
            toolbar->AddChild(pBtn);
        }

        toolbar->AddChild(NoxUI::Label("|", 12.0f));
        auto playBtn = NoxUI::Button("Play / Pause", [this]()
        {
            m_Playing = !m_Playing;
            SyncScenePreview(false);
        });
        playBtn->Padding = NoxUI::UIThickness::Symmetric(8.0f, 4.0f);
        toolbar->AddChild(playBtn);

        auto restartBtn = NoxUI::Button("Restart", [this]()
        {
            m_Playing = true;
            m_Playhead = 0.0f;
            SyncScenePreview(true);
        });
        restartBtn->Padding = NoxUI::UIThickness::Symmetric(8.0f, 4.0f);
        toolbar->AddChild(restartBtn);

        m_StatusLabel = NoxUI::Label("Ready", 12.0f);
        m_StatusLabel->WidthMode = NoxUI::UISizeMode::Fill;
        m_StatusLabel->FlexWeight = 1.0f;
        toolbar->AddChild(m_StatusLabel);

        root->AddChild(toolbar);
        root->AddChild(NoxUI::MakeUI<NoxUI::UISeparator>());

        // 2. Main Split Body (Left: Layers + Blackboard; Right: Selected Layer Details + Curve Editor)
        auto split = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 10.0f);
        split->WidthMode = NoxUI::UISizeMode::Fill;
        split->HeightMode = NoxUI::UISizeMode::Fill;
        split->FlexWeight = 1.0f;

        // Left Column (280px)
        auto leftScroll = NoxUI::MakeUI<NoxUI::UIScrollView>();
        leftScroll->WidthMode = NoxUI::UISizeMode::Fixed;
        leftScroll->HeightMode = NoxUI::UISizeMode::Fill;
        leftScroll->FixedSize = { 290.0f, 0.0f };

        auto leftCol = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 6.0f);
        leftCol->WidthMode = NoxUI::UISizeMode::Fill;

        leftCol->AddChild(NoxUI::Label("EMITTER & LAYER STACK", 12.0f));
        auto addRow1 = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 3.0f);
        auto addRow2 = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 3.0f);
        for (int lt = 0; lt < 8; ++lt)
        {
            const auto type = static_cast<VfxLayerType>(lt);
            auto addBtn = NoxUI::Button(std::string("+") + LayerTypeName(type), [this, type]()
            {
                if (VfxAsset* a = GetAsset())
                {
                    VfxLayer nl{};
                    nl.Name = std::string(LayerTypeName(type)) + " " + std::to_string(a->Layers.size() + 1);
                    nl.Type = type;
                    nl.Enabled = true;
                    a->Layers.push_back(std::move(nl));
                    m_SelectedLayer = static_cast<int>(a->Layers.size()) - 1;
                    MarkDirty(true);
                    RebuildLeftPane();
                    RebuildDetailsPane();
                }
            });
            addBtn->Padding = NoxUI::UIThickness::Symmetric(5.0f, 2.0f);
            (lt < 4 ? addRow1 : addRow2)->AddChild(addBtn);
        }
        leftCol->AddChild(addRow1);
        leftCol->AddChild(addRow2);

        m_LayersBox = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 4.0f);
        m_LayersBox->WidthMode = NoxUI::UISizeMode::Fill;
        leftCol->AddChild(m_LayersBox);

        leftCol->AddChild(NoxUI::MakeUI<NoxUI::UISeparator>());
        leftCol->AddChild(NoxUI::Label("PARAMETERS BLACKBOARD", 12.0f));
        auto paramAddRow = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 4.0f);
        for (int pt = 0; pt < 3; ++pt)
        {
            static const char* pNames[] = { "+Float", "+Vec3", "+Color" };
            auto pBtn = NoxUI::Button(pNames[pt], [this, pt]()
            {
                if (VfxAsset* a = GetAsset())
                {
                    VfxParameter p{};
                    p.Name = "Param" + std::to_string(a->Parameters.size() + 1);
                    p.Type = static_cast<VfxParamType>(pt);
                    a->Parameters.push_back(std::move(p));
                    MarkDirty(false);
                    RebuildLeftPane();
                }
            });
            pBtn->Padding = NoxUI::UIThickness::Symmetric(6.0f, 2.0f);
            paramAddRow->AddChild(pBtn);
        }
        leftCol->AddChild(paramAddRow);

        m_ParamsBox = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 4.0f);
        m_ParamsBox->WidthMode = NoxUI::UISizeMode::Fill;
        leftCol->AddChild(m_ParamsBox);

        leftScroll->AddChild(leftCol);
        split->AddChild(leftScroll);

        // Right Column (Fill): Selected Layer Inspector + Curves
        auto rightScroll = NoxUI::MakeUI<NoxUI::UIScrollView>();
        rightScroll->WidthMode = NoxUI::UISizeMode::Fill;
        rightScroll->HeightMode = NoxUI::UISizeMode::Fill;
        rightScroll->FlexWeight = 1.0f;

        m_DetailsBox = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 6.0f);
        m_DetailsBox->WidthMode = NoxUI::UISizeMode::Fill;
        m_DetailsBox->Padding = NoxUI::UIThickness::Symmetric(6.0f, 2.0f);
        rightScroll->AddChild(m_DetailsBox);
        split->AddChild(rightScroll);

        root->AddChild(split);
        root->AddChild(NoxUI::MakeUI<NoxUI::UISeparator>());

        // 3. Bottom Timeline Scrubber Bar
        auto timeBar = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 8.0f);
        timeBar->WidthMode = NoxUI::UISizeMode::Fill;

        m_TimeLabel = NoxUI::Label("0.00s", 12.0f);
        m_TimeLabel->WidthMode = NoxUI::UISizeMode::Fixed;
        m_TimeLabel->FixedSize = { 54.0f, 24.0f };
        timeBar->AddChild(m_TimeLabel);

        const float initDur = GetAsset() ? std::max(GetAsset()->Duration, 0.5f) : 4.0f;
        m_Timeline = NoxUI::MakeUI<NoxUI::UITimeline>(initDur, 0.0f);
        m_Timeline->WidthMode = NoxUI::UISizeMode::Fill;
        m_Timeline->FlexWeight = 1.0f;
        m_Timeline->OnScrub = [this](float t)
        {
            m_Playhead = t;
            SyncScenePreview(false, &t);
        };
        timeBar->AddChild(m_Timeline);
        root->AddChild(timeBar);

        RebuildLeftPane();
        RebuildDetailsPane();
        SyncTimelineMarkers();
        return root;
    }

    void VfxEditor::RebuildLeftPane()
    {
        VfxAsset* asset = GetAsset();
        if (!asset || !m_LayersBox || !m_ParamsBox)
            return;

        m_LayersBox->ClearChildren();
        for (size_t i = 0; i < asset->Layers.size(); ++i)
        {
            VfxLayer& layer = asset->Layers[i];
            const bool selected = static_cast<int>(i) == m_SelectedLayer;

            auto row = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 4.0f);
            row->WidthMode = NoxUI::UISizeMode::Fill;
            row->Padding = NoxUI::UIThickness::Symmetric(4.0f, 3.0f);

            auto enBox = NoxUI::MakeUI<NoxUI::UICheckBox>("", layer.Enabled);
            enBox->OnChanged = [this, &layer](bool v) { layer.Enabled = v; MarkDirty(false); };
            row->AddChild(enBox);

            auto badge = NoxUI::MakeUI<NoxUI::UIBadge>(LayerTypeName(layer.Type), LayerTypeColor(layer.Type));
            row->AddChild(badge);

            auto selBtn = NoxUI::Button(selected ? ("> " + layer.Name) : layer.Name, [this, i]()
            {
                m_SelectedLayer = static_cast<int>(i);
                RebuildLeftPane();
                RebuildDetailsPane();
            });
            selBtn->WidthMode = NoxUI::UISizeMode::Fill;
            selBtn->FlexWeight = 1.0f;
            selBtn->Padding = NoxUI::UIThickness::Symmetric(6.0f, 3.0f);
            row->AddChild(selBtn);

            auto dupBtn = NoxUI::Button("+", [this, asset, i]()
            {
                VfxLayer copy = asset->Layers[i];
                copy.Name += " Copy";
                asset->Layers.insert(asset->Layers.begin() + static_cast<ptrdiff_t>(i + 1), std::move(copy));
                m_SelectedLayer = static_cast<int>(i + 1);
                MarkDirty(true);
                RebuildLeftPane();
                RebuildDetailsPane();
            });
            dupBtn->WidthMode = NoxUI::UISizeMode::Fixed;
            dupBtn->FixedSize = { 22.0f, 22.0f };
            dupBtn->Padding = NoxUI::UIThickness::All(0.0f);
            row->AddChild(dupBtn);

            auto delBtn = NoxUI::Button("x", [this, asset, i]()
            {
                asset->Layers.erase(asset->Layers.begin() + static_cast<ptrdiff_t>(i));
                if (m_SelectedLayer >= static_cast<int>(asset->Layers.size()))
                    m_SelectedLayer = static_cast<int>(asset->Layers.size()) - 1;
                MarkDirty(true);
                RebuildLeftPane();
                RebuildDetailsPane();
            });
            delBtn->WidthMode = NoxUI::UISizeMode::Fixed;
            delBtn->FixedSize = { 22.0f, 22.0f };
            delBtn->Padding = NoxUI::UIThickness::All(0.0f);
            row->AddChild(delBtn);

            m_LayersBox->AddChild(row);
        }

        // Parameters Blackboard
        m_ParamsBox->ClearChildren();
        for (size_t i = 0; i < asset->Parameters.size(); ++i)
        {
            VfxParameter& p = asset->Parameters[i];
            auto row = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 4.0f);
            row->WidthMode = NoxUI::UISizeMode::Fill;

            auto nameIn = NoxUI::MakeUI<NoxUI::UITextInput>(p.Name, "Name");
            nameIn->WidthMode = NoxUI::UISizeMode::Fixed;
            nameIn->FixedSize = { 100.0f, 22.0f };
            nameIn->OnChanged = [this, &p](const std::string& s) { p.Name = s; MarkDirty(false); };
            row->AddChild(nameIn);

            if (p.Type == VfxParamType::Float)
            {
                auto val = NoxUI::MakeUI<NoxUI::UIDragScalar>(p.FloatValue, 0.05f);
                val->Min = -1000.0f;
                val->Max = 1000.0f;
                val->WidthMode = NoxUI::UISizeMode::Fill;
                val->FlexWeight = 1.0f;
                val->OnChanged = [this, &p](float v) { p.FloatValue = v; MarkDirty(false); };
                row->AddChild(val);
            }
            else if (p.Type == VfxParamType::Color)
            {
                auto col = NoxUI::MakeUI<NoxUI::UIColorPicker>(p.ColorValue);
                col->WidthMode = NoxUI::UISizeMode::Fill;
                col->HeightMode = NoxUI::UISizeMode::Fixed;
                col->FixedSize = { 0.0f, 90.0f };
                col->FlexWeight = 1.0f;
                col->OnColorChanged = [this, &p](const NoxUI::UIColor& c) { p.ColorValue = { c.r, c.g, c.b, c.a }; MarkDirty(false); };
                row->AddChild(col);
            }
            else
            {
                auto vec = NoxUI::MakeUI<NoxUI::UIVectorEdit>(3, glm::vec4(p.Vec3Value, 0.0f), 0.05f);
                vec->WidthMode = NoxUI::UISizeMode::Fill;
                vec->FlexWeight = 1.0f;
                vec->OnChanged = [this, &p](const glm::vec4& v) { p.Vec3Value = glm::vec3(v); MarkDirty(false); };
                row->AddChild(vec);
            }

            auto delBtn = NoxUI::Button("x", [this, asset, i]()
            {
                asset->Parameters.erase(asset->Parameters.begin() + static_cast<ptrdiff_t>(i));
                MarkDirty(false);
                RebuildLeftPane();
            });
            delBtn->WidthMode = NoxUI::UISizeMode::Fixed;
            delBtn->FixedSize = { 22.0f, 22.0f };
            delBtn->Padding = NoxUI::UIThickness::All(0.0f);
            row->AddChild(delBtn);
            m_ParamsBox->AddChild(row);
        }
    }

    void VfxEditor::RebuildDetailsPane()
    {
        VfxAsset* asset = GetAsset();
        if (!asset || !m_DetailsBox)
            return;

        m_DetailsBox->ClearChildren();

        // System-level controls
        m_DetailsBox->AddChild(NoxUI::Label("SYSTEM SETTINGS", 12.5f));
        auto dirty = [this]() { MarkDirty(false); };
        auto dirtyRestart = [this]() { MarkDirty(true); };
        AddFloatRow(m_DetailsBox, "Duration (s)", &asset->Duration, 0.1f, 0.1f, 120.0f, dirty);
        AddBoolRow(m_DetailsBox, "Looping", &asset->Looping, dirty);
        m_DetailsBox->AddChild(NoxUI::MakeUI<NoxUI::UISeparator>());

        if (m_SelectedLayer < 0 || m_SelectedLayer >= static_cast<int>(asset->Layers.size()))
        {
            m_DetailsBox->AddChild(NoxUI::Label("Select or add a layer on the left to edit its properties.", 13.0f));
            return;
        }

        VfxLayer& layer = asset->Layers[static_cast<size_t>(m_SelectedLayer)];
        m_DetailsBox->AddChild(NoxUI::Label(std::string("LAYER: ") + layer.Name + " [" + LayerTypeName(layer.Type) + "]", 13.5f));

        auto nameInput = NoxUI::MakeUI<NoxUI::UITextInput>(layer.Name, "Layer Name");
        nameInput->OnChanged = [this, &layer](const std::string& s) { layer.Name = s; MarkDirty(false); RebuildLeftPane(); };
        AddPropRow(m_DetailsBox, "Layer Name", nameInput);
        AddBoolRow(m_DetailsBox, "Enabled", &layer.Enabled, dirty);
        AddVec3Row(m_DetailsBox, "Local Offset (m)", &layer.LocalOffset, 0.05f, dirty);
        AddVec3Row(m_DetailsBox, "Local Rotation (deg)", &layer.LocalRotation, 0.5f, dirty);
        AddFloatRow(m_DetailsBox, "Start Delay (s)", &layer.StartDelay, 0.05f, 0.0f, 60.0f, dirty);
        AddFloatRow(m_DetailsBox, "Layer Duration (s)", &layer.Duration, 0.05f, 0.0f, 60.0f, dirty);
        m_DetailsBox->AddChild(NoxUI::MakeUI<NoxUI::UISeparator>());

        switch (layer.Type)
        {
        case VfxLayerType::Particles:
        {
            auto& p = layer.Particle;
            m_DetailsBox->AddChild(NoxUI::Label("SPAWN & LIFETIME", 12.0f));
            AddBoolRow(m_DetailsBox, "GPU Simulated", &p.GpuSimulated, dirtyRestart);
            AddFloatRow(m_DetailsBox, "Spawn Rate (/s)", &p.SpawnRate, 5.0f, 0.0f, 100000.0f, dirty);
            AddFloatRow(m_DetailsBox, "Lifetime Min (s)", &p.LifetimeMin, 0.05f, 0.02f, 30.0f, dirty);
            AddFloatRow(m_DetailsBox, "Lifetime Max (s)", &p.LifetimeMax, 0.05f, 0.02f, 30.0f, dirty);
            AddVec3Row(m_DetailsBox, "Shape Size (m)", &p.ShapeSize, 0.05f, dirty);
            AddFloatRow(m_DetailsBox, "Cone Angle (deg)", &p.ConeAngle, 0.5f, 0.0f, 90.0f, dirty);
            AddVec3Row(m_DetailsBox, "Velocity (m/s)", &p.Velocity, 0.05f, dirty);
            AddVec3Row(m_DetailsBox, "Velocity Random", &p.VelocityRandom, 0.05f, dirty);
            AddFloatRow(m_DetailsBox, "Gravity", &p.Gravity, 0.05f, -10.0f, 10.0f, dirty);
            AddFloatRow(m_DetailsBox, "Drag", &p.Drag, 0.05f, 0.0f, 20.0f, dirty);

            m_DetailsBox->AddChild(NoxUI::MakeUI<NoxUI::UISeparator>());
            m_DetailsBox->AddChild(NoxUI::Label("APPEARANCE", 12.0f));
            AddFloatRow(m_DetailsBox, "Size Start (m)", &p.SizeStart, 0.01f, 0.001f, 10.0f, dirty);
            AddFloatRow(m_DetailsBox, "Size End (m)", &p.SizeEnd, 0.01f, 0.0f, 10.0f, dirty);
            AddFloatRow(m_DetailsBox, "HDR Intensity", &p.Intensity, 0.1f, 0.0f, 50.0f, dirty);
            AddColor4Row(m_DetailsBox, "Color Start", &p.ColorStart, dirty);
            AddColor4Row(m_DetailsBox, "Color End", &p.ColorEnd, dirty);

            // Interactive Size Over Life Curve Editor (NoxUI::UICurveEditor)
            m_DetailsBox->AddChild(NoxUI::Label("SIZE OVER LIFE CURVE", 12.0f));
            std::vector<NoxUI::UIVec2> pts;
            if (p.SizeOverLife.size() >= 2)
            {
                for (const auto& k : p.SizeOverLife)
                    pts.push_back({ k.Time, std::clamp(k.Size, 0.0f, 1.0f) });
            }
            else
            {
                pts = { { 0.0f, std::clamp(p.SizeStart, 0.01f, 1.0f) }, { 1.0f, std::clamp(p.SizeEnd, 0.0f, 1.0f) } };
            }
            auto curve = NoxUI::MakeUI<NoxUI::UICurveEditor>(pts);
            curve->WidthMode = NoxUI::UISizeMode::Fill;
            curve->HeightMode = NoxUI::UISizeMode::Fixed;
            curve->FixedSize = { 0.0f, 110.0f };
            curve->OnCurveChanged = [this, &p](const std::vector<NoxUI::UIVec2>& updated)
            {
                p.SizeOverLife.clear();
                for (const auto& pt : updated)
                    p.SizeOverLife.push_back({ pt.x, pt.y });
                MarkDirty(false);
            };
            m_DetailsBox->AddChild(curve);

            m_DetailsBox->AddChild(NoxUI::MakeUI<NoxUI::UISeparator>());
            m_DetailsBox->AddChild(NoxUI::Label("NIAGARA FORCE MODULES", 12.0f));
            AddBoolRow(m_DetailsBox, "Curl Noise", &p.CurlNoiseEnabled, dirty);
            AddFloatRow(m_DetailsBox, "Curl Strength", &p.CurlNoiseStrength, 0.1f, 0.0f, 50.0f, dirty);
            AddFloatRow(m_DetailsBox, "Curl Frequency", &p.CurlNoiseFrequency, 0.05f, 0.01f, 20.0f, dirty);
            AddBoolRow(m_DetailsBox, "Vortex", &p.VortexEnabled, dirty);
            AddFloatRow(m_DetailsBox, "Vortex Strength", &p.VortexStrength, 0.2f, -50.0f, 50.0f, dirty);
            AddFloatRow(m_DetailsBox, "Vortex Pull", &p.VortexRadialPull, 0.1f, -50.0f, 50.0f, dirty);
            AddFloatRow(m_DetailsBox, "Vortex Radius", &p.VortexRadius, 0.1f, 0.1f, 50.0f, dirty);
            AddBoolRow(m_DetailsBox, "Attractor", &p.AttractorEnabled, dirty);
            AddFloatRow(m_DetailsBox, "Attractor Strength", &p.AttractorStrength, 0.2f, 0.0f, 100.0f, dirty);
            AddBoolRow(m_DetailsBox, "Depth Collision", &p.DepthCollisionEnabled, dirty);
            AddFloatRow(m_DetailsBox, "Restitution", &p.CollisionRestitution, 0.02f, 0.0f, 1.0f, dirty);
            break;
        }
        case VfxLayerType::Trail:
        {
            auto& t = layer.Trail;
            AddBoolRow(m_DetailsBox, "Emitting", &t.Emitting, dirty);
            AddFloatRow(m_DetailsBox, "Lifetime (s)", &t.Lifetime, 0.05f, 0.02f, 20.0f, dirty);
            AddFloatRow(m_DetailsBox, "Width Start (m)", &t.WidthStart, 0.02f, 0.01f, 10.0f, dirty);
            AddFloatRow(m_DetailsBox, "Width End (m)", &t.WidthEnd, 0.02f, 0.0f, 10.0f, dirty);
            AddFloatRow(m_DetailsBox, "HDR Intensity", &t.Intensity, 0.1f, 0.0f, 50.0f, dirty);
            AddColor4Row(m_DetailsBox, "Color Start", &t.ColorStart, dirty);
            AddColor4Row(m_DetailsBox, "Color End", &t.ColorEnd, dirty);
            break;
        }
        case VfxLayerType::Beam:
        {
            auto& b = layer.Beam;
            AddVec3Row(m_DetailsBox, "Target Offset (m)", &b.TargetOffset, 0.1f, dirty);
            AddBoolRow(m_DetailsBox, "Local Target", &b.LocalTarget, dirty);
            AddFloatRow(m_DetailsBox, "Width Start (m)", &b.WidthStart, 0.01f, 0.005f, 5.0f, dirty);
            AddFloatRow(m_DetailsBox, "Width End (m)", &b.WidthEnd, 0.01f, 0.005f, 5.0f, dirty);
            AddFloatRow(m_DetailsBox, "Noise Amplitude", &b.NoiseAmplitude, 0.02f, 0.0f, 5.0f, dirty);
            AddFloatRow(m_DetailsBox, "Noise Frequency", &b.NoiseFrequency, 0.05f, 0.1f, 20.0f, dirty);
            AddFloatRow(m_DetailsBox, "Noise Speed", &b.NoiseSpeed, 0.2f, 0.0f, 100.0f, dirty);
            AddFloatRow(m_DetailsBox, "HDR Intensity", &b.Intensity, 0.2f, 0.0f, 50.0f, dirty);
            AddColor4Row(m_DetailsBox, "Color Start", &b.ColorStart, dirty);
            AddColor4Row(m_DetailsBox, "Color End", &b.ColorEnd, dirty);
            break;
        }
        case VfxLayerType::Decal:
        {
            auto& d = layer.Decal;
            AddVec3Row(m_DetailsBox, "Size (m)", &d.Size, 0.1f, dirty);
            AddColor4Row(m_DetailsBox, "Color", &d.Color, dirty);
            AddFloatRow(m_DetailsBox, "Emissive Intensity", &d.EmissiveIntensity, 0.1f, 0.0f, 50.0f, dirty);
            AddFloatRow(m_DetailsBox, "Normal Fade", &d.NormalFade, 0.02f, 0.0f, 1.0f, dirty);
            AddFloatRow(m_DetailsBox, "Edge Fade", &d.EdgeFade, 0.02f, 0.0f, 0.5f, dirty);
            break;
        }
        case VfxLayerType::Distortion:
        {
            auto& s = layer.Distortion;
            AddFloatRow(m_DetailsBox, "Radius (m)", &s.Radius, 0.05f, 0.05f, 50.0f, dirty);
            AddFloatRow(m_DetailsBox, "Strength", &s.Strength, 0.005f, 0.0f, 0.5f, dirty);
            AddFloatRow(m_DetailsBox, "Ring Width", &s.RingWidth, 0.02f, 0.04f, 1.0f, dirty);
            AddFloatRow(m_DetailsBox, "Speed", &s.Speed, 0.05f, 0.0f, 20.0f, dirty);
            AddFloatRow(m_DetailsBox, "Chromatic Aberration", &s.ChromaticAberration, 0.02f, 0.0f, 1.0f, dirty);
            AddBoolRow(m_DetailsBox, "Camera Facing", &s.CameraFacing, dirty);
            AddColor4Row(m_DetailsBox, "Rim Tint", &s.RimTint, dirty);
            break;
        }
        case VfxLayerType::Volume:
        {
            auto& v = layer.Volume;
            AddVec3Row(m_DetailsBox, "Bounds Scale (m)", &v.BoundsScale, 0.1f, dirty);
            AddFloatRow(m_DetailsBox, "Density Scale", &v.DensityScale, 0.1f, 0.0f, 50.0f, dirty);
            AddFloatRow(m_DetailsBox, "Emission Scale", &v.EmissionScale, 0.1f, 0.0f, 50.0f, dirty);
            AddFloatRow(m_DetailsBox, "Blackbody Temp (K)", &v.BlackbodyTemperature, 25.0f, 800.0f, 10000.0f, dirty);
            AddFloatRow(m_DetailsBox, "Anisotropy (g)", &v.Anisotropy, 0.02f, -0.9f, 0.9f, dirty);
            AddFloatRow(m_DetailsBox, "Ambient Strength", &v.AmbientStrength, 0.05f, 0.0f, 10.0f, dirty);
            AddColor3Row(m_DetailsBox, "Albedo", &v.Albedo, dirty);
            break;
        }
        case VfxLayerType::LocalFog:
        {
            auto& f = layer.LocalFog;
            AddVec3Row(m_DetailsBox, "Extents (m)", &f.Extents, 0.1f, dirty);
            AddFloatRow(m_DetailsBox, "Density", &f.Density, 0.02f, 0.0f, 10.0f, dirty);
            AddColor3Row(m_DetailsBox, "Albedo", &f.Albedo, dirty);
            AddColor3Row(m_DetailsBox, "Emissive Color", &f.EmissiveColor, dirty);
            AddFloatRow(m_DetailsBox, "Emissive Intensity", &f.EmissiveIntensity, 0.05f, 0.0f, 50.0f, dirty);
            AddFloatRow(m_DetailsBox, "Edge Falloff", &f.EdgeFalloff, 0.02f, 0.01f, 1.0f, dirty);
            AddFloatRow(m_DetailsBox, "Noise Strength", &f.NoiseStrength, 0.02f, 0.0f, 1.0f, dirty);
            AddFloatRow(m_DetailsBox, "Noise Scale", &f.NoiseScale, 0.05f, 0.01f, 10.0f, dirty);
            AddFloatRow(m_DetailsBox, "Noise Speed", &f.NoiseSpeed, 0.05f, 0.0f, 10.0f, dirty);
            break;
        }
        case VfxLayerType::PointLight:
        {
            auto& l = layer.Light;
            AddColor3Row(m_DetailsBox, "Color", &l.Color, dirty);
            AddFloatRow(m_DetailsBox, "Intensity", &l.Intensity, 0.5f, 0.0f, 500.0f, dirty);
            AddFloatRow(m_DetailsBox, "Radius (m)", &l.Radius, 0.2f, 0.1f, 100.0f, dirty);
            AddFloatRow(m_DetailsBox, "Pulse Speed", &l.PulseSpeed, 0.2f, 0.0f, 40.0f, dirty);
            break;
        }
        }
    }

    void VfxEditor::Update()
    {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - m_LastTick).count(), 0.0f, 0.1f);
        m_LastTick = now;

        VfxAsset* asset = GetAsset();
        if (!asset)
            return;

        if (m_Playing)
        {
            m_Playhead += dt;
            const float dur = std::max(asset->Duration, 0.5f);
            if (m_Playhead >= dur)
                m_Playhead = asset->Looping ? std::fmod(m_Playhead, dur) : dur;
        }
        if (m_Timeline)
            m_Timeline->SetCurrentTime(m_Playhead, false);
        if (m_TimeLabel)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2fs", m_Playhead);
            m_TimeLabel->Text = buf;
        }
    }
}
