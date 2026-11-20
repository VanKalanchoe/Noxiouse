#include "AssetEditorV2.h"

#include <algorithm>

#include "Panels/InspectorWidgetsV2.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/UI/Core/UILayer.h"
#include "NoxCore/UI/Core/UIViewportManager.h"
#include "NoxCore/UI/Layout/UIDockSpace.h"
#include "NoxCore/UI/Layout/UITabPanel.h"
#include "NoxCore/UI/Layout/UIWindow.h"

namespace Nox
{
    namespace
    {
        UI::UIDockSpace* Dock()
        {
            UILayer* uiLayer = Application::Get().GetUILayer();
            return uiLayer ? uiLayer->GetDockSpaceRef().get() : nullptr;
        }

        void Raise(UI::UIDockSpace& dock, const std::string& title)
        {
            if (const Ref<UI::UIWindow> window = dock.FindFloatingWindow(title); window && window->OnActivate)
            {
                window->OnActivate();
                return;
            }
            if (Ref<UI::UIDockNode> node = dock.FindNodeByPanelTitle(title); node && node->TabPanel)
                for (size_t i = 0; i < node->TabPanel->GetTabCount(); ++i)
                    if (node->TabPanel->GetTabTitle(i) == title)
                        node->TabPanel->SetActiveTab(i);
        }
    }

    void AssetEditorManager::Say(UI::UINotificationSeverity severity, std::string title, std::string message) const
    {
        if (m_Hooks.Notify)
            m_Hooks.Notify({ severity, std::move(title), std::move(message) });
    }

    AssetEditorManager::OpenEditor* AssetEditorManager::FindByTitle(const std::string& title)
    {
        const auto found = std::find_if(m_Open.begin(), m_Open.end(), [&](const OpenEditor& open) { return open.Title == title; });
        return found == m_Open.end() ? nullptr : &*found;
    }

    bool AssetEditorManager::IsEditorWindow(const std::string& title) const
    {
        return std::any_of(m_Open.begin(), m_Open.end(), [&](const OpenEditor& open) { return open.Title == title; });
    }

    bool AssetEditorManager::Open(AssetHandle handle)
    {
        UI::UIDockSpace* dock = Dock();
        if (!dock || handle == 0 || !AssetManager::IsAssetHandleValid(handle))
            return false;
        for (OpenEditor& open : m_Open)
        {
            if (open.Editor->GetHandle() == handle)
            {
                Raise(*dock, open.Title);
                return true;
            }
        }
        const AssetEditorRegistry::Factory* factory = m_Registry.Find(AssetManager::GetAssetType(handle));
        if (!factory)
            return false;
        std::unique_ptr<AssetEditorV2> editor = (*factory)(handle);
        if (!editor || !editor->IsValid())
        {
            Say(UI::UINotificationSeverity::Error, AssetDisplayName(handle) + " not opened", "It could not be loaded -- see the Output Log.");
            return false;
        }
        editor->Notify = m_Hooks.Notify;
        editor->OpenAsset = m_Hooks.OpenAsset;
        std::string title = editor->GetWindowTitle();
        const Ref<UI::UIElement> root = editor->BuildUI();
        const float offset = static_cast<float>((m_Open.size() % 5) * 28);
        dock->FloatPanel(title, root, { 220.0f + offset, 90.0f + offset }, editor->GetDefaultSize(), false);
        m_Open.push_back({ std::move(editor), std::move(title), root });
        return true;
    }

