#include "MaterialEditorV2.h"

#include "Panels/InspectorWidgetsV2.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/UI/Core/UILayer.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Layout/UILayout.h"
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

        if (Project::GetActive() && Project::GetActive()->GetEditorAssetManager())
        {
            auto path = UI::Label(Project::GetActive()->GetEditorAssetManager()->GetMetadata(m_Handle).FilePath.generic_string());
            path->FontSizeOverride = 13.0f;
            path->UseColorOverride = true;
            path->ColorOverride = { 0.58f, 0.64f, 0.74f, 1.0f };
            root->AddChild(path);
        }

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

    void MaterialEditorV2::Update()
    {
        if (!m_Body)
            return;
        if (!m_Rebuild)
        {
            for (const auto& sync : m_Syncs)
                sync();
            return;
        }
        m_Syncs.clear();
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
        InspectorCardBuilder builder{ m_Body, m_Syncs, {}, uiLayer ? &uiLayer->GetContext() : nullptr };
        PopulateMaterialPropertiesUI(m_Handle, material->GetData(), builder, m_ExpandedSections,
            {
                [this](AssetHandle child) { if (OpenAsset) OpenAsset(child); },
                [this]() { m_Rebuild = true; }
            });
    }
}
