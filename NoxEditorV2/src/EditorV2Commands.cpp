// EditorV2Layer: its commands (what the menus and the keys run), the key dispatch and the Keyboard Shortcuts window (E4).
#include "EditorV2Layer.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <NoxUI/Backends/noxui_impl_sdl3.h>

#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Input.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/HLODBuilder.h"
#include "NoxCore/Scripting/ScriptEngine.h"
#include <NoxUI/Core/UIBuilder.h>
#include <NoxUI/Core/UIContext.h>
#include "NoxCore/UI/Core/UILayer.h"
#include <NoxUI/Layout/UIDockSpace.h>
#include <NoxUI/Layout/UILayout.h>
#include <NoxUI/Layout/UIWindow.h>
#include <NoxUI/Widgets/UIButton.h>
#include <NoxUI/Widgets/UIImage.h>
#include <NoxUI/Widgets/UILabel.h>
#include <NoxUI/Widgets/UIScrollView.h>
#include <NoxUI/Widgets/UITextInput.h>

namespace Nox
{
    namespace
    {
        constexpr const char* kShortcutsTitle = "Keyboard Shortcuts";

        KeyChord Chord(int scancode, bool ctrl = false, bool shift = false, bool alt = false)
        {
            return KeyChord{ NoxUI::Detail::TranslateSDLScancode(static_cast<SDL_Scancode>(scancode)), ctrl, shift, alt };
        }

        bool IsModifierKey(int scancode)
        {
            return scancode == SDL_SCANCODE_LCTRL || scancode == SDL_SCANCODE_RCTRL || scancode == SDL_SCANCODE_LSHIFT ||
                   scancode == SDL_SCANCODE_RSHIFT || scancode == SDL_SCANCODE_LALT || scancode == SDL_SCANCODE_RALT ||
                   scancode == SDL_SCANCODE_LGUI || scancode == SDL_SCANCODE_RGUI;
        }

