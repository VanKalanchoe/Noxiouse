#include "EditorV2Layer.h"

#include <cmath>
#include <SDL3/SDL_scancode.h>

#include "NoxCore/Core/Application.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Layout/UISplitter.h"
#include "NoxCore/UI/Layout/UITabPanel.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UIGizmo.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIMenu.h"
#include "NoxCore/UI/Widgets/UINodeGraph.h"
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
        InitBasicScene();
        BuildUI();
        RefreshHierarchy();
        RefreshInspector();
    }

    EditorV2Layer::~EditorV2Layer() = default;

    void EditorV2Layer::InitBasicScene()
    {
        m_ActiveScene = Ref<Scene>(new Scene());

        Entity sky = m_ActiveScene->CreateEntity("Sky");
        sky.AddComponent<EnvironmentLightComponent>().Source = EnvironmentSource::ProceduralSky;

        Entity sun = m_ActiveScene->CreateEntity("Sun");
        auto& light = sun.AddComponent<DirectionalLightComponent>();
        light.Intensity = 5.0f;
        sun.GetComponent<TransformComponent>().Rotation = { glm::radians(-50.0f), glm::radians(30.0f), 0.0f };
        sun.MarkTransformDirty();

        Entity cam = m_ActiveScene->CreateEntity("MainCamera");
        cam.AddComponent<CameraComponent>().Primary = true;
        cam.GetComponent<TransformComponent>().Translation = { 0.0, 2.0, 8.0 };
        cam.MarkTransformDirty();

        m_SelectedEntity = static_cast<entt::entity>(sun);

        m_EditorCamera.ApplyWorldUnitDefaults();
        m_EditorCamera.SetFocalPoint(glm::dvec3(0.0, 1.0, 0.0));
        m_EditorCamera.SetDistance(10.0f);
        m_EditorCamera.SetOrientation(0.35f, -0.45f);
    }

    void EditorV2Layer::RefreshHierarchy()
    {
        if (!m_HierarchyTree || !m_ActiveScene)
            return;

        std::vector<UI::UITreeNode> nodes;
        auto view = m_ActiveScene->GetAllEntitiesWith<TagComponent>();
        for (auto e : view)
        {
            const auto& tag = view.get<TagComponent>(e);
            UI::UIColor tagColor{ 0.35f, 0.72f, 0.95f, 1.0f };
            if (m_ActiveScene->GetRegistry().all_of<DirectionalLightComponent>(e))
                tagColor = { 0.96f, 0.78f, 0.24f, 1.0f };
            else if (m_ActiveScene->GetRegistry().all_of<CameraComponent>(e))
                tagColor = { 0.35f, 0.85f, 0.55f, 1.0f };

            nodes.push_back({
                static_cast<uint64_t>(entt::to_integral(e)),
                tag.Tag,
                false,
                tagColor,
                {}
            });
        }
        m_HierarchyTree->SetNodes(std::move(nodes));
        if (m_SelectedEntity != entt::null)
            m_HierarchyTree->SetSelectedId(static_cast<uint64_t>(entt::to_integral(m_SelectedEntity)));
    }

    void EditorV2Layer::RefreshInspector()
    {
        if (!m_Inspector || !m_ActiveScene)
            return;

        m_Inspector->ClearProperties();
        if (m_SelectedEntity == entt::null || !m_ActiveScene->GetRegistry().valid(m_SelectedEntity))
            return;

        Entity entity{ m_SelectedEntity, m_ActiveScene.get() };

        auto entitySec = m_Inspector->AddSection("Entity", true);
        if (entity.HasComponent<TagComponent>())
        {
            auto& tag = entity.GetComponent<TagComponent>();
            m_Inspector->AddString("Name", tag.Tag, "Entity name...", [this, entity](const std::string& v) mutable
            {
                if (entity && entity.HasComponent<TagComponent>())
                {
                    entity.GetComponent<TagComponent>().Tag = v;
                    RefreshHierarchy();
                }
            }, entitySec);
        }

        if (entity.HasComponent<TransformComponent>())
        {
            auto& tr = entity.GetComponent<TransformComponent>();
            auto trSec = m_Inspector->AddSection("Transform", true);

            m_Inspector->AddVec3("Position", glm::vec3(tr.Translation), 0.05f, [entity](const glm::vec3& v) mutable
            {
                if (entity && entity.HasComponent<TransformComponent>())
                {
                    entity.GetComponent<TransformComponent>().Translation = glm::dvec3(v);
                    entity.MarkTransformDirty();
                }
            }, trSec);

            m_Inspector->AddVec3("Rotation", glm::degrees(tr.Rotation), 0.5f, [entity](const glm::vec3& v) mutable
            {
                if (entity && entity.HasComponent<TransformComponent>())
                {
                    entity.GetComponent<TransformComponent>().Rotation = glm::radians(v);
                    entity.MarkTransformDirty();
                }
            }, trSec);

            m_Inspector->AddVec3("Scale", tr.Scale, 0.02f, [entity](const glm::vec3& v) mutable
            {
                if (entity && entity.HasComponent<TransformComponent>())
                {
                    entity.GetComponent<TransformComponent>().Scale = v;
                    entity.MarkTransformDirty();
                }
            }, trSec);
        }

        if (entity.HasComponent<DirectionalLightComponent>())
        {
            auto& dl = entity.GetComponent<DirectionalLightComponent>();
            auto lightSec = m_Inspector->AddSection("Directional Light", true);
            m_Inspector->AddFloatSlider("Intensity", dl.Intensity, 0.0f, 20.0f, [entity](float v) mutable
            {
                if (entity && entity.HasComponent<DirectionalLightComponent>())
                    entity.GetComponent<DirectionalLightComponent>().Intensity = v;
            }, lightSec);
        }
    }

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
            { "New Scene", "Ctrl+N", [this]() { InitBasicScene(); RefreshHierarchy(); RefreshInspector(); } },
            { "Create Entity", "", [this]()
            {
                if (m_ActiveScene)
                {
                    Entity e = m_ActiveScene->CreateEntity("NewEntity");
                    m_SelectedEntity = static_cast<entt::entity>(e);
                    RefreshHierarchy();
                    RefreshInspector();
                }
            } },
            { "", "", {}, true },
            { "Exit", "Alt+F4", []() { Application::Get().Shutdown(); } }
        }, m_UI);
        menuBar->AddMenu("Gizmo", {
            { "Translate (W)", "W", [this]() { if (m_Gizmo) m_Gizmo->Mode = UI::UIGizmoMode::Translate; } },
            { "Rotate (E)", "E", [this]() { if (m_Gizmo) m_Gizmo->Mode = UI::UIGizmoMode::Rotate; } },
            { "Scale (R)", "R", [this]() { if (m_Gizmo) m_Gizmo->Mode = UI::UIGizmoMode::Scale; } }
        }, m_UI);
        menuBar->AddMenu("Window", {
            { "Reset Layout", "", [this]() { BuildUI(); RefreshHierarchy(); RefreshInspector(); } },
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

        auto title = MakeUI<UI::UILabel>("NoxEditorV2  |  Phase 5: Live 3D Scene, Viewport & UIGizmo", 16.0f);
        title->VAlign = UI::UIAlignment::Center;
        title->WidthMode = UI::UISizeMode::Fill;
        topBar->AddChild(title);

        auto addEntityBtn = MakeUI<UI::UIButton>("+ Entity", [this]()
        {
            if (m_ActiveScene)
            {
                Entity e = m_ActiveScene->CreateEntity("Entity_" + std::to_string(++m_ClickCount));
                m_SelectedEntity = static_cast<entt::entity>(e);
                RefreshHierarchy();
                RefreshInspector();
            }
        });
        addEntityBtn->StyleVariant = "primary";
        addEntityBtn->VAlign = UI::UIAlignment::Center;
        topBar->AddChild(addEntityBtn);

        root->AddChild(topBar);

        // Left Dock: Live Scene Hierarchy
        auto leftTabs = MakeUI<UI::UITabPanel>();
        m_HierarchyTree = MakeUI<UI::UITreeView>();
        m_HierarchyTree->OnSelect = [this](uint64_t id)
        {
            m_SelectedEntity = static_cast<entt::entity>(static_cast<uint32_t>(id));
            RefreshInspector();
            if (m_StatusLabel)
                m_StatusLabel->Text = "Selected Entity ID: " + std::to_string(id);
        };
        leftTabs->AddTab("Hierarchy", m_HierarchyTree);
        leftTabs->AddTab("Layers", MakeUI<UI::UILabel>("Render & Collision Layers", 14.0f));

        // Center-Top: Live 3D Viewport + UIGizmo + Material Graph
        auto centerTabs = MakeUI<UI::UITabPanel>();
        m_ViewportSlot = MakeUI<UI::UIViewportSlot>();
        m_Gizmo = MakeUI<UI::UIGizmo>();
        m_Gizmo->OnTransformChanged = [this](const glm::vec3& pos, const glm::vec3& rot, const glm::vec3& scale)
        {
            if (m_SelectedEntity != entt::null && m_ActiveScene && m_ActiveScene->GetRegistry().valid(m_SelectedEntity))
            {
                Entity e{ m_SelectedEntity, m_ActiveScene.get() };
                if (e.HasComponent<TransformComponent>())
                {
                    const glm::dvec3 origin = Application::Get().GetRenderer()
                        ? Application::Get().GetRenderer()->GetRenderOrigin()
                        : glm::dvec3(0.0);
                    auto& tr = e.GetComponent<TransformComponent>();
                    tr.Translation = glm::dvec3(pos) + origin;
                    tr.Rotation = rot;
                    tr.Scale = scale;
                    e.MarkTransformDirty();
                }
            }
        };
        m_ViewportSlot->AddChild(m_Gizmo);
        centerTabs->AddTab("Scene Viewport", m_ViewportSlot);

        auto nodeGraph = MakeUI<UI::UINodeGraph>();
        const uint64_t texOut = 101, colOut = 102, mulInA = 201, mulInB = 202, mulOut = 203;
        const uint64_t pbrBase = 301, pbrRough = 302, pbrMetal = 303;
        nodeGraph->AddNode({ 1, "TextureSample", { 0.20f, 0.52f, 0.38f, 1.0f }, { 40.0f, 60.0f },
            { { 100, "UV", false, { 0.35f, 0.85f, 0.55f, 1.0f } } },
            { { texOut, "RGBA", true, { 0.95f, 0.72f, 0.24f, 1.0f } } } });
        nodeGraph->AddNode({ 2, "ColorTint", { 0.55f, 0.32f, 0.72f, 1.0f }, { 40.0f, 200.0f },
            {},
            { { colOut, "Color", true, { 0.95f, 0.45f, 0.55f, 1.0f } } } });
        nodeGraph->AddNode({ 3, "Multiply", { 0.25f, 0.48f, 0.78f, 1.0f }, { 300.0f, 110.0f },
            { { mulInA, "A", false, { 0.95f, 0.72f, 0.24f, 1.0f } }, { mulInB, "B", false, { 0.95f, 0.45f, 0.55f, 1.0f } } },
            { { mulOut, "Result", true, { 0.35f, 0.72f, 0.98f, 1.0f } } } });
        nodeGraph->AddNode({ 4, "PBR Material Output", { 0.78f, 0.32f, 0.28f, 1.0f }, { 570.0f, 95.0f },
            { { pbrBase, "BaseColor", false, { 0.35f, 0.72f, 0.98f, 1.0f } },
              { pbrRough, "Roughness", false, { 0.65f, 0.65f, 0.75f, 1.0f } },
              { pbrMetal, "Metallic", false, { 0.65f, 0.65f, 0.75f, 1.0f } } },
            {} });
        nodeGraph->AddLink(texOut, mulInA);
        nodeGraph->AddLink(colOut, mulInB);
        nodeGraph->AddLink(mulOut, pbrBase);
        centerTabs->AddTab("Material Graph", nodeGraph);

        // Center-Bottom: Console & Content Browser
        auto bottomTabs = MakeUI<UI::UITabPanel>();
        auto consoleBox = MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 6.0f);
        m_StatusLabel = MakeUI<UI::UILabel>("Phase 5 Ready: Live 3D viewport + Entity Hierarchy + Inspector + W/E/R UIGizmo!", 13.5f);
        consoleBox->AddChild(m_StatusLabel);
        bottomTabs->AddTab("Console", consoleBox);
        bottomTabs->AddTab("Content Browser", MakeUI<UI::UILabel>("Assets / Shaders / Materials / Themes", 14.0f));

        auto centerVSplit = MakeUI<UI::UISplitter>(UI::UIOrientation::Vertical, 0.72f);
        centerVSplit->SetPanes(centerTabs, bottomTabs);

        // Right Dock: Live Inspector
        auto rightTabs = MakeUI<UI::UITabPanel>();
        m_Inspector = MakeUI<UI::UIPropertyGrid>();
        rightTabs->AddTab("Inspector", m_Inspector);
        rightTabs->AddTab("World", MakeUI<UI::UILabel>("Sky, DDGI, Path Tracer & Post-Process", 14.0f));

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
        EventDispatcher dispatcher(event);
        dispatcher.Dispatch<MouseMovedEvent>([this](MouseMovedEvent& e)
        {
            m_LastMousePos = { e.GetX(), e.GetY() };
            if (m_ViewportSlot)
            {
                const UI::UIRect& vb = m_ViewportSlot->GetBounds();
                m_EditorCamera.SetCursorPosition({ m_LastMousePos.x - vb.Min.x, m_LastMousePos.y - vb.Min.y });
            }
            return false;
        });

        dispatcher.Dispatch<KeyPressedEvent>([this](KeyPressedEvent& e)
        {
            if (m_Gizmo && !m_UI.GetFocused())
            {
                if (e.GetKeyCode() == SDL_SCANCODE_W) { m_Gizmo->Mode = UI::UIGizmoMode::Translate; return true; }
                if (e.GetKeyCode() == SDL_SCANCODE_E) { m_Gizmo->Mode = UI::UIGizmoMode::Rotate; return true; }
                if (e.GetKeyCode() == SDL_SCANCODE_R) { m_Gizmo->Mode = UI::UIGizmoMode::Scale; return true; }
            }
            return false;
        });

        m_UI.OnEvent(event);

        if (!event.Handled && m_ViewportSlot && m_ViewportSlot->Visible && m_ViewportSlot->GetBounds().Contains(m_LastMousePos))
            m_EditorCamera.OnEvent(event);
    }

    void EditorV2Layer::OnUpdate(Timestep ts)
    {
        m_LastDeltaSeconds = ts.GetSeconds();

        Application& app = Application::Get();
        Renderer* renderer = app.GetRenderer();
        if (!renderer || !m_ActiveScene)
            return;

        if (m_ViewportSlot && m_ViewportSlot->Visible)
        {
            const UI::UIRect& vb = m_ViewportSlot->GetBounds();
            const uint32_t vw = static_cast<uint32_t>(std::max(1.0f, std::floor(vb.Width())));
            const uint32_t vh = static_cast<uint32_t>(std::max(1.0f, std::floor(vb.Height())));
            const NRI::Extent2D cur = renderer->getViewPortSize();
            if (vw > 16 && vh > 16 && (vw != cur.width || vh != cur.height))
            {
                renderer->onViewportSizeChange({ vw, vh });
                m_EditorCamera.SetViewportSize(static_cast<float>(vw), static_cast<float>(vh));
            }

            const bool viewportHovered = vb.Contains(m_LastMousePos) && (!m_Gizmo || !m_Gizmo->IsUsing());
            m_EditorCamera.OnUpdate(ts, viewportHovered);
        }

        m_ActiveScene->SetRenderer(renderer);
        m_ActiveScene->SetRenderer2D(renderer->getRenderer2D());
        m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
    }

    void EditorV2Layer::OnRender()
    {
        Application& app = Application::Get();
        Renderer* renderer = app.GetRenderer();
        if (!renderer)
            return;

        if (m_ViewportSlot)
        {
            if (auto* tex = renderer->GetSceneResource())
                m_ViewportSlot->TextureSlot = tex->GetDescriptorIndexSlot();
        }

        if (m_Gizmo && m_ActiveScene)
        {
            const glm::dvec3 origin = renderer->GetRenderOrigin();
            m_Gizmo->SetCameraMatrices(
                m_EditorCamera.GetViewMatrix(origin),
                m_EditorCamera.GetViewProjection(origin),
                glm::vec3(m_EditorCamera.GetPosition() - origin)
            );

            if (m_SelectedEntity != entt::null && m_ActiveScene->GetRegistry().valid(m_SelectedEntity))
            {
                Entity e{ m_SelectedEntity, m_ActiveScene.get() };
                if (e.HasComponent<TransformComponent>())
                {
                    const auto& tr = e.GetComponent<TransformComponent>();
                    m_Gizmo->HasTarget = true;
                    m_Gizmo->TargetPosition = glm::vec3(tr.Translation - origin);
                    m_Gizmo->TargetRotation = tr.Rotation;
                    m_Gizmo->TargetScale = tr.Scale;
                }
                else
                    m_Gizmo->HasTarget = false;
            }
            else
                m_Gizmo->HasTarget = false;
        }

        UI::UIDrawList* draw = renderer->GetUIDrawList();
        if (!draw)
            return;

        int width = 1920;
        int height = 1080;
        app.getWindow()->getSizeInPixels(width, height);
        m_UI.UpdateAndRender(*draw, { static_cast<float>(width), static_cast<float>(height) }, m_LastDeltaSeconds);
    }
}
