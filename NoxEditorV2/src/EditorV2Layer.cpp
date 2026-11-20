#include "EditorV2Layer.h"
#if NOX_WITH_MCP
#include "McpEditor.h"
#endif
#include "Panels/AssetPreviewRenderer.h"
#include "Panels/InspectorWidgetsV2.h"
#include "Panels/AnimationClipEditorV2.h"
#include "Panels/TimelineEditorV2.h"
#include "Panels/MaterialEditorV2.h"
#include "Panels/MaterialEdits.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <unordered_set>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <stb_image_write.h>
#include "json.hpp"

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Asset/SceneImporter.h"
#include "NoxCore/Asset/TextureImporter.h"
#include "NoxCore/Audio/AudioMixer.h"
#include "NoxCore/Console/Console.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Input.h"
#include "NoxCore/Core/Window.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Network/NetReplication.h"
#include "NoxCore/Network/NetSession.h"
#include "NoxCore/Physics/MeshColliderGeometry.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/CustomLitShaders.h"
#include "NoxCore/Renderer/MaterialGraph.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/HLODBuilder.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/EntityBounds.h"
#include "NoxCore/Scene/ModelInstance.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/SceneSerializer.h"
#include "NoxCore/Scripting/ScriptEngine.h"
#include "NoxCore/UI/Layout/UIDrawerHost.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Core/UILayer.h"
#include "NoxCore/UI/Core/UIViewportManager.h"
#include "NoxCore/UI/Layout/UIDockSpace.h"
#include "NoxCore/UI/Layout/UITabPanel.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Rendering/UIIcons.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UIImage.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIModal.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"
#include "NoxCore/UI/Widgets/UISlider.h"
#include "NoxCore/UI/Widgets/UITextInput.h"
#include "NoxCore/Utils/Utils.h"
#include "ProjectManager/ProcessLauncher.h"
#include "ProjectManager/ProjectLibrary.h"

namespace Nox
{
    namespace
    {
        void ActivateDockedTab(UI::UIDockSpace& dock, const std::string& title); // (below)

        // In <Project>/Saved/Layout (EditorV2Layer::LayoutDirectory).
        // ("2": the layouts saved before the drawer had the Content Browser and the Output Log docked; they start fresh once.)
        constexpr const char* kLayoutFile = "EditorV2.2.nlayout";
        constexpr const char* kViewportDockLayoutFile = "EditorV2ViewportDock.nlayout";
        constexpr const char* kViewportPrefsFile = "EditorV2Viewports.txt";
        constexpr float kStatusBarHeight = 28.0f;

        // The status bar's left side: a dot (green: nothing is running; amber, pulsing: something loads, streams, imports or
        // builds) and what it is.
        class StatusActivityElement final : public UI::UIElement
        {
        public:
            std::string Text = "Ready";
            bool Busy = false;

        protected:
            UI::UIVec2 ComputeDesiredSize() override
            {
                return { 26.0f + UI::UILabel::MeasureText(Text, nullptr, kFont).x + 8.0f, kStatusBarHeight - 4.0f };
            }

            void OnUpdate(float dt) override { m_Time += dt; }

            void OnPaint(UI::UIDrawList& d, const UI::UITheme& theme) override
            {
                const UI::UIRect& b = GetBounds();
                const UI::UIVec2 dot{ b.Min.x + 12.0f, b.Center().y };
                const UI::UIColor ready = UI::ThemeColor("StatusGood", { 0.36f, 0.80f, 0.46f, 1.0f });
                const UI::UIColor busy = UI::ThemeColor("StatusBusy", { 0.96f, 0.70f, 0.26f, 1.0f });
                if (Busy)
                {
                    const float pulse = 0.5f + 0.5f * std::sin(m_Time * 5.0f);
                    d.AddRoundedRect(dot - UI::UIVec2{ 7.0f }, dot + UI::UIVec2{ 7.0f }, { busy.r, busy.g, busy.b, 0.25f * pulse }, glm::vec4(7.0f));
                }
                d.AddRoundedRect(dot - UI::UIVec2{ 4.0f }, dot + UI::UIVec2{ 4.0f }, Busy ? busy : ready, glm::vec4(4.0f));
                const UI::UIColor text = Busy ? theme.GetToken("Text", { 0.92f, 0.93f, 0.95f, 1.0f }) : theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f });
                const std::string shown = UI::UILabel::ElideText(Text, nullptr, kFont, std::max(0.0f, b.Width() - 30.0f));
                const UI::UIVec2 size = UI::UILabel::MeasureText(shown, nullptr, kFont);
                d.AddText(shown, nullptr, { b.Min.x + 24.0f, b.Center().y - size.y * 0.5f }, kFont, text);
            }

        private:
            static constexpr float kFont = 15.0f;
            float m_Time = 0.0f;
        };

        // A tab of the status bar's left end ("Content Drawer", "Output Log"): flat text, lit while its page is shown.
        class StatusTabButton final : public UI::UIElement
        {
        public:
            StatusTabButton(std::string label, std::function<void()> onClick)
                : m_Label(std::move(label)), m_OnClick(std::move(onClick))
            {
                HitTestMode = UI::UIHitTestMode::Self;
                VAlign = UI::UIAlignment::Center;
            }

            bool Active = false;

            bool OnPointerDown(const UI::UIPointerEvent& e) override
            {
                if (e.Button != 1)
                    return false;
                if (m_OnClick)
                    m_OnClick();
                return true;
            }
            bool OnPointerEnter(const UI::UIPointerEvent&) override { m_Hovered = true; return false; }
            bool OnPointerLeave(const UI::UIPointerEvent&) override { m_Hovered = false; return false; }

        protected:
            UI::UIVec2 ComputeDesiredSize() override
            {
                return { UI::UILabel::MeasureText(m_Label, nullptr, kFont).x + 24.0f, kStatusBarHeight - 4.0f };
            }

            void OnPaint(UI::UIDrawList& d, const UI::UITheme& theme) override
            {
                const UI::UIRect& b = GetBounds();
                const UI::UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
                if (Active || m_Hovered)
                    d.AddRoundedRect(b.Min, b.Max, Active ? UI::UIColor{ accent.r, accent.g, accent.b, 0.22f } : UI::ThemeColor("StatusTabFill", { 1.0f, 1.0f, 1.0f, 0.06f }), glm::vec4(4.0f));
                if (Active)
                    d.AddRoundedRect({ b.Min.x + 6.0f, b.Max.y - 2.0f }, { b.Max.x - 6.0f, b.Max.y }, accent, glm::vec4(1.0f));
                const UI::UIColor text = (Active || m_Hovered) ? theme.GetToken("Text", { 0.92f, 0.93f, 0.95f, 1.0f })
                                                               : theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f });
                const UI::UIVec2 size = UI::UILabel::MeasureText(m_Label, nullptr, kFont);
                d.AddText(m_Label, nullptr, { b.Center().x - size.x * 0.5f, b.Center().y - size.y * 0.5f }, kFont, text);
            }

        private:
            static constexpr float kFont = 14.0f;
            std::string m_Label;
            std::function<void()> m_OnClick;
            bool m_Hovered = false;
        };

        // The status bar's right side: the log's warnings and errors since its last Clear; a click opens the Output Log.
        class StatusCountsElement final : public UI::UIElement
        {
        public:
            StatusCountsElement(const OutputLogV2Panel& log, std::function<void()> onClick)
                : m_Log(log), m_OnClick(std::move(onClick))
            {
                HitTestMode = UI::UIHitTestMode::Self;
                Tooltip = "Warnings and errors in the Output Log since its last Clear -- click to open it";
            }

            bool OnPointerDown(const UI::UIPointerEvent& e) override
            {
                if (e.Button != 1)
                    return false;
                if (m_OnClick)
                    m_OnClick();
                return true;
            }
            bool OnPointerEnter(const UI::UIPointerEvent&) override { m_Hovered = true; return false; }
            bool OnPointerLeave(const UI::UIPointerEvent&) override { m_Hovered = false; return false; }

        protected:
            UI::UIVec2 ComputeDesiredSize() override { return { 150.0f, kStatusBarHeight - 4.0f }; }

            void OnPaint(UI::UIDrawList& d, const UI::UITheme& theme) override
            {
                const UI::UIRect& b = GetBounds();
                if (m_Hovered)
                    d.AddRoundedRect(b.Min, b.Max, UI::ThemeColor("StatusCountsFill", { 1.0f, 1.0f, 1.0f, 0.06f }), glm::vec4(4.0f));
                const UI::UIColor muted = theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f });
                const size_t warnings = m_Log.GetWarningCount(), errors = m_Log.GetErrorCount();
                const UI::UIColor warnColor = warnings > 0 ? UI::ThemeColor("StatusWarning", { 0.96f, 0.74f, 0.28f, 1.0f }) : muted;
                const UI::UIColor errorColor = errors > 0 ? UI::ThemeColor("StatusError", { 0.95f, 0.38f, 0.38f, 1.0f }) : muted;
                const std::string errorText = std::to_string(errors);
                const std::string warnText = std::to_string(warnings);
                const float errorW = UI::UILabel::MeasureText(errorText, nullptr, kFont).x;
                const float warnW = UI::UILabel::MeasureText(warnText, nullptr, kFont).x;
                const float cy = b.Center().y;

                // Right to left: errors, then warnings.
                float x = b.Max.x - 10.0f - errorW;
                d.AddText(errorText, nullptr, { x, cy - UI::UILabel::MeasureText(errorText, nullptr, kFont).y * 0.5f }, kFont, errorColor);
                const UI::UIVec2 disc{ x - 11.0f, cy };
                d.AddRoundedRect(disc - UI::UIVec2{ 6.5f }, disc + UI::UIVec2{ 6.5f }, { errorColor.r, errorColor.g, errorColor.b, 0.22f }, glm::vec4(6.5f), 1.5f, errorColor);
                UI::Icons::DrawSegment(d, disc + UI::UIVec2{ -2.6f, -2.6f }, disc + UI::UIVec2{ 2.6f, 2.6f }, 1.6f, errorColor);
                UI::Icons::DrawSegment(d, disc + UI::UIVec2{ -2.6f, 2.6f }, disc + UI::UIVec2{ 2.6f, -2.6f }, 1.6f, errorColor);

                x = disc.x - 18.0f - warnW;
                d.AddText(warnText, nullptr, { x, cy - UI::UILabel::MeasureText(warnText, nullptr, kFont).y * 0.5f }, kFont, warnColor);
                const UI::UIVec2 tri{ x - 11.0f, cy + 0.5f };
                UI::Icons::DrawSegment(d, { tri.x - 6.5f, tri.y + 5.5f }, { tri.x, tri.y - 6.0f }, 1.6f, warnColor);
                UI::Icons::DrawSegment(d, { tri.x, tri.y - 6.0f }, { tri.x + 6.5f, tri.y + 5.5f }, 1.6f, warnColor);
                UI::Icons::DrawSegment(d, { tri.x + 6.5f, tri.y + 5.5f }, { tri.x - 6.5f, tri.y + 5.5f }, 1.6f, warnColor);
                UI::Icons::DrawSegment(d, { tri.x, tri.y - 2.2f }, { tri.x, tri.y + 1.6f }, 1.6f, warnColor);
                UI::Icons::DrawSegment(d, { tri.x, tri.y + 3.4f }, { tri.x, tri.y + 3.6f }, 1.8f, warnColor);
            }

        private:
            static constexpr float kFont = 14.0f;
            const OutputLogV2Panel& m_Log;
            std::function<void()> m_OnClick;
            bool m_Hovered = false;
        };
        constexpr const char* kViewportTitles[4] = { "Viewport", "Viewport 2", "Viewport 3", "Viewport 4" };
        constexpr int kDefaultSlotViewModes[4] = { 0, 0, 0, 0 };

        std::filesystem::path EditorCamerasFile()
        {
            return Project::GetProjectDirectory() / "Cache" / "EditorCameras.txt";
        }

        std::vector<std::string> ReadEditorCameraLines()
        {
            std::vector<std::string> lines;
            std::ifstream in(EditorCamerasFile());
            for (std::string line; std::getline(in, line);)
            {
                if (!line.empty())
                    lines.push_back(line);
            }
            return lines;
        }
    }

    EditorV2Layer::EditorV2Layer(std::filesystem::path projectFile, bool createBasicScene)
        : Layer("EditorV2Layer")
        , m_EditorCamera(30.0f, 1.778f, 0.01f, 1000.0f)
        , m_CreateBasicScene(createBasicScene)
    {
        Application& app = Application::Get();
        m_Renderer = app.GetRenderer();
        m_Renderer2D = m_Renderer ? m_Renderer->getRenderer2D() : nullptr;

        for (int i = 0; i < kMaxViewports; ++i)
        {
            m_Viewports[i].Title = kViewportTitles[i];
            m_Viewports[i].Camera = m_EditorCamera;
            m_Viewports[i].Camera.SetViewMode(static_cast<EditorViewMode>(kDefaultSlotViewModes[i]));
        }
        m_Viewports[0].Open = true;

        m_IconPlay = TextureImporter::LoadTexture2D("assets/Icons/PlayButton.ktx2", { .flip = true, .generateMips = false });
        m_IconStop = TextureImporter::LoadTexture2D("assets/Icons/StopButton.ktx2", { .flip = true, .generateMips = false });
        m_IconPause = TextureImporter::LoadTexture2D("assets/Icons/PauseButton.ktx2", { .flip = true, .generateMips = false });
        m_IconSimulate = TextureImporter::LoadTexture2D("assets/Icons/SimulateButton.ktx2", { .flip = true, .generateMips = false });
        m_IconStep = TextureImporter::LoadTexture2D("assets/Icons/StepButton.ktx2", { .flip = true, .generateMips = false });

        m_EditorScene = CreateRef<Scene>();
        m_ActiveScene = m_EditorScene;
        m_SceneHierarchyPanel.SetFocusEntityCallback([this](Entity entity) { FocusOnEntity(entity); });
        m_SceneHierarchyPanel.SetCreateShapeCallback([this](PrimitiveShape shape, const std::string& folder) { CreateShape(shape, folder); });
        m_SceneHierarchyPanel.SetSceneMutatedCallback([this]() { m_HistoryCheckRequested = true; });
        m_SceneHierarchyPanel.SetSelection(&m_Selection);
        m_InspectorPanel.SetSelection(&m_Selection);

        m_InspectorPanel.SetSceneMutatedCallback([this]() { m_HistoryCheckRequested = true; });
        m_InspectorPanel.SetTagChangedCallback([this]() { m_SceneHierarchyPanel.MarkDirty(); });
        m_InspectorPanel.SetOpenAssetCallback([this](AssetHandle h) { OpenAssetV2(h); });
        m_InspectorPanel.SetSelectEntityCallback([this](Entity entity)
        {
            if (entity)
                m_Selection.Select(entity);
            else
                m_Selection.Clear();
        });
        SetSceneContext(m_ActiveScene);

        m_SceneHierarchyPanel.SetCreatePrefabCallback([this](UUID entity)
        {
            // Into the folder the Content Browser shows, like entities dragged into it.
            const AssetHandle created = CreatePrefab(entity, m_ContentBrowserPanel.CurrentFolder());
            if (created == 0)
            {
                Notify(UI::UINotificationSeverity::Error, "Prefab not created", "See the Output Log for why.");
                return;
            }
            Notify(UI::UINotificationSeverity::Success, "Prefab created", "In " + (m_ContentBrowserPanel.CurrentFolder().empty() ? std::string("Assets") : m_ContentBrowserPanel.CurrentFolder().generic_string()));
            m_ContentBrowserPanel.ShowNewAsset(created);
        });
        m_ContentBrowserPanel.SetOpenAssetCallback([this](AssetHandle handle) { OpenAssetV2(handle); });
        m_ContentBrowserPanel.SetCreateVariantCallback([this](AssetHandle base, const std::filesystem::path& folder) { return CreateVariant(base, folder); });
        m_ContentBrowserPanel.SetReplaceReferencesCallback([this](AssetHandle from, AssetHandle to) { ReplaceReferences(from, to); });
        m_ContentBrowserPanel.SetCreatePrefabCallback([this](uint64_t dragId, const std::filesystem::path& folder)
        {
            // The hierarchy's drag id of an entity row is the entity's UUID (SceneHierarchyV2Panel::NodeIdForEntity).
            return CreatePrefab(UUID(dragId), folder);
        });
        m_ContentBrowserPanel.SetSceneReferencesCallback([this](std::unordered_set<AssetHandle>& handles)
        {
            if (m_EditorScene)
                m_EditorScene->CollectAssetReferences(handles);
            if (m_ActiveScene && m_ActiveScene != m_EditorScene)
                m_ActiveScene->CollectAssetReferences(handles);
        });
        m_ContentBrowserPanel.SetNotifyCallback([this](UI::UINotification note)
        {
            Notify(note.Severity, std::move(note.Title), std::move(note.Message));
        });

        // The Content Browser's pictures: meshes, prefabs and material spheres drawn by the renderer's thumbnail view.
        AssetPreviewRenderer::SetRenderer(m_Renderer);
        AssetPreviewRenderer::Get().SetSphereProvider([this]() { return GetShapeAsset(PrimitiveShape::Sphere); });

        if (!projectFile.empty())
            NOX_CORE_INFO("Loading project {}", projectFile.generic_string());
        if ((projectFile.empty() || !OpenProject(projectFile)) && !OpenStartupProject(projectFile))
        {
            SDL_Event event{};
            event.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&event);
        }

        if (m_ApplyBasicSceneView)
            ApplyBasicSceneView();

        if (!ScriptEngine::Initialize(SDL_GetBasePath(), true) && !ScriptEngine::IsInitialized())
            NOX_CORE_ERROR("Failed to initialize the .NET scripting backend");
        BuildScriptsIfNeverBuilt();

        Net::RegisterConsoleVariables();
        if (const char* exec = app.GetSpecification().CommandLineArgs.Value("--exec"))
            Console::Get().Execute(exec);

        RegisterCommands();
        RegisterAssetEditors();
#if NOX_WITH_MCP
        CreateMcpServer();
#endif
        LoadViewportPrefs(); // the project's (Saved/Layout)
        BuildUI(true);
    }

    EditorV2Layer::~EditorV2Layer()
    {
        if (m_CursorHidden)
        {
            SDL_ShowCursor();
            m_CursorHidden = false;
        }
#if NOX_WITH_MCP
        DestroyMcpServer(); // (first: its calls reach into the editor)
#endif
        EndMultiplayerPlay();
        SaveEditorCameras();
        ScriptEngine::Shutdown();

        if (UILayer* uiLayer = Application::Get().GetUILayer())
        {
            if (const auto& dock = uiLayer->GetDockSpaceRef())
            {
                dock->CanClosePanel = nullptr; // (it asks this layer)
            }
        }
        ExitPrefabMode(); // (asked about before; the level comes back so the editor closes on it)
        m_AssetEditors.CloseAll(); // (while the project's assets are still loaded; their windows are not saved in the layout)
        SaveLayoutFiles();
        AssetPreviewRenderer::Get().Clear();
        AssetPreviewRenderer::SetRenderer(nullptr);
        if (Project::GetActive() && Project::GetActive()->GetEditorAssetManager())
            Project::GetActive()->GetEditorAssetManager()->Shutdown();
    }

    void EditorV2Layer::BuildScriptsIfNeverBuilt()
    {
        if (!ScriptEngine::IsInitialized() || !Project::GetActive() || ScriptEngine::GetProjectScriptsProject().empty())
            return;
        std::filesystem::path core, game;
        if (!ScriptEngine::FindAssemblies(core, game))
            m_ScriptBuilder.RequestBuild();
    }

    bool EditorV2Layer::NewProject()
    {
        std::filesystem::path path = Utility::SaveFile("Nox Project\0nproj\0");
        if (path.empty())
            return false;
        ProjectLibrary::NewProjectSettings settings;
        settings.Name = path.stem().string();
        settings.Location = path.parent_path();
        std::string error;
        const std::filesystem::path projectFile = ProjectLibrary::CreateProject(settings, error);
        if (projectFile.empty())
        {
            NOX_CORE_ERROR("New Project: {}", error);
            return false;
        }
        m_CreateBasicScene = true;
        return OpenProject(projectFile);
    }

    void EditorV2Layer::SaveAll()
    {
        // Assets first (they do not ask anything); the scene last (an untitled one asks for its name).
        m_AssetEditors.SaveAll();
        MaterialEdits::SaveAll();
        m_AssetEditors.EndPreviews(); // (a timeline's poses are not saved into the scene)
        if (AudioMixer::IsDirty())
            AudioMixer::Save();
        if (m_SceneDirty || m_EditorScenePath.empty())
            SaveScene();
        if (!HasUnsavedChanges(UnsavedAssets | UnsavedMixer))
            Notify(UI::UINotificationSeverity::Success, "Saved", "Everything that was unsaved.");
    }

    void EditorV2Layer::NewProjectAsync()
    {
        Utility::SaveFileAsync("Nox Project\0nproj\0", [this](std::string chosen)
        {
            if (chosen.empty())
                return;
            const std::filesystem::path path(chosen);
            ProjectLibrary::NewProjectSettings settings;
            settings.Name = path.stem().string();
            settings.Location = path.parent_path();
            std::string error;
            const std::filesystem::path projectFile = ProjectLibrary::CreateProject(settings, error);
            if (projectFile.empty())
            {
                NOX_CORE_ERROR("New Project: {}", error);
                Notify(UI::UINotificationSeverity::Error, "Project not made", error);
                return;
            }
            m_CreateBasicScene = true;
            OpenProject(projectFile);
        });
    }

    void EditorV2Layer::OpenProjectAsync()
    {
        Utility::OpenFileAsync("Nox Project\0nproj\0", [this](std::string chosen)
        {
            if (!chosen.empty())
                OpenProject(std::filesystem::path(chosen));
        });
    }

    bool EditorV2Layer::OpenProject()
    {
        const std::string filepath = Utility::OpenFile("Nox Project\0nproj\0");
        if (filepath.empty())
            return false;
        return OpenProject(filepath);
    }

    bool EditorV2Layer::OpenProject(const std::filesystem::path& path)
    {
        if (path.empty())
            return false;

        const bool hadProject = Project::GetActive() != nullptr;
        if (hadProject)
        {
            // The graph and material windows show the project being left: closed while it is still loaded (unsaved edits were
            // asked about before; what is left goes back to the files).
            ExitPrefabMode(); // (asked about before)
            m_AssetEditors.CloseAll();
            MaterialEdits::Clear(); // (asked about before: what is left belonged to the project being left)
            SaveLayoutFiles(); // the project being left keeps its layout
        }
        if (!Project::Load(path))
        {
            NOX_CORE_ERROR("Could not open the project '{}'", path.string());
            Notify(UI::UINotificationSeverity::Error, "Project not opened", path.filename().string() + " -- see the Output Log.");
            return false;
        }
        AssetPreviewRenderer::Get().Clear(); // the thumbnails were the other project's

        Ref<Project> project = Project::GetActive();
        ProjectLibrary::RememberRecentProject(path);
        if (auto win = Application::Get().getWindow(); win && win->getHandle() && project)
            SDL_SetWindowTitle(win->getHandle(), ("Nox Editor - " + project->GetConfig().Name).c_str());

        m_EditorCamera.ApplyWorldUnitDefaults();
        if (m_Renderer && project)
        {
            m_Renderer->ApplyQualitySetting(project->GetConfig().RenderQuality);
            m_Renderer->ApplyProjectRendererSettings(project->GetConfig().Renderer);
        }
        if (project && project->GetEditorAssetManager())
            project->GetEditorAssetManager()->Init();
        AudioMixer::LoadProjectMixer(true);
        if (m_Renderer)
            m_Renderer->InitializeCustomLitShaders();

        m_CustomLitSignature = CustomLitSignature();
        if (!m_CustomLitSignature.empty())
            WriteMaterialShaderEditorSettings();

        m_ScriptBuilder.SetProject(ScriptEngine::GetProjectScriptsProject());
        if (ScriptEngine::IsInitialized())
        {
            ScriptEngine::LoadProjectAssembly();
            BuildScriptsIfNeverBuilt();
        }

        const AssetHandle startScene = project ? project->GetConfig().StartScene : AssetHandle(0);
        if (startScene != 0)
            OpenScene(startScene);
        else if (hadProject)
            NewScene();

        if (m_CreateBasicScene && project && project->GetConfig().StartScene == 0)
            CreateBasicScene();
        m_CreateBasicScene = false;

        {
            std::error_code error;
            m_ThumbnailPending = !std::filesystem::exists(ProjectLibrary::ThumbnailPath(Project::GetActiveProjectDirectory()), error);
            m_ThumbnailTimer = 0.0f;
        }
        m_Commands.Load(KeyBindingsPath()); // (this project's key bindings)
        m_ProjectSettings.MarkDirty();       // (its rows read the project that is open)
        m_ContentBrowserPanel.Refresh();
        if (project)
            Notify(UI::UINotificationSeverity::Success, "Project opened", project->GetConfig().Name);
        return true;
    }

    bool EditorV2Layer::OpenStartupProject(const std::filesystem::path& failed)
    {
        const std::string message = failed.empty()
            ? std::string("No project is open. Open an existing project (.nproj) or make a new one.")
            : "The project " + failed.generic_string() + " could not be opened (the log says why).\n"
              "Open another project (.nproj) or make a new one.";
        while (true)
        {
            const SDL_MessageBoxButtonData buttons[] = {
                { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Open Project..." },
                { 0, 1, "New Project..." },
                { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Quit" },
            };
            SDL_MessageBoxData box{};
            box.flags = SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
            box.window = Application::Get().getWindow()->getHandle();
            box.title = "Nox Editor";
            box.message = message.c_str();
            box.numbuttons = SDL_arraysize(buttons);
            box.buttons = buttons;
            int choice = 2;
            if (!SDL_ShowMessageBox(&box, &choice) || choice == 2)
                return false;
            if (choice == 0 ? OpenProject() : NewProject())
                return true;
        }
    }

    void EditorV2Layer::OpenProjectManager()
    {
        const char* base = SDL_GetBasePath();
        const ApplicationCommandLineArgs& args = Application::Get().GetSpecification().CommandLineArgs;
        std::filesystem::path fileName = args.Count > 0 && args.Args[0]
            ? std::filesystem::path(args.Args[0]).filename()
            : std::filesystem::path("NoxEditorV2");
#ifdef _WIN32
        if (!fileName.has_extension())
            fileName += ".exe";
#endif
        const std::filesystem::path executable = std::filesystem::path(base ? base : "") / fileName;
        std::vector<std::string> arguments{ "--project-manager" };
        for (int index = 1; index < args.Count; ++index)
        {
            if (ApplicationCommandLineArgs::IsSwitch(args.Args[index]) &&
                std::string_view(args.Args[index]) != "--project-manager")
                arguments.emplace_back(args.Args[index]);
        }
        std::string error;
        if (!LaunchDetachedProcess(executable, arguments, error))
        {
            NOX_CORE_ERROR("Project Manager: cannot start {}: {}", executable.generic_string(), error);
            return;
        }
        NOX_CORE_INFO("Project Manager: started {} --project-manager; this editor closes", executable.generic_string());
        Application::Get().Shutdown();
    }

    void EditorV2Layer::NewScene()
    {
        CancelHLODBuild("new scene");
        if (m_SceneState != SceneState::Edit)
            OnSceneStop();
        SaveEditorCameras();
        m_HoveredEntity = Entity();
        m_EditorScene = CreateRef<Scene>();
        m_ActiveScene = m_EditorScene;
        SetSceneContext(m_ActiveScene);
        m_EditorScenePath.clear();
        m_UnloadUnusedAssetsRequested = true;
        m_SceneCleanPending = true;
#if NOX_PROFILE_STATS
        Profiler::Get().ResetStats();
#endif
    }

    void EditorV2Layer::OpenScene(AssetHandle handle)
    {
        CancelHLODBuild("another scene opened");
        if (handle == 0 || !Project::GetActive())
            return;
        if (m_SceneState != SceneState::Edit)
            OnSceneStop();
        const std::filesystem::path rel = Project::GetActive()->GetEditorAssetManager()->GetFilePath(handle);
        if (rel.empty())
            return;
        m_SceneLoad = CreateScope<SceneLoader>(Project::GetActiveAssetDirectory() / rel);
        m_SceneLoadHandle = handle;
    }

    void EditorV2Layer::FinishSceneLoad()
    {
        Scope<SceneLoader> load = std::move(m_SceneLoad);
        const AssetHandle handle = std::exchange(m_SceneLoadHandle, AssetHandle(0));
        if (!load || !Project::GetActive())
            return;
        if (load->Failed())
        {
            Notify(UI::UINotificationSeverity::Error, "Scene not opened",
                   Project::GetActive()->GetEditorAssetManager()->GetFilePath(handle).filename().string() + " could not be read -- see the Output Log.");
            return;
        }
        if (m_SceneState != SceneState::Edit)
            OnSceneStop();

        Ref<Scene> newScene = load->GetScene();
        newScene->LoadSubLevelsForEditing();
        if (m_ViewportImage)
        {
            const UI::UIRect& bounds = m_ViewportImage->GetBounds();
            const uint32_t w = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Width())));
            const uint32_t h = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Height())));
            if (w > 0 && h > 0)
                newScene->OnViewportResize(w, h);
        }

        SaveEditorCameras();
        m_HoveredEntity = Entity();
        m_EditorScene = newScene;
        m_ActiveScene = m_EditorScene;
        m_EditorScenePath = Project::GetActive()->GetEditorAssetManager()->GetFilePath(handle);
        SetSceneContext(m_ActiveScene);
        RefreshSceneLoadOverlay();

        const bool cameraRestored = RestoreEditorCameras();
        if (!cameraRestored && !m_FocusOnOpen && m_EditorScene->IsWorldPartitioned() && !m_EditorScene->GetWorldPartition().Cells.empty())
        {
            glm::dvec3 lowest(std::numeric_limits<double>::max()), highest(std::numeric_limits<double>::lowest());
            for (const auto& [coord, cell] : m_EditorScene->GetWorldPartition().Cells)
            {
                lowest = glm::min(lowest, cell.BoundsMin);
                highest = glm::max(highest, cell.BoundsMax);
            }
            m_FocusOnOpen = (lowest + highest) * 0.5;
        }
        if (m_FocusOnOpen)
        {
            m_EditorCamera.SetFocalPoint(*m_FocusOnOpen);
            m_EditorCamera.SetDistance(WorldUnits::FromMeters(180.0f));
            m_EditorCamera.SetOrientation(0.45f, 0.0f);
            m_FocusOnOpen.reset();
        }
        if (m_EditorScene->IsWorldPartitioned())
        {
            m_EditorScene->LoadWorldCells(
                m_EditorScene->WorldCellsAround(
                    m_EditorCamera.GetFocalPoint(),
                    WorldUnits::FromMeters(m_EditorScene->GetWorldPartition().Settings.LoadingRange)
                )
            );
        }
        m_UnloadUnusedAssetsRequested = true;