        // The pressed key as a chord. Letters follow the keyboard layout (Ctrl+Z is the key labelled Z on an AZERTY keyboard too).
        KeyChord ChordOf(const KeyPressedEvent& key)
        {
            int scancode = key.GetKeyCode();
            const SDL_Keycode layoutKey = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(scancode), SDL_KMOD_NONE, false);
            if (layoutKey >= SDLK_A && layoutKey <= SDLK_Z)
                scancode = SDL_SCANCODE_A + static_cast<int>(layoutKey - SDLK_A);
            const auto mods = key.GetModifiers();
            KeyChord chord;
            chord.Key = NoxUI::Detail::TranslateSDLScancode(static_cast<SDL_Scancode>(scancode));
            chord.Ctrl = (mods & SDL_KMOD_CTRL) != 0 || Input::IsKeyPressed(SDL_SCANCODE_LCTRL) || Input::IsKeyPressed(SDL_SCANCODE_RCTRL);
            chord.Shift = (mods & SDL_KMOD_SHIFT) != 0 || Input::IsKeyPressed(SDL_SCANCODE_LSHIFT) || Input::IsKeyPressed(SDL_SCANCODE_RSHIFT);
            chord.Alt = (mods & SDL_KMOD_ALT) != 0 || Input::IsKeyPressed(SDL_SCANCODE_LALT) || Input::IsKeyPressed(SDL_SCANCODE_RALT);
            return chord;
        }
    }

    std::filesystem::path EditorV2Layer::KeyBindingsPath() const
    {
        if (!Project::GetActive())
            return {};
        return Project::GetProjectDirectory() / "Saved" / "Config" / "EditorKeyBindings.txt";
    }

    void EditorV2Layer::RegisterCommands()
    {
        const auto editing = [this]() { return m_SceneState == SceneState::Edit; };
        auto add = [this](const char* id, const char* label, const char* category, CommandScope scope, KeyChord key,
                          std::function<void()> run, std::function<bool()> canRun = {}, const char* description = "", KeyChord altKey = {})
        {
            EditorCommand command;
            command.Id = id;
            command.Label = label;
            command.Category = category;
            command.Description = description;
            command.Scope = static_cast<uint32_t>(scope);
            command.DefaultKey = key;
            command.DefaultAltKey = altKey;
            command.Execute = std::move(run);
            command.CanExecute = std::move(canRun);
            m_Commands.Add(std::move(command));
        };

        // File (anywhere: also while a Content Browser or graph window has the keyboard -- a graph window takes Ctrl+S itself).
        add("File.NewScene", "New Scene", "File", CommandScope::Global, Chord(SDL_SCANCODE_N, true),
            [this]() { ConfirmUnsavedChanges("making a new scene", [this]() { NewScene(); }, UnsavedScene); });
        add("File.OpenProject", "Open Project...", "File", CommandScope::Global, Chord(SDL_SCANCODE_O, true),
            [this]() { ConfirmUnsavedChanges("opening another project", [this]() { OpenProjectAsync(); }); });
        add("File.SaveScene", "Save Scene", "File", CommandScope::Global, Chord(SDL_SCANCODE_S, true), [this]() { SaveScene(); });
        add("File.SaveSceneAs", "Save Scene As...", "File", CommandScope::Global, Chord(SDL_SCANCODE_S, true, true), [this]() { SaveSceneAs(); });
        add("File.SaveAll", "Save All", "File", CommandScope::Global, Chord(SDL_SCANCODE_S, true, false, true), [this]() { SaveAll(); }, {},
            "The scene, every asset editor's unsaved edits, the materials edited in the Inspector and the mixer.");

        // Edit (the level editor's keys: not while the Content Browser, the Output Log or an asset editor has the keyboard).
        add("Edit.Undo", "Undo", "Edit", CommandScope::Level, Chord(SDL_SCANCODE_Z, true), [this]() { Undo(); },
            [this, editing]() { return editing() && m_History.CanUndo() && !m_HLODBuild; });
        add("Edit.Redo", "Redo", "Edit", CommandScope::Level, Chord(SDL_SCANCODE_Y, true), [this]() { Redo(); },
            [this, editing]() { return editing() && m_History.CanRedo() && !m_HLODBuild; }, "", Chord(SDL_SCANCODE_Z, true, true));
        add("Edit.Duplicate", "Duplicate", "Edit", CommandScope::Level, Chord(SDL_SCANCODE_D, true),
            [this]() { m_SceneHierarchyPanel.DuplicateSelection(); }, [this, editing]() { return editing() && !m_Selection.IsEmpty(); },
            "Copies of the selected entities, selected.");
        add("Edit.Delete", "Delete", "Edit", CommandScope::Level, Chord(SDL_SCANCODE_DELETE),
            [this]() { m_SceneHierarchyPanel.DeleteSelection(); }, [this, editing]() { return editing() && !m_Selection.IsEmpty(); },
            "Deletes the selected entities.");
        add("Edit.FindInHierarchy", "Search the Hierarchy", "Edit", CommandScope::Level, Chord(SDL_SCANCODE_F, true),
            [this]() { m_SceneHierarchyPanel.FocusSearch(); }, [this]() { return m_ShowSceneHierarchy; });
        add("Edit.SnapToFloor", "Snap Selection to Floor", "Edit", CommandScope::Level, Chord(SDL_SCANCODE_END),
            [this]() { SnapSelectionToFloor(); }, [this, editing]() { return editing() && !m_Selection.IsEmpty(); });
        add("Edit.KeyboardShortcuts", "Keyboard Shortcuts...", "Edit", CommandScope::Global, {}, [this]() { OpenKeyboardShortcuts(); },
            {}, "Every command and its keys; click a key to change it.");

        // Viewport.
        add("Viewport.FocusSelection", "Focus Selection", "Viewport", CommandScope::Level, Chord(SDL_SCANCODE_F),
            [this]() { FocusOnEntity(m_Selection.GetPrimary()); }, [this, editing]() { return editing() && m_Selection.GetPrimary(); },
            "The camera frames the selected entity.");
        add("Viewport.MeasureTool", "Measure Tool", "Viewport", CommandScope::Viewport, Chord(SDL_SCANCODE_M),
            [this]()
            {
                m_ViewportTools.MeasureTool = !m_ViewportTools.MeasureTool;
                if (!m_ViewportTools.MeasureTool)
                    m_ViewportTools.MeasureStage = 0;
                m_ViewportToolbar.Sync();
            },
            editing, "Click two points in the viewport to measure the distance (Esc ends it).");
        add("Viewport.CancelMeasure", "End the Measure Tool", "Viewport", CommandScope::Level, Chord(SDL_SCANCODE_ESCAPE),
            [this]()
            {
                if (m_ViewportTools.MeasureStage != 0)
                    m_ViewportTools.MeasureStage = 0;
                else
                    m_ViewportTools.MeasureTool = false;
                m_ViewportToolbar.Sync();
            },
            [this]() { return m_ViewportTools.MeasureTool; });
        struct ViewKey { const char* Id; const char* Label; int Key; bool Ctrl; EditorViewMode Mode; };
        const ViewKey views[] = {
            { "Viewport.ViewFront", "Front View", SDL_SCANCODE_KP_1, false, EditorViewMode::Front },
            { "Viewport.ViewBack", "Back View", SDL_SCANCODE_KP_1, true, EditorViewMode::Back },
            { "Viewport.ViewRight", "Right View", SDL_SCANCODE_KP_3, false, EditorViewMode::Right },
            { "Viewport.ViewLeft", "Left View", SDL_SCANCODE_KP_3, true, EditorViewMode::Left },
            { "Viewport.ViewTop", "Top View", SDL_SCANCODE_KP_7, false, EditorViewMode::Top },
            { "Viewport.ViewBottom", "Bottom View", SDL_SCANCODE_KP_7, true, EditorViewMode::Bottom },
            { "Viewport.ViewPerspective", "Perspective", SDL_SCANCODE_KP_5, false, EditorViewMode::Perspective },
        };
        for (const ViewKey& view : views)
        {
            const EditorViewMode mode = view.Mode;
            add(view.Id, view.Label, "Viewport", CommandScope::Viewport, Chord(view.Key, view.Ctrl), [this, mode]() { SetViewMode(mode); });
        }

        // Play.
        add("Play.Stop", "Stop", "Play", CommandScope::Level, Chord(SDL_SCANCODE_ESCAPE), [this]() { OnSceneStop(); },
            [this]() { return m_SceneState != SceneState::Edit; });

        // Window.
        add("Window.ContentDrawer", "Content Drawer", "Window", CommandScope::Global, Chord(SDL_SCANCODE_SPACE, true),
            [this]() { ToggleDrawer(DrawerPage::Content); }, {}, "Opens or closes the Content Browser drawer over the viewport.");
        add("Window.CloseDrawer", "Close the Drawer", "Window", CommandScope::Global, Chord(SDL_SCANCODE_ESCAPE), [this]() { CloseDrawer(); },
            [this]() { return IsDrawerOpen(); });
        add("Window.OutputLogCommand", "Output Log Command Line", "Window", CommandScope::Global, Chord(SDL_SCANCODE_GRAVE),
            [this]() { ShowOutputLog(); m_OutputLogPanel.FocusCommandLine(); }, {}, "Shows the Output Log and puts the cursor in its Cmd line.");

        // Script.
        add("Script.Build", "Build and Reload", "Script", CommandScope::Global, Chord(SDL_SCANCODE_B, true),
            [this]() { m_ScriptBuilder.RequestBuild(); }, [this]() { return !m_ScriptBuilder.IsBuilding(); });
        add("Script.ReloadAssembly", "Reload Assembly", "Script", CommandScope::Global, Chord(SDL_SCANCODE_R, true),
            []() { ScriptEngine::ReloadAssembly(); }, []() { return ScriptEngine::IsInitialized(); });

        m_Commands.Load(KeyBindingsPath());
    }

    bool EditorV2Layer::IsLevelFocus() const
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer)
            return false;
        const NoxUI::UIElement* focused = uiLayer->GetContext().GetFocused();
        if (!focused)
            return true;
        // The level editor's panels; anything else (the Content Browser, the Output Log, an asset editor, a panel ported later)
        // keeps the level's keys out -- a new panel is safe without being added to a list.
        const NoxUI::UIElement* levelRoots[] = {
            m_ViewportDockSpace.get(), m_SceneHierarchyPanel.GetRoot().get(), m_InspectorPanel.GetRoot().get(), m_ToolBar.get()
        };
        for (const NoxUI::UIElement* cur = focused; cur; cur = cur->GetParent())
            for (const NoxUI::UIElement* root : levelRoots)
                if (root && cur == root)
                    return true;
        return false;
    }

    bool EditorV2Layer::DispatchCommandKey(KeyPressedEvent& key)
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer)
            return false; // (no UI: nothing dispatched here)
        NoxUI::UIContext& ui = uiLayer->GetContext();

        // A key for the Keyboard Shortcuts window being assigned.
        if (!m_CaptureCommand.empty())
        {
            if (IsModifierKey(key.GetKeyCode()) || key.IsRepeat())
                return true;
            const std::string id = m_CaptureCommand;
            m_CaptureCommand.clear();
            const KeyChord chord = ChordOf(key);
            if (chord.Key != NoxUI::UIKey::Escape || chord.Ctrl || chord.Shift || chord.Alt)
            {
                const bool clear = chord.Key == NoxUI::UIKey::Backspace && !chord.Ctrl && !chord.Shift && !chord.Alt;
                const std::string taken = m_Commands.Bind(id, clear ? KeyChord{} : chord, m_CapturePrimary);
                m_Commands.Save(KeyBindingsPath());
                if (!taken.empty())
                    if (const EditorCommand* other = m_Commands.Find(taken))
                        Notify(NoxUI::UINotificationSeverity::Warning, chord.ToString() + " moved",
                               "It was the key of \"" + other->Label + "\", which has no key now.");
            }
            RebuildKeyboardShortcuts();
            return true;
        }

        // The focused widget and its parents first: a text field types, a panel takes its own keys (Delete in the Content Browser
        // deletes assets), a graph window takes Ctrl+Z. What none of them takes goes to the commands.
        // (Dispatched here, so the UI layer after this one must not get it again: the key counts as handled either way.)
        const bool widgetTook = ui.OnKeyDown(
            NoxUI::Detail::TranslateSDLScancode(static_cast<SDL_Scancode>(key.GetKeyCode())),
            NoxUI::Detail::TranslateSDLMods(static_cast<SDL_Keymod>(key.GetModifiers())),
            key.IsRepeat());
        key.Handled = true;
        if (widgetTook || key.IsRepeat() || IsModifierKey(key.GetKeyCode()))
            return true;
        // An open menu, popup or dialog has the keyboard: no command runs behind it (Delete would delete entities under a dialog).
        if (ui.GetOverlay())
            return true;

        const NoxUI::UIElement* viewport = m_ViewportCanvas ? static_cast<const NoxUI::UIElement*>(m_ViewportCanvas.get())
                                                         : static_cast<const NoxUI::UIElement*>(m_ViewportImage.get());
        EditorCommandList::KeyContext context;
        context.TextInputFocused = dynamic_cast<const NoxUI::UITextInput*>(ui.GetFocused()) != nullptr;
        context.LevelFocus = IsLevelFocus();
        context.ViewportHovered = viewport && NoxUI::HasState(viewport->GetState(), NoxUI::UIState::Hovered);
        if (const EditorCommand* command = m_Commands.FindForKey(ChordOf(key), context))
        {
            const std::function<void()> run = command->Execute;
            run();
        }
        return true;
    }

    void EditorV2Layer::OpenKeyboardShortcuts()
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer || !uiLayer->GetDockSpaceRef())
            return;
        const auto& dock = uiLayer->GetDockSpaceRef();
        if (const Ref<NoxUI::UIWindow> window = dock->FindFloatingWindow(kShortcutsTitle))
        {
            if (window->OnActivate)
                window->OnActivate();
            return;
        }
        if (dock->HasPanel(kShortcutsTitle))
            return;

        auto root = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 6.0f);
        root->WidthMode = NoxUI::UISizeMode::Fill;
        root->HeightMode = NoxUI::UISizeMode::Fill;
        root->StyleClass = "Surface";
        root->DrawBackground = true;
        root->Padding = NoxUI::UIThickness::All(10.0f);

        auto intro = NoxUI::Label("Click a key to change it, then press the new key (Esc: keep it, Backspace: no key). A key another "
                               "command used moves here. Saved for this project in Saved/Config.", 13.0f);
        intro->WordWrap = true;
        intro->WidthMode = NoxUI::UISizeMode::Fill;
        intro->UseColorOverride = true;
        intro->ColorOverride = NoxUI::ThemeColor("TextMuted", { 0.60f, 0.64f, 0.72f, 1.0f });
        root->AddChild(intro);

        Ref<NoxUI::UIButton> resetAll(new NoxUI::UIButton("Reset All to Defaults", [this]()
        {
            m_CaptureCommand.clear();
            m_Commands.ResetAll();
            m_Commands.Save(KeyBindingsPath());
            RebuildKeyboardShortcuts();
        }));
        resetAll->HAlign = NoxUI::UIAlignment::Start;
        root->AddChild(resetAll);

        auto scroll = NoxUI::MakeUI<NoxUI::UIScrollView>();
        scroll->WidthMode = NoxUI::UISizeMode::Fill;
        scroll->HeightMode = NoxUI::UISizeMode::Fill;
        root->AddChild(scroll);
        m_ShortcutsList = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 2.0f);
        m_ShortcutsList->WidthMode = NoxUI::UISizeMode::Fill;
        scroll->AddChild(m_ShortcutsList);
        RebuildKeyboardShortcuts();

        dock->FloatPanel(kShortcutsTitle, root, { 300.0f, 120.0f }, { 620.0f, 640.0f }, false);
    }

    void EditorV2Layer::RebuildKeyboardShortcuts()
    {
        if (!m_ShortcutsList)
            return;
        m_ShortcutsList->ClearChildren();
        std::string category;
        for (const EditorCommand& command : m_Commands.GetCommands())
        {
            if (command.Category != category)
            {
                category = command.Category;
                auto header = NoxUI::Label(category, 14.5f);
                header->Margin = NoxUI::UIThickness{ 0.0f, 8.0f, 0.0f, 2.0f };
                m_ShortcutsList->AddChild(header);
            }
            auto row = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 6.0f);
            row->WidthMode = NoxUI::UISizeMode::Fill;

            auto label = NoxUI::Label(command.Label, 13.5f);
            label->WidthMode = NoxUI::UISizeMode::Fill;
            label->VAlign = NoxUI::UIAlignment::Center;
            const char* scope = static_cast<CommandScope>(command.Scope) == CommandScope::Global ? "anywhere in the editor"
                              : static_cast<CommandScope>(command.Scope) == CommandScope::Viewport ? "the pointer over a viewport, the level editor has the keyboard"
                                                                        : "the level editor (viewport, Scene Hierarchy, Inspector) has the keyboard";
            label->Tooltip = (command.Description.empty() ? std::string() : command.Description + "\n") + "Works when: " + scope;
            row->AddChild(label);

            const std::string id = command.Id;
            for (int slot = 0; slot < 2; ++slot)
            {
                const bool primary = slot == 0;
                const bool capturing = m_CaptureCommand == id && m_CapturePrimary == primary;
                const KeyChord& chord = primary ? command.Key : command.AltKey;
                Ref<NoxUI::UIButton> keyButton(new NoxUI::UIButton(capturing ? "Press a key..." : chord.IsSet() ? chord.ToString() : "-",
                                                             [this, id, primary]()
                {
                    m_CaptureCommand = id;
                    m_CapturePrimary = primary;
                    RebuildKeyboardShortcuts();
                }));
                keyButton->WidthMode = NoxUI::UISizeMode::Fixed;
                keyButton->FixedSize = { 150.0f, 24.0f };
                keyButton->StyleVariant = capturing ? "primary" : "";
                keyButton->Tooltip = primary ? "The key (click, then press the new one)" : "A second key (click, then press it)";
                row->AddChild(keyButton);
            }
            const bool isDefault = command.Key == command.DefaultKey && command.AltKey == command.DefaultAltKey;
            Ref<NoxUI::UIButton> reset(new NoxUI::UIButton("Reset", [this, id]()
            {
                m_CaptureCommand.clear();
                m_Commands.ResetToDefault(id);
                m_Commands.Save(KeyBindingsPath());
                RebuildKeyboardShortcuts();
            }));
            reset->SetEnabled(!isDefault);
            reset->Tooltip = "Back to " + (command.DefaultKey.IsSet() ? command.DefaultKey.ToString() : std::string("no key"));
            row->AddChild(reset);
            m_ShortcutsList->AddChild(row);
        }
    }
}
