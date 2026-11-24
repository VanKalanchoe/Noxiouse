#include "MaterialEditor.h"

#include "Panels/AssetPreviewRenderer.h"
#include "Panels/InspectorWidgets.h"
#include "Panels/MaterialEdits.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/UI/Core/UILayer.h"
#include "NoxCore/Project/Project.h"
#include <NoxUI/Core/UIBuilder.h>
#include <NoxUI/Core/UIContext.h>
#include <NoxUI/Layout/UILayout.h>
#include <NoxUI/Widgets/UIButton.h>
#include <NoxUI/Widgets/UIImage.h>
#include <NoxUI/Widgets/UILabel.h>
#include <NoxUI/Widgets/UIScrollView.h>

namespace Nox
{
    MaterialEditor::MaterialEditor(AssetHandle material)
        : m_Handle(material), m_Name(AssetDisplayName(material))
    {
    }

    Ref<NoxUI::UIElement> MaterialEditor::BuildUI()
    {
        auto root = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 6.0f);
        root->WidthMode = NoxUI::UISizeMode::Fill;
        root->HeightMode = NoxUI::UISizeMode::Fill;
        root->StyleClass = "Surface";
        root->DrawBackground = true;
        root->Padding = NoxUI::UIThickness::All(10.0f);

        // Toolbar: Save (only when there is something to save), what is unsaved, the file.
        auto bar = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 8.0f);
        bar->WidthMode = NoxUI::UISizeMode::Fill;
        Ref<NoxUI::UIButton> save(new NoxUI::UIButton("Save", [this]() { Save(); }));
        m_SaveButton = save;
        bar->AddChild(save);
        auto status = NoxUI::Label("", 13.0f);
        status->UseColorOverride = true;
        status->ColorOverride = NoxUI::ThemeColor("TextMuted", { 0.58f, 0.64f, 0.74f, 1.0f });
        status->VAlign = NoxUI::UIAlignment::Center;
        m_Status = status;
        bar->AddChild(status);
        root->AddChild(bar);

        // The material on a sphere, drawn every frame this window shows (edits show at once).
        Ref<NoxUI::UIImage> preview(new NoxUI::UIImage(~0u, { 160.0f, 160.0f }));
        preview->WidthMode = NoxUI::UISizeMode::Fixed;
        preview->HeightMode = NoxUI::UISizeMode::Fixed;
        preview->FixedSize = { 160.0f, 160.0f };
        preview->HAlign = NoxUI::UIAlignment::Center;
        preview->Tooltip = "The material on a sphere, drawn with every change";
        m_Preview = preview;
        root->AddChild(preview);

        auto scroll = NoxUI::MakeUI<NoxUI::UIScrollView>();
        scroll->WidthMode = NoxUI::UISizeMode::Fill;
        scroll->HeightMode = NoxUI::UISizeMode::Fill;
        scroll->ChildSpacing = 6.0f;
        root->AddChild(scroll);

        m_Body = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 6.0f);
        m_Body->WidthMode = NoxUI::UISizeMode::Fill;
        scroll->AddChild(m_Body);
        m_Rebuild = true;
        Update();
        return root;
    }

    bool MaterialEditor::IsDirty() const { return MaterialEdits::IsUnsaved(m_Handle); }

    void MaterialEditor::Save()
    {
        if (!IsDirty())
            return;
        if (MaterialEdits::Save(m_Handle))
        {
            if (Notify)
                Notify({ NoxUI::UINotificationSeverity::Success, "Material saved", m_Name });
        }
        else if (Notify)
            Notify({ NoxUI::UINotificationSeverity::Error, "Material not saved", m_Name + " could not be written -- see the Output Log." });
    }

    bool MaterialEditor::RevertToDisk()
    {
        if (!IsDirty())
            return true;
        if (!MaterialEdits::Revert(m_Handle))
        {
            if (Notify)
                Notify({ NoxUI::UINotificationSeverity::Error, "Changes not discarded", m_Name + ": the file could not be read back. The edits stay." });
            return false;
        }
        m_Rebuild = true; // (the rows pointed at the values that were replaced)
        return true;
    }

    void MaterialEditor::Update()
    {
        if (!m_Body)
            return;
        const bool dirty = IsDirty();
        if (auto* save = static_cast<NoxUI::UIButton*>(m_SaveButton.get()))
        {
            save->SetEnabled(dirty);
            save->StyleVariant = dirty ? "primary" : "";
            save->Tooltip = dirty ? "Write the material to its file" : "Saved: nothing changed since";
        }
        if (auto* status = static_cast<NoxUI::UILabel*>(m_Status.get()))
        {
            std::string text = Project::GetActive() && Project::GetActive()->GetEditorAssetManager()
                ? Project::GetActive()->GetEditorAssetManager()->GetMetadata(m_Handle).FilePath.generic_string() : std::string();
            if (dirty)
                text += "   |   unsaved";
            status->Text = text;
        }
        if (auto* preview = static_cast<NoxUI::UIImage*>(m_Preview.get()))
        {
            Texture2D* sphere = AssetPreviewRenderer::Get().GetLivePreview(m_Handle, 160);
            preview->TextureId = sphere ? sphere->GetDescriptorIndexSlot() : ~0u;
        }
        if (!m_Rebuild)
            return; // (the rows follow the material themselves: UIElement::OnSync)
        m_Body->ClearChildren();
        Ref<Material> material = AssetManager::GetAsset<Material>(m_Handle);
        if (!material)
        {
            // (Still loading: built again next frame -- it said "Loading material..." for good before.)
            auto loading = NoxUI::Label("Loading material...");
            loading->FontSizeOverride = 14.5f;
            m_Body->AddChild(loading);
            return;
        }
        m_Rebuild = false;
        UILayer* uiLayer = Application::Get().GetUILayer();
        InspectorCardBuilder builder{ m_Body, {}, uiLayer ? &uiLayer->GetContext() : nullptr };
        PopulateMaterialPropertiesUI(m_Handle, material->GetData(), builder, m_ExpandedSections,
            {
                [this](AssetHandle child) { if (OpenAsset) OpenAsset(child); },
                [this]() { m_Rebuild = true; }
            });
    }
}
