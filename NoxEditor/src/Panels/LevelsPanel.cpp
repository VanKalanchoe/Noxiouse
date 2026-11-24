#include "LevelsPanel.h"

#include <filesystem>
#include <format>

#include "Panels/InspectorWidgets.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/SceneSerializer.h"
#include <NoxUI/Core/UIBuilder.h>
#include <NoxUI/Core/UIContext.h>
#include <NoxUI/Layout/UILayout.h>
#include <NoxUI/Widgets/UIButton.h>
#include <NoxUI/Widgets/UICheckBox.h>
#include <NoxUI/Widgets/UIDragScalar.h>
#include <NoxUI/Widgets/UIDropdown.h>
#include <NoxUI/Widgets/UILabel.h>
#include <NoxUI/Widgets/UIMenu.h>
#include <NoxUI/Widgets/UIModal.h>
#include <NoxUI/Widgets/UIPopup.h>
#include <NoxUI/Widgets/UIScrollView.h>
#include <NoxUI/Widgets/UITextInput.h>

namespace Nox
{
    namespace
    {
        constexpr float kRadiansToDegrees = 57.29578f;

        std::string LevelName(AssetHandle level)
        {
            return AssetManager::IsAssetHandleValid(level) ? AssetManager::GetMetadata(level).FilePath.stem().string() : std::string("(missing scene)");
        }

        const char* StateText(Scene::LevelState state)
        {
            return state == Scene::LevelState::Loaded ? "loaded" : state == Scene::LevelState::Loading ? "loading" : "unloaded";
        }

        Ref<NoxUI::UILabel> Muted(const std::string& text, float size = 13.0f)
        {
            Ref<NoxUI::UILabel> label(new NoxUI::UILabel(text, size));
            label->UseColorOverride = true;
            label->ColorOverride = NoxUI::ThemeColor("TextMuted", { 0.60f, 0.64f, 0.72f, 1.0f });
            label->VAlign = NoxUI::UIAlignment::Center;
            return label;
        }

        Ref<NoxUI::UIElement> FixedWidth(Ref<NoxUI::UIElement> element, float width)
        {
            element->WidthMode = NoxUI::UISizeMode::Fixed;
            element->FixedSize = { width, element->FixedSize.y > 0.0f ? element->FixedSize.y : 24.0f };
            element->VAlign = NoxUI::UIAlignment::Center;
            return element;
        }
    }

    void LevelsPanel::Changed() const
    {
        if (m_Hooks.SceneChanged)
            m_Hooks.SceneChanged();
    }