#if NOX_PROFILE_STATS
        Profiler::Get().ResetStats();
#endif
        m_SceneCleanPending = true;
        Notify(UI::UINotificationSeverity::Info, "Scene opened", m_EditorScenePath.filename().string());
    }

    void EditorV2Layer::SaveScene(std::function<void(bool saved)> then)
    {
        if (!m_ActiveScene || !Project::GetActive())
        {
            if (then)
                then(false);
            return;
        }
        if (AudioMixer::IsDirty())
            AudioMixer::Save();
        if (m_PrefabMode.Active)
        {
            // Ctrl+S in Prefab Mode saves the prefab, never the level.
            const bool saved = SavePrefabMode();
            if (then)
                then(saved);
            return;
        }
        if (m_EditorScenePath.empty())
        {
            SaveSceneAs(std::move(then)); // (never saved: named first, in a dialog that does not stop the editor)
            return;
        }
        const std::filesystem::path fullPath = m_EditorScenePath.is_absolute()
            ? m_EditorScenePath
            : Project::GetActiveAssetDirectory() / m_EditorScenePath;
        std::error_code error;
        std::filesystem::create_directories(fullPath.parent_path(), error);
        const auto before = std::filesystem::last_write_time(fullPath, error);
        const bool existed = !error;
        SerializeScene(m_ActiveScene, fullPath);
        const auto after = std::filesystem::last_write_time(fullPath, error);
        if (error || (existed && after == before))
        {
            Notify(UI::UINotificationSeverity::Error, "Scene not saved", fullPath.filename().string() + " could not be written -- see the Output Log.");
            if (then)
                then(false);
            return;
        }
        AdoptStartSceneIfNone(fullPath);
        CaptureProjectThumbnail();
        m_SceneDirty = false;
        m_HistoryPositionSeen = m_History.GetPosition();
        Notify(UI::UINotificationSeverity::Success, "Scene saved", m_EditorScenePath.generic_string());
        if (then)
            then(true);
    }

    void EditorV2Layer::SaveSceneAs(std::function<void(bool saved)> then)
    {
        if (!m_ActiveScene || !Project::GetActive())
        {
            if (then)
                then(false);
            return;
        }
        if (m_PrefabMode.Active)
        {
            const bool saved = SavePrefabMode(); // (a prefab is saved where it is; Duplicate in the Content Browser copies it)
            if (then)
                then(saved);
            return;
        }
        // The dialog does not stop the editor (E7); the scene is written when it answers.
        Utility::SaveFileAsync("Nox Scene *.nox\0nox\0", [this, then](std::string chosen)
        {
            const bool saved = !chosen.empty() && SaveSceneTo(chosen);
            if (then)
                then(saved);
        });
    }

    bool EditorV2Layer::SaveSceneTo(std::filesystem::path path)
    {
        if (!m_ActiveScene || !Project::GetActive() || path.empty() || m_PrefabMode.Active)
            return false;
        m_AssetEditors.EndPreviews();
        if (!path.has_extension())
            path += ".nox";
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        m_ActiveScene->CopyWorldCellsTo(path);
        SerializeScene(m_ActiveScene, path);
        if (!std::filesystem::exists(path, error))
        {
            Notify(UI::UINotificationSeverity::Error, "Scene not saved", path.filename().string() + " could not be written -- see the Output Log.");
            return false;
        }

        const std::filesystem::path rel = std::filesystem::relative(path, Project::GetActiveAssetDirectory(), error);
        if (!error && !rel.empty() && !rel.native().starts_with(std::filesystem::path("..").native()))
            m_EditorScenePath = rel;
        else
            m_EditorScenePath = path;

        AdoptStartSceneIfNone(path);
        CaptureProjectThumbnail();
        m_ContentBrowserPanel.Refresh();
        m_SceneDirty = false;
        m_HistoryPositionSeen = m_History.GetPosition();
        Notify(UI::UINotificationSeverity::Success, "Scene saved", m_EditorScenePath.generic_string());
        return true;
    }

    void EditorV2Layer::SerializeScene(const Ref<Scene>& scene, const std::filesystem::path& path)
    {
        SceneImporter::SaveScene(scene, path);
    }

    void EditorV2Layer::PlaceAssets(const std::vector<AssetHandle>& handles, const glm::dvec3& point, const std::string& folder)
    {
        if (!m_ActiveScene || !Project::GetActive())
            return;
        m_Selection.Clear();
        Entity lastCreated;
        for (AssetHandle handle : handles)
        {
            const AssetType type = AssetManager::GetAssetType(handle);
            if (type == AssetType::Prefab && m_PrefabMode.Active && handle == m_PrefabMode.Prefab)
            {
                // (an instance of the prefab inside itself would spawn itself without end)
                Notify(UI::UINotificationSeverity::Warning, "Not placed", "A prefab cannot contain an instance of itself.");
                continue;
            }
            if (type == AssetType::Prefab)
            {
                const AssetMetadata& prefabMetadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
                std::string prefabName = prefabMetadata.FilePath.filename().stem().string();
                Entity prefabRoot = m_ActiveScene->CreateEntity(prefabName.empty() ? "Prefab" : prefabName);
                auto& prefabInstance = prefabRoot.AddComponent<PrefabInstanceComponent>();
                prefabInstance.Prefab = handle;
                prefabInstance.InitTransform = true;
                prefabRoot.GetComponent<TransformComponent>().Translation = point;
                if (!folder.empty())
                    prefabRoot.AddComponent<FolderComponent>(folder);
                prefabRoot.MarkTransformDirty();
                AssetManager::RequestAsset(handle);
                lastCreated = prefabRoot;
                continue;
            }
            if (type != AssetType::Mesh && type != AssetType::StaticMesh)
                continue;

            const AssetMetadata& metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
            std::string entityName = metadata.FilePath.filename().stem().string();
            Entity root = m_ActiveScene->CreateEntity(entityName.empty() ? "Model" : entityName);
            auto& instance = root.AddComponent<ModelInstanceComponent>();
            instance.Model = handle;
            instance.AtFileLayout = true;
            root.GetComponent<TransformComponent>().Translation = point;
            if (!folder.empty())
                root.AddComponent<FolderComponent>(folder);
            root.MarkTransformDirty();
            AssetManager::RequestAsset(handle);
            lastCreated = root;
        }
        if (lastCreated)
            m_Selection.Select(lastCreated);
        m_SceneHierarchyPanel.MarkDirty();
        m_HistoryCheckRequested = true;
    }

    void EditorV2Layer::CreateShape(PrimitiveShape shape, const std::string& folder)
    {
        if (!m_ActiveScene)
            return;
        const AssetHandle mesh = GetShapeAsset(shape);
        if (mesh == 0)
            return;
        PlaceAssets({ mesh }, m_EditorCamera.GetPosition() + glm::dvec3(m_EditorCamera.GetForwardDirection() * m_EditorCamera.GetDistance()), folder);
    }

    void EditorV2Layer::FocusOnEntity(Entity entity)
    {
        if (!entity || !m_ActiveScene)
            return;

        glm::dvec3 centre;
        glm::vec3 boxSize{ 0.0f };
        const WorldBounds bounds = ComputeWorldBounds(*m_ActiveScene, entity);
        if (bounds.Valid)
        {
            centre = bounds.Center();
            boxSize = bounds.Size();
        }
        else if (entity.HasComponent<WorldTransformComponent>())
        {
            centre = entity.GetComponent<WorldTransformComponent>().GetPosition();
        }
        else
        {
            return;
        }

        m_ViewportTools.ViewTurn.Active = false;

        if (m_EditorCamera.IsOrthographic())
        {
            const float orthoRadius = bounds.Valid
                ? std::max({ boxSize.x, boxSize.y, boxSize.z }) * 0.5f
                : 0.0f;
            m_EditorCamera.Focus(centre, orthoRadius);
            return;
        }

        const float tanHalfFov = std::max(std::tan(glm::radians(m_EditorCamera.GetFOV()) * 0.5f), 0.01f);
        const float aspect = std::max(m_EditorCamera.GetAspectRatio(), 0.25f);
        float focusDist = WorldUnits::FromMeters(2.2f);
        if (bounds.Valid)
        {
            const float distV = (0.5f * boxSize.y * 1.18f) / tanHalfFov;
            const float distH = (0.5f * std::max(boxSize.x, boxSize.z) * 1.18f) / (tanHalfFov * aspect);
            focusDist = std::max({ distV, distH, WorldUnits::FromCentimeters(45.0f) });
        }
        const double entityRadius = bounds.Valid ? 0.5 * static_cast<double>(glm::length(boxSize)) : 0.0;

        // Avoid placing the camera behind an occluding mesh (e.g. looking sideways through a nearby ramp/wall).
        std::unordered_set<UUID> excluded;
        std::function<void(Entity)> excludeSubtree = [&](Entity e)
        {
            excluded.insert(e.GetUUID());
            if (!e.HasComponent<RelationshipComponent>())
                return;
            for (UUID cid : e.GetComponent<RelationshipComponent>().Children)
            {
                if (Entity c = m_ActiveScene->GetEntityByUUID(cid))
                    excludeSubtree(c);
            }
        };
        excludeSubtree(entity);

        struct Occluder
        {
            glm::dmat4 InvWorld;
            glm::vec3 LocalMin;
            glm::vec3 LocalMax;
        };
        std::vector<Occluder> occluders;
        auto meshView = m_ActiveScene->GetAllEntitiesWith<MeshComponent, WorldTransformComponent>();
        for (auto handle : meshView)
        {
            Entity cand(handle, m_ActiveScene.get());
            if (!cand || excluded.contains(cand.GetUUID()) || !m_ActiveScene->IsActiveInHierarchy(cand))
                continue;
            const WorldBounds wb = ComputeWorldBounds(*m_ActiveScene, cand, false);
            if (!wb.Valid)
                continue;
            if (wb.Max.y <= (bounds.Valid ? bounds.Min.y + boxSize.y * 0.15 : centre.y))
                continue;
            glm::vec3 lMin, lMax;
            if (!ComputeLocalMeshBounds(cand, lMin, lMax))
                continue;
            const glm::dmat4& w = cand.GetComponent<WorldTransformComponent>().WorldMatrix;
            if (std::abs(glm::determinant(w)) < 1e-12)
                continue;
            occluders.push_back({ glm::inverse(w), lMin, lMax });
        }

        auto clearFraction = [&](float pitch, float yaw, float dist) -> double
        {
            const glm::quat q(glm::vec3(-pitch, -yaw, 0.0f));
            const glm::vec3 fwd = glm::rotate(q, glm::vec3(0.0f, 0.0f, -1.0f));
            const glm::dvec3 camPos = centre - glm::dvec3(fwd) * static_cast<double>(dist);
            const double startT = (dist > 1e-3f) ? std::clamp((entityRadius * 0.55) / static_cast<double>(dist), 0.02, 0.5) : 0.02;

            double bestT = 1.0;
            for (const Occluder& occ : occluders)
            {
                const glm::dvec3 oA = glm::dvec3(occ.InvWorld * glm::dvec4(centre, 1.0));
                const glm::dvec3 oB = glm::dvec3(occ.InvWorld * glm::dvec4(camPos, 1.0));
                const glm::dvec3 oD = oB - oA;

                double tMin = startT;
                double tMax = bestT;
                bool hit = true;
                for (int axis = 0; axis < 3; ++axis)
                {
                    const double b0 = static_cast<double>(occ.LocalMin[axis]);
                    const double b1 = static_cast<double>(occ.LocalMax[axis]);
                    if (std::abs(oD[axis]) < 1e-9)
                    {
                        if (oA[axis] < b0 || oA[axis] > b1) { hit = false; break; }
                    }
                    else
                    {
                        double t1 = (b0 - oA[axis]) / oD[axis];
                        double t2 = (b1 - oA[axis]) / oD[axis];
                        if (t1 > t2) std::swap(t1, t2);
                        tMin = std::max(tMin, t1);
                        tMax = std::min(tMax, t2);
                        if (tMin > tMax) { hit = false; break; }
                    }
                }
                if (hit && tMin < bestT)
                    bestT = tMin;
            }
            return bestT;
        };

        float targetPitch = m_EditorCamera.GetPitch();
        float targetYaw = m_EditorCamera.GetYaw();
        double bestFrac = clearFraction(targetPitch, targetYaw, focusDist);
        if (bestFrac < 0.999)
        {
            float frontYaw = targetYaw;
            if (entity.HasComponent<WorldTransformComponent>())
            {
                const glm::mat4 w = m_Renderer
                    ? m_Renderer->ToRenderSpace(entity.GetComponent<WorldTransformComponent>().WorldMatrix)
                    : glm::mat4(entity.GetComponent<WorldTransformComponent>().WorldMatrix);
                const glm::vec3 entFwd = glm::vec3(w[2]);
                if (glm::length(glm::vec2(entFwd.x, entFwd.z)) > 1e-4f)
                    frontYaw = -std::atan2(entFwd.x, entFwd.z);
            }

            const float candidatePitches[] = { std::clamp(targetPitch, 0.08f, 0.45f), 0.22f, 0.45f };
            const float yawOffsets[] = { 0.0f, 0.55f, -0.55f, 1.1f, -1.1f, 1.57f, -1.57f, 2.2f, -2.2f, 3.14159f };
            float bestCandidatePitch = targetPitch;
            float bestCandidateYaw = targetYaw;
            double bestCandidateFrac = bestFrac;

            auto tryCandidates = [&](float baseYaw)
            {
                for (float p : candidatePitches)
                {
                    for (float dy : yawOffsets)
                    {
                        const float y = baseYaw + dy;
                        const double f = clearFraction(p, y, focusDist);
                        if (f > bestCandidateFrac)
                        {
                            bestCandidateFrac = f;
                            bestCandidatePitch = p;
                            bestCandidateYaw = y;
                            if (bestCandidateFrac >= 0.999)
                                return true;
                        }
                    }
                }
                return false;
            };

            if (!tryCandidates(targetYaw))
                tryCandidates(frontYaw);

            if (bestCandidateFrac > bestFrac + 0.05)
            {
                targetPitch = bestCandidatePitch;
                targetYaw = bestCandidateYaw;
                bestFrac = bestCandidateFrac;
                m_EditorCamera.SetOrientation(targetPitch, targetYaw);
            }
            if (bestFrac < 0.999)
                focusDist = std::max(focusDist * static_cast<float>(bestFrac) * 0.88f, WorldUnits::FromCentimeters(40.0f));
        }

        m_EditorCamera.SetFocalPoint(centre);
        m_EditorCamera.SetDistance(focusDist);
    }

    void EditorV2Layer::SnapSelectionToFloor()
    {
        if (!m_ActiveScene || !m_Renderer)
            return;
        const auto& selection = m_Selection.GetEntities();
        if (selection.empty())
            return;

        std::vector<Entity> tops;
        for (Entity entity : selection)
        {
            if (!entity)
                continue;
            const bool parentSelected = entity.HasComponent<RelationshipComponent>() &&
                                        entity.GetComponent<RelationshipComponent>().Parent != 0 &&
                                        m_Selection.Contains(m_ActiveScene->GetEntityByUUID(entity.GetComponent<RelationshipComponent>().Parent));
            if (!parentSelected)
                tops.push_back(entity);
        }

        std::unordered_set<UUID> excluded;
        std::function<void(Entity)> exclude = [&](Entity entity)
        {
            excluded.insert(entity.GetUUID());
            if (!entity.HasComponent<RelationshipComponent>())
                return;
            for (UUID childID : entity.GetComponent<RelationshipComponent>().Children)
            {
                if (Entity child = m_ActiveScene->GetEntityByUUID(childID))
                    exclude(child);
            }
        };
        for (Entity top : tops)
            exclude(top);

        for (Entity top : tops)
        {
            const WorldBounds mover = ComputeWorldBounds(*m_ActiveScene, top);
            if (!mover.Valid)
                continue;
            const double centreY = 0.5 * (mover.Min.y + mover.Max.y);

            double floorY = 0.0;
            bool found = false;
            for (auto handle : m_ActiveScene->GetAllEntitiesWith<MeshComponent, WorldTransformComponent>())
            {
                Entity candidate(handle, m_ActiveScene.get());
                if (excluded.contains(candidate.GetUUID()))
                    continue;
                const WorldBounds bounds = ComputeWorldBounds(*m_ActiveScene, candidate, false);
                if (!bounds.Valid)
                    continue;
                const bool overlapsX = bounds.Min.x < mover.Max.x && bounds.Max.x > mover.Min.x;
                const bool overlapsZ = bounds.Min.z < mover.Max.z && bounds.Max.z > mover.Min.z;
                if (!overlapsX || !overlapsZ || bounds.Max.y > centreY)
                    continue;
                if (!found || bounds.Max.y > floorY)
                {
                    floorY = bounds.Max.y;
                    found = true;
                }
            }

            struct SnapRays
            {
                UUID Entity = 0;
                double BoundsFloor = 0.0;
                double OriginY = 0.0;
                int Pending = 0;
                bool Hit = false;
                float NearestDistance = 0.0f;
            };
            auto snap = std::make_shared<SnapRays>();
            snap->Entity = top.GetUUID();
            snap->BoundsFloor = floorY;
            snap->OriginY = mover.Min.y - WorldUnits::FromMeters(0.01f);
            const glm::dvec3 centre = mover.Center();
            const glm::dvec3 halfInset = 0.25 * (mover.Max - mover.Min);
            const glm::vec2 offsets[5] = { { 0.0f, 0.0f }, { -1.0f, -1.0f }, { 1.0f, -1.0f }, { -1.0f, 1.0f }, { 1.0f, 1.0f } };
            snap->Pending = 5;
            for (const glm::vec2& offset : offsets)
            {
                const glm::dvec3 origin(centre.x + offset.x * halfInset.x, snap->OriginY, centre.z + offset.y * halfInset.z);
                m_Renderer->RequestRaycast(origin, glm::vec3(0.0f, -1.0f, 0.0f), WorldUnits::FromMeters(1000.0f), [this, snap](bool hit, float distance)
                {
                    if (hit && (!snap->Hit || distance < snap->NearestDistance))
                    {
                        snap->Hit = true;
                        snap->NearestDistance = distance;
                    }
                    if (--snap->Pending > 0)
                        return;
                    Entity entity = m_ActiveScene ? m_ActiveScene->GetEntityByUUID(snap->Entity) : Entity{};
                    if (!entity || !entity.HasComponent<WorldTransformComponent>())
                        return;
                    const WorldBounds now = ComputeWorldBounds(*m_ActiveScene, entity);
                    if (!now.Valid)
                        return;
                    const double floor = snap->Hit ? snap->OriginY - snap->NearestDistance : snap->BoundsFloor;
                    const double drop = floor - now.Min.y;
                    entity.SetWorldTransform(glm::translate(glm::dmat4(1.0), glm::dvec3(0.0, drop, 0.0)) *
                                             entity.GetComponent<WorldTransformComponent>().WorldMatrix);
                    m_HistoryCheckRequested = true;
                });
            }
        }
    }

    void EditorV2Layer::SnapSelectionToGrid()
    {
        if (!m_ActiveScene)
            return;
        const float step = m_ViewportTools.LocationSnapWorldUnits();
        if (step <= 0.0f)
            return;
        for (Entity entity : m_Selection.GetEntities())
        {
            if (!entity || !entity.HasComponent<WorldTransformComponent>())
                continue;
            const bool parentSelected = entity.HasComponent<RelationshipComponent>() &&
                                        entity.GetComponent<RelationshipComponent>().Parent != 0 &&
                                        m_Selection.Contains(m_ActiveScene->GetEntityByUUID(entity.GetComponent<RelationshipComponent>().Parent));
            if (parentSelected)
                continue;
            glm::dmat4 world = entity.GetComponent<WorldTransformComponent>().WorldMatrix;
            const glm::dvec3 position(world[3]);
            const double gridStep = step;
            world[3] = glm::dvec4(glm::round(position / gridStep) * gridStep, 1.0);
            entity.SetWorldTransform(world);
        }
        m_HistoryCheckRequested = true;
    }

    AssetHandle EditorV2Layer::GetShapeAsset(PrimitiveShape shape)
    {
        Ref<Project> project = Project::GetActive();
        if (!project)
            return 0;

        auto assetManager = project->GetEditorAssetManager();
        const std::string name = PrimitiveShapes::GetName(shape);
        const std::filesystem::path meshPath = std::filesystem::path("Shapes") / (name + ".nsmesh");
        if (const AssetHandle existing = assetManager->FindHandleByPath(meshPath, AssetType::StaticMesh); existing != 0)
            return existing;

        const std::filesystem::path sourcePath = std::filesystem::path("Shapes") / (name + ".gltf");
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        if (!std::filesystem::exists(assetDirectory / sourcePath) && PrimitiveShapes::WriteGltf(shape, assetDirectory / "Shapes").empty())
        {
            NOX_CORE_ERROR("Could not write the {} shape into {}", name, (assetDirectory / "Shapes").generic_string());
            return 0;
        }

        MeshImportSettings settings;
        settings.ImportSkeletalMeshes = false;
        settings.ImportAnimations = false;
        settings.StaticCombine = MeshCombineMode::CombineAll;
        settings.ImportScale = WorldUnits::PerSourceUnit(WorldUnits::SourceUnit::Meters);
        if (!assetManager->ImportModel(sourcePath, meshPath, settings))
            return 0;
        return assetManager->FindHandleByPath(meshPath, AssetType::StaticMesh);
    }

    AssetHandle EditorV2Layer::RegisterSceneAsset(const std::filesystem::path& scenePath)
    {
        Ref<Project> project = Project::GetActive();
        if (!project || scenePath.empty())
            return 0;
        auto assets = project->GetEditorAssetManager();
        std::error_code error;
        const std::filesystem::path relative = std::filesystem::relative(scenePath, Project::GetActiveAssetDirectory(), error);
        if (error || relative.empty() || relative.native().starts_with(std::filesystem::path("..").native()))
            return 0;
        AssetHandle handle = assets->FindHandleByPath(relative, AssetType::Scene);
        if (handle == 0)
        {
            assets->ImportAsset(relative, relative, AssetType::Scene);
            handle = assets->FindHandleByPath(relative, AssetType::Scene);
        }
        return handle;
    }

    void EditorV2Layer::AdoptStartSceneIfNone(const std::filesystem::path& scenePath)
    {
        Ref<Project> project = Project::GetActive();
        if (!project || project->GetConfig().StartScene != 0)
            return;
        if (const AssetHandle handle = RegisterSceneAsset(scenePath); handle != 0)
        {
            project->GetConfig().StartScene = handle;
            Project::SaveActive();
            NOX_CORE_INFO("Start Scene: {} (the project opens with it; Settings > Start Scene changes it)", scenePath.filename().string());
        }
    }

    void EditorV2Layer::CreateBasicScene()
    {
        NewScene();
        Scene& scene = *m_EditorScene;

        const std::string lighting = "Lighting";
        scene.AddFolder(lighting);

        Entity sky = scene.CreateEntity("Sky");
        sky.AddComponent<EnvironmentLightComponent>().Source = EnvironmentSource::ProceduralSky;
        sky.AddComponent<FolderComponent>(lighting);

        Entity sun = scene.CreateEntity("Sun");
        sun.AddComponent<FolderComponent>(lighting);
        sun.AddComponent<DirectionalLightComponent>().Intensity = DefaultSkySunIntensity;
        sun.GetComponent<TransformComponent>().Rotation = glm::eulerAngles(DefaultSkySunRotation());
        sun.MarkTransformDirty();

        if (const AssetHandle plane = GetShapeAsset(PrimitiveShape::Plane); plane != 0)
        {
            Entity floor = scene.CreateEntity("Floor");
            auto& instance = floor.AddComponent<ModelInstanceComponent>();
            instance.Model = plane;
            instance.AtFileLayout = true;
            floor.GetComponent<TransformComponent>().Scale = glm::vec3(50.0f, 1.0f, 50.0f);
            floor.MarkTransformDirty();
            floor.AddComponent<RigidBody3DComponent>().Type = RigidBody3DComponent::BodyType::Static;
            auto& box = floor.AddComponent<BoxCollider3DComponent>();
            box.HalfExtents = glm::vec3(WorldUnits::FromMeters(0.5f), WorldUnits::FromMeters(0.05f), WorldUnits::FromMeters(0.5f));
            box.Offset = glm::vec3(0.0f, -WorldUnits::FromMeters(0.05f), 0.0f);
            AssetManager::RequestAsset(plane);
        }

        Entity camera = scene.CreateEntity("Camera");
        camera.AddComponent<CameraComponent>().Primary = true;
        auto& cameraTransform = camera.GetComponent<TransformComponent>();
        cameraTransform.Translation = glm::dvec3(0.0, WorldUnits::FromMeters(2.0f), WorldUnits::FromMeters(8.0f));
        cameraTransform.Rotation = glm::vec3(glm::radians(-12.0f), 0.0f, 0.0f);
        camera.MarkTransformDirty();

        const std::filesystem::path path = Project::GetActiveAssetDirectory() / "Scenes" / "Main.nox";
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        SerializeScene(m_EditorScene, path);
        m_EditorScenePath = path;
        AdoptStartSceneIfNone(path);
        SetSceneContext(m_ActiveScene);
        m_HistoryCheckRequested = true;
        m_ApplyBasicSceneView = true;
        ApplyBasicSceneView();
    }

    void EditorV2Layer::ApplyBasicSceneView()
    {
        m_EditorCamera.SetFocalPoint(glm::dvec3(0.0));
        m_EditorCamera.SetDistance(WorldUnits::FromMeters(15.0f));
        m_EditorCamera.SetOrientation(glm::radians(25.0f), glm::radians(35.0f));
    }

    void EditorV2Layer::CaptureProjectThumbnail()
    {
        Ref<Project> project = Project::GetActive();
        Ref<Texture2D> image = m_Renderer ? m_Renderer->GetSceneResourceRef() : nullptr;
        if (!project || !image || image->GetWidth() < 16 || image->GetHeight() < 16)
            return;
        const std::filesystem::path file = ProjectLibrary::ThumbnailPath(Project::GetActiveProjectDirectory());
        const bool bgra = m_Renderer->IsSurfaceFormatBGRA();
        m_Renderer->ReadbackTexture(std::move(image), [file, bgra](std::vector<uint8_t> pixels, uint32_t width, uint32_t height)
        {
            if (pixels.size() != static_cast<size_t>(width) * height * 4 || width == 0 || height == 0)
                return;
            constexpr uint32_t outWidth = 384, outHeight = 216;
            uint32_t cropWidth = width, cropHeight = static_cast<uint32_t>(width * 9ull / 16ull);
            if (cropHeight > height)
            {
                cropHeight = height;
                cropWidth = static_cast<uint32_t>(height * 16ull / 9ull);
            }
            const uint32_t x0 = (width - cropWidth) / 2, y0 = (height - cropHeight) / 2;
            std::vector<uint8_t> out(static_cast<size_t>(outWidth) * outHeight * 4);
            for (uint32_t y = 0; y < outHeight; ++y)
            {
                for (uint32_t x = 0; x < outWidth; ++x)
                {
                    const uint32_t sx0 = x0 + x * cropWidth / outWidth, sx1 = std::max(sx0 + 1, x0 + (x + 1) * cropWidth / outWidth);
                    const uint32_t sy0 = y0 + y * cropHeight / outHeight, sy1 = std::max(sy0 + 1, y0 + (y + 1) * cropHeight / outHeight);
                    uint32_t sum[3] = { 0, 0, 0 }, count = 0;
                    for (uint32_t sy = sy0; sy < sy1 && sy < height; ++sy)
                    {
                        for (uint32_t sx = sx0; sx < sx1 && sx < width; ++sx)
                        {
                            const uint8_t* texel = &pixels[(static_cast<size_t>(sy) * width + sx) * 4];
                            sum[0] += texel[0];
                            sum[1] += texel[1];
                            sum[2] += texel[2];
                            ++count;
                        }
                    }
                    uint8_t* target = &out[(static_cast<size_t>(y) * outWidth + x) * 4];
                    count = std::max(count, 1u);
                    target[0] = static_cast<uint8_t>(sum[bgra ? 2 : 0] / count);
                    target[1] = static_cast<uint8_t>(sum[1] / count);
                    target[2] = static_cast<uint8_t>(sum[bgra ? 0 : 2] / count);
                    target[3] = 255;
                }
            }
            std::error_code error;
            std::filesystem::create_directories(file.parent_path(), error);
            if (!stbi_write_png(file.string().c_str(), outWidth, outHeight, 4, out.data(), outWidth * 4))
                NOX_CORE_WARN("Project thumbnail: cannot write {}", file.generic_string());
        });
    }

    void EditorV2Layer::SaveEditorCameras()
    {
        if (!Project::GetActive() || m_EditorScenePath.empty())
            return;
        const std::string scene = m_EditorScenePath.generic_string();
        std::vector<std::string> lines = ReadEditorCameraLines();
        std::erase_if(lines, [&](const std::string& line) { return line.rfind(scene + "|", 0) == 0; });
        for (int index = 0; index < kMaxViewports; ++index)
        {
            const ViewportSlot& slot = m_Viewports[index];
            if (index != m_ActiveViewport && !slot.CameraSet)
                continue;
            const EditorCamera& camera = index == m_ActiveViewport ? m_EditorCamera : slot.Camera;
            const glm::dvec3 focal = camera.GetFocalPoint();
            char line[512];
            std::snprintf(line, sizeof(line), "%s|%d|%.17g %.17g %.17g %.6g %.6g %.6g %d %.6g",
                          scene.c_str(), index, focal.x, focal.y, focal.z,
                          camera.GetDistance(), camera.GetPitch(), camera.GetYaw(),
                          static_cast<int>(camera.GetViewMode()), camera.GetOrthoHalfHeight());
            lines.push_back(line);
        }
        std::error_code error;
        std::filesystem::create_directories(EditorCamerasFile().parent_path(), error);
        std::ofstream out(EditorCamerasFile(), std::ios::trunc);
        for (const std::string& entry : lines)
            out << entry << "\n";
    }

    bool EditorV2Layer::RestoreEditorCameras()
    {
        bool activeRestored = false;
        if (!Project::GetActive() || m_EditorScenePath.empty())
            return activeRestored;
        const std::string prefix = m_EditorScenePath.generic_string() + "|";
        for (const std::string& line : ReadEditorCameraLines())
        {
            if (line.rfind(prefix, 0) != 0)
                continue;
            int index = -1, mode = 0;
            glm::dvec3 focal(0.0);
            float distance = 0.0f, pitch = 0.0f, yaw = 0.0f, orthoHalfHeight = 10.0f;
            if (sscanf(line.c_str() + prefix.size(), "%d|%lf %lf %lf %f %f %f %d %f",
                       &index, &focal.x, &focal.y, &focal.z, &distance, &pitch, &yaw,
                       &mode, &orthoHalfHeight) != 9 || index < 0 || index >= kMaxViewports ||
                mode < 0 || mode > static_cast<int>(EditorViewMode::Right))
                continue;
            EditorCamera& camera = index == m_ActiveViewport ? m_EditorCamera : m_Viewports[index].Camera;
            camera.SetViewMode(static_cast<EditorViewMode>(mode));
            camera.SetFocalPoint(focal);
            camera.SetDistance(distance);
            camera.SetOrientation(pitch, yaw);
            if (camera.IsOrthographic())
                camera.SetOrthoHalfHeight(orthoHalfHeight);
            m_Viewports[index].CameraSet = true;
            activeRestored |= (index == m_ActiveViewport);
        }
        return activeRestored;
    }

    void EditorV2Layer::WriteMaterialShaderEditorSettings()
    {
        const std::filesystem::path projectDirectory = Project::GetActiveProjectDirectory();
        const std::filesystem::path shaderFolder = std::filesystem::absolute("assets/shaders").lexically_normal();
        CustomLitShaders::WriteCodeEditorConfig(projectDirectory, shaderFolder);

        const std::filesystem::path settingsPath = projectDirectory / ".vscode" / "settings.json";
        const std::string folder = shaderFolder.generic_string();
        nlohmann::json settings = nlohmann::json::object();
        std::error_code error;
        if (std::filesystem::exists(settingsPath, error))
        {
            std::ifstream file(settingsPath, std::ios::binary);
            std::stringstream text;
            text << file.rdbuf();
            settings = nlohmann::json::parse(text.str(), nullptr, false);
            if (settings.is_discarded() || !settings.is_object())
                return;
        }
        nlohmann::json& paths = settings["slang.additionalSearchPaths"];
        if (!paths.is_array())
            paths = nlohmann::json::array();
        for (const nlohmann::json& path : paths)
        {
            if (path.is_string() && path.get<std::string>() == folder)
                return;
        }
        paths.push_back(folder);
        std::filesystem::create_directories(settingsPath.parent_path(), error);
        std::ofstream out(settingsPath, std::ios::binary | std::ios::trunc);
        out << settings.dump(4) << "\n";
    }

    std::string EditorV2Layer::CustomLitSignature() const
    {
        std::string signature;
        const Ref<Project> project = Project::GetActive();
        if (!project)
            return signature;
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        for (const auto& [handle, metadata] : project->GetEditorAssetManager()->GetAssetRegistry())
        {
            if (metadata.Type != AssetType::MaterialShader && metadata.Type != AssetType::MaterialGraph)
                continue;
            std::error_code error;
            const std::filesystem::path file = assetDirectory / metadata.FilePath;
            const auto written = std::filesystem::last_write_time(file, error);
            const auto size = error ? 0 : std::filesystem::file_size(file, error);
            signature += metadata.FilePath.generic_string() + '|' +
                         std::to_string(error ? 0 : written.time_since_epoch().count()) + '|' +
                         std::to_string(size) + '\n';
        }
        return signature;
    }

    void EditorV2Layer::PollCustomLitShaders()
    {
        if (!m_Renderer || !Project::GetActive())
            return;
        std::string signature = CustomLitSignature();
        if (signature == m_CustomLitSignature)
            return;
        m_CustomLitSignature = std::move(signature);
        bool complete = true;
        const std::vector<CustomLitSource> sources = CustomLitShaders::GatherProjectSources(complete);
        if (!sources.empty())
            WriteMaterialShaderEditorSettings();
        bool graphsComplete = true;
        const std::vector<ProjectMaterialGraph> graphs = GatherProjectMaterialGraphs(graphsComplete);
        NOX_CORE_INFO("Custom Lit: Material Shaders or material graphs changed, building them again");
        m_Renderer->SetCustomLitShaders(sources, graphs);
    }

    void EditorV2Layer::UpdateHistory()
    {
        if (m_SceneState != SceneState::Edit || !m_ActiveScene || !Project::GetActive())
            return;
        EditorHistory::FrameInfo frame;
        frame.InteractionActive = m_ViewportTools.GizmoUsing;
        frame.InteractionEnded = m_HistoryCheckRequested || (m_ViewportTools.GizmoWasUsing && !m_ViewportTools.GizmoUsing);
        // A timeline preview poses its targets (not edits): nothing is taken while it runs; when it ends (what they had back) the
        // scene is checked once, so an edit made meanwhile to something else becomes a step then.
        const bool previewing = m_AssetEditors.AnyPreviewing();
        if (previewing)
        {
            frame.InteractionActive = true;
            frame.InteractionEnded = false;
        }
        else if (m_TimelinePreviewWasOn)
            frame.InteractionEnded = true;
        m_TimelinePreviewWasOn = previewing;
        m_ViewportTools.GizmoWasUsing = m_ViewportTools.GizmoUsing;
        m_HistoryCheckRequested = false;
        frame.AssetsLoading = Project::GetActive()->GetEditorAssetManager()->GetLoadingCount() > 0;
        if (Entity watched = m_Selection.GetPrimary())
            frame.Watched = watched.GetUUID();
        frame.ExternalStep = std::move(m_HistoryExternalStep); // (an MCP edit this frame: one step with its name)
        m_HistoryExternalStep.clear();
        m_History.Update(m_ActiveScene, frame);
        UpdateSceneDirtyState();
    }

    void EditorV2Layer::UpdateSceneDirtyState()
    {
        const size_t position = m_History.GetPosition();
        if (m_SceneCleanPending)
        {
            m_SceneCleanPending = false;
            m_SceneDirty = false;
            m_HistoryPositionSeen = position;
        }
        else if (position != m_HistoryPositionSeen)
        {
            m_SceneDirty = true;
            m_HistoryPositionSeen = position;
        }
        UpdatePrefabMode();
        // The window's title: project, scene, and * while there is something unsaved (as every editor does).
        if (auto win = Application::Get().getWindow(); win && win->getHandle() && Project::GetActive())
        {
            const std::string scene = m_PrefabMode.Active ? "Prefab: " + m_PrefabMode.Name
                                    : m_EditorScenePath.empty() ? std::string("Untitled") : m_EditorScenePath.stem().string();
            const std::string title = "Nox Editor - " + Project::GetActive()->GetConfig().Name + " - " + scene + (HasUnsavedChanges() ? " *" : "");
            if (title != m_WindowTitleShown)
            {
                m_WindowTitleShown = title;
                SDL_SetWindowTitle(win->getHandle(), title.c_str());
            }
        }
    }

    void EditorV2Layer::SetSceneContext(const Ref<Scene>& scene)
    {
        // The selection first: the panels read it for the new scene (same ids kept: undo / redo, Play; others dropped).
        m_Selection.SetScene(scene);
        m_SceneHierarchyPanel.SetContext(scene);
        m_InspectorPanel.SetContext(scene);
    }

    bool EditorV2Layer::HasUnsavedChanges(uint32_t scope) const
    {
        if ((scope & UnsavedScene) && (m_SceneDirty || (m_PrefabMode.Active && m_PrefabMode.ReturnDirty)))
            return true;
        if ((scope & UnsavedMixer) && AudioMixer::IsDirty())
            return true;
        return (scope & UnsavedAssets) && (m_AssetEditors.HasUnsaved() || MaterialEdits::Any());
    }

    void EditorV2Layer::ShowSaveChangesDialog(const std::string& text, std::function<void()> save, std::function<void()> dontSave)
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer)
            return;
        Ref<UI::UILabel> label(new UI::UILabel(text, 14.0f));
        label->WordWrap = true;
        label->WidthMode = UI::UISizeMode::Fill;
        std::vector<UI::UIModal::Button> buttons;
        buttons.push_back({ "Save", std::move(save), true });
        buttons.push_back({ "Don't Save", std::move(dontSave) });
        buttons.push_back({ "Cancel", {} });
        UI::UIModal::ShowCustom(uiLayer->GetContext(), "Save changes?", label, std::move(buttons), 480.0f);
    }

    void EditorV2Layer::ConfirmUnsavedChanges(const std::string& action, std::function<void()> proceed, uint32_t scope)
    {
        // Everything that asks about the scene replaces the level: Prefab Mode is left first (asked about on its own), then the level
        // waiting behind it is asked about as usual.
        if (m_PrefabMode.Active && (scope & UnsavedScene))
        {
            RequestExitPrefabMode([this, action, proceed, scope]() { ConfirmUnsavedChanges(action, proceed, scope); });
            return;
        }
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!HasUnsavedChanges(scope) || !uiLayer)
        {
            if (proceed)
                proceed();
            return;
        }
        // What is unsaved, by name.
        std::vector<std::string> unsaved;
        if ((scope & UnsavedScene) && m_SceneDirty)
            unsaved.push_back("the scene " + (m_EditorScenePath.empty() ? std::string("(untitled)") : m_EditorScenePath.filename().string()));
        if (scope & UnsavedAssets)
        {
            for (std::string& name : m_AssetEditors.UnsavedNames())
                unsaved.push_back(std::move(name));
            for (AssetHandle material : MaterialEdits::Unsaved()) // (edited in the Inspector, no window open)
                if (!m_AssetEditors.Find(material))
                    unsaved.push_back("the material " + AssetDisplayName(material));
        }
        if ((scope & UnsavedMixer) && AudioMixer::IsDirty())
            unsaved.push_back("the audio mixer");
        std::string list;
        for (size_t i = 0; i < unsaved.size(); ++i)
            list += (i == 0 ? "" : i + 1 == unsaved.size() ? " and " : ", ") + unsaved[i];

        ShowSaveChangesDialog("You have unsaved changes in " + list + ".\nSave them before " + action + "?",
            [this, proceed, scope]()
            {
                const auto finish = [this, proceed, scope]()
                {
                    if (scope & UnsavedAssets)
                    {
                        m_AssetEditors.SaveAll();
                        MaterialEdits::SaveAll();
                    }
                    if ((scope & UnsavedMixer) && AudioMixer::IsDirty())
                        AudioMixer::Save();
                    // Still unsaved (Save As was cancelled, a file could not be written): nothing is dropped.
                    if (HasUnsavedChanges(scope))
                    {
                        Notify(UI::UINotificationSeverity::Warning, "Not everything was saved", "Nothing was closed: save again or choose Don't Save.");
                        return;
                    }
                    if (proceed)
                        proceed();
                };
                // The scene first: an untitled one asks for its name, and the rest goes on when that dialog answers.
                if ((scope & UnsavedScene) && m_SceneDirty)
                    SaveScene([finish](bool) { finish(); });
                else
                    finish();
            },
            [this, proceed, scope]()
            {
                if (scope & UnsavedScene)
                    m_SceneDirty = false; // (what follows replaces it; the title must not keep the *)
                // The asset editors' edits are dropped: back to their files (a playing scene runs what is saved again).
                if (scope & UnsavedAssets)
                {
                    m_AssetEditors.RevertAll();
                    MaterialEdits::RevertAll();
                }
                if (proceed)
                    proceed();
            });
    }

    void EditorV2Layer::Undo()
    {
        if (m_SceneState != SceneState::Edit || !m_History.CanUndo() || m_HLODBuild) // (an HLOD build reads the scene it started from)
            return;
        m_AssetEditors.EndPreviews(); // (a timeline's poses come off first; it poses again from the restored scene)
        const std::string name = m_History.GetUndoName();
        RestoreHistoryState(m_History.StepBack());
        NOX_CORE_INFO("Undo: {}", name);
    }

    void EditorV2Layer::Redo()
    {
        if (m_SceneState != SceneState::Edit || !m_History.CanRedo() || m_HLODBuild)
            return;
        m_AssetEditors.EndPreviews();
        const std::string name = m_History.GetRedoName();
        RestoreHistoryState(m_History.StepForward());
        NOX_CORE_INFO("Redo: {}", name);
    }

    void EditorV2Layer::RestoreHistoryState(const std::string& text)
    {
        NOX_PROFILE_SCOPE("Undo Restore");
        Ref<Scene> restored = CreateRef<Scene>();
        SceneSerializer serializer(restored);
        if (!serializer.DeserializeText(text))
        {
            NOX_CORE_ERROR("Undo / redo: the history state could not be read; the scene is left as it is");
            return;
        }

        if (m_ViewportImage)
        {
            const UI::UIRect& bounds = m_ViewportImage->GetBounds();
            const uint32_t w = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Width())));
            const uint32_t h = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Height())));
            if (w > 0 && h > 0)
                restored->OnViewportResize(w, h);
        }
        m_HoveredEntity = Entity();
        restored->AdoptWorldState(*m_EditorScene);
        m_EditorScene = restored;
        m_ActiveScene = restored;
        SetSceneContext(restored); // (the selection carries over by id)
        m_UnloadUnusedAssetsRequested = true;
        m_History.OnSceneRestored(restored);
    }

    void EditorV2Layer::UnloadUnusedAssets()
    {
        if (!Project::GetActive() || !Project::GetActive()->GetEditorAssetManager())
            return;
        std::unordered_set<AssetHandle> referencedAssets;
        if (m_EditorScene && m_ActiveScene != m_EditorScene)
        {
            if (!m_EditorSceneReferencesValid)
            {
                m_EditorSceneReferences.clear();
                m_EditorScene->CollectAssetReferences(m_EditorSceneReferences);
                m_EditorSceneReferencesValid = true;
            }
            referencedAssets = m_EditorSceneReferences;
        }
        else if (m_EditorScene)
        {
            m_EditorScene->CollectAssetReferences(referencedAssets);
            m_EditorSceneReferencesValid = false;
        }
        if (m_ActiveScene && m_ActiveScene != m_EditorScene)
            m_ActiveScene->CollectAssetReferences(referencedAssets);
        if (m_PrefabMode.ReturnScene) // (the level waits while a prefab is edited: its assets stay)
            m_PrefabMode.ReturnScene->CollectAssetReferences(referencedAssets);
        AssetPreviewRenderer::Get().CollectAssetReferences(referencedAssets); // what the thumbnails draw right now
        m_ContentBrowserPanel.CollectAssetReferences(referencedAssets);       // textures shown as tile pictures
        if (m_HLODBuild)                                                       // the cell being built waits for its models and prefabs
            m_HLODBuild->CollectAssetReferences(referencedAssets);
        Project::GetActive()->GetEditorAssetManager()->UnloadUnusedAssets(referencedAssets);
    }

    void EditorV2Layer::OnScenePlay()
    {
        if (m_PrefabMode.Active)
        {
            Notify(UI::UINotificationSeverity::Info, "Not in Prefab Mode", "Exit Prefab Mode (toolbar) to play the level.");
            return;
        }
        CancelHLODBuild("Play");
        m_AssetEditors.EndPreviews(); // (Play copies the scene: without the timeline's poses)
        m_EditorSceneReferencesValid = false;
        SetViewMode(EditorViewMode::Perspective);
        if (m_SceneState == SceneState::Simulate)
            OnSceneStop();

        if (m_PlayNet.Mode != PlayNetMode::Standalone)
        {
            if (!m_PlayNet.SaveWithoutAsking)
            {
                m_OpenSaveAndPlay = true;
                return;
            }
            SaveScene();
            BeginMultiplayerPlay();
            RefreshToolBar();
            return;
        }

        m_SceneState = SceneState::Play;
        StartPlayScene(Scene::NetMode::Standalone);
        RefreshToolBar();
    }

    void EditorV2Layer::StartPlayScene(Scene::NetMode netMode)
    {
        m_HoveredEntity = Entity();
        m_ActiveScene = Scene::Copy(m_EditorScene);
        m_ActiveScene->SetNetMode(netMode);
        if (m_PlaySession && m_PlaySession->GetReplication())
            m_PlaySession->GetReplication()->SetScene(m_ActiveScene.get());
        m_ActiveScene->SetRenderer(m_Renderer);
        m_ActiveScene->SetRenderer2D(m_Renderer2D);
        if (m_ViewportImage)
        {
            const UI::UIRect& bounds = m_ViewportImage->GetBounds();
            const uint32_t w = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Width())));
            const uint32_t h = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Height())));
            if (w > 0 && h > 0)
                m_ActiveScene->OnViewportResize(w, h);
        }
        m_ActiveScene->OnRuntimeStart();
        SetSceneContext(m_ActiveScene);
    }

    bool EditorV2Layer::LaunchPlayer(const std::vector<std::string>& arguments, bool console, const char* what)
    {
#ifdef _WIN32
        const std::filesystem::path runtime = std::filesystem::path(Application::GetExecutableRootPath()) / "NoxRuntime.exe";
#else
        const std::filesystem::path runtime = std::filesystem::path(Application::GetExecutableRootPath()) / "NoxRuntime";
#endif
        std::error_code exists;
        if (!std::filesystem::exists(runtime, exists))
        {
            NOX_CORE_ERROR("Multiplayer Play: NoxRuntime is not next to the editor ({}); build the NoxRuntime target", runtime.generic_string());
            return false;
        }
        auto process = std::make_unique<ChildProcess>();
        std::string error;
        if (!process->Launch(runtime, arguments, console, error))
        {
            NOX_CORE_ERROR("Multiplayer Play: could not start {} ({})", what, error);
            return false;
        }
        NOX_CORE_INFO("Multiplayer Play: started {}", what);
        m_PlayProcesses.push_back(std::move(process));
        return true;
    }

    std::filesystem::path EditorV2Layer::EditorSceneAssetPath() const
    {
        if (m_EditorScenePath.empty() || !Project::GetActive())
            return {};
        if (!m_EditorScenePath.is_absolute())
            return m_EditorScenePath.lexically_normal();
        std::error_code error;
        const std::filesystem::path relative = std::filesystem::relative(m_EditorScenePath, Project::GetActiveAssetDirectory(), error);
        if (error || relative.empty() || *relative.begin() == "..")
            return {};
        return relative.lexically_normal();
    }

    AssetHandle EditorV2Layer::EditorSceneHandle() const
    {
        const std::filesystem::path relative = EditorSceneAssetPath();
        if (relative.empty())
            return 0;
        return Project::GetActive()->GetEditorAssetManager()->ResolvePath(relative, AssetType::Scene);
    }

    void EditorV2Layer::BeginMultiplayerPlay()
    {
        if (m_EditorScenePath.empty() || !Project::GetActive())
        {
            NOX_CORE_WARN("Multiplayer Play: save the scene first (the other players load it from disk)");
            return;
        }
        std::filesystem::path projectFile;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(Project::GetProjectDirectory(), error))
        {
            if (entry.is_regular_file(error) && entry.path().extension() == ".nproj")
            {
                projectFile = entry.path();
                break;
            }
        }
        const std::string scene = EditorSceneAssetPath().generic_string();
        const AssetHandle sceneHandle = EditorSceneHandle();
        if (projectFile.empty())
        {
            NOX_CORE_ERROR("Multiplayer Play: no .nproj file in {}", Project::GetProjectDirectory().generic_string());
            return;
        }
        if (scene.empty() || sceneHandle == 0)
        {
            NOX_CORE_ERROR("Multiplayer Play: the scene '{}' is not an asset of the project (save it under {})",
                           m_EditorScenePath.generic_string(), Project::GetActiveAssetDirectory().generic_string());
            return;
        }

        static const char* emulationNames[] = { "off", "average", "bad" };
        const std::string emulation = emulationNames[std::clamp(m_PlayNet.Emulation, 0, 2)];
        Net::NetEmulation emulationValues;
        Net::NetEmulation::FromPreset(emulation, emulationValues);
        const uint16_t port = static_cast<uint16_t>(std::clamp(m_PlayNet.Port, 1, 65535));

        SDL_Rect usable{ 0, 0, 1920, 1080 };
        SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
        const int windowWidth = std::min(usable.w / 2, 1280);
        const int windowHeight = std::min(usable.h / 2, 720);
        auto playerArguments = [&](int player)
        {
            const int slot = (player - 2) % 4;
            return std::vector<std::string>{
                projectFile.string(),
                "--connect=127.0.0.1:" + std::to_string(port),
                "--connect-retry=30",
                "--windowed",
                "--name=Player" + std::to_string(player),
                "--scene=" + scene,
                "--net-emulation=" + emulation,
                std::format("--window-size={},{}", windowWidth, windowHeight),
                std::format("--window-pos={},{}", usable.x + (slot % 2) * windowWidth, usable.y + (slot / 2) * windowHeight)
            };
        };
        auto withDebug = [this](std::vector<std::string> arguments)
        {
            if (m_PlayNet.DebugTrace)
                arguments.push_back("--net-debug");
            return arguments;
        };

        Net::NetSessionConfig config;
        config.Port = port;
        config.Emulation = emulationValues;
        config.PlayerName = "Player1";
        config.DebugTrace = m_PlayNet.DebugTrace;
        m_PlaySessionEnded = false;

        if (m_PlayNet.Mode == PlayNetMode::ListenServer)
        {
            config.Role = Net::SessionRole::ListenServer;
            m_PlaySession = std::make_unique<Net::NetSession>(config);
            m_PlaySession->FillWelcome = [this, sceneHandle](Net::NetWelcome& welcome)
            {
                welcome.ServerTick = m_ActiveScene ? m_ActiveScene->GetTick() : 0;
                welcome.TickRate = static_cast<uint16_t>(Scene::TickRate);
                welcome.SceneHandle = static_cast<uint64_t>(sceneHandle);
            };
            if (!m_PlaySession->Start())
            {
                NOX_CORE_ERROR("Multiplayer Play: port {} cannot be opened (another server running? change it in the Play settings)", port);
                m_PlaySession.reset();
                return;
            }
            m_SceneState = SceneState::Play;
            StartPlayScene(Scene::NetMode::Server);
        }
        else
        {
            if (!LaunchPlayer(withDebug({ projectFile.string(), "--server", "--port=" + std::to_string(port),
                                          "--scene=" + scene, "--net-emulation=" + emulation }),
                              true, "the dedicated server"))
                return;
            config.Role = Net::SessionRole::Client;
            config.Address = "127.0.0.1:" + std::to_string(port);
            config.ConnectRetrySeconds = 30.0;
            m_PlaySession = std::make_unique<Net::NetSession>(config);
            m_PlaySession->OnJoined = [this, sceneHandle](AssetHandle serverScene)
            {
                if (serverScene != sceneHandle)
                    return false;
                m_PlayWaitingForServer = false;
                StartPlayScene(Scene::NetMode::Client);
                return true;
            };
            m_PlaySession->OnLeft = [this](Net::DisconnectReason reason, const std::string& text, bool)
            {
                NOX_CORE_WARN("Multiplayer Play: the session ended -- {} ({})", Net::ToString(reason), text);
                m_PlaySessionEnded = true;
            };
            m_SceneState = SceneState::Play;
            m_PlayWaitingForServer = true;
            if (!m_PlaySession->Start())
                m_PlaySessionEnded = true;
        }

        for (int player = 2; player <= std::clamp(m_PlayNet.Players, 1, 8); ++player)
            LaunchPlayer(withDebug(playerArguments(player)), false, std::format("player {}", player).c_str());
    }

    void EditorV2Layer::EndMultiplayerPlay()
    {
        m_PlayProcesses.clear();
        m_PlaySession.reset();
        m_PlaySessionEnded = false;
    }

    void EditorV2Layer::ShowPlaySettingsOverlay(UI::UIElement* anchor)
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer || !anchor)
            return;

        UI::UIContext& ctx = uiLayer->GetContext();
        auto backdrop = UI::MakeUI<UI::UIAnchorBox>();
        backdrop->WidthMode = UI::UISizeMode::Fill;
        backdrop->HeightMode = UI::UISizeMode::Fill;
        backdrop->HitTestMode = UI::UIHitTestMode::Self;

        auto card = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 8.0f);
        card->StyleClass = "Surface";
        card->DrawBackground = true;
        card->HitTestMode = UI::UIHitTestMode::Self;
        card->Padding = UI::UIThickness::All(12.0f);
        card->WidthMode = UI::UISizeMode::Fixed;
        card->HeightMode = UI::UISizeMode::Auto;
        card->FixedSize = { 310.0f, 0.0f };

        const UI::UIRect anchorBounds = anchor->GetBounds();
        float leftX = anchorBounds.Min.x;
        float topY = anchorBounds.Max.y + 4.0f;
        if (const auto& root = ctx.GetRoot())
        {
            const UI::UIRect& rootBounds = root->GetBounds();
            if (rootBounds.Width() > 330.0f)
                leftX = std::clamp(leftX, 8.0f, rootBounds.Max.x - 320.0f);
            if (rootBounds.Height() > 360.0f)
                topY = std::clamp(topY, 8.0f, rootBounds.Max.y - 340.0f);
        }
        card->AnchorMin = { 0.0f, 0.0f };
        card->AnchorMax = { 0.0f, 0.0f };
        card->AnchorOffsets = { leftX, topY, 0.0f, 0.0f };

        auto title = UI::Label("Multiplayer Play Settings").Get();
        card->AddChild(title);
        card->AddChild(UI::Separator().Get());

        auto modeLabel = UI::Label("Net Mode").Get();
        card->AddChild(modeLabel);
        std::array<Ref<UI::UIButton>, 3> modeBtns;
        std::array<Ref<UI::UIButton>, 3> emuBtns;
        Ref<UI::UISlider> playersSlider;
        Ref<UI::UILabel> playersLabel;
        Ref<UI::UITextInput> portInput;
        Ref<UI::UICheckBox> saveCheck;
        Ref<UI::UICheckBox> debugCheck;

        auto syncControls = [this, &modeBtns, &emuBtns, &playersSlider, &playersLabel, &portInput, &saveCheck, &debugCheck]() mutable
        {
            const bool netEnabled = m_PlayNet.Mode != PlayNetMode::Standalone;
            for (int i = 0; i < 3; ++i)
            {
                if (modeBtns[i])
                    modeBtns[i]->StyleVariant = (static_cast<int>(m_PlayNet.Mode) == i) ? "primary" : "";
                if (emuBtns[i])
                {
                    emuBtns[i]->StyleVariant = (m_PlayNet.Emulation == i) ? "primary" : "";
                    emuBtns[i]->SetEnabled(netEnabled);
                }
            }
            if (playersLabel)
                playersLabel->Text = std::format("Number of Players: {}", m_PlayNet.Players);
            if (playersSlider)
                playersSlider->SetEnabled(netEnabled);
            if (portInput)
                portInput->SetEnabled(netEnabled);
            if (saveCheck)
                saveCheck->SetEnabled(netEnabled);
            if (debugCheck)
                debugCheck->SetEnabled(netEnabled);
        };

        const char* modeLabels[3] = { "Standalone", "Listen Server", "Client" };
        auto modeRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
        modeRow->WidthMode = UI::UISizeMode::Fill;
        modeRow->HeightMode = UI::UISizeMode::Auto;
        for (int i = 0; i < 3; ++i)
        {
            modeBtns[i] = UI::MakeUI<UI::UIButton>(modeLabels[i]);
            modeBtns[i]->WidthMode = UI::UISizeMode::Fill;
            modeBtns[i]->Padding = UI::UIThickness::Symmetric(6.0f, 4.0f);
            modeRow->AddChild(modeBtns[i]);
        }
        card->AddChild(modeRow);

        playersLabel = UI::Label(std::format("Number of Players: {}", m_PlayNet.Players)).Get();
        card->AddChild(playersLabel);
        playersSlider = UI::MakeUI<UI::UISlider>(static_cast<float>(m_PlayNet.Players), 1.0f, 8.0f);
        playersSlider->Step = 1.0f;
        playersSlider->Format = "{:.0f}";
        playersSlider->WidthMode = UI::UISizeMode::Fill;
        playersSlider->OnChanged = [this, playersLabel](float value)
        {
            m_PlayNet.Players = std::clamp(static_cast<int>(std::round(value)), 1, 8);
            if (playersLabel)
                playersLabel->Text = std::format("Number of Players: {}", m_PlayNet.Players);
        };
        card->AddChild(playersSlider);

        card->AddChild(UI::Label("Network Emulation").Get());
        const char* emuLabels[3] = { "Off", "Average", "Bad" };
        auto emuRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
        emuRow->WidthMode = UI::UISizeMode::Fill;
        emuRow->HeightMode = UI::UISizeMode::Auto;
        for (int i = 0; i < 3; ++i)
        {
            emuBtns[i] = UI::MakeUI<UI::UIButton>(emuLabels[i]);
            emuBtns[i]->WidthMode = UI::UISizeMode::Fill;
            emuBtns[i]->Padding = UI::UIThickness::Symmetric(6.0f, 4.0f);
            emuRow->AddChild(emuBtns[i]);
        }
        card->AddChild(emuRow);

        auto portRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 8.0f);
        portRow->WidthMode = UI::UISizeMode::Fill;
        portRow->HeightMode = UI::UISizeMode::Auto;
        portRow->AddChild(UI::Label("Port").VAlign(UI::UIAlignment::Center).Get());
        portInput = UI::MakeUI<UI::UITextInput>(std::to_string(m_PlayNet.Port), "7777");
        portInput->WidthMode = UI::UISizeMode::Fill;
        portInput->OnChanged = [this](const std::string& txt)
        {
            const int parsed = std::atoi(txt.c_str());
            if (parsed > 0)
                m_PlayNet.Port = std::clamp(parsed, 1, 65535);
        };
        portRow->AddChild(portInput);
        card->AddChild(portRow);

        saveCheck = UI::MakeUI<UI::UICheckBox>("Save the scene without asking", m_PlayNet.SaveWithoutAsking);
        saveCheck->OnChanged = [this](bool v) { m_PlayNet.SaveWithoutAsking = v; };
        card->AddChild(saveCheck);

        debugCheck = UI::MakeUI<UI::UICheckBox>("Network debug log (--net-debug)", m_PlayNet.DebugTrace);
        debugCheck->OnChanged = [this](bool v) { m_PlayNet.DebugTrace = v; };
        card->AddChild(debugCheck);

        auto closeRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 6.0f);
        closeRow->WidthMode = UI::UISizeMode::Fill;
        closeRow->HeightMode = UI::UISizeMode::Auto;
        auto closeSpacer = UI::MakeUI<UI::UIElement>();
        closeSpacer->WidthMode = UI::UISizeMode::Fill;
        auto closeBtn = UI::MakeUI<UI::UIButton>("Done", [&ctx]() { ctx.ClearOverlay(); });
        closeBtn->Padding = UI::UIThickness::Symmetric(12.0f, 4.0f);
        closeRow->AddChild(closeSpacer);
        closeRow->AddChild(closeBtn);
        card->AddChild(closeRow);

        for (int i = 0; i < 3; ++i)
        {
            modeBtns[i]->OnClick = [this, i, modeBtns, emuBtns, playersSlider, playersLabel, portInput, saveCheck, debugCheck]()
            {
                m_PlayNet.Mode = static_cast<PlayNetMode>(i);
                const bool netEnabled = m_PlayNet.Mode != PlayNetMode::Standalone;
                for (int j = 0; j < 3; ++j)
                {
                    modeBtns[j]->StyleVariant = (i == j) ? "primary" : "";
                    emuBtns[j]->StyleVariant = (m_PlayNet.Emulation == j) ? "primary" : "";
                    emuBtns[j]->SetEnabled(netEnabled);
                }
                playersLabel->Text = std::format("Number of Players: {}", m_PlayNet.Players);
                playersSlider->SetEnabled(netEnabled);
                portInput->SetEnabled(netEnabled);
                saveCheck->SetEnabled(netEnabled);
                debugCheck->SetEnabled(netEnabled);
            };
            emuBtns[i]->OnClick = [this, i, emuBtns]()
            {
                m_PlayNet.Emulation = i;
                for (int j = 0; j < 3; ++j)
                    emuBtns[j]->StyleVariant = (i == j) ? "primary" : "";
            };
        }

        syncControls();

        UI::UIElement* cardPtr = card.get();
        backdrop->OnMouseDown = [cardPtr, &ctx](const UI::UIPointerEvent& e)
        {
            if (!cardPtr->GetBounds().Contains(e.Position))
            {
                ctx.ClearOverlay();
                return true;
            }
            return false;
        };
        backdrop->AddChild(card);
        ctx.SetOverlay(backdrop);
    }

    void EditorV2Layer::ShowSaveAndPlayOverlay()
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer)
            return;

        UI::UIContext& ctx = uiLayer->GetContext();
        auto backdrop = UI::MakeUI<UI::UIAnchorBox>();
        backdrop->WidthMode = UI::UISizeMode::Fill;
        backdrop->HeightMode = UI::UISizeMode::Fill;
        backdrop->HitTestMode = UI::UIHitTestMode::Self;
        backdrop->DrawBackground = true;
        backdrop->StyleClass = "Backdrop";

        auto card = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 10.0f);
        card->StyleClass = "Surface";
        card->DrawBackground = true;
        card->HitTestMode = UI::UIHitTestMode::Self;
        card->Padding = UI::UIThickness::All(16.0f);
        card->WidthMode = UI::UISizeMode::Fixed;
        card->HeightMode = UI::UISizeMode::Auto;
        card->FixedSize = { 390.0f, 0.0f };
        card->AnchorMin = { 0.5f, 0.5f };
        card->AnchorMax = { 0.5f, 0.5f };
        card->AnchorOffsets = { -195.0f, -95.0f, 195.0f, 95.0f };

        card->AddChild(UI::Label("Save and Play").Get());
        auto desc = UI::Label("Multiplayer Play starts the other players (NoxRuntime) on the saved scene: the scene is saved first.").Get();
        desc->WordWrap = true;
        card->AddChild(desc);

        auto dontAsk = UI::CheckBox("Don't ask again (this session)", m_PlayNet.SaveWithoutAsking,
                                    [this](bool checked) { m_PlayNet.SaveWithoutAsking = checked; }).Get();
        card->AddChild(dontAsk);

        auto btnRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 8.0f);
        btnRow->WidthMode = UI::UISizeMode::Fill;
        btnRow->HeightMode = UI::UISizeMode::Auto;
        auto spacer = UI::MakeUI<UI::UIElement>();
        spacer->WidthMode = UI::UISizeMode::Fill;
        auto saveBtn = UI::MakeUI<UI::UIButton>("Save and Play", [this, &ctx]()
        {
            ctx.ClearOverlay();
            SaveScene();
            BeginMultiplayerPlay();
            RefreshToolBar();
        });
        saveBtn->StyleVariant = "primary";
        saveBtn->Padding = UI::UIThickness::Symmetric(12.0f, 5.0f);

        auto cancelBtn = UI::MakeUI<UI::UIButton>("Cancel", [this, &ctx]()
        {
            m_PlayNet.SaveWithoutAsking = false;
            ctx.ClearOverlay();
        });
        cancelBtn->Padding = UI::UIThickness::Symmetric(12.0f, 5.0f);

        btnRow->AddChild(spacer);
        btnRow->AddChild(saveBtn);
        btnRow->AddChild(cancelBtn);
        card->AddChild(btnRow);

        backdrop->AddChild(card);
        ctx.SetOverlay(backdrop);
    }

    void EditorV2Layer::OnSceneSimulate()
    {
        if (m_PrefabMode.Active)
        {
            Notify(UI::UINotificationSeverity::Info, "Not in Prefab Mode", "Exit Prefab Mode (toolbar) to simulate the level.");
            return;
        }
        CancelHLODBuild("Simulate");
        m_AssetEditors.EndPreviews();
        m_EditorSceneReferencesValid = false;
        if (m_SceneState == SceneState::Play)
            OnSceneStop();
        m_SceneState = SceneState::Simulate;
        m_HoveredEntity = Entity();
        m_ActiveScene = Scene::Copy(m_EditorScene);
        SetSceneContext(m_ActiveScene);
        m_ActiveScene->SetRenderer(m_Renderer);
        m_ActiveScene->SetRenderer2D(m_Renderer2D);
        const glm::dvec3 streamingSource = m_EditorCamera.GetPosition();
        m_ActiveScene->OnSimulationStart(&streamingSource);
        RefreshToolBar();
    }

    void EditorV2Layer::OnSceneStop()
    {
        const bool started = !(m_SceneState == SceneState::Play && m_PlayWaitingForServer);
        EndMultiplayerPlay();
        m_PlayWaitingForServer = false;
        if (m_SceneState == SceneState::Play && started)
            m_ActiveScene->OnRuntimeStop();
        else if (m_SceneState == SceneState::Simulate)
            m_ActiveScene->OnSimulationStop();

        m_SceneState = SceneState::Edit;
        if (Input::IsRelativeMouseMode())
            Input::SetRelativeMouseMode(false);
        Input::ClearInjected();
        m_HoveredEntity = Entity();
        m_ActiveScene = m_EditorScene;
        SetSceneContext(m_ActiveScene);
        m_UnloadUnusedAssetsRequested = true;
        RefreshToolBar();
    }

    void EditorV2Layer::OnScenePause()
    {
        if (m_SceneState != SceneState::Edit && m_ActiveScene)
            m_ActiveScene->SetPaused(true);
    }

    void EditorV2Layer::LoadViewportPrefs()
    {
        if (LayoutDirectory().empty())
            return;
        std::ifstream in(LayoutDirectory() / kViewportPrefsFile);
        if (!in.is_open())
            return;
        for (std::string line; std::getline(in, line);)
        {
            int index = 0, open = 0, view = 0, grid = 1, resolution = 0, realtime = 1, cameraPos = 0;
            unsigned flags = Renderer::ExtraViewOutline;
            const int read = sscanf(line.c_str(), "Viewport%d=%d,%d,%u,%d,%d,%d,%d",
                                    &index, &open, &view, &flags, &grid, &resolution, &realtime, &cameraPos);
            if (read >= 3 && index >= 0 && index < kMaxViewports && view >= 0 && view <= static_cast<int>(EditorViewMode::Right))
            {
                ViewportSlot& slot = m_Viewports[index];
                slot.Open = (index == 0) || (open != 0);
                slot.Camera.SetViewMode(static_cast<EditorViewMode>(view));
                if (read >= 7)
                {
                    slot.ShowFlags = flags;
                    slot.ShowGrid = (grid != 0);
                    slot.ResolutionIndex = std::clamp(resolution, 0, 2);
                    slot.Realtime = (realtime != 0);
                    slot.ShowCameraPosition = (read == 8 && cameraPos != 0);
                }
                if (index == 0)
                    m_EditorCamera.SetViewMode(static_cast<EditorViewMode>(view));
            }
        }
    }

    void EditorV2Layer::SaveViewportPrefs() const
    {
        if (LayoutDirectory().empty())
            return;
        std::error_code error;
        std::filesystem::create_directories(LayoutDirectory(), error);
        std::ofstream out(LayoutDirectory() / kViewportPrefsFile, std::ios::trunc);
        if (!out.is_open())
            return;
        for (int index = 0; index < kMaxViewports; ++index)
        {
            const ViewportSlot& slot = m_Viewports[index];
            const int view = static_cast<int>(index == m_ActiveViewport ? m_EditorCamera.GetViewMode() : slot.Camera.GetViewMode());
            out << "Viewport" << index << "="
                << (slot.Open ? 1 : 0) << ","
                << view << ","
                << slot.ShowFlags << ","
                << (slot.ShowGrid ? 1 : 0) << ","
                << slot.ResolutionIndex << ","
                << (slot.Realtime ? 1 : 0) << ","
                << (slot.ShowCameraPosition ? 1 : 0) << "\n";
        }
    }

    void EditorV2Layer::OpenViewport(int index)
    {
        if (index < 0 || index >= kMaxViewports)
            return;
        ViewportSlot& slot = m_Viewports[index];
        if (!slot.CameraSet)
        {
            const EditorViewMode keepMode = slot.Camera.GetViewMode();
            slot.Camera = m_EditorCamera;
            slot.Camera.SetViewMode(keepMode);
            slot.CameraSet = true;
        }
        slot.Open = true;

        if (index > 0)
        {
            if (m_ViewportSlotRoots[index] && m_ExtraViewportHosts[index] && index != m_ActiveViewport)
            {
                m_ViewportSlotRoots[index]->ClearChildren();
                m_ViewportSlotRoots[index]->AddChild(m_ExtraViewportHosts[index]);
            }
            if (m_ViewportDockSpace && !m_ViewportDockSpace->FindNodeByPanelTitle(slot.Title))
            {
                const UI::UIDockDropZone zone = (index == 2) ? UI::UIDockDropZone::Bottom : UI::UIDockDropZone::Right;
                const std::string target = (index == 3 && m_Viewports[1].Open) ? std::string(m_Viewports[1].Title) : std::string("Viewport");
                const UI::UIDockDropZone useZone = (index == 3 && m_Viewports[1].Open) ? UI::UIDockDropZone::Bottom : zone;
                m_ViewportDockSpace->AddPanel(slot.Title, m_ViewportSlotRoots[index], useZone, target, 0.5f);
            }
        }
        SyncViewportTabBars();
    }

    void EditorV2Layer::CloseViewport(int index)
    {
        if (index <= 0 || index >= kMaxViewports)
            return;
        if (m_ActiveViewport == index)
            ActivateViewport(0);

        ViewportSlot& slot = m_Viewports[index];
        slot.Open = false;
        slot.Hovered = false;
        slot.Size = { 0, 0 };
        slot.DrawnSize = { 0, 0 };
        if (m_Renderer && m_Renderer->GetExtraViewTexture(static_cast<uint32_t>(index)))
            m_Renderer->ReleaseExtraView(static_cast<uint32_t>(index));
        if (m_ExtraViewportImages[index])
            m_ExtraViewportImages[index]->TextureDescriptorSlot = ~0u;

        if (m_ViewportDockSpace)
            m_ViewportDockSpace->RemovePanel(slot.Title);
        SyncViewportTabBars();
    }

    void EditorV2Layer::ActivateViewport(int index)
    {
        if (index < 0 || index >= kMaxViewports || index == m_ActiveViewport)
            return;

        ViewportSlot& previous = m_Viewports[m_ActiveViewport];
        ViewportSlot& next = m_Viewports[index];
        if (!next.Open)
            OpenViewport(index);
        if (!next.CameraSet)
        {
            const EditorViewMode keepMode = next.Camera.GetViewMode();
            next.Camera = m_EditorCamera;
            next.Camera.SetViewMode(keepMode);
            next.CameraSet = true;
        }

        const bool wasOrthographic = m_EditorCamera.IsOrthographic();
        const int prevIdx = m_ActiveViewport;
        previous.Camera = m_EditorCamera;
        previous.CameraSet = true;
        previous.DrawnSize = { 0, 0 };
        m_EditorCamera = next.Camera;
        m_ActiveViewport = index;

        if (m_ViewportSlotRoots[prevIdx] && m_ExtraViewportHosts[prevIdx])
        {
            m_ViewportSlotRoots[prevIdx]->ClearChildren();
            m_ViewportSlotRoots[prevIdx]->AddChild(m_ExtraViewportHosts[prevIdx]);
        }
        if (m_ViewportSlotRoots[index] && m_ActiveViewportHost)
        {
            m_ViewportSlotRoots[index]->ClearChildren();
            m_ViewportSlotRoots[index]->AddChild(m_ActiveViewportHost);
            const UI::UIRect& b = m_ViewportSlotRoots[index]->GetBounds();
            if (b.Width() > 1.0f && b.Height() > 1.0f)
                m_EditorCamera.SetViewportSize(b.Width(), b.Height());
        }

        m_HoveredEntity = Entity();
        m_ViewportTools.IsBoxSelecting = false;
        SyncOrthoRenderSettings(wasOrthographic);
        if (m_Renderer)
            m_Renderer->ResetCameraHistory();
        m_ViewportToolbar.Sync();
    }

    void EditorV2Layer::ApplyViewportLayout(ViewportLayout layout)
    {
        if (!m_ViewportDockSpace)
            return;

        const auto& dock = m_ViewportDockSpace;
        int count = 1;
        switch (layout)
        {
        case ViewportLayout::One: count = 1; break;
        case ViewportLayout::TwoSideBySide:
        case ViewportLayout::TwoStacked: count = 2; break;
        case ViewportLayout::Three: count = 3; break;
        case ViewportLayout::Four: count = 4; break;
        }

        if (m_ActiveViewport >= count)
            ActivateViewport(0);

        for (int index = 1; index < kMaxViewports; ++index)
        {
            dock->RemovePanel(m_Viewports[index].Title);
            m_Viewports[index].Open = false;
            m_Viewports[index].Hovered = false;
            m_Viewports[index].Size = { 0, 0 };
            m_Viewports[index].DrawnSize = { 0, 0 };
            if (m_Renderer && m_Renderer->GetExtraViewTexture(static_cast<uint32_t>(index)))
                m_Renderer->ReleaseExtraView(static_cast<uint32_t>(index));
            if (m_ExtraViewportImages[index])
                m_ExtraViewportImages[index]->TextureDescriptorSlot = ~0u;
        }

        if (!dock->FindNodeByPanelTitle("Viewport") && m_ViewportSlotRoots[0])
        {
            const UI::UIDockNodeFlags vpFlags = (count > 1)
                ? UI::UIDockNodeFlags::NoUndocking
                : (UI::UIDockNodeFlags::NoUndocking | UI::UIDockNodeFlags::NoTabBar);
            dock->AddPanel("Viewport", m_ViewportSlotRoots[0], UI::UIDockDropZone::Center, {}, 0.5f, vpFlags);
        }

        auto prepareSlot = [this](int index)
        {
            ViewportSlot& slot = m_Viewports[index];
            if (!slot.CameraSet)
            {
                const EditorViewMode keepMode = slot.Camera.GetViewMode();
                slot.Camera = m_EditorCamera;
                slot.Camera.SetViewMode(keepMode);
                slot.CameraSet = true;
            }
            slot.Open = true;
            if (index != m_ActiveViewport && m_ViewportSlotRoots[index] && m_ExtraViewportHosts[index])
            {
                m_ViewportSlotRoots[index]->ClearChildren();
                m_ViewportSlotRoots[index]->AddChild(m_ExtraViewportHosts[index]);
            }
        };

        for (int i = 1; i < count; ++i)
            prepareSlot(i);

        switch (layout)
        {
        case ViewportLayout::One:
            break;
        case ViewportLayout::TwoSideBySide:
            dock->AddPanel(m_Viewports[1].Title, m_ViewportSlotRoots[1], UI::UIDockDropZone::Right, "Viewport", 0.5f);
            break;
        case ViewportLayout::TwoStacked:
            dock->AddPanel(m_Viewports[1].Title, m_ViewportSlotRoots[1], UI::UIDockDropZone::Bottom, "Viewport", 0.5f);
            break;
        case ViewportLayout::Three:
            dock->AddPanel(m_Viewports[1].Title, m_ViewportSlotRoots[1], UI::UIDockDropZone::Right, "Viewport", 0.4f);
            dock->AddPanel(m_Viewports[2].Title, m_ViewportSlotRoots[2], UI::UIDockDropZone::Bottom, m_Viewports[1].Title, 0.5f);
            break;
        case ViewportLayout::Four:
            dock->AddPanel(m_Viewports[1].Title, m_ViewportSlotRoots[1], UI::UIDockDropZone::Right, "Viewport", 0.5f);
            dock->AddPanel(m_Viewports[2].Title, m_ViewportSlotRoots[2], UI::UIDockDropZone::Bottom, "Viewport", 0.5f);
            dock->AddPanel(m_Viewports[3].Title, m_ViewportSlotRoots[3], UI::UIDockDropZone::Bottom, m_Viewports[1].Title, 0.5f);
            break;
        }
        SyncViewportTabBars();
    }

    void EditorV2Layer::SyncViewportTabBars()
    {
        if (!m_ViewportDockSpace)
            return;
        const bool multi = (OpenViewportCount() > 1);
        const UI::UIDockNodeFlags vpFlags = multi
            ? UI::UIDockNodeFlags::NoUndocking
            : (UI::UIDockNodeFlags::NoUndocking | UI::UIDockNodeFlags::NoTabBar);
        m_ViewportDockSpace->SetPanelFlags("Viewport", vpFlags);
    }

    int EditorV2Layer::HoveredViewport() const
    {
        for (int index = 0; index < kMaxViewports; ++index)
        {
            if (m_Viewports[index].Open && m_Viewports[index].Hovered)
                return index;
        }
        return -1;
    }

    int EditorV2Layer::OpenViewportCount() const
    {
        int count = 0;
        for (const ViewportSlot& slot : m_Viewports)
            count += slot.Open ? 1 : 0;
        return count;
    }

    void EditorV2Layer::SubmitExtraViewports(Timestep ts)
    {
        if (!m_Renderer)
            return;
        const float cell = std::max(m_ViewportTools.LocationSnapWorldUnits(), WorldUnits::FromMeters(1.0f));
        for (int index = 0; index < kMaxViewports; ++index)
        {
            ViewportSlot& slot = m_Viewports[index];
            slot.Submitted = false;
            if (index == m_ActiveViewport || !slot.Open || slot.Size.x == 0 || slot.Size.y == 0)
            {
                slot.DrawnSize = { 0, 0 };
                continue;
            }

            const float width = static_cast<float>(slot.Size.x);
            const float height = static_cast<float>(slot.Size.y);
            if (slot.Camera.GetAspectRatio() != width / height)
                slot.Camera.SetViewportSize(width, height);
            slot.Camera.OnUpdate(ts, false);

            const uint32_t divisor = 1u << std::clamp(slot.ResolutionIndex, 0, 2);
            const glm::uvec2 drawSize = glm::max(slot.Size / divisor, glm::uvec2(1));
            uint32_t flags = slot.ShowFlags;

            const glm::mat4 viewProjection = slot.Camera.GetProjection() * slot.Camera.GetViewMatrix(m_Renderer->GetRenderOrigin());
            size_t selection = m_Selection.GetEntities().size();
            for (const Entity& selected : m_Selection.GetEntities())
                selection = selection * 31 + static_cast<uint32_t>(selected);
            slot.SinceDrawn += ts.GetSeconds();
            if (!slot.Realtime && m_SceneState == SceneState::Edit && slot.DrawnSize == drawSize && slot.DrawnFlags == flags &&
                slot.DrawnViewProjection == viewProjection && slot.DrawnHistory == m_History.GetPosition() && slot.DrawnSelection == selection &&
                !m_ViewportTools.GizmoUsing && slot.SinceDrawn < 2.0f && m_Renderer->GetExtraViewTexture(static_cast<uint32_t>(index)))
            {
                slot.Submitted = true;
                continue;
            }

            slot.DrawnSize = drawSize;
            slot.DrawnFlags = flags;
            slot.DrawnViewProjection = viewProjection;
            slot.DrawnHistory = m_History.GetPosition();
            slot.DrawnSelection = selection;
            slot.SinceDrawn = 0.0f;

            shaderio::PushConstantGrid grid{};
            const bool showGrid = (m_SceneState != SceneState::Play && m_ViewportTools.ShowGrid && slot.ShowGrid);
            if (showGrid)
            {
                const EditorViewMode mode = slot.Camera.GetViewMode();
                const int axis = (mode == EditorViewMode::Front || mode == EditorViewMode::Back) ? 2 :
                                 (mode == EditorViewMode::Left || mode == EditorViewMode::Right) ? 0 : 1;
                grid = slot.Camera.IsOrthographic()
                    ? Renderer2D::MakeGrid(slot.Camera.GetFocalPoint(), m_Renderer->GetRenderOrigin(), cell, axis, slot.Camera.GetOrthoHalfHeight())
                    : Renderer2D::MakeGrid(slot.Camera.GetPosition(), m_Renderer->GetRenderOrigin(), cell, 1, slot.Camera.GetDistance());
            }
            if (m_SceneState == SceneState::Play)
                flags &= ~Renderer::ExtraViewOutline;

            m_Renderer->SubmitExtraView(static_cast<uint32_t>(index), slot.Camera, NRI::Extent2D{ drawSize.x, drawSize.y }, showGrid ? &grid : nullptr, false, flags);
            slot.Submitted = true;
        }
    }

    std::vector<UI::UIMenuItem> EditorV2Layer::BuildViewportOptionsItems(int index)
    {
        ViewportSlot& slot = m_Viewports[index];
        auto toggleFlag = [&slot](uint32_t bit, bool inverted)
        {
            const bool on = ((slot.ShowFlags & bit) != 0) != inverted;
            slot.ShowFlags = (!on != inverted) ? (slot.ShowFlags | bit) : (slot.ShowFlags & ~bit);
        };
        const bool lit = (slot.ShowFlags & Renderer::ExtraViewUnlit) == 0;
        const bool wf = (slot.ShowFlags & Renderer::ExtraViewWireframe) != 0;
        const bool sky = (slot.ShowFlags & Renderer::ExtraViewNoSky) == 0;
        const bool particles = (slot.ShowFlags & Renderer::ExtraViewNoParticles) == 0;
        const bool outline = (slot.ShowFlags & Renderer::ExtraViewOutline) != 0;

        return {
            {"While it is not the active viewport (full render)", "", {}, false, false, false, true},
            {"Lit", "", [&slot]() { slot.ShowFlags &= ~Renderer::ExtraViewUnlit; }, false, true, lit},
            {"Unlit", "", [&slot]() { slot.ShowFlags |= Renderer::ExtraViewUnlit; }, false, true, !lit, false,
             "Base colors without lighting or exposure."},
            {"", "", {}, true},
            {"Wireframe", "", [toggleFlag]() mutable { toggleFlag(Renderer::ExtraViewWireframe, false); }, false, true, wf, false,
             "The triangles' edges over the image."},
            {"Sky", "", [toggleFlag]() mutable { toggleFlag(Renderer::ExtraViewNoSky, true); }, false, true, sky, false,
             "The sky behind the geometry (off: black)."},
            {"Particles", "", [toggleFlag]() mutable { toggleFlag(Renderer::ExtraViewNoParticles, true); }, false, true, particles, false,
             "The scene's particle systems."},
            {"Selection Outline", "", [toggleFlag]() mutable { toggleFlag(Renderer::ExtraViewOutline, false); }, false, true, outline, false,
             "The orange outline of the selected entities."},
            {"Grid", "", [&slot]() { slot.ShowGrid = !slot.ShowGrid; }, false, true, slot.ShowGrid},
            {"", "", {}, true},
            {"Full Resolution", "", [&slot]() { slot.ResolutionIndex = 0; }, false, true, slot.ResolutionIndex == 0},
            {"Half Resolution", "", [&slot]() { slot.ResolutionIndex = 1; }, false, true, slot.ResolutionIndex == 1},
            {"Quarter Resolution", "", [&slot]() { slot.ResolutionIndex = 2; }, false, true, slot.ResolutionIndex == 2},
            {"", "", {}, true},
            {"Camera Position", "", [&slot]() { slot.ShowCameraPosition = !slot.ShowCameraPosition; }, false, true, slot.ShowCameraPosition, false,
             "Show world-unit coordinates next to the axis indicator."},
            {"Realtime", "", [&slot]() { slot.Realtime = !slot.Realtime; }, false, true, slot.Realtime, false,
             "Off: drawn again only when its camera, the scene or the selection change."}
        };
    }

    int EditorV2Layer::GridPlaneAxis() const
    {
        switch (m_EditorCamera.GetViewMode())
        {
        case EditorViewMode::Front:
        case EditorViewMode::Back: return 2;
        case EditorViewMode::Left:
        case EditorViewMode::Right: return 0;
        default: return 1;
        }
    }

    void EditorV2Layer::SetViewMode(EditorViewMode mode)
    {
        const bool wasOrthographic = m_EditorCamera.IsOrthographic();
        m_EditorCamera.SetViewMode(mode);
        SyncOrthoRenderSettings(wasOrthographic);
    }

    void EditorV2Layer::SyncOrthoRenderSettings(bool wasOrthographic)
    {
        if (!m_Renderer)
            return;
        const bool isOrthographic = m_EditorCamera.IsOrthographic();
        if (isOrthographic && !wasOrthographic)
        {
            m_OrthoSaved = { true, m_Renderer->isDLSSEnabled(), m_Renderer->getRayTracingEnabled(), m_Renderer->isPathTracingEnabled() };
            m_Renderer->setDLSSEnabled(false);
            m_Renderer->setRayTracingEnabled(false);
            m_Renderer->setPathTracingEnabled(false);
        }
        else if (!isOrthographic && wasOrthographic && m_OrthoSaved.Valid)
        {
            m_Renderer->setDLSSEnabled(m_OrthoSaved.Dlss);
            m_Renderer->setRayTracingEnabled(m_OrthoSaved.RayTracing);
            m_Renderer->setPathTracingEnabled(m_OrthoSaved.PathTracing);
            m_OrthoSaved.Valid = false;
        }
    }

    void EditorV2Layer::DrawWorldGrid()
    {
        if (!m_Renderer2D || !m_Renderer)
            return;
        const float cell = std::max(m_ViewportTools.LocationSnapWorldUnits(), WorldUnits::FromMeters(1.0f));
        if (m_EditorCamera.IsOrthographic())
            m_Renderer2D->DrawGrid(m_EditorCamera.GetFocalPoint(), m_Renderer->GetRenderOrigin(), cell, GridPlaneAxis(), m_EditorCamera.GetOrthoHalfHeight());
        else
            m_Renderer2D->DrawGrid(m_EditorCamera.GetPosition(), m_Renderer->GetRenderOrigin(), cell, 1, m_EditorCamera.GetDistance());
    }

    void EditorV2Layer::DrawReferenceFigure(const glm::dvec3& worldPosition)
    {
        if (!m_Renderer2D || !m_Renderer)
            return;
        const glm::vec3 position = m_Renderer->ToRenderSpace(worldPosition);
        auto cm = [](float centimeters) { return WorldUnits::FromCentimeters(centimeters); };
        const glm::vec4 color(1.0f, 0.62f, 0.2f, 0.9f);

        auto box = [&](float minX, float maxX, float minY, float maxY, float minZ, float maxZ)
        {
            const glm::vec3 low = position + glm::vec3(cm(minX), cm(minY), cm(minZ));
            const glm::vec3 high = position + glm::vec3(cm(maxX), cm(maxY), cm(maxZ));
            const glm::vec3 corners[8] = {
                { low.x, low.y, low.z }, { high.x, low.y, low.z }, { high.x, low.y, high.z }, { low.x, low.y, high.z },
                { low.x, high.y, low.z }, { high.x, high.y, low.z }, { high.x, high.y, high.z }, { low.x, high.y, high.z }
            };
            constexpr int edges[12][2] = { {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7} };
            for (const auto& edge : edges)
                m_Renderer2D->DrawThinLine(corners[edge[0]], corners[edge[1]], color);
        };

        box(-18.0f, -1.0f, 0.0f, 88.0f, -10.0f, 10.0f);
        box(1.0f, 18.0f, 0.0f, 88.0f, -10.0f, 10.0f);
        box(-22.0f, 22.0f, 88.0f, 143.0f, -11.0f, 11.0f);
        box(-31.0f, -22.0f, 83.0f, 143.0f, -5.0f, 5.0f);
        box(22.0f, 31.0f, 83.0f, 143.0f, -5.0f, 5.0f);
        box(-4.0f, 4.0f, 143.0f, 151.0f, -4.0f, 4.0f);
        box(-10.0f, 10.0f, 151.0f, 180.0f, -12.0f, 12.0f);
    }

    namespace
    {
        glm::vec3 DecomposeWorldScale(const glm::mat4& worldMatrix)
        {
            return glm::vec3(
                glm::length(glm::vec3(worldMatrix[0])),
                glm::length(glm::vec3(worldMatrix[1])),
                glm::length(glm::vec3(worldMatrix[2])));
        }

        glm::mat4 RigidPart(const glm::mat4& worldMatrix)
        {
            glm::mat4 result = worldMatrix;
            for (int c = 0; c < 3; ++c)
            {
                const float l = glm::length(glm::vec3(worldMatrix[c]));
                if (l > 1e-6f)
                    result[c] = glm::vec4(glm::vec3(worldMatrix[c]) / l, 0.0f);
            }
            return result;
        }
    }

    void EditorV2Layer::DrawPhysicsColliders3D()
    {
        if (!m_ActiveScene || !m_Renderer || !m_Renderer2D)
            return;

        constexpr glm::vec4 kActiveColor(0.2f, 1.0f, 0.3f, 0.9f);
        constexpr glm::vec4 kInactiveColor(1.0f, 0.3f, 0.25f, 0.9f);
        constexpr glm::vec4 kCharacterColor(0.3f, 0.75f, 1.0f, 0.9f);
        constexpr int kCircleSegments = 32;

        auto colorOf = [this, kActiveColor, kInactiveColor](entt::entity handle)
        {
            for (Entity entity(handle, m_ActiveScene.get()); entity; entity = entity.GetParent())
            {
                if (entity.HasComponent<RigidBody3DComponent>())
                    return kActiveColor;
            }
            return kInactiveColor;
        };
        auto line = [this](const glm::vec3& a, const glm::vec3& b, const glm::vec4& color) { m_Renderer2D->DrawXRayLine(a, b, color); };

        auto drawCircle = [&](const glm::mat4& transform, const glm::vec3& centre, float radius, int axis, const glm::vec4& color)
        {
            const int a = (axis + 1) % 3, b = (axis + 2) % 3;
            glm::vec3 previous(0.0f);
            for (int i = 0; i <= kCircleSegments; ++i)
            {
                const float t = glm::two_pi<float>() * static_cast<float>(i) / kCircleSegments;
                glm::vec3 local = centre;
                local[a] += std::cos(t) * radius;
                local[b] += std::sin(t) * radius;
                const glm::vec3 point = glm::vec3(transform * glm::vec4(local, 1.0f));
                if (i > 0)
                    line(previous, point, color);
                previous = point;
            }
        };

        auto drawCapsule = [&](const glm::mat4& transform, const glm::vec3& centre, float halfHeight, float top, float bottom, const glm::vec4& color)
        {
            const float sine = halfHeight > 1e-6f ? std::clamp((bottom - top) / (2.0f * halfHeight), -1.0f, 1.0f) : 0.0f;
            const float tangent = std::asin(sine);
            const glm::vec3 topCentre = centre + glm::vec3(0.0f, halfHeight, 0.0f);
            const glm::vec3 bottomCentre = centre - glm::vec3(0.0f, halfHeight, 0.0f);
            auto at = [&](const glm::vec3& c, float radius, const glm::vec3& across, float angle)
            {
                return glm::vec3(transform * glm::vec4(c + radius * (std::cos(angle) * across + std::sin(angle) * glm::vec3(0.0f, 1.0f, 0.0f)), 1.0f));
            };
            auto arc = [&](const glm::vec3& c, float radius, const glm::vec3& across, float from, float to)
            {
                constexpr int kSegments = 16;
                glm::vec3 previous = at(c, radius, across, from);
                for (int i = 1; i <= kSegments; ++i)
                {
                    const glm::vec3 point = at(c, radius, across, from + (to - from) * static_cast<float>(i) / kSegments);
                    line(previous, point, color);
                    previous = point;
                }
            };
            for (const glm::vec3& across : { glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f) })
            {
                arc(topCentre, top, across, tangent, glm::pi<float>() - tangent);
                arc(bottomCentre, bottom, across, glm::pi<float>() - tangent, glm::two_pi<float>() + tangent);
                line(at(topCentre, top, across, tangent), at(bottomCentre, bottom, across, tangent), color);
                line(at(topCentre, top, across, glm::pi<float>() - tangent), at(bottomCentre, bottom, across, glm::pi<float>() - tangent), color);
            }
            drawCircle(transform, topCentre + glm::vec3(0.0f, top * sine, 0.0f), top * std::cos(tangent), 1, color);
            drawCircle(transform, bottomCentre + glm::vec3(0.0f, bottom * sine, 0.0f), bottom * std::cos(tangent), 1, color);
        };

        auto boxView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, BoxCollider3DComponent>();
        for (auto entity : boxView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, box] = boxView.get<WorldTransformComponent, BoxCollider3DComponent>(entity);
            const glm::vec4 color = colorOf(entity);
            const glm::vec3 halfExtents = box.HalfExtents * DecomposeWorldScale(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            const glm::mat4 transform = RigidPart(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            glm::vec3 corners[8];
            for (int i = 0; i < 8; ++i)
            {
                const glm::vec3 sign((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
                corners[i] = glm::vec3(transform * glm::vec4(box.Offset + sign * halfExtents, 1.0f));
            }
            constexpr int edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
            for (const auto& edge : edges)
                line(corners[edge[0]], corners[edge[1]], color);
        }

        auto sphereView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, SphereCollider3DComponent>();
        for (auto entity : sphereView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, sphere] = sphereView.get<WorldTransformComponent, SphereCollider3DComponent>(entity);
            const glm::vec4 color = colorOf(entity);
            const glm::vec3 worldScale = DecomposeWorldScale(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            const float radius = sphere.Radius * std::max({ worldScale.x, worldScale.y, worldScale.z });
            const glm::mat4 transform = RigidPart(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            for (int axis = 0; axis < 3; ++axis)
                drawCircle(transform, sphere.Offset, radius, axis, color);
        }

        auto capsuleView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, CapsuleCollider3DComponent>();
        for (auto entity : capsuleView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, capsule] = capsuleView.get<WorldTransformComponent, CapsuleCollider3DComponent>(entity);
            const glm::vec3 worldScale = DecomposeWorldScale(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            const float horizontal = std::max(worldScale.x, worldScale.z);
            const float top = capsule.Radius * horizontal;
            const float bottom = capsule.BottomRadius > 0.0f ? capsule.BottomRadius * horizontal : top;
            drawCapsule(RigidPart(m_Renderer->ToRenderSpace(wtc.WorldMatrix)), capsule.Offset, capsule.HalfHeight * worldScale.y, top, bottom, colorOf(entity));
        }

        auto characterView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, CharacterController3DComponent>();
        for (auto entity : characterView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, controller] = characterView.get<WorldTransformComponent, CharacterController3DComponent>(entity);
            const float radius = std::max(controller.Radius, WorldUnits::FromCentimeters(1.0f));
            const float halfHeight = 0.5f * std::max(controller.Height, 0.0f);
            const glm::mat4 transform = glm::translate(glm::mat4(1.0f), m_Renderer->ToRenderSpace(wtc.GetPosition()));
            drawCapsule(transform, glm::vec3(0.0f, halfHeight + radius, 0.0f), halfHeight, radius, radius, kCharacterColor);
        }

        auto cylinderView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, CylinderCollider3DComponent>();
        for (auto entity : cylinderView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, cylinder] = cylinderView.get<WorldTransformComponent, CylinderCollider3DComponent>(entity);
            const glm::vec4 color = colorOf(entity);
            const glm::vec3 worldScale = DecomposeWorldScale(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            const float halfHeight = cylinder.HalfHeight * worldScale.y;
            const float horizontal = std::max(worldScale.x, worldScale.z);
            const float top = cylinder.Radius * horizontal;
            const float bottom = (cylinder.BottomRadius > 0.0f ? cylinder.BottomRadius : cylinder.Radius) * horizontal;
            const glm::mat4 transform = RigidPart(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            const glm::vec3 topCentre = cylinder.Offset + glm::vec3(0.0f, halfHeight, 0.0f);
            const glm::vec3 bottomCentre = cylinder.Offset - glm::vec3(0.0f, halfHeight, 0.0f);
            if (top > 0.0f)
                drawCircle(transform, topCentre, top, 1, color);
            drawCircle(transform, bottomCentre, bottom, 1, color);
            for (int i = 0; i < 8; ++i)
            {
                const float t = glm::quarter_pi<float>() * static_cast<float>(i);
                const glm::vec3 direction(std::cos(t), 0.0f, std::sin(t));
                line(glm::vec3(transform * glm::vec4(topCentre + direction * top, 1.0f)),
                     glm::vec3(transform * glm::vec4(bottomCentre + direction * bottom, 1.0f)), color);
            }
        }

        glm::dvec3 viewPosition = m_EditorCamera.GetPosition();
        if (m_SceneState == SceneState::Play)
        {
            if (Entity camera = m_ActiveScene->GetPrimaryCameraEntity())
                viewPosition = camera.GetComponent<WorldTransformComponent>().GetPosition();
        }
        const glm::vec3 eye = m_Renderer->ToRenderSpace(viewPosition);
        auto planeView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, PlaneCollider3DComponent>();
        for (auto entity : planeView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, plane] = planeView.get<WorldTransformComponent, PlaneCollider3DComponent>(entity);
            const glm::vec4 color = colorOf(entity);
            const glm::vec3 worldScale = DecomposeWorldScale(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            const glm::mat4 transform = RigidPart(m_Renderer->ToRenderSpace(wtc.WorldMatrix));
            auto point = [&](float x, float y, float z) { return glm::vec3(transform * glm::vec4(plane.Offset + glm::vec3(x, y, z), 1.0f)); };
            if (plane.HalfSize.x > 0.0f && plane.HalfSize.y > 0.0f)
            {
                const float x = plane.HalfSize.x * worldScale.x, z = plane.HalfSize.y * worldScale.z;
                line(point(-x, 0, -z), point(x, 0, -z), color);
                line(point(x, 0, -z), point(x, 0, z), color);
                line(point(x, 0, z), point(-x, 0, z), color);
                line(point(-x, 0, z), point(-x, 0, -z), color);
                line(point(0, 0, 0), point(0, WorldUnits::FromMeters(1.0f), 0), color);
                continue;
            }
            glm::vec4 endlessColor = color;
            for (Entity owner(entity, m_ActiveScene.get()); owner; owner = owner.GetParent())
            {
                if (owner.HasComponent<RigidBody3DComponent>())
                {
                    if (owner.GetComponent<RigidBody3DComponent>().Type == RigidBody3DComponent::BodyType::Dynamic)
                        endlessColor = kInactiveColor;
                    break;
                }
            }
            const glm::vec3 origin = point(0, 0, 0);
            const glm::vec3 alongX = glm::vec3(transform[0]), alongZ = glm::vec3(transform[2]);
            const float step = WorldUnits::FromMeters(5.0f);
            const float reach = WorldUnits::FromMeters(50.0f);
            const float nearX = std::round(glm::dot(eye - origin, alongX) / step) * step;
            const float nearZ = std::round(glm::dot(eye - origin, alongZ) / step) * step;
            for (float offset = -reach; offset <= reach + 0.5f * step; offset += step)
            {
                line(point(nearX - reach, 0, nearZ + offset), point(nearX + reach, 0, nearZ + offset), endlessColor);
                line(point(nearX + offset, 0, nearZ - reach), point(nearX + offset, 0, nearZ + reach), endlessColor);
            }
            line(origin, point(0, WorldUnits::FromMeters(1.0f), 0), endlessColor);
        }

        auto animatorView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, AnimatorComponent>();
        for (auto entity : animatorView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, animator] = animatorView.get<WorldTransformComponent, AnimatorComponent>(entity);
            const AnimationGraphInstance& graph = animator.GraphInstance;
            if (graph.FootProbes.empty())
                continue;
            const glm::mat4 world = m_Renderer->ToRenderSpace(wtc.WorldMatrix);
            const float meter = WorldUnits::FromMeters(1.0f);
            for (const AnimFootProbe& probe : graph.FootProbes)
            {
                const glm::vec3 foot = glm::vec3(world * glm::vec4(probe.Position, 1.0f));
                const glm::vec3 start = foot + glm::vec3(0.0f, probe.Up * meter, 0.0f);
                const glm::vec3 end = foot - glm::vec3(0.0f, probe.Down * meter, 0.0f);
                const auto ground = graph.FootGround.find(probe.Bone);
                if (ground == graph.FootGround.end() || !ground->second.Hit)
                {
                    line(start, end, UI::ThemeColor("DebugFootNoGround", { 1.0f, 0.25f, 0.2f, 1.0f }));
                    continue;
                }
                const glm::vec3 hit = glm::vec3(world * glm::vec4(ground->second.Point, 1.0f));
                const glm::vec4 green(0.2f, 1.0f, 0.3f, 1.0f);
                line(start, hit, green);
                const float c = WorldUnits::FromCentimeters(4.0f);
                line(hit - glm::vec3(c, 0, 0), hit + glm::vec3(c, 0, 0), green);
                line(hit - glm::vec3(0, 0, c), hit + glm::vec3(0, 0, c), green);
                line(hit, hit + glm::vec3(world * glm::vec4(ground->second.Normal, 0.0f)) * (3.0f * c), green);
            }
        }

        auto meshView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, MeshCollider3DComponent>();
        for (auto entity : meshView)
        {
            if (!m_ActiveScene->IsActiveInHierarchy(entity))
                continue;
            auto [wtc, mesh] = meshView.get<WorldTransformComponent, MeshCollider3DComponent>(entity);
            if (mesh.Mesh == 0)
                continue;
            bool dynamic = false;
            for (Entity owner(entity, m_ActiveScene.get()); owner; owner = owner.GetParent())
            {
                if (owner.HasComponent<RigidBody3DComponent>())
                {
                    dynamic = owner.GetComponent<RigidBody3DComponent>().Type == RigidBody3DComponent::BodyType::Dynamic;
                    break;
                }
            }
            const std::vector<glm::vec3>& lines = MeshColliderWireframe(mesh, mesh.Convex || dynamic);
            const glm::vec4 color = colorOf(entity);
            const glm::mat4 world = m_Renderer->ToRenderSpace(wtc.WorldMatrix);
            for (size_t i = 0; i + 1 < lines.size(); i += 2)
                line(glm::vec3(world * glm::vec4(lines[i], 1.0f)), glm::vec3(world * glm::vec4(lines[i + 1], 1.0f)), color);
        }
    }

    void EditorV2Layer::OnOverlayRender()
    {
        if (!m_Renderer || !m_Renderer2D || !m_ActiveScene)
            return;

        if (m_SceneState == SceneState::Play)
        {
            Entity camera = m_ActiveScene->GetPrimaryCameraEntity();
            if (!camera)
                return;
            m_Renderer->BeginScene(camera.GetComponent<CameraComponent>().Camera, camera.GetComponent<WorldTransformComponent>().WorldMatrix);
        }
        else
        {
            m_Renderer->BeginScene(m_EditorCamera);
        }

        if (m_SceneState != SceneState::Play && m_ViewportTools.ShowGrid)
            DrawWorldGrid();

        if (m_SceneState != SceneState::Play && m_ViewportTools.ShowReferenceFigure)
        {
            glm::dvec3 underCursor;
            const bool vpHovered = m_ViewportCanvas && UI::HasState(m_ViewportCanvas->GetState(), UI::UIState::Hovered);
            if (m_ViewportTools.ReferenceFigureAtCursor && vpHovered && m_ViewportCanvas->PickWorldPoint(underCursor))
                m_ViewportTools.ReferenceFigurePosition = underCursor;
            DrawReferenceFigure(m_ViewportTools.ReferenceFigureAtCursor ? m_ViewportTools.ReferenceFigurePosition : glm::dvec3(0.0));
        }

        if (m_SceneState != SceneState::Play)
        {
            auto rectLights = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, RectLightComponent>();
            for (auto entity : rectLights)
            {
                auto [wtc, light] = rectLights.get<WorldTransformComponent, RectLightComponent>(entity);
                const glm::mat4 world = m_Renderer->ToRenderSpace(wtc.WorldMatrix);
                const glm::vec3 centre = glm::vec3(world[3]);
                const glm::vec3 halfX = glm::vec3(world[0]) * (0.5f * light.Width);
                const glm::vec3 halfY = glm::vec3(world[1]) * (0.5f * light.Height);
                const glm::vec3 corners[4] = { centre - halfX - halfY, centre + halfX - halfY, centre + halfX + halfY, centre - halfX + halfY };
                const glm::vec4 colour(glm::mix(light.Color, glm::vec3(1.0f, 0.9f, 0.4f), 0.5f), 1.0f);
                for (int i = 0; i < 4; ++i)
                    m_Renderer2D->DrawXRayLine(corners[i], corners[(i + 1) % 4], colour);
                const glm::vec3 forward = -glm::vec3(world[2]);
                if (glm::length(forward) > 1e-6f)
                    m_Renderer2D->DrawXRayLine(centre, centre + glm::normalize(forward) * 0.5f * (glm::length(halfX) + glm::length(halfY)), colour);
            }

            auto probes = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, ReflectionProbeComponent>();
            for (auto entity : probes)
            {
                auto [wtc, probe] = probes.get<WorldTransformComponent, ReflectionProbeComponent>(entity);
                const glm::mat4 world = m_Renderer->ToRenderSpace(wtc.WorldMatrix);
                glm::vec3 corners[8];
                for (int i = 0; i < 8; ++i)
                    corners[i] = glm::vec3(world * glm::vec4((i & 1) ? probe.Extents.x : -probe.Extents.x, (i & 2) ? probe.Extents.y : -probe.Extents.y,
                                                             (i & 4) ? probe.Extents.z : -probe.Extents.z, 1.0f));
                const glm::vec4 colour(0.4f, 0.8f, 1.0f, 1.0f);
                for (int i = 0; i < 8; ++i)
                    for (int axis = 1; axis < 8; axis <<= 1)
                        if (!(i & axis))
                            m_Renderer2D->DrawXRayLine(corners[i], corners[i | axis], colour);
            }

            auto volumes = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, IrradianceVolumeComponent>();
            for (auto entity : volumes)
            {
                auto [wtc, volume] = volumes.get<WorldTransformComponent, IrradianceVolumeComponent>(entity);
                const glm::mat4 world = m_Renderer->ToRenderSpace(wtc.WorldMatrix);
                glm::vec3 corners[8];
                for (int i = 0; i < 8; ++i)
                    corners[i] = glm::vec3(world * glm::vec4((i & 1) ? volume.Extents.x : -volume.Extents.x, (i & 2) ? volume.Extents.y : -volume.Extents.y,
                                                             (i & 4) ? volume.Extents.z : -volume.Extents.z, 1.0f));
                const glm::vec4 colour(1.0f, 0.75f, 0.3f, 1.0f);
                for (int i = 0; i < 8; ++i)
                    for (int axis = 1; axis < 8; axis <<= 1)
                        if (!(i & axis))
                            m_Renderer2D->DrawXRayLine(corners[i], corners[i | axis], colour);
            }

            auto discLights = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, DiscLightComponent>();
            for (auto entity : discLights)
            {
                auto [wtc, light] = discLights.get<WorldTransformComponent, DiscLightComponent>(entity);
                const glm::mat4 world = m_Renderer->ToRenderSpace(wtc.WorldMatrix);
                const glm::vec3 centre = glm::vec3(world[3]);
                const float scale = glm::max(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])));
                const glm::vec3 axisX = glm::length(glm::vec3(world[0])) > 1e-6f ? glm::normalize(glm::vec3(world[0])) * light.Radius * scale : glm::vec3(0.0f);
                const glm::vec3 axisY = glm::length(glm::vec3(world[1])) > 1e-6f ? glm::normalize(glm::vec3(world[1])) * light.Radius * scale : glm::vec3(0.0f);
                const glm::vec4 colour(glm::mix(light.Color, glm::vec3(1.0f, 0.9f, 0.4f), 0.5f), 1.0f);
                constexpr int Segments = 32;
                for (int i = 0; i < Segments; ++i)
                {
                    const float a0 = glm::two_pi<float>() * static_cast<float>(i) / Segments;
                    const float a1 = glm::two_pi<float>() * static_cast<float>(i + 1) / Segments;
                    m_Renderer2D->DrawXRayLine(centre + axisX * std::cos(a0) + axisY * std::sin(a0), centre + axisX * std::cos(a1) + axisY * std::sin(a1), colour);
                }
                const glm::vec3 forward = -glm::vec3(world[2]);
                if (glm::length(forward) > 1e-6f)
                    m_Renderer2D->DrawXRayLine(centre, centre + glm::normalize(forward) * light.Radius * scale, colour);
            }

            Entity selected = m_Selection.GetPrimary();
            if (selected && selected.HasComponent<AudioSourceComponent>() && selected.HasComponent<WorldTransformComponent>() &&
                selected.GetComponent<AudioSourceComponent>().Spatial && !selected.HasComponent<InactiveInHierarchyComponent>())
            {
                const auto& source = selected.GetComponent<AudioSourceComponent>();
                const glm::vec3 centre = glm::vec3(m_Renderer->ToRenderSpace(selected.GetComponent<WorldTransformComponent>().WorldMatrix)[3]);
                auto sphere = [&](float meters, const glm::vec4& colour)
                {
                    const float radius = WorldUnits::FromMeters(meters);
                    constexpr int Segments = 48;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        const int a = (axis + 1) % 3, b = (axis + 2) % 3;
                        for (int i = 0; i < Segments; ++i)
                        {
                            const float t0 = glm::two_pi<float>() * static_cast<float>(i) / Segments;
                            const float t1 = glm::two_pi<float>() * static_cast<float>(i + 1) / Segments;
                            glm::vec3 p0 = centre, p1 = centre;
                            p0[a] += std::cos(t0) * radius; p0[b] += std::sin(t0) * radius;
                            p1[a] += std::cos(t1) * radius; p1[b] += std::sin(t1) * radius;
                            m_Renderer2D->DrawXRayLine(p0, p1, colour);
                        }
                    }
                };
                sphere(source.Spatialization.MinDistance, UI::ThemeColor("AudioMinDistance", { 0.55f, 0.85f, 1.0f, 0.9f }));
                sphere(source.Spatialization.MaxDistance, UI::ThemeColor("AudioMaxDistance", { 0.2f, 0.4f, 1.0f, 0.7f }));
            }

            constexpr glm::vec4 kVolumeColor(1.0f, 0.55f, 0.1f, 0.9f);
            constexpr glm::vec4 kAudioVolumeColor(0.3f, 0.6f, 1.0f, 0.9f);
            constexpr int edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
            auto drawBox = [&](const WorldTransformComponent& wtc, const glm::vec3& halfExtents, const glm::vec4& color)
            {
                glm::vec3 corners[8];
                for (int i = 0; i < 8; ++i)
                {
                    const glm::vec3 sign((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
                    corners[i] = glm::vec3(m_Renderer->ToRenderSpace(wtc.WorldMatrix) * glm::vec4(sign * halfExtents, 1.0f));
                }
                for (const auto& edge : edges)
                    m_Renderer2D->DrawXRayLine(corners[edge[0]], corners[edge[1]], color);
            };
            auto streamView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, LevelStreamingVolumeComponent>();
            for (auto entity : streamView)
            {
                if (Entity(entity, m_ActiveScene.get()).HasComponent<InactiveInHierarchyComponent>())
                    continue;
                auto [wtc, volume] = streamView.get<WorldTransformComponent, LevelStreamingVolumeComponent>(entity);
                drawBox(wtc, volume.HalfExtents, kVolumeColor);
            }
            constexpr glm::vec4 kPostProcessVolumeColor(0.75f, 0.45f, 1.0f, 0.9f);
            auto postView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, PostProcessVolumeComponent>();
            for (auto entity : postView)
            {
                if (Entity(entity, m_ActiveScene.get()).HasComponent<InactiveInHierarchyComponent>())
                    continue;
                auto [wtc, volume] = postView.get<WorldTransformComponent, PostProcessVolumeComponent>(entity);
                if (!volume.Unbound)
                    drawBox(wtc, volume.HalfExtents, kPostProcessVolumeColor);
            }
            auto audioView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, AudioVolumeComponent>();
            for (auto entity : audioView)
            {
                if (Entity(entity, m_ActiveScene.get()).HasComponent<InactiveInHierarchyComponent>())
                    continue;
                auto [wtc, volume] = audioView.get<WorldTransformComponent, AudioVolumeComponent>(entity);
                drawBox(wtc, volume.HalfExtents, kAudioVolumeColor);
            }
            constexpr glm::vec4 kProbeVolumeColor(0.35f, 0.9f, 0.55f, 0.9f);
            auto probeView = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, AcousticProbeVolumeComponent>();
            for (auto entity : probeView)
            {
                if (Entity(entity, m_ActiveScene.get()).HasComponent<InactiveInHierarchyComponent>())
                    continue;
                auto [wtc, volume] = probeView.get<WorldTransformComponent, AcousticProbeVolumeComponent>(entity);
                drawBox(wtc, volume.HalfExtents, kProbeVolumeColor);
                if (selected != Entity(entity, m_ActiveScene.get()) || volume.BakedData == 0 || AssetManager::RequestAsset(volume.BakedData) != AssetState::Ready)
                    continue;
                const Ref<AcousticData> data = AssetManager::GetAsset<AcousticData>(volume.BakedData);
                const float arm = WorldUnits::FromMeters(0.15f);
                for (const glm::vec3& probe : data->ProbePositions)
                {
                    const glm::vec3 p = glm::vec3(m_Renderer->ToRenderSpace(data->Origin + glm::dvec3(probe)));
                    m_Renderer2D->DrawXRayLine(p - glm::vec3(arm, 0, 0), p + glm::vec3(arm, 0, 0), kProbeVolumeColor);
                    m_Renderer2D->DrawXRayLine(p - glm::vec3(0, arm, 0), p + glm::vec3(0, arm, 0), kProbeVolumeColor);
                    m_Renderer2D->DrawXRayLine(p - glm::vec3(0, 0, arm), p + glm::vec3(0, 0, arm), kProbeVolumeColor);
                }
            }
        }

        // A world's loaded cells: the content bounds of each, in green (World Partition window open, Show in Viewport).
        if (m_WorldPartitionPanel.GetRoot() && m_WorldPartitionPanel.ShowCellsInViewport() && m_ActiveScene && m_ActiveScene->IsWorldPartitioned())
        {
            UILayer* ui = Application::Get().GetUILayer();
            if (ui && ui->GetDockSpaceRef() && ui->GetDockSpaceRef()->HasPanel("World Partition"))
            {
                constexpr glm::vec4 kCellColor(0.3f, 0.85f, 0.45f, 0.8f);
                constexpr int edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
                const WorldPartition& world = m_ActiveScene->GetWorldPartition();
                for (const WorldCellCoord& cell : m_ActiveScene->GetLoadedWorldCells(false))
                {
                    const auto info = world.Cells.find(cell);
                    if (info == world.Cells.end())
                        continue;
                    glm::vec3 corners[8];
                    for (int i = 0; i < 8; ++i)
                        corners[i] = m_Renderer->ToRenderSpace(glm::dvec3((i & 1) ? info->second.BoundsMax.x : info->second.BoundsMin.x,
                                                                          (i & 2) ? info->second.BoundsMax.y : info->second.BoundsMin.y,
                                                                          (i & 4) ? info->second.BoundsMax.z : info->second.BoundsMin.z));
                    for (const auto& edge : edges)
                        m_Renderer2D->DrawXRayLine(corners[edge[0]], corners[edge[1]], kCellColor);
                }
            }
        }

        if (m_ViewportTools.ShowPhysicsColliders)
        {
            {
                auto view = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, BoxCollider2DComponent>();
                for (auto entity : view)
                {
                    auto [wtc, bc2d] = view.get<WorldTransformComponent, BoxCollider2DComponent>(entity);
                    glm::mat4 transform = m_Renderer->ToRenderSpace(wtc.WorldMatrix) *
                                          glm::translate(glm::mat4(1.0f), glm::vec3(bc2d.Offset, 0.001f)) *
                                          glm::scale(glm::mat4(1.0f), glm::vec3(bc2d.Size * 2.0f, 1.0f));
                    m_Renderer2D->DrawRect(transform, glm::vec4(0, 1, 0, 1));
                }
            }
            {
                auto view = m_ActiveScene->GetAllEntitiesWith<WorldTransformComponent, CircleCollider2DComponent>();
                for (auto entity : view)
                {
                    auto [wtc, cc2d] = view.get<WorldTransformComponent, CircleCollider2DComponent>(entity);
                    glm::mat4 transform = m_Renderer->ToRenderSpace(wtc.WorldMatrix) *
                                          glm::translate(glm::mat4(1.0f), glm::vec3(cc2d.Offset, 0.001f)) *
                                          glm::scale(glm::mat4(1.0f), glm::vec3(cc2d.Radius * 2.0f));
                    m_Renderer2D->DrawCircle(transform, glm::vec4(0, 1, 0, 1), 0.01f);
                }
            }
            DrawPhysicsColliders3D();
        }

        m_Renderer->EndScene();
    }

    void EditorV2Layer::RefreshToolBar()
    {
        const bool isPlay = m_SceneState == SceneState::Play;
        const bool isSim = m_SceneState == SceneState::Simulate;
        const bool isEdit = m_SceneState == SceneState::Edit;
        const bool isPaused = !isEdit && m_ActiveScene && m_ActiveScene->IsPaused();
        const bool prefab = m_PrefabMode.Active; // (nothing plays while a prefab is edited: its bar takes the toolbar)

        if (m_PlayBtnWrap)
            m_PlayBtnWrap->Visible = !prefab && (isEdit || isPlay);
        if (m_SimulateBtnWrap)
            m_SimulateBtnWrap->Visible = !prefab && (isEdit || isSim);
        if (m_PrefabBar)
            m_PrefabBar->Visible = prefab;
        if (m_PauseBtnWrap)
            m_PauseBtnWrap->Visible = !isEdit;
        if (m_StepBtnWrap)
            m_StepBtnWrap->Visible = !isEdit && isPaused;

        if (m_PlayBtnWrap)
        {
            const Ref<Texture2D>& tex = isPlay ? m_IconStop : m_IconPlay;
            m_PlayBtnWrap->TextureSlot = tex ? tex->GetDescriptorIndexSlot() : ~0u;
        }
        if (m_SimulateBtnWrap)
        {
            const Ref<Texture2D>& tex = isSim ? m_IconStop : m_IconSimulate;
            m_SimulateBtnWrap->TextureSlot = tex ? tex->GetDescriptorIndexSlot() : ~0u;
        }
        if (m_PlaySettingsCogBtn)
        {
            m_PlaySettingsCogBtn->SetEnabled(isEdit);
            m_PlaySettingsCogBtn->Visible = !prefab;
        }
        if (m_PlayNetStatusLabel)
        {
            if (m_PlaySession)
            {
                m_PlayNetStatusLabel->Visible = true;
                m_PlayNetStatusLabel->Text = m_PlayWaitingForServer
                    ? ("Waiting for dedicated server -- " + m_PlaySession->Describe())
                    : m_PlaySession->Describe();
            }
            else
            {
                m_PlayNetStatusLabel->Visible = false;
            }
        }
    }

    void EditorV2Layer::RefreshSceneLoadOverlay()
    {
        if (!m_SceneLoadBanner)
            return;
        if (!m_SceneLoad)
        {
            m_SceneLoadBanner->Visible = false;
            return;
        }
        m_SceneLoadBanner->Visible = true;
        if (m_SceneLoadTitleLabel)
            m_SceneLoadTitleLabel->Text = "Opening " + m_SceneLoad->GetPath().filename().string();
        if (m_SceneLoadStageLabel)
        {
            char pct[128];
            std::snprintf(pct, sizeof(pct), "%s (%.0f%%)", m_SceneLoad->GetStage().c_str(), m_SceneLoad->GetProgress() * 100.0f);
            m_SceneLoadStageLabel->Text = pct;
        }
        if (m_SceneLoadProgressFill)
            m_SceneLoadProgressFill->FixedSize.x = std::clamp(m_SceneLoad->GetProgress(), 0.0f, 1.0f) * 300.0f;
    }

    void EditorV2Layer::RefreshStatusBar()
    {
        auto* activity = static_cast<StatusActivityElement*>(m_StatusActivity.get());
        if (!activity || !m_Renderer || !Project::GetActive() || !Project::GetActive()->GetEditorAssetManager())
            return;
        auto assetManager = Project::GetActive()->GetEditorAssetManager();
        const EditorAssetManager::ImportProgress import = assetManager->GetImportProgress();
        const size_t loading = assetManager->GetLoadingCount();
        const size_t streaming = assetManager->GetStreamingCount();
        const double pendingMB = static_cast<double>(assetManager->GetPendingUploadBytes()) / (1024.0 * 1024.0);
        const size_t blasBuilds = m_Renderer->GetPendingBlasBuilds();

        // Most important first: what the user started (an import, a script build, a scene opening), then background work.
        std::string text;
        if (import.Imports > 0)
            text = import.Total == 0 ? std::string("Importing: reading the file...") : std::format("Importing meshes {} / {}", import.Done, import.Total);
        else if (m_ScriptBuilder.IsBuilding())
            text = "Building scripts...";
        else if (m_SceneLoad)
            text = "Opening scene...";
        else if (m_TestWorldGeneration)
            text = std::format("Generating the streaming test world {:.0f} %: {}", m_TestWorldGeneration->GetProgress() * 100.0f, m_TestWorldGeneration->GetStage());
        else if (m_HLODBuild)
            text = std::format("Building HLODs {:.0f} %: {}", m_HLODBuild->GetProgress() * 100.0f, m_HLODBuild->GetStage());
        else if (const std::string busy = m_ContentBrowserPanel.GetBusyText(); !busy.empty())
            text = busy;
        if (loading > 0 || streaming > 0 || blasBuilds > 0)
        {
            std::string background;
            auto add = [&background](const std::string& part)
            {
                background += (background.empty() ? "" : "   |   ") + part;
            };
            if (loading > 0)
                add(std::format("Loading {} asset{}", loading, loading == 1 ? "" : "s"));
            if (streaming > 0)
                add(std::format("Streaming {} texture{}", streaming, streaming == 1 ? "" : "s"));
            if (pendingMB >= 0.05)
                add(std::format("{:.1f} MB to upload", pendingMB));
            if (blasBuilds > 0)
                add(std::format("{} BLAS build{}", blasBuilds, blasBuilds == 1 ? "" : "s"));
            text += (text.empty() ? "" : "   |   ") + background;
        }
        if (const auto& index = assetManager->GetReferenceIndex(); index && index->GetProgress().Building)
        {
            // The reference cache being built (a project's first open, or files changed since the last session).
            const AssetReferenceIndex::Progress progress = index->GetProgress();
            text += (text.empty() ? "" : "   |   ") + std::format("Indexing asset references {} / {}", progress.Done, progress.Total);
        }
        activity->Busy = !text.empty();
        activity->Text = text.empty() ? std::string("Ready") : text;
    }

    void EditorV2Layer::OnUpdate(Timestep ts)
    {
        if (!Project::GetActive())
            return;

#if NOX_WITH_MCP
        UpdateMcpServer(); // (the tool calls that came in run here, at the start of the frame)
#endif
        if (m_ResetLayoutRequested)
        {
            // Here, between frames: the menu that asked for it is gone and no UI event is being dispatched.
            m_ResetLayoutRequested = false;
            ResetLayout();
        }

        if (m_OpenSaveAndPlay)
        {
            m_OpenSaveAndPlay = false;
            ShowSaveAndPlayOverlay();
        }

        if (m_ThumbnailPending && m_SceneState == SceneState::Edit && !m_SceneLoad)
        {
            m_ThumbnailTimer += ts;
            if (m_ThumbnailTimer > 4.0f)
            {
                m_ThumbnailPending = false;
                CaptureProjectThumbnail();
            }
        }

        if (m_SceneLoad && m_SceneLoad->Step(8.0))
            FinishSceneLoad();

        m_CustomLitPollTimer += ts;
        if (m_CustomLitPollTimer > 0.5f)
        {
            m_CustomLitPollTimer = 0.0f;
            PollCustomLitShaders();
        }

        if (m_TestWorldGeneration && m_TestWorldGeneration->Step())
        {
            const AssetHandle scene = m_TestWorldGeneration->Failed() ? AssetHandle(0) : m_TestWorldGeneration->GetScene();
            const glm::dvec3 middle = m_TestWorldGeneration->GetMiddle();
            m_TestWorldGeneration.reset();
            if (scene != 0)
            {
                m_FocusOnOpen = middle;
                OpenScene(scene);
            }
        }

        // Build HLODs: a step a frame; done, the scene is saved (its index names the new HLOD cells).
        if (m_HLODBuild && m_HLODBuild->Step())
        {
            const bool failed = m_HLODBuild->Failed();
            const size_t built = m_HLODBuild->GetBuiltCount();
            m_HLODBuild.reset();
            if (!failed)
                SaveScene();
            NOX_CORE_INFO("Build HLODs: {} ({} HLODs)", failed ? "failed (see above)" : "done", built);
            Notify(failed ? UI::UINotificationSeverity::Error : UI::UINotificationSeverity::Success, failed ? "HLOD build failed" : "HLODs built",
                   failed ? std::string("See the Output Log.") : std::to_string(built) + " HLODs; the scene was saved.");
        }

        m_ScriptBuilder.Update();
        if (const ScriptBuilder::BuildResult& build = m_ScriptBuilder.GetLastResult(); build.Number != m_LastScriptBuildNotified)
        {
            m_LastScriptBuildNotified = build.Number;
            size_t errors = 0, warnings = 0;
            for (const ScriptDiagnostic& diagnostic : build.Diagnostics)
                (diagnostic.Severity == "error" ? errors : warnings) += 1;
            if (build.Succeeded)
                Notify(UI::UINotificationSeverity::Success, build.Reloaded ? "Scripts built and reloaded" : "Scripts built",
                       std::format("{:.1f} s{}", build.Seconds, warnings > 0 ? std::format(", {} warning{}", warnings, warnings == 1 ? "" : "s") : std::string()));
            else
                Notify(UI::UINotificationSeverity::Error, "Script build failed",
                       std::format("{} error{} -- the Output Log lists them.", errors, errors == 1 ? "" : "s"));
        }

        auto assetManager = Project::GetActive()->GetEditorAssetManager();
        if (assetManager)
        {
            if (m_EditorScene && m_EditorScene->ConsumeAssetReferencesChanged())
            {
                m_UnloadUnusedAssetsRequested = true;
                m_EditorSceneReferencesValid = false;
            }
            if (m_ActiveScene && m_ActiveScene != m_EditorScene && m_ActiveScene->ConsumeAssetReferencesChanged())
                m_UnloadUnusedAssetsRequested = true;
            if (assetManager->ConsumeLoadsSettledAfterSweep())
                m_UnloadUnusedAssetsRequested = true;
            if (m_UnloadUnusedAssetsRequested && std::chrono::steady_clock::now() - m_LastUnloadSweep >= std::chrono::seconds(1))
            {
                NOX_PROFILE_SCOPE("Unload Unused Assets");
                m_UnloadUnusedAssetsRequested = false;
                m_LastUnloadSweep = std::chrono::steady_clock::now();
                UnloadUnusedAssets();
            }

            for (const ModelInstance::LevelDescription& level : assetManager->ConsumeLevelImports())
            {
                if (m_EditorScene)
                {
                    if (Entity imported = ModelInstance::SpawnLevel(*m_EditorScene, level))
                    {
                        m_Selection.Select(imported);
                        m_SceneHierarchyPanel.MarkDirty();
                    }
                }
            }

            assetManager->Update();
        }

        RefreshSceneLoadOverlay();
        m_ContentBrowserPanel.Update(ts);
        m_OutputLogPanel.Update(ts);

        // The drawer: an asset dragged out of it closes it (the viewport under it takes the drop); its tabs show what is open;
        // "Dock in Layout" only while a page is in the drawer.
        if (UILayer* uiLayer = Application::Get().GetUILayer(); uiLayer && IsDrawerOpen())
        {
            const UI::UIContext& ui = uiLayer->GetContext();
            if (ui.IsDragging() && m_CenterArea && !m_CenterArea->DrawerRect().Contains(ui.GetLastMousePos()))
                CloseDrawer();
        }
        const bool contentDocked = IsPanelDocked("Content Browser"), logDocked = IsPanelDocked("Output Log");
        if (auto* tab = static_cast<StatusTabButton*>(m_ContentDrawerTab.get()))
            tab->Active = IsDrawerOpen() && m_DrawerPage == DrawerPage::Content;
        if (auto* tab = static_cast<StatusTabButton*>(m_LogDrawerTab.get()))
            tab->Active = IsDrawerOpen() && m_DrawerPage == DrawerPage::Log;
        if (m_ContentDockButton)
            m_ContentDockButton->Visible = !contentDocked;
        if (m_LogDockButton)
            m_LogDockButton->Visible = !logDocked;

        if (!m_Renderer || !m_ActiveScene || !m_ViewportImage)
            return;

        const bool wantHideCursor = m_EditorCamera.ShouldHideCursor() || (m_SceneState != SceneState::Edit && Input::IsRelativeMouseMode());
        if (wantHideCursor != m_CursorHidden)
        {
            if (wantHideCursor)
                SDL_HideCursor();
            else
                SDL_ShowCursor();
            m_CursorHidden = wantHideCursor;
        }

        const UI::UIRect& bounds = m_ViewportImage->GetBounds();
        const uint32_t wantW = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Width())));
        const uint32_t wantH = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Height())));
        if (wantW > 0 && wantH > 0)
        {
            m_ActiveScene->OnViewportResize(wantW, wantH);
            const NRI::Extent2D currentSize = m_Renderer->getViewPortSize();
            UILayer* uiLayer = Application::Get().GetUILayer();
            const bool dockDragging = (uiLayer && uiLayer->GetDockSpace() && uiLayer->GetDockSpace()->IsDragging()) ||
                                      (m_ViewportDockSpace && m_ViewportDockSpace->IsDragging());
            if (!dockDragging && (wantW != currentSize.width || wantH != currentSize.height))
            {
                m_Renderer->onViewportSizeChange({wantW, wantH});
                m_EditorCamera.SetViewportSize(static_cast<float>(wantW), static_cast<float>(wantH));
            }
        }

        const UI::UIElement* targetEl = m_ViewportCanvas ? static_cast<const UI::UIElement*>(m_ViewportCanvas.get())
                                                         : static_cast<const UI::UIElement*>(m_ViewportImage.get());
        const bool hovered = targetEl && UI::HasState(targetEl->GetState(), UI::UIState::Hovered);
        const bool focused = targetEl && UI::HasState(targetEl->GetState(), UI::UIState::Focused);

        m_Viewports[m_ActiveViewport].Size = { wantW, wantH };
        m_Viewports[m_ActiveViewport].Hovered = hovered;
        for (int index = 0; index < kMaxViewports; ++index)
        {
            if (index == m_ActiveViewport)
                continue;
            ViewportSlot& slot = m_Viewports[index];
            if (slot.Open && m_ExtraViewportImages[index] && m_ExtraViewportCanvases[index])
            {
                const UI::UIRect& eb = m_ExtraViewportImages[index]->GetBounds();
                slot.Size = {
                    static_cast<uint32_t>(std::max(0.0f, std::floor(eb.Width()))),
                    static_cast<uint32_t>(std::max(0.0f, std::floor(eb.Height())))
                };
                slot.Hovered = UI::HasState(m_ExtraViewportCanvases[index]->GetState(), UI::UIState::Hovered);
            }
            else
            {
                slot.Size = { 0, 0 };
                slot.Hovered = false;
            }
        }

        Input::SetGameInputEnabled(focused || hovered, hovered);

        m_ActiveScene->SetRenderer(m_Renderer);
        m_ActiveScene->SetRenderer2D(m_Renderer2D);

        switch (m_SceneState)
        {
        case SceneState::Edit:
            m_EditorCamera.OnUpdate(ts, hovered || m_ViewportMouseCaptured);
            m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
            break;
        case SceneState::Simulate:
            m_EditorCamera.OnUpdate(ts, hovered || m_ViewportMouseCaptured);
            m_ActiveScene->OnUpdateSimulation(ts, m_EditorCamera);
            break;
        case SceneState::Play:
            if (m_PlaySession)
                m_PlaySession->Receive(static_cast<float>(ts));
            if (m_PlaySessionEnded)
            {
                OnSceneStop();
                break;
            }
            if (!m_PlayWaitingForServer)
                m_ActiveScene->OnUpdateRuntime(ts);
            if (m_PlaySession)
            {
                m_PlaySession->AfterScene(static_cast<float>(ts));
                m_PlaySession->Send();
            }
            break;
        }

        m_Viewports[m_ActiveViewport].Camera = m_EditorCamera;
        m_Viewports[m_ActiveViewport].CameraSet = true;

        if (m_ViewportCanvas)
        {
            const UI::UIRect& vpBounds = m_ViewportCanvas->GetBounds();
            glm::vec2 mouse = Input::GetMousePosition();
            if (UILayer* uiLayer = Application::Get().GetUILayer())
                mouse = uiLayer->GetContext().GetLastMousePos();

            const int mouseX = static_cast<int>(mouse.x - vpBounds.Min.x);
            const int mouseY = static_cast<int>(mouse.y - vpBounds.Min.y);
            const int vpW = static_cast<int>(vpBounds.Width());
            const int vpH = static_cast<int>(vpBounds.Height());
            if (mouseX >= 0 && mouseY >= 0 && mouseX < vpW && mouseY < vpH)
            {
                const int32_t pixelData = m_Renderer->getPickedEntityID();
                m_HoveredEntity = pixelData == -1 ? Entity() : Entity(static_cast<entt::entity>(pixelData), m_ActiveScene.get());

                if (!m_ViewportTools.IsBoxSelecting)
                {
                    if (hovered)
                        m_Renderer->setPickRequest(mouseX, mouseY, true);
                }
                else
                {
                    const glm::vec2 boxMin = {
                        std::min(m_ViewportTools.BoxSelectStart.x, m_ViewportTools.BoxSelectEnd.x) - vpBounds.Min.x,
                        std::min(m_ViewportTools.BoxSelectStart.y, m_ViewportTools.BoxSelectEnd.y) - vpBounds.Min.y
                    };
                    const glm::vec2 boxMax = {
                        std::max(m_ViewportTools.BoxSelectStart.x, m_ViewportTools.BoxSelectEnd.x) - vpBounds.Min.x,
                        std::max(m_ViewportTools.BoxSelectStart.y, m_ViewportTools.BoxSelectEnd.y) - vpBounds.Min.y
                    };
                    const int startX = std::max(0, static_cast<int>(boxMin.x));
                    const int startY = std::max(0, static_cast<int>(boxMin.y));
                    const int width = std::max(1, static_cast<int>(boxMax.x - boxMin.x));
                    const int height = std::max(1, static_cast<int>(boxMax.y - boxMin.y));
                    m_Renderer->setBoxPickRequest(startX, startY, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
                }
            }
        }

        m_Selection.Prune(); // (entities destroyed this frame -- by a script, a shortcut, the hierarchy -- are not selected)
        if (m_ShowSceneHierarchy)
            m_SceneHierarchyPanel.Update();
        if (m_ShowInspector)
        {
            m_InspectorPanel.Update();
        }
        m_AssetEditors.Update();
        UpdateToolWindows();

        std::vector<int32_t> selectedIDs;
        for (const Entity& entity : m_Selection.GetEntities())
        {
            if (entity)
                selectedIDs.push_back(static_cast<int32_t>(static_cast<uint32_t>(entity)));
        }
        m_Renderer->SetSelectedEntityID(selectedIDs);

        OnOverlayRender();
        SubmitExtraViewports(ts);
        AssetPreviewRenderer::Get().Update(); // the Content Browser thumbnail in progress
        UpdateHistory();
        RefreshToolBar();
        m_ViewportToolbar.Sync();

        m_StatusRefreshTimer += ts;
        if (m_StatusRefreshTimer >= 0.25f)
        {
            m_StatusRefreshTimer = 0.0f;
            RefreshStatusBar();
        }

        m_FpsAccumulator += ts;
        ++m_FpsFrames;
        if (m_FpsAccumulator >= 0.25f)
        {
            if (m_FpsLabel)
            {
                char fpsBuf[32];
                const float fps = static_cast<float>(m_FpsFrames) / m_FpsAccumulator;
                std::snprintf(fpsBuf, sizeof(fpsBuf), "FPS: %.1f", fps);
                m_FpsLabel->Text = fpsBuf;
            }
            m_FpsAccumulator = 0.0f;
            m_FpsFrames = 0;
        }

        Texture2D* sceneTex = m_Renderer->GetSceneResource();
        m_ViewportImage->TextureDescriptorSlot = sceneTex ? sceneTex->GetDescriptorIndexSlot() : ~0u;
        for (int index = 0; index < kMaxViewports; ++index)
        {
            if (!m_ExtraViewportImages[index])
                continue;
            if (index != m_ActiveViewport && m_Viewports[index].Open && m_Viewports[index].Submitted)
            {
                Texture2D* extraTex = m_Renderer->GetExtraViewTexture(static_cast<uint32_t>(index));
                m_ExtraViewportImages[index]->TextureDescriptorSlot = extraTex ? extraTex->GetDescriptorIndexSlot() : ~0u;
            }
            else
            {
                m_ExtraViewportImages[index]->TextureDescriptorSlot = ~0u;
            }
        }
    }

    void EditorV2Layer::OnEvent(Event& event)
    {
        if (!Project::GetActive())
            return;

        if (event.GetEventType() == EventType::ExternalFileDrop)
        {
            const auto& drop = static_cast<const ExternalFileDropEvent&>(event);
            // Into the folder the Content Browser shows (a model opens the import dialog first).
            ShowPage(DrawerPage::Content);
            m_ContentBrowserPanel.ImportFile(drop.GetPath());
            event.Handled = true;
            return;
        }

        // The window's close button / Alt+F4 with unsaved work: kept open, asked first (Application quits after Save / Don't Save).
        if (event.GetEventType() == EventType::WindowClose && HasUnsavedChanges())
        {
            event.Handled = true;
            ConfirmUnsavedChanges("quitting", []() { Application::Get().Shutdown(); });
            return;
        }

        // A press outside the open drawer closes it (not while one of its menus or dialogs is open, and not on the status bar's
        // tabs, which toggle it themselves).
        if (event.GetEventType() == EventType::MouseButtonPressed && IsDrawerOpen())
        {
            if (UILayer* uiLayer = Application::Get().GetUILayer())
            {
                const UI::UIContext& ui = uiLayer->GetContext();
                const UI::UIVec2 pos = ui.GetLastMousePos();
                const bool onTab = (m_ContentDrawerTab && m_ContentDrawerTab->GetBounds().Contains(pos)) ||
                                   (m_LogDrawerTab && m_LogDrawerTab->GetBounds().Contains(pos));
                if (!ui.GetOverlay() && m_CenterArea && !m_CenterArea->DrawerRect().Contains(pos) && !onTab)
                    CloseDrawer();
            }
        }

        // Keys: the focused widget first, then the editor's commands (EditorV2Commands.cpp).
        if (event.GetEventType() == EventType::KeyPressed)
        {
            if (DispatchCommandKey(static_cast<KeyPressedEvent&>(event)))
            {
                event.Handled = true;
                return;
            }
        }

        const UI::UIElement* scrollEl = m_ViewportSlotRoots[m_ActiveViewport]
            ? m_ViewportSlotRoots[m_ActiveViewport].get()
            : (m_ViewportCanvas ? static_cast<const UI::UIElement*>(m_ViewportCanvas.get())
                                : static_cast<const UI::UIElement*>(m_ViewportImage.get()));
        if (!scrollEl)
            return;

        if (event.GetEventType() == EventType::MouseScrolled && m_SceneState == SceneState::Edit)
        {
            const UI::UIElement* hoveredUi = nullptr;
            glm::vec2 mouse = Input::GetMousePosition();
            if (UILayer* uiLayer = Application::Get().GetUILayer())
            {
                hoveredUi = uiLayer->GetContext().GetHovered();
                mouse = uiLayer->GetContext().GetLastMousePos();
            }
            const bool activeViewportHovered =
                (m_ViewportCanvas && (hoveredUi == m_ViewportCanvas.get() || UI::HasState(m_ViewportCanvas->GetState(), UI::UIState::Hovered))) ||
                (m_ExtraViewportCanvases[m_ActiveViewport] && hoveredUi == m_ExtraViewportCanvases[m_ActiveViewport].get());
            if (activeViewportHovered)
            {
                const UI::UIRect& b = scrollEl->GetBounds();
                m_EditorCamera.SetCursorPosition({ mouse.x - b.Min.x, mouse.y - b.Min.y });
                m_EditorCamera.OnEvent(event);
            }
        }
        else if (event.GetEventType() == EventType::MouseButtonReleased)
        {
            const bool anyDown = Input::IsMouseButtonPressed(SDL_BUTTON_LEFT) ||
                                 Input::IsMouseButtonPressed(SDL_BUTTON_RIGHT) ||
                                 Input::IsMouseButtonPressed(SDL_BUTTON_MIDDLE);
            if (!anyDown)
                m_ViewportMouseCaptured = false;
        }
    }

    void EditorV2Layer::BuildUI(bool loadSavedLayout)
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer)
            return;

        if (const auto& dock = uiLayer->GetDockSpaceRef())
        {
            dock->ClearPanels();
            dock->CanClosePanel = [this](const std::string& title) { return m_AssetEditors.CanClosePanel(title); };
        }

        m_ViewportImage = UI::MakeUI<UI::UIImage>();
        m_ViewportImage->AnchorMin = { 0.0f, 0.0f };
        m_ViewportImage->AnchorMax = { 1.0f, 1.0f };
        m_ViewportImage->AnchorOffsets = { 0.0f, 0.0f, 0.0f, 0.0f };
        m_ViewportImage->HitTestMode = UI::UIHitTestMode::None;

        m_ViewportCanvas = UI::MakeUI<ViewportCanvas>();
        m_ViewportCanvas->AnchorMin = { 0.0f, 0.0f };
        m_ViewportCanvas->AnchorMax = { 1.0f, 1.0f };
        m_ViewportCanvas->AnchorOffsets = { 0.0f, 0.0f, 0.0f, 0.0f };
        m_ViewportCanvas->HitTestMode = UI::UIHitTestMode::Self;
        m_ViewportCanvas->Focusable = true;
        m_ViewportCanvas->Bind({
            .Render = m_Renderer,
            .Camera = &m_EditorCamera,
            .ActiveScene = &m_ActiveScene,
            .Selection = &m_Selection,
            .HoveredEntity = &m_HoveredEntity,
            .Tools = &m_ViewportTools,
            .IsPlayModeFn = [this]() { return m_SceneState == SceneState::Play; },
            .IsEditModeFn = [this]() { return m_SceneState == SceneState::Edit; },
            .ShowCameraPositionFn = [this]() { return m_Viewports[m_ActiveViewport].ShowCameraPosition; },
            .ShowActiveFrameFn = [this]() { return OpenViewportCount() > 1; },
            .GridPlaneAxisFn = [this]() { return GridPlaneAxis(); },
            .OnSceneMutated = [this]() { m_HistoryCheckRequested = true; },
            .OnCaptureChanged = [this](bool captured, int) { m_ViewportMouseCaptured = captured; }
        });
        // Assets dragged from the Content Browser: meshes and prefabs are placed where the pointer is (on the surface under it,
        // else on the ground plane); a scene opens.
        m_ViewportCanvas->OnDrop = [this](const UI::UIDragPayload& payload) -> bool
        {
            if (payload.Type != "CONTENT_BROWSER_ITEM" && payload.Type != "CONTENT_BROWSER_ITEMS")
                return false;
            if (m_SceneState != SceneState::Edit)
            {
                Notify(UI::UINotificationSeverity::Info, "Stop Play to place assets", "Assets are placed in the edited scene.");
                return true;
            }
            std::vector<AssetHandle> place;
            for (uint64_t id : payload.Ids.empty() ? std::vector<uint64_t>{ payload.UserId } : payload.Ids)
            {
                const AssetHandle handle(id);
                const AssetType type = AssetManager::GetAssetType(handle);
                if (type == AssetType::Scene && payload.Ids.size() <= 1)
                {
                    ConfirmUnsavedChanges("opening " + AssetDisplayName(handle), [this, handle]() { OpenScene(handle); }, UnsavedScene);
                    return true;
                }
                if (type == AssetType::Mesh || type == AssetType::StaticMesh || type == AssetType::Prefab)
                    place.push_back(handle);
            }
            if (place.empty())
            {
                Notify(UI::UINotificationSeverity::Info, "Nothing to place", "Meshes, prefabs and scenes can be dropped into the viewport.");
                return true;
            }
            glm::dvec3 point{ 0.0 };
            if (!m_ViewportCanvas->PickWorldPoint(point))
                point = m_EditorCamera.GetFocalPoint();
            PlaceAssets(place, point, {});
            return true;
        };

        Ref<UI::UIElement> overlayToolbar = m_ViewportToolbar.Build(m_Renderer, &m_EditorCamera, &m_ViewportTools, {
            .SetViewMode = [this](EditorViewMode mode) { SetViewMode(mode); },
            .SnapToFloor = [this]() { SnapSelectionToFloor(); },
            .SnapToGrid = [this]() { SnapSelectionToGrid(); },
            .GetShowCameraPosition = [this]() { return m_Viewports[m_ActiveViewport].ShowCameraPosition; },
            .SetShowCameraPosition = [this](bool show) { m_Viewports[m_ActiveViewport].ShowCameraPosition = show; }
        });

        // Floating scene load progress banner inside the bottom center of the viewport
        m_SceneLoadTitleLabel = UI::Label("Opening scene...").Get();
        m_SceneLoadStageLabel = UI::Label("").Get();
        m_SceneLoadStageLabel->UseColorOverride = true;
        m_SceneLoadStageLabel->ColorOverride = UI::ThemeColor("SceneLoadText", { 0.72f, 0.76f, 0.82f, 1.0f });

        auto progressTrack = UI::MakeUI<UI::UIElement>();
        progressTrack->WidthMode = UI::UISizeMode::Fixed;
        progressTrack->HeightMode = UI::UISizeMode::Fixed;
        progressTrack->FixedSize = { 300.0f, 6.0f };
        progressTrack->DrawBackground = true;
        progressTrack->StyleClass = "Surface";
        progressTrack->ClipChildren = true;

        m_SceneLoadProgressFill = UI::MakeUI<UI::UIElement>();
        m_SceneLoadProgressFill->WidthMode = UI::UISizeMode::Fixed;
        m_SceneLoadProgressFill->HeightMode = UI::UISizeMode::Fill;
        m_SceneLoadProgressFill->FixedSize = { 0.0f, 6.0f };
        m_SceneLoadProgressFill->DrawBackground = true;
        m_SceneLoadProgressFill->StyleClass = "Button";
        m_SceneLoadProgressFill->StyleVariant = "primary";
        progressTrack->AddChild(m_SceneLoadProgressFill);

        m_SceneLoadBanner = UI::VStack(4.0f, { m_SceneLoadTitleLabel, m_SceneLoadStageLabel, progressTrack })
            .Padding(10.0f, 8.0f)
            .Style("Surface")
            .Background(true)
            .Get();
        m_SceneLoadBanner->WidthMode = UI::UISizeMode::Auto;
        m_SceneLoadBanner->HeightMode = UI::UISizeMode::Auto;
        m_SceneLoadBanner->AnchorMin = { 0.5f, 1.0f };
        m_SceneLoadBanner->AnchorMax = { 0.5f, 1.0f };
        m_SceneLoadBanner->AnchorOffsets = { -160.0f, -96.0f, 160.0f, -20.0f };
        m_SceneLoadBanner->Visible = false;

        auto viewportHost = UI::MakeUI<UI::UIAnchorBox>();
        viewportHost->WidthMode = UI::UISizeMode::Fill;
        viewportHost->HeightMode = UI::UISizeMode::Fill;
        viewportHost->AnchorMin = { 0.0f, 0.0f };
        viewportHost->AnchorMax = { 1.0f, 1.0f };
        viewportHost->AnchorOffsets = { 0.0f, 0.0f, 0.0f, 0.0f };
        viewportHost->AddChild(m_ViewportImage);
        viewportHost->AddChild(m_ViewportCanvas);
        viewportHost->AddChild(overlayToolbar);
        viewportHost->AddChild(m_SceneLoadBanner);
        m_ActiveViewportHost = viewportHost;

        for (int index = 0; index < kMaxViewports; ++index)
        {
            auto extraImg = UI::MakeUI<UI::UIImage>();
            extraImg->AnchorMin = { 0.0f, 0.0f };
            extraImg->AnchorMax = { 1.0f, 1.0f };
            extraImg->AnchorOffsets = { 0.0f, 0.0f, 0.0f, 0.0f };
            extraImg->HitTestMode = UI::UIHitTestMode::None;
            m_ExtraViewportImages[index] = extraImg;

            auto extraCanvas = UI::MakeUI<ExtraViewportCanvas>();
            extraCanvas->AnchorMin = { 0.0f, 0.0f };
            extraCanvas->AnchorMax = { 1.0f, 1.0f };
            extraCanvas->AnchorOffsets = { 0.0f, 0.0f, 0.0f, 0.0f };
            extraCanvas->HitTestMode = UI::UIHitTestMode::Self;
            extraCanvas->Bind({
                .Index = index,
                .Render = m_Renderer,
                .Camera = &m_Viewports[index].Camera,
                .Selection = &m_Selection,
                .GizmoMode = &m_ViewportTools.GizmoMode,
                .ShowCameraPosition = &m_Viewports[index].ShowCameraPosition,
                .ShowFlags = &m_Viewports[index].ShowFlags,
                .ResolutionIndex = &m_Viewports[index].ResolutionIndex,
                .Realtime = &m_Viewports[index].Realtime,
                .IsPlayModeFn = [this]() { return m_SceneState == SceneState::Play; },
                .HasImageFn = [this, index]() { return m_ExtraViewportImages[index] && m_ExtraViewportImages[index]->TextureDescriptorSlot != ~0u; },
                .OnActivate = [this](int idx) { ActivateViewport(idx); }
            });
            m_ExtraViewportCanvases[index] = extraCanvas;

            auto extraHost = UI::MakeUI<UI::UIAnchorBox>();
            extraHost->WidthMode = UI::UISizeMode::Fill;
            extraHost->HeightMode = UI::UISizeMode::Fill;
            extraHost->AnchorMin = { 0.0f, 0.0f };
            extraHost->AnchorMax = { 1.0f, 1.0f };
            extraHost->AnchorOffsets = { 0.0f, 0.0f, 0.0f, 0.0f };
            extraHost->AddChild(extraImg);
            extraHost->AddChild(extraCanvas);
            m_ExtraViewportHosts[index] = extraHost;

            auto root = UI::MakeUI<UI::UIAnchorBox>();
            root->WidthMode = UI::UISizeMode::Fill;
            root->HeightMode = UI::UISizeMode::Fill;
            root->AddChild(index == m_ActiveViewport ? m_ActiveViewportHost : m_ExtraViewportHosts[index]);
            m_ViewportSlotRoots[index] = root;
        }

        // Play / Simulate / Pause / Step: icon buttons with the toolbar's textures.
        const auto makeToolbarIconBtn = [](const Ref<Texture2D>& icon, const std::string& tip, std::function<void()> onClick)
        {
            return UI::UIIconButton::FromTexture(icon ? icon->GetDescriptorIndexSlot() : ~0u, tip, std::move(onClick));
        };

        m_PlayBtnWrap = makeToolbarIconBtn(m_IconPlay, "Play / Stop (Esc)", [this]()
        {
            if (m_SceneState == SceneState::Play)
                OnSceneStop();
            else
                OnScenePlay();
        });

        m_SimulateBtnWrap = makeToolbarIconBtn(m_IconSimulate, "Simulate / Stop", [this]()
        {
            if (m_SceneState == SceneState::Simulate)
                OnSceneStop();
            else
                OnSceneSimulate();
        });

        m_PauseBtnWrap = makeToolbarIconBtn(m_IconPause, "Pause / Resume", [this]()
        {
            if (m_SceneState != SceneState::Edit && m_ActiveScene)
                m_ActiveScene->SetPaused(!m_ActiveScene->IsPaused());
            RefreshToolBar();
        });

        m_StepBtnWrap = makeToolbarIconBtn(m_IconStep, "Step one frame", [this]()
        {
            if (m_SceneState != SceneState::Edit && m_ActiveScene)
                m_ActiveScene->Step();
        });

        m_PlaySettingsCogBtn = UI::MakeUI<UIVectorIconButton>(
            EditorIconKind::Cogwheel,
            "Play Settings (Multiplayer: Net Mode, Players, Emulation)",
            [this]() { ShowPlaySettingsOverlay(m_PlaySettingsCogBtn.get()); });
        m_PlaySettingsCogBtn->FixedSize = { 30.0f, 30.0f };
        m_PlaySettingsCogBtn->IconScale = 1.35f;
        m_PlaySettingsCogBtn->VAlign = UI::UIAlignment::Center;

        m_PlayNetStatusLabel = UI::Label("").VAlign(UI::UIAlignment::Center).Margin(6.0f, 0.0f);
        m_PlayNetStatusLabel->UseColorOverride = true;
        m_PlayNetStatusLabel->ColorOverride = UI::ThemeColor("PlayNetStatus", { 0.72f, 0.78f, 0.86f, 1.0f });
        m_PlayNetStatusLabel->Visible = false;

        auto tbLeftSpacer = UI::MakeUI<UI::UIElement>();
        tbLeftSpacer->WidthMode = UI::UISizeMode::Fill;
        tbLeftSpacer->HeightMode = UI::UISizeMode::Fill;
        auto tbRightSpacer = UI::MakeUI<UI::UIElement>();
        tbRightSpacer->WidthMode = UI::UISizeMode::Fill;
        tbRightSpacer->HeightMode = UI::UISizeMode::Fill;

        m_ToolBar = UI::HStack(4.0f, {
            tbLeftSpacer,
            BuildPrefabBar(),
            m_PlayBtnWrap,
            m_SimulateBtnWrap,
            m_PauseBtnWrap,
            m_StepBtnWrap,
            m_PlaySettingsCogBtn,
            m_PlayNetStatusLabel,
            tbRightSpacer
        }).Padding(6.0f, 2.0f).Style("Surface").Background(true).Get();
        m_ToolBar->WidthMode = UI::UISizeMode::Fill;
        m_ToolBar->HeightMode = UI::UISizeMode::Fill;
        RefreshToolBar();

        // Inner dock space inside "Viewports" holds Viewport 1..4 (matching ImGui NoxViewportDockSpace),
        // so Toolbar / Scene Hierarchy can dock across the full width/height of all 4 viewports cleanly.
        m_ViewportDockSpace = UI::MakeUI<UI::UIDockSpace>();
        m_ViewportDockSpace->WidthMode = UI::UISizeMode::Fill;
        m_ViewportDockSpace->HeightMode = UI::UISizeMode::Fill;
        m_ViewportDockSpace->SetContext(&uiLayer->GetContext());
        m_ViewportDockSpace->AddPanel(
            "Viewport",
            m_ViewportSlotRoots[0],
            UI::UIDockDropZone::Center,
            {},
            0.5f,
            (OpenViewportCount() > 1)
                ? UI::UIDockNodeFlags::NoUndocking
                : (UI::UIDockNodeFlags::NoUndocking | UI::UIDockNodeFlags::NoTabBar)
        );

        for (int index = 1; index < kMaxViewports; ++index)
        {
            if (m_Viewports[index].Open)
            {
                const UI::UIDockDropZone zone = (index == 2) ? UI::UIDockDropZone::Bottom : UI::UIDockDropZone::Right;
                const std::string target = (index == 3 && m_Viewports[1].Open) ? std::string(m_Viewports[1].Title) : std::string("Viewport");
                const UI::UIDockDropZone useZone = (index == 3 && m_Viewports[1].Open) ? UI::UIDockDropZone::Bottom : zone;
                m_ViewportDockSpace->AddPanel(m_Viewports[index].Title, m_ViewportSlotRoots[index], useZone, target, 0.5f);
            }
        }

        // Dock outer panels: Viewports Center -> Scene Hierarchy Left -> Toolbar Top of Viewports (no tab strip)
        uiLayer->AddPanel(
            "Viewports",
            m_ViewportDockSpace,
            UI::UIDockDropZone::Center,
            {},
            0.5f,
            UI::UIDockNodeFlags::CentralNode
        );

        if (m_ShowSceneHierarchy)
        {
            uiLayer->AddPanel(
                "Scene Hierarchy",
                m_SceneHierarchyPanel.BuildUI(),
                UI::UIDockDropZone::Left,
                "Viewports",
                0.155f
            );
        }

        if (m_ShowInspector)
        {
            uiLayer->AddPanel(
                "Inspector",
                m_InspectorPanel.BuildUI(),
                UI::UIDockDropZone::Right,
                "Viewports",
                0.22f
            );
        }

        if (m_ShowToolBar)
        {
            uiLayer->AddPanel(
                "Toolbar",
                m_ToolBar,
                UI::UIDockDropZone::Top,
                "Viewports",
                0.038f,
                UI::UIDockNodeFlags::NoTabBar | UI::UIDockNodeFlags::NoDockingOver
            );
        }

        // The Content Browser and the Output Log: in the drawer (opened from the status bar), or docked under the viewports when
        // the saved layout has them there ("Dock in Layout").
        auto dockButton = [this](DrawerPage page)
        {
            Ref<UI::UIButton> button = UI::MakeUI<UI::UIButton>("Dock in Layout", [this, page]() { DockPage(page); });
            button->Padding = UI::UIThickness::Symmetric(10.0f, 2.0f);
            button->HeightMode = UI::UISizeMode::Fixed;
            button->FixedSize = { 0.0f, 26.0f };
            button->Tooltip = "Keep it open as a tab under the viewport (Window > Back to Drawer puts it back here)";
            return button;
        };
        m_ContentDockButton = dockButton(DrawerPage::Content);
        m_LogDockButton = dockButton(DrawerPage::Log);
        m_ContentBrowserPanel.SetToolbarExtra(m_ContentDockButton);
        m_OutputLogPanel.SetToolbarExtra(m_LogDockButton);
        m_ContentBrowserRoot = m_ContentBrowserPanel.BuildUI();
        m_OutputLogRoot = m_OutputLogPanel.BuildUI();

        std::string savedLayout;
        if (loadSavedLayout)
        {
            std::ifstream layoutFile(LayoutDirectory() / kLayoutFile);
            std::stringstream ss;
            ss << layoutFile.rdbuf();
            savedLayout = ss.str();
        }
        if (savedLayout.find("Content Browser") != std::string::npos)
            uiLayer->AddPanel("Content Browser", m_ContentBrowserRoot, UI::UIDockDropZone::Bottom, "Viewports", 0.30f);
        if (savedLayout.find("Output Log") != std::string::npos)
        {
            const bool besideBrowser = savedLayout.find("Content Browser") != std::string::npos;
            uiLayer->AddPanel("Output Log", m_OutputLogRoot, besideBrowser ? UI::UIDockDropZone::Center : UI::UIDockDropZone::Bottom,
                              besideBrowser ? "Content Browser" : "Viewports", 0.30f);
        }

        // The dock space with the drawer sliding up over it (UI::UIDrawerHost); dragging its top edge keeps the height for the session.
        m_CenterArea = Ref<UI::UIDrawerHost>(new UI::UIDrawerHost(uiLayer->GetDockSpaceRef()));
        m_CenterArea->HeightFraction = m_DrawerFraction;
        m_CenterArea->OnResized = [this](float fraction) { m_DrawerFraction = fraction; };
        m_DrawerPage = DrawerPage::None;

        // Status bar: the drawer's tabs and what runs on the left, the log's warnings / errors on the right; toasts stack above it
        // at the right.
        Ref<StatusTabButton> contentTab(new StatusTabButton("Content Drawer", [this]() { ToggleDrawer(DrawerPage::Content); }));
        contentTab->Tooltip = "The Content Browser (Ctrl+Space)";
        Ref<StatusTabButton> logTab(new StatusTabButton("Output Log", [this]() { ToggleDrawer(DrawerPage::Log); }));
        logTab->Tooltip = "The Output Log and the console's Cmd line (the ` key)";
        m_ContentDrawerTab = contentTab;
        m_LogDrawerTab = logTab;
        m_StatusActivity = UI::MakeUI<StatusActivityElement>();
        m_StatusActivity->WidthMode = UI::UISizeMode::Fill;
        m_StatusActivity->Margin = UI::UIThickness{ 8.0f, 0.0f, 0.0f, 0.0f };
        m_StatusActivity->Tooltip = "Imports, script builds, assets loading and textures streaming show here; Ready when nothing runs";
        m_StatusCounts = Ref<UI::UIElement>(new StatusCountsElement(m_OutputLogPanel, [this]() { ShowOutputLog(); }));
        m_StatusBar = UI::HStack(4.0f, { contentTab, logTab, m_StatusActivity, m_StatusCounts }).Padding(4.0f, 2.0f).Style("Surface").Background(true).Get();
        m_StatusBar->WidthMode = UI::UISizeMode::Fill;
        m_StatusBar->HeightMode = UI::UISizeMode::Fixed;
        m_StatusBar->FixedSize = { 0.0f, kStatusBarHeight };
        uiLayer->GetContext().SetToastBottomInset(kStatusBarHeight + 6.0f);
        RefreshStatusBar();

        if (loadSavedLayout)
        {
            bool shouldLoad = true;
            std::ifstream layoutFile(LayoutDirectory() / kLayoutFile);
            if (layoutFile.is_open())
            {
                std::stringstream ss;
                ss << layoutFile.rdbuf();
                const std::string saved = ss.str();
                if (saved.find("Viewports") == std::string::npos ||
                    (m_ShowSceneHierarchy && saved.find("Scene Hierarchy") == std::string::npos) ||
                    (m_ShowInspector && saved.find("Inspector") == std::string::npos) ||
                    (m_ShowToolBar && (saved.find("Toolbar") == std::string::npos || saved.find("flags: 6") == std::string::npos)))
                    shouldLoad = false;
            }
            if (shouldLoad)
            {
                if (const auto& dock = uiLayer->GetDockSpaceRef(); dock && std::filesystem::exists(LayoutDirectory() / kLayoutFile))
                    dock->LoadLayout(LayoutDirectory() / kLayoutFile);
                if (m_ViewportDockSpace && std::filesystem::exists(LayoutDirectory() / kViewportDockLayoutFile))
                    m_ViewportDockSpace->LoadLayout(LayoutDirectory() / kViewportDockLayoutFile);
            }
        }
        SyncViewportTabBars();

        auto menuBar = UI::MakeUI<UI::UIMenuBar>();
        menuBar->AddDynamicMenu("File", [this, uiLayer]() -> std::vector<UI::UIMenuItem>
        {
            return {
                {"New Project...", "", [this]() { ConfirmUnsavedChanges("making a new project", [this]() { NewProjectAsync(); }); }},
                {"Open Project...", m_Commands.ShortcutText("File.OpenProject"), [this]() { m_Commands.Execute("File.OpenProject"); }},
                {"Project Manager...", "", [this]() { ConfirmUnsavedChanges("closing this project", [this]() { OpenProjectManager(); }); }, false, true, false, false,
                 "Close this project and show the Project Manager (unsaved changes are asked about first)."},
                {"", "", {}, true},
                {"New Scene", m_Commands.ShortcutText("File.NewScene"), [this]() { m_Commands.Execute("File.NewScene"); }},
                {"Save Scene", m_Commands.ShortcutText("File.SaveScene"), [this]() { m_Commands.Execute("File.SaveScene"); }},
                {"Save Scene As...", m_Commands.ShortcutText("File.SaveSceneAs"), [this]() { m_Commands.Execute("File.SaveSceneAs"); }},
                {"Save All", m_Commands.ShortcutText("File.SaveAll"), [this]() { m_Commands.Execute("File.SaveAll"); }, false, true, false, false,
                 "The scene, every asset editor's unsaved edits, the materials edited in the Inspector and the mixer."},
                {"", "", {}, true},
                {"Package Game...", "", [this]() { ShowPackageDialog(false); }, false, true, false, false,
                 "A folder that runs the game on its own (the runtime, scripts, shaders and the cooked assets the scenes need)."},
                {"Package Server...", "", [this]() { ShowPackageDialog(true); }, false, true, false, false,
                 "A dedicated server build (no window, GPU or sound; clients join with --connect)."},
                {"", "", {}, true},
                {"Save Layout", "", [this]()
                {
                    SaveLayoutFiles();
                    Notify(UI::UINotificationSeverity::Success, "Layout saved", "In the project's Saved/Layout folder (it is also saved on exit).");
                }},
                {"Exit", "Alt+F4", [this]() { ConfirmUnsavedChanges("quitting", []() { Application::Get().Shutdown(); }); }}
            };
        });

        menuBar->AddDynamicMenu("Edit", [this]() -> std::vector<UI::UIMenuItem>
        {
            const bool editing = m_SceneState == SceneState::Edit;
            const std::string undoLabel = m_History.CanUndo() ? "Undo " + m_History.GetUndoName() : std::string("Undo");
            const std::string redoLabel = m_History.CanRedo() ? "Redo " + m_History.GetRedoName() : std::string("Redo");
            char historyText[96];
            const size_t steps = m_History.GetStateCount() > 0 ? m_History.GetStateCount() - 1 : 0;
            std::snprintf(historyText, sizeof(historyText), "History: %zu steps, %.1f MB",
                          steps, static_cast<double>(m_History.GetMemoryBytes()) / (1024.0 * 1024.0));
            return {
                {undoLabel, m_Commands.ShortcutText("Edit.Undo"), [this]() { m_Commands.Execute("Edit.Undo"); }, false, m_Commands.CanExecute("Edit.Undo")},
                {redoLabel, m_Commands.ShortcutText("Edit.Redo"), [this]() { m_Commands.Execute("Edit.Redo"); }, false, m_Commands.CanExecute("Edit.Redo")},
                {"", "", {}, true},
                {"Duplicate", m_Commands.ShortcutText("Edit.Duplicate"), [this]() { m_Commands.Execute("Edit.Duplicate"); }, false, m_Commands.CanExecute("Edit.Duplicate")},
                {"Delete", m_Commands.ShortcutText("Edit.Delete"), [this]() { m_Commands.Execute("Edit.Delete"); }, false, m_Commands.CanExecute("Edit.Delete")},
                {"Snap Selection to Floor", m_Commands.ShortcutText("Edit.SnapToFloor"), [this]() { m_Commands.Execute("Edit.SnapToFloor"); }, false, m_Commands.CanExecute("Edit.SnapToFloor")},
                {"", "", {}, true},
                {"Keyboard Shortcuts...", "", [this]() { m_Commands.Execute("Edit.KeyboardShortcuts"); }, false, true, false, false,
                 "Every command and its keys; click a key to change it."},
                {"", "", {}, true},
                {historyText, "", {}, false, false, false, true}
            };
        });

        menuBar->AddDynamicMenu("Create", [this]() -> std::vector<UI::UIMenuItem>
        {
            std::vector<UI::UIMenuItem> shapeItems;
            for (uint8_t index = 0; index < static_cast<uint8_t>(PrimitiveShape::Count); ++index)
            {
                const PrimitiveShape shape = static_cast<PrimitiveShape>(index);
                shapeItems.push_back({PrimitiveShapes::GetName(shape), "", [this, shape]() { CreateShape(shape); }});
            }

            return {
                {"Empty Entity", "", [this]()
                {
                    if (m_ActiveScene)
                    {
                        Entity created = m_ActiveScene->CreateEntity("Empty Entity");
                        m_Selection.Select(created);
                        m_SceneHierarchyPanel.MarkDirty();
                        m_HistoryCheckRequested = true;
                    }
                }},
                {"Shapes", "", {}, false, true, false, false, "", std::move(shapeItems)},
                {"", "", {}, true},
                {"Streaming Test World...", "", [this]() { ShowStreamingTestWorldDialog(); }, false, !m_TestWorldGeneration && m_SceneState == SceneState::Edit, false, false,
                 "A world partition larger than RAM and VRAM (every cell its own terrain mesh and texture), to fly through and see streaming keep up."},
                {"Cancel Streaming Test World", "", [this]() { CancelStreamingTestWorld(); }, false, m_TestWorldGeneration != nullptr, false, false,
                 "Stops the generation (what was written stays on disk, unregistered)."}
            };
        });

