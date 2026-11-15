#include "EditorV2Layer.h"

#include "NoxCore/Core/Application.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Layout/UISplitter.h"
#include "NoxCore/UI/Layout/UITabPanel.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIMenu.h"
#include "NoxCore/UI/Widgets/UIPropertyGrid.h"
#include "NoxCore/UI/Widgets/UITreeView.h"
#include "NoxCore/UI/Widgets/UIViewportSlot.h"

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
        auto root = MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 8.0f);
        root->Padding = UI::UIThickness::All(10.0f);
        root->DrawBackground = true;
        root->StyleClass = "Background";

        // Menu Bar
        auto menuBar = MakeUI<UI::UIMenuBar>();
        menuBar->AddMenu("File", {
            { "New Scene", "Ctrl+N", [this]() { if (m_StatusLabel) m_StatusLabel->Text = "Menu: File -> New Scene"; } },
            { "Open Scene...", "Ctrl+O", [this]() { if (m_StatusLabel) m_StatusLabel->Text = "Menu: File -> Open Scene"; } },
            { "Save Scene", "Ctrl+S", [this]() { if (m_StatusLabel) m_StatusLabel->Text = "Menu: File -> Save Scene"; } },
            { "", "", {}, true },
            { "Exit", "Alt+F4", []() { Application::Get().Shutdown(); } }
        }, m_UI);
        menuBar->AddMenu("Edit", {
            { "Undo", "Ctrl+Z", [this]() { if (m_StatusLabel) m_StatusLabel->Text = "Menu: Edit -> Undo"; } },
            { "Redo", "Ctrl+Y", [this]() { if (m_StatusLabel) m_StatusLabel->Text = "Menu: Edit -> Redo"; } }
        }, m_UI);
        menuBar->AddMenu("Window", {
            { "Reset Layout", "", [this]() { BuildUI(); } },
            { "Reload Theme", "", [theme]() { theme->LoadFromFile("assets/themes/DefaultDark.nstyle"); } }
        }, m_UI);
        root->AddChild(menuBar);

        // Top Bar (HBox)
        auto topBar = MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 10.0f);
        topBar->StyleClass = "TopBar";
        topBar->DrawBackground = true;
        topBar->HeightMode = UI::UISizeMode::Fixed;
        topBar->FixedSize.y = 44.0f;
        topBar->Padding = UI::UIThickness::Symmetric(14.0f, 6.0f);

        auto title = MakeUI<UI::UILabel>("NoxEditorV2  |  Phase 3: Splitters, Tabs, Menus & Viewport Slot", 16.0f);
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

        // Left Dock: Hierarchy + Outliner Tabs
        auto leftTabs = MakeUI<UI::UITabPanel>();
        auto tree = MakeUI<UI::UITreeView>();
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
        leftTabs->AddTab("Hierarchy", tree);
        leftTabs->AddTab("Layers", MakeUI<UI::UILabel>("Render & Collision Layers", 14.0f));

        // Center-Top: Viewport Tabs
        auto centerTabs = MakeUI<UI::UITabPanel>();
        auto viewportSlot = MakeUI<UI::UIViewportSlot>();
        centerTabs->AddTab("Scene Viewport", viewportSlot);
        centerTabs->AddTab("Game View", MakeUI<UI::UILabel>("Game Camera Output", 14.0f));

        // Center-Bottom: Content Browser & Console Tabs
        auto bottomTabs = MakeUI<UI::UITabPanel>();
        auto consoleBox = MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 6.0f);
        m_StatusLabel = MakeUI<UI::UILabel>("Phase 3 Ready: Drag splitters, switch tabs, open File/Edit/Window menus, or drag scrollbars!", 13.5f);
        consoleBox->AddChild(m_StatusLabel);
        bottomTabs->AddTab("Console", consoleBox);
        bottomTabs->AddTab("Content Browser", MakeUI<UI::UILabel>("Assets / Shaders / Materials / Themes", 14.0f));

        auto centerVSplit = MakeUI<UI::UISplitter>(UI::UIOrientation::Vertical, 0.72f);
        centerVSplit->SetPanes(centerTabs, bottomTabs);

        // Right Dock: Inspector & Lighting Tabs
        auto rightTabs = MakeUI<UI::UITabPanel>();
        auto inspector = MakeUI<UI::UIPropertyGrid>();

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

        rightTabs->AddTab("Inspector", inspector);
        rightTabs->AddTab("World", MakeUI<UI::UILabel>("Sky, DDGI, Path Tracer & Post-Process", 14.0f));

        // Main 3-way horizontal splitter: Left | (Center / Bottom) | Right
        auto centerRightSplit = MakeUI<UI::UISplitter>(UI::UIOrientation::Horizontal, 0.72f);
        centerRightSplit->SetPanes(centerVSplit, rightTabs);

        auto mainSplit = MakeUI<UI::UISplitter>(UI::UIOrientation::Horizontal, 0.22f);
        mainSplit->HeightMode = UI::UISizeMode::Fill;
        mainSplit->SetPanes(leftTabs, centerRightSplit);

        root->AddChild(mainSplit);
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
