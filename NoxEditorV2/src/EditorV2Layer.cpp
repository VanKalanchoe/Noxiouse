#include "EditorV2Layer.h"

#include "NoxCore/Core/Application.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox
{
    EditorV2Layer::EditorV2Layer() : Layer("EditorV2Layer")
    {
        BuildUI();
    }

    EditorV2Layer::~EditorV2Layer() = default;

    void EditorV2Layer::BuildUI()
    {
        auto theme = UI::UITheme::CreateDefault();
        theme->LoadFromFile("assets/themes/DefaultDark.nstyle");
        if (Renderer* renderer = Application::Get().GetRenderer())
            theme->WatchFile(renderer->GetFileWatcher(), "assets/themes/DefaultDark.nstyle");
        m_UI.SetTheme(theme);

        // Root vertical stack
        auto root = CreateIntrusive<UI::UIStackBox>(UI::UIOrientation::Vertical, 12.0f);
        root->Padding = UI::UIThickness::All(12.0f);
        root->DrawBackground = true;
        root->StyleClass = "Background";

        // Top Bar (HBox)
        auto topBar = CreateIntrusive<UI::UIStackBox>(UI::UIOrientation::Horizontal, 10.0f);
        topBar->StyleClass = "TopBar";
        topBar->DrawBackground = true;
        topBar->HeightMode = UI::UISizeMode::Fixed;
        topBar->FixedSize.y = 48.0f;
        topBar->Padding = UI::UIThickness::Symmetric(14.0f, 8.0f);

        auto title = CreateIntrusive<UI::UILabel>("NoxEditorV2  |  Phase 1: Retained UI + .nstyle Hot Reload", 17.0f);
        title->VAlign = UI::UIAlignment::Center;
        title->WidthMode = UI::UISizeMode::Fill;
        topBar->AddChild(title);

        auto playBtn = CreateIntrusive<UI::UIButton>("Play", [this]()
        {
            ++m_ClickCount;
            if (m_StatusLabel)
                m_StatusLabel->Text = "Play clicked (" + std::to_string(m_ClickCount) + ") - edit DefaultDark.nstyle to hot-reload!";
        });
        playBtn->StyleVariant = "primary";
        playBtn->VAlign = UI::UIAlignment::Center;
        topBar->AddChild(playBtn);

        auto simBtn = CreateIntrusive<UI::UIButton>("Simulate", [this]()
        {
            ++m_ClickCount;
            if (m_StatusLabel)
                m_StatusLabel->Text = "Simulate clicked (" + std::to_string(m_ClickCount) + ")";
        });
        simBtn->VAlign = UI::UIAlignment::Center;
        topBar->AddChild(simBtn);

        root->AddChild(topBar);

        // Main 3-column workspace (HBox)
        auto body = CreateIntrusive<UI::UIStackBox>(UI::UIOrientation::Horizontal, 12.0f);
        body->HeightMode = UI::UISizeMode::Fill;

        auto leftPanel = CreateIntrusive<UI::UIStackBox>(UI::UIOrientation::Vertical, 8.0f);
        leftPanel->StyleClass = "Panel";
        leftPanel->DrawBackground = true;
        leftPanel->WidthMode = UI::UISizeMode::Fixed;
        leftPanel->FixedSize.x = 300.0f;
        leftPanel->Padding = UI::UIThickness::All(14.0f);
        leftPanel->AddChild(CreateIntrusive<UI::UILabel>("Scene Hierarchy (Phase 2)", 16.0f));
        leftPanel->AddChild(CreateIntrusive<UI::UILabel>("Hover & click buttons in the top bar!", 13.0f));
        body->AddChild(leftPanel);

        auto centerPanel = CreateIntrusive<UI::UIStackBox>(UI::UIOrientation::Vertical, 10.0f);
        centerPanel->StyleClass = "Panel";
        centerPanel->DrawBackground = true;
        centerPanel->WidthMode = UI::UISizeMode::Fill;
        centerPanel->Padding = UI::UIThickness::All(16.0f);
        centerPanel->AddChild(CreateIntrusive<UI::UILabel>("Workspace Canvas", 16.0f));

        m_StatusLabel = CreateIntrusive<UI::UILabel>("Ready - edit NoxEditor/assets/themes/DefaultDark.nstyle to test live reload.", 14.0f);
        centerPanel->AddChild(m_StatusLabel);
        body->AddChild(centerPanel);

        auto rightPanel = CreateIntrusive<UI::UIStackBox>(UI::UIOrientation::Vertical, 8.0f);
        rightPanel->StyleClass = "Panel";
        rightPanel->DrawBackground = true;
        rightPanel->WidthMode = UI::UISizeMode::Fixed;
        rightPanel->FixedSize.x = 320.0f;
        rightPanel->Padding = UI::UIThickness::All(14.0f);
        rightPanel->AddChild(CreateIntrusive<UI::UILabel>("Inspector (Phase 2)", 16.0f));
        body->AddChild(rightPanel);

        root->AddChild(body);
        m_UI.SetRoot(root);
    }

    void EditorV2Layer::OnEvent(Event& event)
    {
        m_UI.OnEvent(event);
    }

    void EditorV2Layer::OnUpdate(Timestep ts)
    {
        m_LastDeltaSeconds = ts.GetSeconds();
    }

    void EditorV2Layer::OnRender()
    {
        Application& app = Application::Get();
        Renderer* renderer = app.GetRenderer();
        if (!renderer)
            return;

        UI::UIDrawList* draw = renderer->GetUIDrawList();
        if (!draw)
            return;

        int width = 1920;
        int height = 1080;
        app.getWindow()->getSizeInPixels(width, height);
        m_UI.UpdateAndRender(*draw, { static_cast<float>(width), static_cast<float>(height) }, m_LastDeltaSeconds);
    }
}