    void AssetEditorManager::Update()
    {
        if (m_Open.empty())
            return;
        UI::UIDockSpace* dock = Dock();
        // A window open nowhere (not docked, floating or torn off) ends its editor. Closing asked about unsaved edits
        // (CanClosePanel); a window gone another way drops them (the asset is the file's again) and says so. An asset deleted
        // while open ends its editor too.
        std::erase_if(m_Open, [&](OpenEditor& open)
        {
            const bool assetGone = !AssetManager::IsAssetHandleValid(open.Editor->GetHandle());
            if (!assetGone && dock && dock->HasPanel(open.Title))
                return false;
            if (assetGone && dock && !dock->RemovePanel(open.Title))
                if (UILayer* uiLayer = Application::Get().GetUILayer(); uiLayer && uiLayer->GetViewportManager())
                    uiLayer->GetViewportManager()->ClosePanel(open.Title);
            open.Editor->EndPreview();
            if (open.Root)
                open.Root->ClearSyncsRecursive();
            if (open.Editor->IsDirty() && !assetGone)
                Say(UI::UINotificationSeverity::Warning, open.Editor->GetName() + ": unsaved changes dropped",
                    "Its window closed without the Save question; the asset is the file's version again.");
            return true;
        });
        for (OpenEditor& open : m_Open)
            open.Editor->Update();
    }

    void AssetEditorManager::CloseWindow(const std::string& title)
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer)
            return;
        bool closed = false;
        if (const auto& dock = uiLayer->GetDockSpaceRef())
            closed = dock->RemovePanel(title);
        if (!closed)
            if (UI::UIViewportManager* windows = uiLayer->GetViewportManager())
                windows->ClosePanel(title);
    }

    bool AssetEditorManager::CanClosePanel(const std::string& title)
    {
        OpenEditor* open = FindByTitle(title);
        if (!open || !open->Editor->IsDirty())
            return true;
        if (!m_Hooks.AskSave)
            return true;
        // Kept open; the answer closes it (or not). Looked up by title again then: the dialog outlives this call.
        m_Hooks.AskSave("Save changes to the " + std::string(open->Editor->GetKind()) + " " + open->Editor->GetName() + " before closing it?",
            [this, title]()
            {
                OpenEditor* editor = FindByTitle(title);
                if (!editor)
                    return;
                editor->Editor->Save();
                if (!editor->Editor->IsDirty()) // (not written: the window stays, the toast says why)
                    CloseWindow(title);
            },
            [this, title]()
            {
                OpenEditor* editor = FindByTitle(title);
                if (editor && editor->Editor->RevertToDisk())
                    CloseWindow(title);
            });
        return false;
    }

    void AssetEditorManager::CloseAll()
    {
        for (const OpenEditor& open : m_Open)
        {
            open.Editor->EndPreview();
            if (open.Root)
                open.Root->ClearSyncsRecursive();
            CloseWindow(open.Title);
        }
        m_Open.clear(); // (edits not saved by now go back to the files: the caller asked)
    }

    AssetEditorV2* AssetEditorManager::Find(AssetHandle handle) const
    {
        for (const OpenEditor& open : m_Open)
            if (open.Editor->GetHandle() == handle)
                return open.Editor.get();
        return nullptr;
    }

    bool AssetEditorManager::AnyPreviewing() const
    {
        return std::any_of(m_Open.begin(), m_Open.end(), [](const OpenEditor& open) { return open.Editor->IsPreviewing(); });
    }

    void AssetEditorManager::EndPreviews()
    {
        for (OpenEditor& open : m_Open)
            open.Editor->EndPreview();
    }

    bool AssetEditorManager::HasUnsaved() const
    {
        return std::any_of(m_Open.begin(), m_Open.end(), [](const OpenEditor& open) { return open.Editor->IsDirty(); });
    }

    std::vector<std::string> AssetEditorManager::UnsavedNames() const
    {
        std::vector<std::string> names;
        for (const OpenEditor& open : m_Open)
            if (open.Editor->IsDirty())
                names.push_back("the " + std::string(open.Editor->GetKind()) + " " + open.Editor->GetName());
        return names;
    }

    void AssetEditorManager::SaveAll()
    {
        for (OpenEditor& open : m_Open)
            if (open.Editor->IsDirty())
                open.Editor->Save();
    }

    void AssetEditorManager::RevertAll()
    {
        for (OpenEditor& open : m_Open)
            if (open.Editor->IsDirty())
                open.Editor->RevertToDisk();
    }
}
