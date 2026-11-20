#include "MaterialEditorV2.h"

#include "Panels/AssetPreviewRenderer.h"
#include "Panels/InspectorWidgetsV2.h"
#include "Panels/MaterialEdits.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/UI/Core/UILayer.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UIImage.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"

namespace Nox
{
    MaterialEditorV2::MaterialEditorV2(AssetHandle material)
        : m_Handle(material), m_Name(AssetDisplayName(material))
    {
    }

    Ref<UI::UIElement> MaterialEditorV2::BuildUI()
    {
        auto root = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 6.0f);
        root->WidthMode = UI::UISizeMode::Fill;
        root->HeightMode = UI::UISizeMode::Fill;
        root->StyleClass = "Surface";
        root->DrawBackground = true;
        root->Padding = UI::UIThickness::All(10.0f);

        // Toolbar: Save (only when there is something to save), what is unsaved, the file.
        auto bar = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 8.0f);
        bar->WidthMode = UI::UISizeMode::Fill;
        Ref<UI::UIButton> save(new UI::UIButton("Save", [this]() { Save(); }));
        m_SaveButton = save;
        bar->AddChild(save);
        auto status = UI::Label("", 13.0f);
        status->UseColorOverride = true;
        status->ColorOverride = { 0.58f, 0.64f, 0.74f, 1.0f };
        status->VAlign = UI::UIAlignment::Center;
        m_Status = status;
        bar->AddChild(status);
        root->AddChild(bar);

        // The material on a sphere, drawn every frame this window shows (edits show at once).
        Ref<UI::UIImage> preview(new UI::UIImage(~0u, { 160.0f, 160.0f }));
        preview->WidthMode = UI::UISizeMode::Fixed;
        preview->HeightMode = UI::UISizeMode::Fixed;
        preview->FixedSize = { 160.0f, 160.0f };
        preview->HAlign = UI::UIAlignment::Center;
        preview->Tooltip = "The material on a sphere, drawn with every change";
        m_Preview = preview;
        root->AddChild(preview);

        auto scroll = UI::MakeUI<UI::UIScrollView>();
        scroll->WidthMode = UI::UISizeMode::Fill;
        scroll->HeightMode = UI::UISizeMode::Fill;
        scroll->ChildSpacing = 6.0f;
        root->AddChild(scroll);

        m_Body = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 6.0f);
        m_Body->WidthMode = UI::UISizeMode::Fill;
        scroll->AddChild(m_Body);
        m_Rebuild = true;
        Update();
        return root;
    }

    bool MaterialEditorV2::IsDirty() const { return MaterialEdits::IsUnsaved(m_Handle); }

    void MaterialEditorV2::Save()
    {
        if (!IsDirty())
            return;
        if (MaterialEdits::Save(m_Handle))
        {
            if (Notify)
                Notify({ UI::UINotificationSeverity::Success, "Material saved", m_Name });
        }
        else if (Notify)
            Notify({ UI::UINotificationSeverity::Error, "Material not saved", m_Name + " could not be written -- see the Output Log." });
    }

    bool MaterialEditorV2::RevertToDisk()
    {
        if (!IsDirty())
            return true;
        if (!MaterialEdits::Revert(m_Handle))
        {
            if (Notify)
                Notify({ UI::UINotificationSeverity::Error, "Changes not discarded", m_Name + ": the file could not be read back. The edits stay." });
            return false;
        }
        m_Rebuild = true; // (the rows pointed at the values that were replaced)
        return true;
    }

    void MaterialEditorV2::Update()
    {
        if (!m_Body)
            return;
        const bool dirty = IsDirty();
        if (auto* save = static_cast<UI::UIButton*>(m_SaveButton.get()))
        {
            save->SetEnabled(dirty);
            save->StyleVariant = dirty ? "primary" : "";
            save->Tooltip = dirty ? "Write the material to its file" : "Saved: nothing changed since";
        }
        if (auto* status = static_cast<UI::UILabel*>(m_Status.get()))
        {
            std::string text = Project::GetActive() && Project::GetActive()->GetEditorAssetManager()
                ? Project::GetActive()->GetEditorAssetManager()->GetMetadata(m_Handle).FilePath.generic_string() : std::string();
            if (dirty)
                text += "   |   unsaved";
            status->Text = text;
        }
        if (auto* preview = static_cast<UI::UIImage*>(m_Preview.get()))
        {
            Texture2D* sphere = AssetPreviewRenderer::Get().GetLivePreview(m_Handle, 160);
            preview->TextureDescriptorSlot = sphere ? sphere->GetDescriptorIndexSlot() : ~0u;
        }
        if (!m_Rebuild)
            return; // (the rows follow the material themselves: UIElement::OnSync)
        m_Body->ClearChildren();
        Ref<Material> material = AssetManager::GetAsset<Material>(m_Handle);
        if (!material)
        {
            // (Still loading: built again next frame -- it said "Loading material..." for good before.)
            auto loading = UI::Label("Loading material...");
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
