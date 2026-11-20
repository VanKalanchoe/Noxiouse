#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/UI/Core/UITypes.h"

namespace Nox
{
    namespace UI
    {
        class UIDockSpace;
        class UIElement;
    }

    // One asset open in its own editor window (Unreal's FAssetEditorToolkit): a graph, a material ... The window around it, the
    // "Save changes?" question when it closes and the editor's end belong to AssetEditorManager.
    class AssetEditorV2
    {
    public:
        virtual ~AssetEditorV2() = default;

        virtual AssetHandle GetHandle() const = 0;
        // The asset's name ("UAL_Locomotion") and what it is, for questions ("graph", "material").
        virtual const std::string& GetName() const = 0;
        virtual const char* GetKind() const = 0;
        // The window's title ("Graph: UAL_Locomotion"); one window per asset.
        virtual std::string GetWindowTitle() const = 0;
        virtual UI::UIVec2 GetDefaultSize() const { return { 900.0f, 600.0f }; }

        // False: the asset could not be loaded (no window is made).
        virtual bool IsValid() const { return true; }
        virtual Ref<UI::UIElement> BuildUI() = 0;
        // Once a frame while the window is open.
        virtual void Update() {}

        // Unsaved edits: asked about when the window closes or the editor would drop them.
        virtual bool IsDirty() const { return false; }
        // Writes the asset (IsDirty() then says whether it worked).
        virtual void Save() {}
        // Drops the unsaved edits (the asset is the file's again); false when that failed (the edits stay).
        virtual bool RevertToDisk() { return true; }

        // An editor that poses the scene to preview (a timeline): while it does, the scene's undo history waits (the poses are not
        // edits), and saving the scene or starting Play ends the preview first (what the targets had comes back).
        virtual bool IsPreviewing() const { return false; }
        virtual void EndPreview() {}

        // Set by the manager: toasts, and opening another asset (a material's texture).
        std::function<void(UI::UINotification)> Notify;
        std::function<void(AssetHandle)> OpenAsset;
    };

    // Which editor opens an asset type (Unreal's asset type actions): a type registers a factory; the Content Browser and the
    // Inspector open assets through it, and the editor layer has no switch over asset types.
    class AssetEditorRegistry
    {
    public:
        using Factory = std::function<std::unique_ptr<AssetEditorV2>(AssetHandle)>;
        void Register(AssetType type, Factory factory) { m_Factories[type] = std::move(factory); }
        const Factory* Find(AssetType type) const
        {
            const auto found = m_Factories.find(type);
            return found == m_Factories.end() ? nullptr : &found->second;
        }

    private:
        std::unordered_map<AssetType, Factory> m_Factories;
    };

    // The open asset editors (Unreal's UAssetEditorSubsystem): opens one window per asset (or raises it), runs them each frame,
    // asks before a window with unsaved edits closes (Save / Don't Save / Cancel), and ends an editor when its window is gone.
    class AssetEditorManager
    {
    public:
        struct Hooks
        {
            std::function<void(UI::UINotification)> Notify;
            // The "Save changes?" dialog: its text, then Save and Don't Save (Cancel does nothing).
            std::function<void(const std::string& text, std::function<void()> save, std::function<void()> dontSave)> AskSave;
            std::function<void(AssetHandle)> OpenAsset;
        };

        AssetEditorRegistry& GetRegistry() { return m_Registry; }
        void SetHooks(Hooks hooks) { m_Hooks = std::move(hooks); }

        bool HasEditorFor(AssetType type) const { return m_Registry.Find(type) != nullptr; }
        // Opens the asset's editor in a floating window, or brings its open window to the front. False: no editor for the type,
        // or the asset could not be loaded (a toast says so).
        bool Open(AssetHandle handle);
        // Once a frame: editors whose window is gone end (unsaved edits dropped, said); the others update.
        void Update();

        // UIDockSpace::CanClosePanel for the editors' windows: false keeps a window with unsaved edits open and asks.
        bool CanClosePanel(const std::string& title);
        bool IsEditorWindow(const std::string& title) const;
        // Every editor window closes now (the project changes, the layout is reset, the editor ends); asked about before.
        void CloseAll();

        // The open editor of the asset (null: none open).
        AssetEditorV2* Find(AssetHandle handle) const;
        bool AnyPreviewing() const;
        void EndPreviews();
        bool HasUnsaved() const;
        // "the graph UAL_Locomotion", one per editor with unsaved edits.
        std::vector<std::string> UnsavedNames() const;
        void SaveAll();
        void RevertAll();

    private:
        struct OpenEditor
        {
            std::unique_ptr<AssetEditorV2> Editor;
            std::string Title;
            Ref<UI::UIElement> Root; // (its syncs are dropped before the editor goes: the window can show for one more frame)
        };
        static void CloseWindow(const std::string& title);
        OpenEditor* FindByTitle(const std::string& title);
        void Say(UI::UINotificationSeverity severity, std::string title, std::string message) const;

        AssetEditorRegistry m_Registry;
        Hooks m_Hooks;
        std::vector<OpenEditor> m_Open;
    };
}
