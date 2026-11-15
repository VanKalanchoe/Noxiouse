#include "EditorV2Layer.h"

#include <algorithm>
#include <cmath>

#include "NoxCore/Audio/AudioMixer.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/SceneSerializer.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UILayer.h"
#include "NoxCore/UI/Demo/UIDemoGallery.h"
#include "NoxCore/UI/Layout/UIDockSpace.h"
#include "NoxCore/UI/Widgets/UIImage.h"

namespace Nox
{
    namespace
    {
        constexpr const char* kLayoutPath = "assets/layouts/EditorV2.nlayout";
    }

    EditorV2Layer::EditorV2Layer(std::filesystem::path projectFile)
        : Layer("EditorV2Layer")
        , m_EditorCamera(45.0f, 1.778f, 0.1f, 1000.0f)
    {
        m_ActiveScene = CreateRef<Scene>();
        OpenProject(projectFile);
        BuildUI(true);
    }

    EditorV2Layer::~EditorV2Layer()
    {
        if (UILayer* uiLayer = Application::Get().GetUILayer())
        {
            if (const auto& dock = uiLayer->GetDockSpaceRef())
                dock->SaveLayout(kLayoutPath);
        }
        if (Project::GetActive() && Project::GetActive()->GetEditorAssetManager())
            Project::GetActive()->GetEditorAssetManager()->Shutdown();
    }

    void EditorV2Layer::OpenProject(const std::filesystem::path& path)
    {
        Renderer* renderer = Application::Get().GetRenderer();
        const bool loaded = !path.empty() && Project::Load(path) != nullptr;
        if (!loaded)
            Project::New();

        Ref<Project> project = Project::GetActive();
        m_EditorCamera.ApplyWorldUnitDefaults();
        if (renderer && project)
        {
            renderer->ApplyQualitySetting(project->GetConfig().RenderQuality);
            renderer->ApplyProjectRendererSettings(project->GetConfig().Renderer);
        }
        if (project && project->GetEditorAssetManager())
            project->GetEditorAssetManager()->Init();
        if (loaded)
            AudioMixer::LoadProjectMixer(true);
        if (renderer)
            renderer->InitializeCustomLitShaders();

        if (loaded && project && project->GetConfig().StartScene)
        {
            const std::filesystem::path scenePath =
                Project::GetActiveAssetDirectory() /
                project->GetEditorAssetManager()->GetFilePath(project->GetConfig().StartScene);
            Ref<Scene> loadedScene = CreateRef<Scene>();
            SceneSerializer serializer(loadedScene);
            if (serializer.Deserialize(scenePath))
                m_ActiveScene = loadedScene;
        }
    }

    void EditorV2Layer::OnUpdate(Timestep ts)
    {
        if (Project::GetActive() && Project::GetActive()->GetEditorAssetManager())
            Project::GetActive()->GetEditorAssetManager()->Update();

        Renderer* renderer = Application::Get().GetRenderer();
        if (!renderer || !m_ActiveScene || !m_ViewportImage)
            return;

        const UI::UIRect& bounds = m_ViewportImage->GetBounds();
        const uint32_t wantW = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Width())));
        const uint32_t wantH = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Height())));
        if (wantW > 0 && wantH > 0)
        {
            m_ActiveScene->OnViewportResize(wantW, wantH);
            const NRI::Extent2D currentSize = renderer->getViewPortSize();
            if (wantW != currentSize.width || wantH != currentSize.height)
            {
                renderer->onViewportSizeChange({wantW, wantH});
                m_EditorCamera.SetViewportSize(static_cast<float>(wantW), static_cast<float>(wantH));
            }
        }

        m_ActiveScene->SetRenderer(renderer);
        m_ActiveScene->SetRenderer2D(renderer->getRenderer2D());

        const bool hovered = UI::HasState(m_ViewportImage->GetState(), UI::UIState::Hovered);
        m_EditorCamera.OnUpdate(ts, hovered || m_ViewportMouseCaptured);
        m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);

        Texture2D* sceneTex = renderer->GetSceneResource();
        m_ViewportImage->TextureDescriptorSlot = sceneTex ? sceneTex->GetDescriptorIndexSlot() : ~0u;
    }

    void EditorV2Layer::OnEvent(Event& event)
    {
        if (!m_ViewportImage)
            return;

        if (event.GetEventType() == EventType::MouseScrolled &&
            UI::HasState(m_ViewportImage->GetState(), UI::UIState::Hovered))
        {
            m_EditorCamera.OnEvent(event);
        }
        else if (event.GetEventType() == EventType::MouseButtonReleased)
        {
            m_ViewportMouseCaptured = false;
        }
    }

    void EditorV2Layer::BuildUI(bool loadSavedLayout)
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer)
            return;

        if (const auto& dock = uiLayer->GetDockSpaceRef())
            dock->ClearPanels();

        m_ViewportImage = UI::MakeUI<UI::UIImage>();
        m_ViewportImage->WidthMode = UI::UISizeMode::Fill;
        m_ViewportImage->HeightMode = UI::UISizeMode::Fill;
        m_ViewportImage->HitTestMode = UI::UIHitTestMode::Self;
        m_ViewportImage->Focusable = true;
        m_ViewportImage->OnMouseDown = [this](const UI::UIPointerEvent&)
        {
            m_ViewportMouseCaptured = true;
            return true;
        };
        m_ViewportImage->OnMouseUp = [this](const UI::UIPointerEvent&)
        {
            m_ViewportMouseCaptured = false;
            return false;
        };

        uiLayer->AddPanel(
            "Viewport",
            m_ViewportImage,
            UI::UIDockDropZone::Center,
            {},
            0.5f,
            UI::UIDockNodeFlags::CentralNode
        );

        m_StatusLabel = UI::Label("Ready").Margin(10.0f, 4.0f);

        auto panels = UI::CreateUIDemoPanels(uiLayer->GetContext(),
                                             [this](const std::string& msg) { if (m_StatusLabel) m_StatusLabel->Text = msg; });
        for (size_t i = 0; i < panels.size(); ++i)
        {
            if (i == 0)
                uiLayer->AddPanel(panels[i].Title, panels[i].Content, UI::UIDockDropZone::Right, "Viewport", 0.30f);
            else
                uiLayer->AddPanel(panels[i].Title, panels[i].Content, UI::UIDockDropZone::Center, panels[0].Title);
        }

        if (loadSavedLayout)
        {
            if (const auto& dock = uiLayer->GetDockSpaceRef())
                dock->LoadLayout(kLayoutPath);
        }

        uiLayer->SetRoot(
            UI::VStack(0.0f, {
                           UI::MenuBar({
                               {
                                   "File", {
                                       {
                                           "Save Layout", "", [uiLayer]()
                                           {
                                               if (const auto& dock = uiLayer->GetDockSpaceRef())
                                                   dock->SaveLayout(kLayoutPath);
                                           }
                                       },
                                       {"Exit", "Alt+F4", []() { Application::Get().Shutdown(); }}
                                   }
                               },
                               {
                                   "Window", {
                                       {"Reset Layout", "", [this]() { BuildUI(false); }},
                                       {"Reload Theme", "", [uiLayer]() { uiLayer->ReloadTheme(); }}
                                   }
                               }
                           }),
                           uiLayer->GetDockSpaceRef(),
                           m_StatusLabel
                       })
            .Padding(0.0f)
            .Style("Background")
            .Background(true)
        );
    }
}
