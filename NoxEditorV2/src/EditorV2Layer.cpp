#include "EditorV2Layer.h"

#include "NoxCore/Core/Application.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIPropertyGrid.h"
#include "NoxCore/UI/Widgets/UITreeView.h"

namespace Nox
{
    namespace
    {
        template <typename T, typename... Args>
        Ref<T> MakeUI(Args&&... args)
        {
            return Ref<T>(new T(std::forward<Args>(args)...));
        }
    }

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
        auto root = MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 12.0f);
        root->Padding = UI::UIThickness::All(12.0f);
        root->DrawBackground = true;
        root->StyleClass = "Background";

        // Top Bar (HBox)
        auto topBar = MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 10.0f);
        topBar->StyleClass = "TopBar";
        topBar->DrawBackground = true;
        topBar->HeightMode = UI::UISizeMode::Fixed;
        topBar->FixedSize.y = 48.0f;
        topBar->Padding = UI::UIThickness::Symmetric(14.0f, 8.0f);

        auto title = MakeUI<UI::UILabel>("NoxEditorV2  |  Phase 1: Retained UI + .nstyle Hot Reload", 17.0f);
        title->VAlign = UI::UIAlignment::Center;
        title->WidthMode = UI::UISizeMode::Fill;
        topBar->AddChild(title);

        auto playBtn = MakeUI<UI::UIButton>("Play", [this]()
        {
            ++m_ClickCount;
            if (m_StatusLabel)
                m_StatusLabel->Text = "Play clicked (" + std::to_string(m_ClickCount) + ") - edit DefaultDark.nstyle to hot-reload!";
        });
        playBtn->StyleVariant = "primary";
        playBtn->VAlign = UI::UIAlignment::Center;
        topBar->AddChild(playBtn);

        auto simBtn = MakeUI<UI::UIButton>("Simulate", [this]()
        {
            ++m_ClickCount;
            if (m_StatusLabel)
                m_StatusLabel->Text = "Simulate clicked (" + std::to_string(m_ClickCount) + ")";
        });
        simBtn->VAlign = UI::UIAlignment::Center;
        topBar->AddChild(simBtn);

        root->AddChild(topBar);

        // Main 3-column workspace (HBox)
        auto body = MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 12.0f);
        body->HeightMode = UI::UISizeMode::Fill;

        auto leftPanel = MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 8.0f);
        leftPanel->StyleClass = "Panel";
        leftPanel->DrawBackground = true;
        leftPanel->WidthMode = UI::UISizeMode::Fixed;
        leftPanel->FixedSize.x = 300.0f;
        leftPanel->Padding = UI::UIThickness::All(12.0f);
        leftPanel->AddChild(MakeUI<UI::UILabel>("Scene Hierarchy (1,500 Virtualized)", 15.0f));

        auto tree = MakeUI<UI::UITreeView>();
        tree->HeightMode = UI::UISizeMode::Fill;
        std::vector<UI::UITreeNode> nodes;
        UI::UITreeNode sceneRoot{ 1, "MainScene", true, { 0.24f, 0.56f, 0.96f, 1.0f }, {} };
        sceneRoot.Children.push_back({ 2, "DirectionalLight", false, { 0.96f, 0.78f, 0.24f, 1.0f }, {} });
        sceneRoot.Children.push_back({ 3, "MainCamera", false, { 0.35f, 0.85f, 0.55f, 1.0f }, {} });

        UI::UITreeNode envGroup{ 4, "Environment (1,500 Meshes)", true, { 0.65f, 0.45f, 0.95f, 1.0f }, {} };
        for (int i = 0; i < 1500; ++i)
        {
            envGroup.Children.push_back({
                static_cast<uint64_t>(100 + i),
                "MeshInstance_" + std::to_string(i),
                false,
                { 0.35f, 0.72f, 0.95f, 1.0f },
                {}
            });
        }
        sceneRoot.Children.push_back(std::move(envGroup));
        nodes.push_back(std::move(sceneRoot));
        tree->SetNodes(std::move(nodes));
        tree->SetSelectedId(2);
        tree->OnSelect = [this](uint64_t id)
        {
            if (m_StatusLabel)
                m_StatusLabel->Text = "Selected Entity ID: " + std::to_string(id);
        };
        leftPanel->AddChild(tree);
        body->AddChild(leftPanel);

        auto centerPanel = MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 10.0f);
        centerPanel->StyleClass = "Panel";
        centerPanel->DrawBackground = true;
        centerPanel->WidthMode = UI::UISizeMode::Fill;
        centerPanel->Padding = UI::UIThickness::All(16.0f);
        centerPanel->AddChild(MakeUI<UI::UILabel>("Workspace Canvas", 16.0f));

        m_StatusLabel = MakeUI<UI::UILabel>("Phase 2: Test hover/click, text input, drag scalars, sliders & 1,500-item tree!", 14.0f);
        centerPanel->AddChild(m_StatusLabel);
        body->AddChild(centerPanel);

        auto rightPanel = MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 8.0f);
        rightPanel->StyleClass = "Panel";
        rightPanel->DrawBackground = true;
        rightPanel->WidthMode = UI::UISizeMode::Fixed;
        rightPanel->FixedSize.x = 340.0f;
        rightPanel->Padding = UI::UIThickness::All(12.0f);
        rightPanel->AddChild(MakeUI<UI::UILabel>("Inspector", 15.0f));

        auto inspector = MakeUI<UI::UIPropertyGrid>();
        inspector->HeightMode = UI::UISizeMode::Fill;

        auto entitySec = inspector->AddSection("Entity", true);
        inspector->AddString("Name", "DirectionalLight", "Entity name...", [this](const std::string& v)
        {
            if (m_StatusLabel)
                m_StatusLabel->Text = "Renamed: " + v;
        }, entitySec);
        inspector->AddBool("Visible", true, [this](bool v)
        {
            if (m_StatusLabel)
                m_StatusLabel->Text = std::string("Visible: ") + (v ? "true" : "false");
        }, entitySec);

        auto transformSec = inspector->AddSection("Transform", true);
        inspector->AddVec3("Position", { 0.0f, 12.0f, -4.5f }, 0.05f, [this](const glm::vec3& v)
        {
            if (m_StatusLabel)
                m_StatusLabel->Text = "Position: (" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")";
        }, transformSec);
        inspector->AddVec3("Rotation", { -45.0f, 30.0f, 0.0f }, 0.2f, {}, transformSec);
        inspector->AddVec3("Scale", { 1.0f, 1.0f, 1.0f }, 0.02f, {}, transformSec);

        auto lightSec = inspector->AddSection("Light & PBR", true);
        inspector->AddFloatSlider("Intensity", 3.5f, 0.0f, 20.0f, [this](float v)
        {
            if (m_StatusLabel)
                m_StatusLabel->Text = "Intensity: " + std::to_string(v);
        }, lightSec);
        inspector->AddFloatSlider("Roughness", 0.35f, 0.0f, 1.0f, {}, lightSec);
        inspector->AddDragFloat("Shadow Bias", 0.002f, 0.0005f, {}, lightSec);
        inspector->AddBool("Cast Shadows", true, {}, lightSec);

        rightPanel->AddChild(inspector);
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