    const Ref<NoxUI::UIElement>& LevelsPanel::BuildUI(NoxUI::UIContext* context)
    {
        m_Context = context;
        auto root = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 0.0f);
        root->WidthMode = NoxUI::UISizeMode::Fill;
        root->HeightMode = NoxUI::UISizeMode::Fill;
        root->StyleClass = "Surface";
        root->DrawBackground = true;
        auto scroll = NoxUI::MakeUI<NoxUI::UIScrollView>();
        scroll->WidthMode = NoxUI::UISizeMode::Fill;
        scroll->HeightMode = NoxUI::UISizeMode::Fill;
        root->AddChild(scroll);
        m_Body = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 4.0f);
        m_Body->WidthMode = NoxUI::UISizeMode::Fill;
        m_Body->Padding = NoxUI::UIThickness::All(8.0f);
        scroll->AddChild(m_Body);
        m_Root = root;
        m_Rebuild = true;
        Update();
        return m_Root;
    }

    uint64_t LevelsPanel::Signature() const
    {
        // What changes the rows themselves: the scene, its levels, edit or play, world partition.
        const Ref<Scene> scene = m_Hooks.ActiveScene ? m_Hooks.ActiveScene() : nullptr;
        uint64_t h = reinterpret_cast<uintptr_t>(scene.get()) * 0x9E3779B97F4A7C15ull;
        if (!scene)
            return h;
        h ^= (m_Hooks.Editing && m_Hooks.Editing() ? 1ull : 2ull) + (h << 6) + (h >> 2);
        h ^= (scene->IsWorldPartitioned() ? 3ull : 4ull) + (h << 6) + (h >> 2);
        for (const Scene::SubLevel& level : scene->GetSubLevels())
            h ^= static_cast<uint64_t>(level.Level) + (level.Loaded ? 7ull : 11ull) + (h << 6) + (h >> 2);
        return h;
    }

    void LevelsPanel::Update()
    {
        if (!m_Body)
            return;
        const uint64_t signature = Signature();
        if (m_Rebuild || signature != m_Signature)
        {
            m_Signature = signature;
            m_Rebuild = false;
            Rebuild();
        }
    }

    void LevelsPanel::Rebuild()
    {
        m_Body->ClearChildren();
        const Ref<Scene> scene = m_Hooks.ActiveScene ? m_Hooks.ActiveScene() : nullptr;
        if (!scene)
        {
            m_Body->AddChild(Muted("No scene"));
            return;
        }
        if (scene->IsWorldPartitioned())
        {
            auto note = Muted("The scene is a world: it has no sub-levels (its cells are loaded in Window > World Partition).");
            note->WordWrap = true;
            note->WidthMode = NoxUI::UISizeMode::Fill;
            m_Body->AddChild(note);
            return;
        }
        const bool editing = m_Hooks.Editing && m_Hooks.Editing();
        std::function<Ref<Scene>()> getScene = m_Hooks.ActiveScene;

        // Header row.
        {
            auto header = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 6.0f);
            header->WidthMode = NoxUI::UISizeMode::Fill;
            header->AddChild(FixedWidth(Muted("Current"), 54.0f));
            header->AddChild(FixedWidth(Muted("Visible"), 46.0f));
            header->AddChild(FixedWidth(Muted("Locked"), 46.0f));
            auto level = Muted("Level");
            level->WidthMode = NoxUI::UISizeMode::Fill;
            header->AddChild(level);
            header->AddChild(FixedWidth(Muted("Loading"), 130.0f));
            header->AddChild(FixedWidth(Muted("Placement"), 150.0f));
            header->AddChild(FixedWidth(Muted(""), 70.0f));
            m_Body->AddChild(header);
        }

        // The persistent level: the scene itself.
        {
            auto row = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 6.0f);
            row->WidthMode = NoxUI::UISizeMode::Fill;
            Ref<NoxUI::UICheckBox> current(new NoxUI::UICheckBox("", scene->GetCurrentLevel() == 0, [this, getScene](bool)
            {
                if (Ref<Scene> s = getScene())
                {
                    s->SetCurrentLevel(0);
                    Changed();
                }
            }));
            current->Tooltip = "The current level: new entities go into it";
            current->SetEnabled(editing);
            row->AddChild(FixedWidth(current, 54.0f));
            row->AddChild(FixedWidth(Muted(""), 46.0f));
            row->AddChild(FixedWidth(Muted(""), 46.0f));
            const AssetHandle openScene = m_Hooks.OpenScene ? m_Hooks.OpenScene() : AssetHandle(0);
            auto name = NoxUI::Label("Persistent Level (" + (openScene != 0 ? LevelName(openScene) : std::string("unsaved scene")) + ")", 14.0f);
            name->WidthMode = NoxUI::UISizeMode::Fill;
            name->VAlign = NoxUI::UIAlignment::Center;
            row->AddChild(name);
            row->AddChild(FixedWidth(Muted("always"), 130.0f));
            row->AddChild(FixedWidth(Muted(""), 150.0f));
            row->AddChild(FixedWidth(Muted(""), 70.0f));
            m_Body->AddChild(row);
            NoxUI::AppendSync(*row, [current, getScene]() { if (Ref<Scene> s = getScene()) current->Checked = s->GetCurrentLevel() == 0; });
        }

        for (const Scene::SubLevel& subLevel : scene->GetSubLevels())
        {
            const AssetHandle level = subLevel.Level;
            // Each row reads its level again (the list can change under it): a level gone is skipped until the rows are rebuilt.
            const auto find = [getScene, level]() -> const Scene::SubLevel*
            {
                const Ref<Scene> s = getScene();
                return s ? s->FindSubLevel(level) : nullptr;
            };
            auto row = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 6.0f);
            row->WidthMode = NoxUI::UISizeMode::Fill;

            Ref<NoxUI::UICheckBox> current(new NoxUI::UICheckBox("", false, [this, getScene, level](bool)
            {
                if (Ref<Scene> s = getScene())
                {
                    s->SetCurrentLevel(level);
                    Changed();
                }
            }));
            current->Tooltip = "The current level: new entities go into it (a level must be loaded, visible and unlocked)";
            row->AddChild(FixedWidth(current, 54.0f));

            Ref<NoxUI::UICheckBox> visible(new NoxUI::UICheckBox("", subLevel.Visible, [this, getScene, level](bool v)
            {
                if (Ref<Scene> s = getScene())
                {
                    s->SetSubLevelVisible(level, v);
                    Changed();
                }
            }));
            visible->Tooltip = "Shown in the editor. Hidden: its entities are kept aside (not drawn, lit, picked or simulated); it is "
                               "still saved. In Play its loading setting decides, not this.";
            visible->SetEnabled(editing);
            row->AddChild(FixedWidth(visible, 46.0f));

            Ref<NoxUI::UICheckBox> locked(new NoxUI::UICheckBox("", subLevel.Locked, [this, getScene, level](bool v)
            {
                if (Ref<Scene> s = getScene())
                {
                    s->SetSubLevelLocked(level, v);
                    Changed();
                }
            }));
            locked->Tooltip = "Locked: its entities cannot be selected (so not moved, edited or deleted)";
            locked->SetEnabled(editing);
            row->AddChild(FixedWidth(locked, 46.0f));

            auto name = NoxUI::Label(LevelName(level), 14.0f);
            Ref<NoxUI::UILabel> nameRef = name;
            nameRef->WidthMode = NoxUI::UISizeMode::Fill;
            nameRef->VAlign = NoxUI::UIAlignment::Center;
            nameRef->Tooltip = AssetManager::IsAssetHandleValid(level) ? AssetManager::GetMetadata(level).FilePath.generic_string() : std::string();
            row->AddChild(nameRef);

            auto loading = NoxUI::MakeUI<NoxUI::UIDropdown>(m_Context, std::vector<std::string>{ "Always Loaded", "Streamed" }, subLevel.AlwaysLoaded ? 0 : 1,
                [this, getScene, level](int index)
                {
                    if (Ref<Scene> s = getScene())
                    {
                        s->SetSubLevelAlwaysLoaded(level, index == 0);
                        Changed();
                    }
                });
            loading->Tooltip = "Always Loaded: in from the start of Play, with the persistent level. Streamed: loaded by Level Streaming "
                               "Volumes naming it, or a script (Scene.LoadLevel).";
            loading->SetEnabled(editing);
            row->AddChild(FixedWidth(loading, 130.0f));

            Ref<NoxUI::UIButton> placement(new NoxUI::UIButton("as built"));
            NoxUI::UIButton* placementRaw = placement.get();
            placement->OnClick = [this, level, placementRaw]() { ShowPlacement(level, placementRaw->GetBounds()); };
            placement->Tooltip = "The level transform: moves and turns the whole level (all its entities) at once. Its file keeps it where "
                                 "it was built; the offset is applied when it is read and taken out when it is saved.";
            row->AddChild(FixedWidth(placement, 150.0f));

            Ref<NoxUI::UIButton> remove(new NoxUI::UIButton("Remove", [this, getScene, level]()
            {
                const auto removeLevel = [this, getScene, level]()
                {
                    if (Ref<Scene> s = getScene())
                    {
                        s->RemoveSubLevel(level);
                        Changed();
                    }
                };
                if (!m_Hooks.AskSave)
                {
                    removeLevel();
                    return;
                }
                // Save / Don't Save / Cancel, as every other question that can drop edits.
                m_Hooks.AskSave("Remove " + LevelName(level) + " from this scene? Its file stays; changes to it not saved yet are lost unless "
                                "saved now.",
                    [this, removeLevel]() { if (m_Hooks.Save) m_Hooks.Save(); removeLevel(); },
                    removeLevel);
            }));
            remove->SetEnabled(editing);
            row->AddChild(FixedWidth(remove, 70.0f));
            m_Body->AddChild(row);

            NoxUI::AppendSync(*row, [=]() // (on the row: what it updates is inside it)
            {
                const Scene::SubLevel* now = find();
                if (!now)
                    return;
                const Ref<Scene> s = getScene();
                current->Checked = s && s->GetCurrentLevel() == level;
                current->SetEnabled(editing && now->Loaded && now->Visible && !now->Locked);
                if (!NoxUI::HasState(visible->GetState(), NoxUI::UIState::Pressed))
                    visible->Checked = now->Visible;
                if (!NoxUI::HasState(locked->GetState(), NoxUI::UIState::Pressed))
                    locked->Checked = now->Locked;
                if (loading->GetSelectedIndex() != (now->AlwaysLoaded ? 0 : 1))
                    loading->SetSelectedIndex(now->AlwaysLoaded ? 0 : 1, false);
                std::string text = LevelName(level);
                if (editing && !now->Loaded)
                    text += "  (not read: see the log; its file is not written)";
                if (!editing && s)
                    text += std::string("  (") + StateText(s->GetLevelState(level)) + ")";
                if (nameRef->Text != text)
                    nameRef->Text = text;
                const double meter = static_cast<double>(WorldUnits::FromMeters(1.0f));
                const std::string place = now->HasTransform()
                    ? std::format("{:.1f}, {:.1f}, {:.1f} m  {:.0f} deg", now->Offset.x / meter, now->Offset.y / meter, now->Offset.z / meter, now->Yaw * kRadiansToDegrees)
                    : std::string("as built");
                if (placement->GetText() != place)
                    placement->SetText(place);
                placement->SetEnabled(editing && !now->Locked);
            });
        }

        if (!editing)
        {
            auto note = Muted("Play / Simulate: Always Loaded levels are in from the start, the others follow their volumes and scripts.");
            note->WordWrap = true;
            note->WidthMode = NoxUI::UISizeMode::Fill;
            m_Body->AddChild(note);
            return;
        }

        // Actions.
        auto actions = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 6.0f);
        actions->WidthMode = NoxUI::UISizeMode::Fill;
        actions->Margin = NoxUI::UIThickness{ 0.0f, 8.0f, 0.0f, 0.0f };
        Ref<NoxUI::UIButton> addExisting(new NoxUI::UIButton("Add Existing Level..."));
        NoxUI::UIButton* addRaw = addExisting.get();
        addExisting->OnClick = [this, getScene, addRaw]()
        {
            const Ref<Scene> s = getScene();
            auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr;
            if (!s || !assets || !m_Context)
                return;
            const AssetHandle openScene = m_Hooks.OpenScene ? m_Hooks.OpenScene() : AssetHandle(0);
            std::vector<NoxUI::UIMenuItem> items;
            for (const auto& [handle, metadata] : assets->GetAssetRegistry())
            {
                if (metadata.Type != AssetType::Scene || handle == openScene || s->FindSubLevel(handle))
                    continue;
                const AssetHandle h = handle;
                const std::string stem = metadata.FilePath.stem().string();
                items.push_back({ stem, "", [this, getScene, h, stem]()
                {
                    const Ref<Scene> target = getScene();
                    const bool added = target && target->AddSubLevel(h);
                    if (m_Message)
                        m_Message->Text = added ? "Added " + stem : "Could not add " + stem + " (see the log)";
                    if (added)
                        Changed();
                }, false, true, false, false, metadata.FilePath.generic_string() });
            }
            if (items.empty())
                items.push_back({ "No other scenes in the project", "", {}, false, false, false, true });
            NoxUI::UIMenuPopup::ShowPopupMenu(*m_Context, addRaw->GetBounds(), std::move(items));
        };
        addExisting->Tooltip = "A scene made on its own, edited here together with this one (it keeps its own file)";
        actions->AddChild(addExisting);

        Ref<NoxUI::UIButton> newLevel(new NoxUI::UIButton("New Level...", [this]() { ShowNewLevelDialog(); }));
        newLevel->Tooltip = "Makes an empty scene file next to the open scene, adds it as a sub-level and makes it the current level";
        actions->AddChild(newLevel);

        Ref<NoxUI::UIButton> move(new NoxUI::UIButton("Move Selection to Current", [this, getScene]()
        {
            const Ref<Scene> s = getScene();
            const std::vector<Entity> selection = m_Hooks.Selection ? m_Hooks.Selection() : std::vector<Entity>{};
            if (!s || selection.empty())
                return;
            s->MoveToLevel(selection, s->GetCurrentLevel());
            Changed();
        }));
        actions->AddChild(move);
        NoxUI::AppendSync(*actions, [this, move, getScene]()
        {
            const bool any = m_Hooks.Selection && !m_Hooks.Selection().empty();
            move->SetEnabled(any);
            const Ref<Scene> s = getScene();
            const AssetHandle current = s ? s->GetCurrentLevel() : AssetHandle(0);
            move->Tooltip = "The selected entities (with their parents and children: a tree stays in one level) into the current level (" +
                            (current != 0 ? LevelName(current) : std::string("the persistent level")) + ")";
        });
        m_Body->AddChild(actions);

        auto unsaved = Muted("");
        Ref<NoxUI::UILabel> unsavedRef = unsaved;
        unsavedRef->UseColorOverride = true;
        unsavedRef->ColorOverride = NoxUI::ThemeColor("StatusWarning", { 1.0f, 0.75f, 0.3f, 1.0f });
        m_Body->AddChild(unsavedRef);
        NoxUI::AppendSync(*unsavedRef, [this, unsavedRef = unsavedRef.get(), getScene]()
        {
            const Ref<Scene> s = getScene();
            const bool warn = s && !s->GetSubLevels().empty() && (!m_Hooks.OpenScene || m_Hooks.OpenScene() == 0);
            unsavedRef->Text = warn ? "The scene was never saved: save it to keep its sub-level list." : "";
            unsavedRef->Visible = warn;
        });
        m_Message = Muted("");
        m_Body->AddChild(m_Message);
    }

    void LevelsPanel::ShowPlacement(AssetHandle level, const NoxUI::UIRect& anchor)
    {
        const Ref<Scene> scene = m_Hooks.ActiveScene ? m_Hooks.ActiveScene() : nullptr;
        const Scene::SubLevel* subLevel = scene ? scene->FindSubLevel(level) : nullptr;
        if (!subLevel || !m_Context)
            return;
        std::function<Ref<Scene>()> getScene = m_Hooks.ActiveScene;
        auto box = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 4.0f);
        box->Padding = NoxUI::UIThickness::All(10.0f);
        box->FixedSize = { 300.0f, 0.0f };
        box->AddChild(NoxUI::Label(LevelName(level), 14.0f));
        const double meter = static_cast<double>(WorldUnits::FromMeters(1.0f));
        // Each field writes its part of the transform, reading the others from the scene (so the three axes and the yaw stay one).
        const auto apply = [this, getScene, level, meter](int axis, float value)
        {
            const Ref<Scene> s = getScene();
            const Scene::SubLevel* now = s ? s->FindSubLevel(level) : nullptr;
            if (!now)
                return;
            glm::dvec3 offset = now->Offset;
            float yaw = now->Yaw;
            if (axis < 3)
                offset[axis] = static_cast<double>(value) * meter;
            else
                yaw = value / kRadiansToDegrees;
            s->SetSubLevelTransform(level, offset, yaw);
            Changed();
        };
        const char* labels[] = { "Offset X (m)", "Offset Y (m)", "Offset Z (m)", "Yaw (deg)" };
        for (int axis = 0; axis < 4; ++axis)
        {
            auto row = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 6.0f);
            row->WidthMode = NoxUI::UISizeMode::Fill;
            row->AddChild(FixedWidth(Muted(labels[axis]), 100.0f));
            const float value = axis < 3 ? static_cast<float>(subLevel->Offset[axis] / meter) : subLevel->Yaw * kRadiansToDegrees;
            auto field = NoxUI::MakeUI<NoxUI::UIDragScalar>(value, axis < 3 ? 0.05f : 0.5f, [apply, axis](float v) { apply(axis, v); });
            field->WidthMode = NoxUI::UISizeMode::Fill;
            field->Format = axis < 3 ? "{:.2f}" : "{:.1f}";
            if (axis == 3)
            {
                field->Min = -360.0f;
                field->Max = 360.0f;
            }
            row->AddChild(field);
            box->AddChild(row);
        }
        Ref<NoxUI::UIButton> reset(new NoxUI::UIButton("Reset", [this, getScene, level]()
        {
            if (Ref<Scene> s = getScene())
            {
                s->SetSubLevelTransform(level, glm::dvec3(0.0), 0.0f);
                Changed();
            }
            if (m_Context)
                m_Context->ClearOverlay();
        }));
        reset->Tooltip = "Back to where it was built";
        reset->HAlign = NoxUI::UIAlignment::Start;
        box->AddChild(reset);
        NoxUI::UIPopup::Show(*m_Context, anchor, box);
    }

    void LevelsPanel::ShowNewLevelDialog()
    {
        if (!m_Context)
            return;
        const AssetHandle openScene = m_Hooks.OpenScene ? m_Hooks.OpenScene() : AssetHandle(0);
        const std::filesystem::path folder = openScene != 0 ? AssetManager::GetMetadata(openScene).FilePath.parent_path() : std::filesystem::path();
        auto content = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 6.0f);
        content->WidthMode = NoxUI::UISizeMode::Fill;
        content->AddChild(NoxUI::Label("A new empty level in " + (folder.empty() ? std::string("the assets folder") : "Assets/" + folder.generic_string()), 14.0f));
        auto name = NoxUI::MakeUI<NoxUI::UITextInput>();
        name->Text = "NewLevel";
        name->WidthMode = NoxUI::UISizeMode::Fill;
        content->AddChild(name);
        auto error = Muted("");
        Ref<NoxUI::UILabel> errorRef = error;
        errorRef->UseColorOverride = true;
        errorRef->ColorOverride = NoxUI::ThemeColor("StatusError", { 1.0f, 0.6f, 0.3f, 1.0f });
        errorRef->WordWrap = true;
        errorRef->WidthMode = NoxUI::UISizeMode::Fill;
        content->AddChild(errorRef);
        NoxUI::UITextInput* nameRaw = name.get();
        const auto create = [this, nameRaw, errorRef]()
        {
            const Ref<Scene> s = m_Hooks.ActiveScene ? m_Hooks.ActiveScene() : nullptr;
            std::string message;
            if (s && CreateLevel(*s, nameRaw->Text, message))
            {
                if (m_Message)
                    m_Message->Text = message;
                Changed();
                if (m_Context)
                    m_Context->ClearOverlay();
                return;
            }
            errorRef->Text = message; // (the dialog stays: fix the name)
        };
        name->OnSubmit = [create](const std::string&) { create(); };
        // Create keeps the dialog open on an error (a modal button closes it): the field's Enter and this button run the same check.
        Ref<NoxUI::UIButton> createButton(new NoxUI::UIButton("Create", create));
        createButton->StyleVariant = "primary";
        createButton->HAlign = NoxUI::UIAlignment::Start;
        content->AddChild(createButton);
        NoxUI::UIModal::ShowCustom(*m_Context, "New Level", content, { { "Cancel", {} } }, 440.0f);
        m_Context->SetFocus(nameRaw);
    }

    bool LevelsPanel::CreateLevel(Scene& scene, const std::string& name, std::string& message)
    {
        if (name.empty() || name.find_first_of("/\\:*?\"<>|") != std::string::npos)
        {
            message = "Give it a name without / \\ : * ? \" < > |";
            return false;
        }
        const AssetHandle openScene = m_Hooks.OpenScene ? m_Hooks.OpenScene() : AssetHandle(0);
        const std::filesystem::path folder = openScene != 0 ? AssetManager::GetMetadata(openScene).FilePath.parent_path() : std::filesystem::path();
        const std::filesystem::path relative = (folder / (name + ".nox")).lexically_normal();
        const std::filesystem::path path = Project::GetActiveAssetDirectory() / relative;
        if (std::filesystem::exists(path))
        {
            message = relative.generic_string() + " exists already (use Add Existing Level)";
            return false;
        }
        // Written by the scene serializer (the old panel wrote the file's text by hand, a second copy of the format).
        SceneSerializer(CreateRef<Scene>()).Serialize(path);
        if (!std::filesystem::exists(path))
        {
            message = path.generic_string() + " could not be written";
            return false;
        }
        EditorAssetManager& assets = *Project::GetActive()->GetEditorAssetManager();
        AssetHandle handle = assets.FindHandleByPath(relative, AssetType::Scene);
        if (handle == 0)
            handle = assets.RegisterExistingFile(relative, AssetType::Scene);
        if (handle == 0 || !scene.AddSubLevel(handle))
        {
            message = relative.generic_string() + " was written but could not be added (see the log)";
            return false;
        }
        scene.SetCurrentLevel(handle);
        message = "Created " + relative.generic_string() + " (the current level now)";
        NOX_CORE_INFO("New level '{}'", relative.generic_string());
        return true;
    }
}