#if NOX_WITH_MCP
        menuBar->AddDynamicMenu("Tools", [this]() -> std::vector<UI::UIMenuItem>
        {
            return { {"MCP Server", "", {}, false, true, false, false, "", BuildMcpMenu()} };
        });
#endif

        menuBar->AddDynamicMenu("Window", [this, uiLayer]() -> std::vector<UI::UIMenuItem>
        {
            std::vector<UI::UIMenuItem> viewportItems = {
                {"1 Viewport", "", [this]() { ApplyViewportLayout(ViewportLayout::One); }},
                {"2 Side by Side", "", [this]() { ApplyViewportLayout(ViewportLayout::TwoSideBySide); }},
                {"2 Stacked", "", [this]() { ApplyViewportLayout(ViewportLayout::TwoStacked); }},
                {"3 Viewports", "", [this]() { ApplyViewportLayout(ViewportLayout::Three); }},
                {"4 Viewports (2 x 2)", "", [this]() { ApplyViewportLayout(ViewportLayout::Four); }},
                {"", "", {}, true}
            };
            for (int index = 1; index < kMaxViewports; ++index)
            {
                const bool open = m_Viewports[index].Open;
                viewportItems.push_back({
                    m_Viewports[index].Title,
                    "",
                    [this, index, open]()
                    {
                        if (open)
                            CloseViewport(index);
                        else
                            OpenViewport(index);
                    },
                    false, true, open
                });
            }
            viewportItems.push_back({"", "", {}, true});
            for (int index = 0; index < kMaxViewports; ++index)
            {
                viewportItems.push_back({
                    std::string(m_Viewports[index].Title) + " Options",
                    "",
                    {},
                    false, true, false, false, "",
                    BuildViewportOptionsItems(index)
                });
            }
            viewportItems.push_back({"", "", {}, true});
            viewportItems.push_back({"Click a viewport to use it; drag its tab to move it", "", {}, false, false, false, true});

            std::vector<UI::UIMenuItem> animGraphItems;
            if (Ref<Project> project = Project::GetActive(); project && project->GetEditorAssetManager())
            {
                for (const auto& [handle, metadata] : project->GetEditorAssetManager()->GetAssetRegistry())
                {
                    if (metadata.Type != AssetType::AnimationGraph)
                        continue;
                    const AssetHandle graphHandle = handle;
                    animGraphItems.push_back({metadata.FilePath.stem().string(), "", [this, graphHandle]() { OpenAssetV2(graphHandle); }, false, true, false, false,
                                              "Opens the graph in its editor window"});
                }
            }
            if (animGraphItems.empty())
                animGraphItems.push_back({"No .nanimgraph assets found", "", {}, false, false, false, true});

            return {
                {"Scene Hierarchy", "", [this, uiLayer]()
                {
                    m_ShowSceneHierarchy = !m_ShowSceneHierarchy;
                    if (const auto& dock = uiLayer->GetDockSpaceRef())
                    {
                        if (m_ShowSceneHierarchy)
                            dock->AddPanel("Scene Hierarchy", m_SceneHierarchyPanel.BuildUI(), UI::UIDockDropZone::Left, "Viewports", 0.155f);
                        else
                            dock->RemovePanel("Scene Hierarchy");
                    }
                }, false, true, m_ShowSceneHierarchy},
                {"Inspector", "", [this, uiLayer]()
                {
                    m_ShowInspector = !m_ShowInspector;
                    if (const auto& dock = uiLayer->GetDockSpaceRef())
                    {
                        if (m_ShowInspector)
                        {
                            dock->AddPanel("Inspector", m_InspectorPanel.BuildUI(), UI::UIDockDropZone::Right, "Viewports", 0.22f);
                            m_InspectorPanel.MarkDirty();
                            m_InspectorPanel.Update();
                        }
                        else
                        {
                            dock->RemovePanel("Inspector");
                        }
                    }
                }, false, true, m_ShowInspector},
                {"Toolbar", "", [this, uiLayer]()
                {
                    m_ShowToolBar = !m_ShowToolBar;
                    if (const auto& dock = uiLayer->GetDockSpaceRef())
                    {
                        if (m_ShowToolBar)
                            dock->AddPanel("Toolbar", m_ToolBar, UI::UIDockDropZone::Top, "Viewports", 0.038f,
                                           UI::UIDockNodeFlags::NoTabBar | UI::UIDockNodeFlags::NoDockingOver);
                        else
                            dock->RemovePanel("Toolbar");
                    }
                }, false, true, m_ShowToolBar},
                {"Renderer Settings", "", [this, uiLayer]()
                {
                    if (!m_RendererSettings.GetRoot())
                        m_RendererSettings.BuildUI(&uiLayer->GetContext());
                    OpenToolWindow("Renderer Settings", m_RendererSettings.GetRoot(), { 470.0f, 720.0f });
                }, false, true, false, false, "How this session renders: debug views, exposure, DLSS, shadows, ray tracing, GI, denoisers, streaming, post effects (not saved)."},
                {"Project Settings", "", [this, uiLayer]()
                {
                    if (!m_ProjectSettings.GetRoot())
                        m_ProjectSettings.BuildUI(&uiLayer->GetContext());
                    OpenToolWindow("Project Settings", m_ProjectSettings.GetRoot(), { 470.0f, 640.0f });
                }, false, true, false, false, "What the project keeps: start scene, render quality and its saved settings, game window, physical surfaces."},
                {"Render Graph", "", [this, uiLayer]()
                {
                    if (!m_RenderGraphPanel.GetRoot())
                    {
                        m_RenderGraphPanel.SetRenderer(m_Renderer);
                        m_RenderGraphPanel.BuildUI(&uiLayer->GetContext());
                    }
                    OpenToolWindow("Render Graph", m_RenderGraphPanel.GetRoot(), { 1000.0f, 700.0f });
                }, false, true, false, false, "The frame's passes, resources, validation and a texture inspector; culling and LOD switches."},
                {"Content Browser", m_Commands.ShortcutText("Window.ContentDrawer"), [this]() { ShowPage(DrawerPage::Content); }, false, true, false, false,
                 "In the drawer over the viewport (or its tab, when it is docked in the layout)."},
                {"Output Log", m_Commands.ShortcutText("Window.OutputLogCommand"), [this]() { ShowPage(DrawerPage::Log); }},
                {IsPanelDocked("Content Browser") ? "Content Browser: Back to Drawer" : "Content Browser: Dock in Layout", "",
                 [this]() { if (IsPanelDocked("Content Browser")) UndockPage(DrawerPage::Content); else DockPage(DrawerPage::Content); }},
                {IsPanelDocked("Output Log") ? "Output Log: Back to Drawer" : "Output Log: Dock in Layout", "",
                 [this]() { if (IsPanelDocked("Output Log")) UndockPage(DrawerPage::Log); else DockPage(DrawerPage::Log); }},
                {"Profiler", "", [this, uiLayer]()
                {
                    if (!m_ProfilerPanel.GetRoot())
                        m_ProfilerPanel.BuildUI(&uiLayer->GetContext());
                    OpenToolWindow("Profiler", m_ProfilerPanel.GetRoot(), { 1100.0f, 680.0f });
                }, false, true, false, false, "Frame times, the frame's timeline per thread and GPU queue, scopes, spikes and captures."},
                {"World Partition", "", [this, uiLayer]()
                {
                    if (!m_WorldPartitionPanel.GetRoot())
                        m_WorldPartitionPanel.BuildUI(&uiLayer->GetContext());
                    OpenToolWindow("World Partition", m_WorldPartitionPanel.GetRoot(), { 560.0f, 680.0f });
                }, false, true, false, false, "A world's cells on a map: load and unload them, its settings, HLODs."},
                {"Levels", "", [this, uiLayer]()
                {
                    if (!m_LevelsPanel.GetRoot())
                        m_LevelsPanel.BuildUI(&uiLayer->GetContext());
                    OpenToolWindow("Levels", m_LevelsPanel.GetRoot(), { 820.0f, 320.0f });
                }, false, true, false, false, "The open scene and its sub-levels: current, visible, locked, loading, placement."},
                {AudioMixer::IsDirty() ? "Mixer *" : "Mixer", "", [this, uiLayer]()
                {
                    if (!m_MixerPanel.GetRoot())
                        m_MixerPanel.BuildUI(&uiLayer->GetContext());
                    OpenToolWindow("Mixer", m_MixerPanel.GetRoot(), { 960.0f, 680.0f });
                }, false, true, false, false, "The project's audio mixer: buses, effects, sends, ducking, snapshots, acoustics (* : unsaved)."},
                {"Viewports", "", {}, false, true, false, false, "", std::move(viewportItems)},
                {"Animation Graphs", "", {}, false, true, false, false, "", std::move(animGraphItems)},
                {"", "", {}, true},
                {"Reset Layout", "", [this]() { ConfirmUnsavedChanges("resetting the layout (it closes the asset editor windows)", [this]() { m_ResetLayoutRequested = true; }, UnsavedAssets); }, false, true, false, false,
                 "Back to the default panels (closes windows torn off into their own OS window and the graph and material windows); the saved layout is replaced."},
                {"Reload Theme", "", [uiLayer]() { uiLayer->ReloadTheme(); }}
            };
        });

        menuBar->AddDynamicMenu("Script", [this]() -> std::vector<UI::UIMenuItem>
        {
            const bool waitForDbg = ScriptEngine::GetWaitForDebugger();
            const bool canReload = ScriptEngine::IsInitialized();
            std::vector<UI::UIMenuItem> items = {
                {"Build and Reload", m_Commands.ShortcutText("Script.Build"), [this]() { m_Commands.Execute("Script.Build"); }, false, m_Commands.CanExecute("Script.Build")},
                {"Reload assembly", m_Commands.ShortcutText("Script.ReloadAssembly"), [this]() { m_Commands.Execute("Script.ReloadAssembly"); }, false, canReload},
                {"Build on Save", "", [this]() { m_ScriptBuilder.AutoBuild = !m_ScriptBuilder.AutoBuild; }, false, true, m_ScriptBuilder.AutoBuild, false,
                 "Saving a .cs file of the project builds its scripts and reloads them a moment later."},
                {"Wait for Debugger on Play", "", [waitForDbg]() { ScriptEngine::SetWaitForDebugger(!waitForDbg); }, false, true, waitForDbg, false,
                 "Play waits (up to 60 s) until a .NET debugger attaches to the editor, then breaks once."}
            };
            if (!m_ScriptBuilder.GetStatus().empty())
            {
                items.push_back({"", "", {}, true});
                items.push_back({m_ScriptBuilder.GetStatus(), "", {}, false, false, false, true});
            }
            return items;
        });

        auto spacer = UI::MakeUI<UI::UIElement>();
        spacer->WidthMode = UI::UISizeMode::Fill;
        menuBar->AddChild(spacer);

        Renderer* renderer = Application::Get().GetRenderer();
        auto vsyncBox = UI::CheckBox("VSync", renderer ? renderer->getVSync() : false, [renderer](bool enabled)
        {
            if (renderer)
                renderer->setVSync(enabled);
        }).VAlign(UI::UIAlignment::Center).Margin(6.0f, 0.0f);
        menuBar->AddChild(vsyncBox);

        m_FpsLabel = UI::Label("FPS: --.-").VAlign(UI::UIAlignment::Center).Margin(10.0f, 0.0f);
        m_FpsLabel->UseColorOverride = true;
        m_FpsLabel->ColorOverride = UI::ThemeColor("FpsText", { 0.72f, 0.76f, 0.82f, 1.0f });
        menuBar->AddChild(m_FpsLabel);

        uiLayer->SetRoot(
            UI::VStack(0.0f, {
                           menuBar,
                           m_CenterArea,
                           m_StatusBar
                       })
            .Padding(0.0f)
            .Style("Background")
            .Background(true)
        );
        m_LayoutBuilt = true;
    }

    void EditorV2Layer::OpenAssetV2(AssetHandle handle)
    {
        if (handle == 0 || !AssetManager::IsAssetHandleValid(handle))
            return;

        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer || !uiLayer->GetDockSpaceRef())
            return;

        const AssetType type = AssetManager::GetAssetType(handle);
        if (type == AssetType::Scene)
        {
            ConfirmUnsavedChanges("opening " + AssetDisplayName(handle), [this, handle]() { OpenScene(handle); }, UnsavedScene);
            return;
        }
        if (type == AssetType::Prefab)
        {
            OpenPrefabMode(handle);
            return;
        }
        // The editor registered for the type (RegisterAssetEditors); one window per asset.
        if (m_AssetEditors.Open(handle) || !AssetManager::IsAssetHandleValid(handle))
            return;
        if (!m_AssetEditors.HasEditorFor(type))
            Notify(UI::UINotificationSeverity::Info, "No editor for " + AssetDisplayName(handle) + " yet",
                   "NoxEditorV2 opens scenes, prefabs, materials, graphs, animation clips and timelines so far; the old NoxEditor has the other editors.");
    }

    void EditorV2Layer::RegisterAssetEditors()
    {
        AssetEditorRegistry& registry = m_AssetEditors.GetRegistry();
        const auto graph = [](AssetHandle handle) -> std::unique_ptr<AssetEditorV2> { return std::make_unique<NodeGraphEditorV2>(handle); };
        registry.Register(AssetType::AnimationGraph, graph);
        registry.Register(AssetType::AudioGraph, graph);
        registry.Register(AssetType::MaterialGraph, graph);
        registry.Register(AssetType::Material, [](AssetHandle handle) -> std::unique_ptr<AssetEditorV2> { return std::make_unique<MaterialEditorV2>(handle); });
        registry.Register(AssetType::AnimationSequence, [](AssetHandle handle) -> std::unique_ptr<AssetEditorV2> { return std::make_unique<AnimationClipEditorV2>(handle); });
        registry.Register(AssetType::Timeline, [this](AssetHandle handle) -> std::unique_ptr<AssetEditorV2>
        {
            TimelineEditorV2::Hooks timelineHooks;
            timelineHooks.ActiveScene = [this]() { return m_ActiveScene; };
            timelineHooks.Editing = [this]() { return m_SceneState == SceneState::Edit; };
            timelineHooks.Selected = [this]() { return m_Selection.GetPrimary(); };
            return std::make_unique<TimelineEditorV2>(handle, std::move(timelineHooks));
        });

        AssetEditorManager::Hooks hooks;
        hooks.Notify = [this](UI::UINotification note) { Notify(note.Severity, std::move(note.Title), std::move(note.Message)); };
        hooks.AskSave = [this](const std::string& text, std::function<void()> save, std::function<void()> dontSave)
        {
            ShowSaveChangesDialog(text, std::move(save), std::move(dontSave));
        };
        hooks.OpenAsset = [this](AssetHandle handle) { OpenAssetV2(handle); };
        m_AssetEditors.SetHooks(std::move(hooks));

        RendererSettingsV2Panel::Hooks rendererHooks;
        rendererHooks.Render = m_Renderer;
        rendererHooks.Camera = &m_EditorCamera;
        rendererHooks.Hovered = [this]() { return m_HoveredEntity; };
        rendererHooks.DumpFrameGraph = [this]()
        {
            // Next to Nox.log; open the .dot files with GraphViz.
            const std::filesystem::path directory = std::filesystem::current_path();
            if (!m_ActiveScene || !m_ActiveScene->DumpSystemGraphs(directory))
                return false;
            Notify(UI::UINotificationSeverity::Success, "Frame graph written", (directory / "SceneUpdate.dot").generic_string());
            return true;
        };
        m_RendererSettings.SetHooks(std::move(rendererHooks));

        ProjectSettingsV2Panel::Hooks projectHooks;
        projectHooks.Render = m_Renderer;
        projectHooks.ApplyQuality = [this](int quality) { ApplyQualityPreset(quality); };
        projectHooks.SurfaceRenamed = [this](const std::string& from, const std::string& to)
        {
            if (!m_ActiveScene)
                return;
            bool changed = false;
            for (auto [entity, body] : m_ActiveScene->GetAllEntitiesWith<RigidBody3DComponent>().each())
            {
                if (body.PhysicalSurface == from)
                {
                    body.PhysicalSurface = to;
                    changed = true;
                }
            }
            if (changed)
                m_HistoryCheckRequested = true; // (the scene changed: an undo step, and unsaved)
        };
        m_ProjectSettings.SetHooks(std::move(projectHooks));

        LevelsV2Panel::Hooks levelHooks;
        levelHooks.ActiveScene = [this]() { return m_ActiveScene; };
        levelHooks.Editing = [this]() { return m_SceneState == SceneState::Edit && !m_PrefabMode.Active; };
        levelHooks.OpenScene = [this]() { return EditorSceneHandle(); };
        levelHooks.Selection = [this]() { return m_Selection.GetEntities(); };
        levelHooks.Save = [this]() { SaveScene(); };
        levelHooks.SceneChanged = [this]() { m_HistoryCheckRequested = true; m_SceneHierarchyPanel.MarkDirty(); };
        levelHooks.AskSave = [this](const std::string& text, std::function<void()> save, std::function<void()> dontSave)
        {
            ShowSaveChangesDialog(text, std::move(save), std::move(dontSave));
        };
        m_LevelsPanel.SetHooks(std::move(levelHooks));

        WorldPartitionV2Panel::Hooks worldHooks;
        worldHooks.ActiveScene = [this]() { return m_ActiveScene; };
        worldHooks.Editing = [this]() { return m_SceneState == SceneState::Edit && !m_PrefabMode.Active; };
        worldHooks.StreamingSource = [this]()
        {
            // The camera the cells stream around: the primary camera in Play, the editor camera in Simulate; editing, what the
            // editor camera looks at (where a world opens its cells).
            glm::dvec3 source = m_SceneState == SceneState::Edit ? m_EditorCamera.GetFocalPoint() : m_EditorCamera.GetPosition();
            if (m_SceneState == SceneState::Play && m_ActiveScene)
                if (Entity camera = m_ActiveScene->GetPrimaryCameraEntity(); camera && camera.HasComponent<WorldTransformComponent>())
                    source = camera.GetComponent<WorldTransformComponent>().GetPosition();
            return source;
        };
        worldHooks.Save = [this]() { SaveScene(); };
        worldHooks.SceneChanged = [this]() { m_HistoryCheckRequested = true; };
        worldHooks.BuildHLODs = [this]() { StartHLODBuild(); };
        worldHooks.RunningBuild = [this]() -> const HLODBuilder* { return m_HLODBuild.get(); };
        worldHooks.AskSave = [this](const std::string& text, std::function<void()> save, std::function<void()> dontSave)
        {
            ShowSaveChangesDialog(text, std::move(save), std::move(dontSave));
        };
        m_WorldPartitionPanel.SetHooks(std::move(worldHooks));
    }

    void EditorV2Layer::CancelHLODBuild(const char* reason)
    {
        if (!m_HLODBuild)
            return;
        NOX_CORE_WARN("Build HLODs cancelled ({}): the HLODs built so far are imported, the HLOD grid is not written", reason);
        Notify(UI::UINotificationSeverity::Warning, "HLOD build cancelled", std::string(reason) + ": the HLOD grid was not written.");
        m_HLODBuild.reset();
    }

    void EditorV2Layer::StartHLODBuild()
    {
        if (m_HLODBuild || m_SceneLoad || m_SceneState != SceneState::Edit || m_PrefabMode.Active || !m_EditorScene || !m_EditorScene->IsWorldPartitioned())
            return;
        if (m_EditorScenePath.empty())
        {
            Notify(UI::UINotificationSeverity::Warning, "Save the scene first", "Its cells are built from their files.");
            return;
        }
        SaveScene(); // the cells' files are what is built
        m_HLODBuild = CreateScope<HLODBuilder>(m_EditorScene);
        if (m_HLODBuild->Failed())
        {
            m_HLODBuild.reset();
            Notify(UI::UINotificationSeverity::Error, "HLOD build not started", "See the Output Log.");
        }
    }


    void EditorV2Layer::ApplyQualityPreset(int quality)
    {
        if (!m_Renderer)
            return;
        m_Renderer->ApplyQualityPreset(static_cast<Renderer::RenderQuality>(quality));
        // An orthographic view stays raster only; the preset's features come back when it is left.
        if (m_EditorCamera.IsOrthographic())
        {
            m_OrthoSaved = { true, m_Renderer->isDLSSEnabled(), m_Renderer->getRayTracingEnabled(), m_Renderer->isPathTracingEnabled() };
            m_Renderer->setDLSSEnabled(false);
            m_Renderer->setRayTracingEnabled(false);
            m_Renderer->setPathTracingEnabled(false);
        }
    }

    void EditorV2Layer::OpenToolWindow(const std::string& title, const Ref<UI::UIElement>& root, UI::UIVec2 size)
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer || !uiLayer->GetDockSpaceRef() || !root)
            return;
        const auto& dock = uiLayer->GetDockSpaceRef();
        if (const Ref<UI::UIWindow> window = dock->FindFloatingWindow(title))
        {
            if (window->OnActivate)
                window->OnActivate();
            return;
        }
        if (dock->FindNodeByPanelTitle(title))
        {
            ActivateDockedTab(*dock, title);
            return;
        }
        if (dock->HasPanel(title))
            return; // (torn off into its own OS window)
        dock->FloatPanel(title, root, { 260.0f, 90.0f }, size, false);
    }

    void EditorV2Layer::UpdateToolWindows()
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        const auto& dock = uiLayer ? uiLayer->GetDockSpaceRef() : nullptr;
        if (!dock)
            return;
        if (m_RendererSettings.GetRoot() && dock->HasPanel("Renderer Settings"))
            m_RendererSettings.Update();
        if (m_ProjectSettings.GetRoot() && dock->HasPanel("Project Settings"))
            m_ProjectSettings.Update();
        if (m_LevelsPanel.GetRoot() && dock->HasPanel("Levels"))
            m_LevelsPanel.Update();
        if (m_WorldPartitionPanel.GetRoot() && dock->HasPanel("World Partition"))
            m_WorldPartitionPanel.Update();
        if (m_MixerPanel.GetRoot() && dock->HasPanel("Mixer"))
            m_MixerPanel.Update();
        // (every frame: closed, it stops the profiler keeping frames)
        m_ProfilerPanel.Update(m_ProfilerPanel.GetRoot() && dock->HasPanel("Profiler"));
        m_RenderGraphPanel.Update(m_RenderGraphPanel.GetRoot() && dock->HasPanel("Render Graph")); // (closed: no report is made)
    }

    std::filesystem::path EditorV2Layer::LayoutDirectory() const
    {
        if (!Project::GetActive())
            return {};
        return Project::GetProjectDirectory() / "Saved" / "Layout";
    }

    void EditorV2Layer::SaveLayoutFiles()
    {
        const std::filesystem::path directory = LayoutDirectory();
        if (directory.empty() || !m_LayoutBuilt)
            return;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (UILayer* uiLayer = Application::Get().GetUILayer())
        {
            if (const auto& dock = uiLayer->GetDockSpaceRef())
                dock->SaveLayout(directory / kLayoutFile);
        }
        if (m_ViewportDockSpace)
            m_ViewportDockSpace->SaveLayout(directory / kViewportDockLayoutFile);
        SaveViewportPrefs();
    }

    void EditorV2Layer::ResetLayout()
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer)
            return;

        // Panels torn off into OS windows close first: the default layout docks every panel again, and a window left open would
        // keep a second copy of one.
        if (UI::UIViewportManager* windows = uiLayer->GetViewportManager())
            windows->Shutdown();

        // The saved files go, so the next start shows the default layout too (until the layout is saved again on exit).
        std::error_code error;
        for (const char* file : { kLayoutFile, kViewportDockLayoutFile, kViewportPrefsFile })
            std::filesystem::remove(LayoutDirectory() / file, error);

        for (int i = 0; i < kMaxViewports; ++i)
        {
            m_Viewports[i].Open = (i == 0);
            m_Viewports[i].Camera.SetViewMode(static_cast<EditorViewMode>(kDefaultSlotViewModes[i]));
        }
        m_EditorCamera.SetViewMode(static_cast<EditorViewMode>(kDefaultSlotViewModes[0]));
        m_ActiveViewport = 0;
        m_ShowSceneHierarchy = true;
        m_ShowInspector = true;
        m_ShowToolBar = true;
        m_AssetEditors.CloseAll(); // (their windows go with the old layout; unsaved edits were asked about)
        m_DrawerPage = DrawerPage::None;
        m_DrawerFraction = 0.42f;

        BuildUI(false);
        SaveLayoutFiles();
        Notify(UI::UINotificationSeverity::Success, "Layout reset", "The default panels are back.");
    }

    bool EditorV2Layer::IsPanelDocked(const std::string& title) const
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        return uiLayer && uiLayer->GetDockSpaceRef() && uiLayer->GetDockSpaceRef()->FindNodeByPanelTitle(title) != nullptr;
    }

    namespace
    {
        const char* PageTitle(int page) { return page == 2 ? "Output Log" : "Content Browser"; }

        void ActivateDockedTab(UI::UIDockSpace& dock, const std::string& title)
        {
            if (Ref<UI::UIDockNode> node = dock.FindNodeByPanelTitle(title); node && node->TabPanel)
            {
                for (size_t i = 0; i < node->TabPanel->GetTabCount(); ++i)
                {
                    if (node->TabPanel->GetTabTitle(i) == title)
                        node->TabPanel->SetActiveTab(i);
                }
            }
        }
    }

    bool EditorV2Layer::IsDrawerOpen() const
    {
        return m_CenterArea && m_CenterArea->Open;
    }

    void EditorV2Layer::ShowPage(DrawerPage page)
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer || !uiLayer->GetDockSpaceRef() || page == DrawerPage::None)
            return;
        const std::string title = PageTitle(static_cast<int>(page));
        if (IsPanelDocked(title))
        {
            ActivateDockedTab(*uiLayer->GetDockSpaceRef(), title);
            return;
        }
        OpenDrawer(page);
    }

    void EditorV2Layer::ToggleDrawer(DrawerPage page)
    {
        if (IsPanelDocked(PageTitle(static_cast<int>(page))))
        {
            ShowPage(page);
            return;
        }
        if (IsDrawerOpen() && m_DrawerPage == page)
            CloseDrawer();
        else
            OpenDrawer(page);
    }

    void EditorV2Layer::OpenDrawer(DrawerPage page)
    {
        if (!m_CenterArea || page == DrawerPage::None || IsPanelDocked(PageTitle(static_cast<int>(page))))
            return;
        Ref<UI::UIElement> root = page == DrawerPage::Content ? m_ContentBrowserRoot : m_OutputLogRoot;
        if (!root)
            return;
        m_CenterArea->SetPage(root);
        m_DrawerPage = page;
        m_CenterArea->HeightFraction = m_DrawerFraction;
        m_CenterArea->Open = true;
    }

    void EditorV2Layer::CloseDrawer()
    {
        if (m_CenterArea)
            m_CenterArea->Open = false;
    }

    void EditorV2Layer::DockPage(DrawerPage page)
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer || !uiLayer->GetDockSpaceRef() || page == DrawerPage::None)
            return;
        const auto& dock = uiLayer->GetDockSpaceRef();
        const std::string title = PageTitle(static_cast<int>(page));
        if (dock->FindNodeByPanelTitle(title))
            return;
        Ref<UI::UIElement> root = page == DrawerPage::Content ? m_ContentBrowserRoot : m_OutputLogRoot;
        if (!root)
            return;
        // Out of the drawer (closed at once: the page is needed in the dock now).
        if (m_DrawerPage == page)
        {
            if (m_CenterArea)
            {
                m_CenterArea->SetPage({});
                m_CenterArea->CloseNow();
            }
            m_DrawerPage = DrawerPage::None;
        }
        // Under the viewports, as a tab beside the other page when that one is docked already.
        const std::string partner = PageTitle(page == DrawerPage::Log ? 1 : 2);
        if (dock->FindNodeByPanelTitle(partner))
            dock->AddPanel(title, root, UI::UIDockDropZone::Center, partner);
        else
            dock->AddPanel(title, root, UI::UIDockDropZone::Bottom, "Viewports", 0.30f);
        ActivateDockedTab(*dock, title);
    }

    void EditorV2Layer::UndockPage(DrawerPage page)
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer || !uiLayer->GetDockSpaceRef())
            return;
        uiLayer->GetDockSpaceRef()->RemovePanel(PageTitle(static_cast<int>(page)));
        OpenDrawer(page);
    }

    AssetHandle EditorV2Layer::CreatePrefab(UUID dragged, const std::filesystem::path& relativeFolder)
    {
        if (!m_ActiveScene || !Project::GetActive())
            return 0;
        Entity draggedEntity = m_ActiveScene->GetEntityByUUID(dragged);
        if (!draggedEntity)
        {
            NOX_CORE_WARN("Create Prefab: drag entities from the Scene Hierarchy (a folder row is not an entity)");
            return 0;
        }

        // The dragged entity, or the whole selection when it is part of it; the dragged one first (the pivot of the others).
        std::vector<Entity> entities{ draggedEntity };
        if (m_Selection.Contains(draggedEntity))
        {
            for (Entity selected : m_Selection.GetEntities())
            {
                if (selected && !(selected == draggedEntity))
                    entities.push_back(selected);
            }
        }

        std::string name = entities.size() == 1 ? draggedEntity.GetName() : std::string("New Prefab");
        for (char& character : name)
        {
            if (std::strchr("\\/:*?\"<>|", character) != nullptr)
                character = '_';
        }
        if (name.empty())
            name = "New Prefab";

        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        std::filesystem::path relativePath = relativeFolder / (name + ".nprefab");
        for (int suffix = 2; std::filesystem::exists(assetDirectory / relativePath); ++suffix)
            relativePath = relativeFolder / (name + " " + std::to_string(suffix) + ".nprefab");

        if (!SceneSerializer::SerializePrefab(*m_ActiveScene, entities, relativePath.stem().string(), assetDirectory / relativePath))
        {
            NOX_CORE_ERROR("Create Prefab: could not write {}", relativePath.generic_string());
            return 0;
        }
        return Project::GetActive()->GetEditorAssetManager()->RegisterExistingFile(relativePath, AssetType::Prefab);
    }

    void EditorV2Layer::Notify(UI::UINotificationSeverity severity, std::string title, std::string message)
    {
        if (UILayer* uiLayer = Application::Get().GetUILayer())
            uiLayer->GetContext().PushNotification({ severity, std::move(title), std::move(message) });
    }
}
